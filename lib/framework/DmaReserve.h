#pragma once

// Contiguous internal-RAM reserve (novotnyllc/MoonLight#15).
// Preset switching and network I/O fragment internal DMA-capable RAM long before
// total heap is exhausted. Carve a block while healthy; release it when the largest
// contiguous block drops below the floor so mDNS/WiFi can allocate again.
#include <esp_heap_caps.h>

#if defined(ARDUINO) && ARDUINO
  #include <Arduino.h>
  #define DMA_RESERVE_LOG(...) ESP_LOGW("DmaReserve", __VA_ARGS__)
#else
  #define DMA_RESERVE_LOG(...)
#endif

namespace dmaReserve {

constexpr size_t kReserveBytes = 4096;       // mDNS announce + lwIP burst
constexpr size_t kReleaseFloorBytes = 2048;  // release when largest block drops below this
constexpr size_t kRearmMinBytes = 4096;    // re-arm when heap recovers (was 8192)
constexpr uint32_t kCheckIntervalMs = 10000;
constexpr uint32_t kBootGraceMs = 60000;

inline uint8_t* s_block = nullptr;
inline uint32_t s_releases = 0;
inline uint32_t s_lastCheck = 0;
inline bool s_bootGraceComplete = false;

inline size_t largestInternalBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}

inline bool armed() { return s_block != nullptr; }
inline uint32_t releases() { return s_releases; }

inline void init() {
  if (s_block) return;
  s_block = (uint8_t*)heap_caps_malloc(kReserveBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (s_block)
    DMA_RESERVE_LOG("armed %u bytes (largest DMA=%u)", (unsigned)kReserveBytes,
                    (unsigned)largestInternalBlock());
  else
    DMA_RESERVE_LOG("init failed: no %u-byte contiguous block (largest=%u)",
                    (unsigned)kReserveBytes, (unsigned)largestInternalBlock());
}

// Returns true when the reserve was just released (caller should restart mDNS).
inline bool check(uint32_t nowMs) {
  if (!s_bootGraceComplete) {
    if (nowMs >= kBootGraceMs) s_bootGraceComplete = true;
  }
  if (s_lastCheck && (uint32_t)(nowMs - s_lastCheck) < kCheckIntervalMs) return false;
  s_lastCheck = nowMs;

  const size_t largest = largestInternalBlock();
  if (s_block) {
    // Emergency release is never blocked by boot grace.
    if (largest < kReleaseFloorBytes) {
      heap_caps_free(s_block);
      s_block = nullptr;
      s_releases++;
      DMA_RESERVE_LOG("RELEASED after fragmentation floor (largest was %u) — +%uB contiguous for WiFi/mDNS",
                      (unsigned)largest, (unsigned)kReserveBytes);
      return true;
    }
    return false;
  }
  if (s_bootGraceComplete && largest >= kRearmMinBytes) init();
  return false;
}

}  // namespace dmaReserve
