/**
    Wearable field boot policy: survive panics without safe-mode cripple; break boot loops with one auto-golden.
**/
#pragma once

#include <stdint.h>

#ifdef ARDUINO
  #include <esp_system.h>
#endif

inline constexpr uint32_t WEARABLE_PANIC_BOOT_THRESHOLD = 3;
inline constexpr uint32_t WEARABLE_STABLE_UPTIME_MS = 180000;  // 3 minutes

#ifdef ARDUINO
  #include <esp_attr.h>

RTC_DATA_ATTR static uint32_t g_wearablePanicBootCount = 0;
RTC_DATA_ATTR static uint32_t g_wearableAutoGoldenUsed = 0;

inline bool wearableIsUnstableBootReset() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      return true;
    default:
      return false;
  }
}

inline void wearableRecordBootResetReason() {
  if (wearableIsUnstableBootReset()) {
    g_wearablePanicBootCount++;
    return;
  }
  if (esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_SW || esp_reset_reason() == ESP_RST_USB) {
    g_wearablePanicBootCount = 0;
    g_wearableAutoGoldenUsed = 0;
  }
}

inline bool wearableShouldOfferAutoGoldenRestore() {
  return g_wearablePanicBootCount >= WEARABLE_PANIC_BOOT_THRESHOLD && g_wearableAutoGoldenUsed == 0;
}

inline void wearableMarkAutoGoldenRestoreUsed() {
  g_wearableAutoGoldenUsed = 1;
  g_wearablePanicBootCount = 0;
}

inline void wearableClearBootRecoveryCounters() {
  g_wearablePanicBootCount = 0;
  g_wearableAutoGoldenUsed = 0;
}

inline void wearableMaybeMarkStable(uint32_t uptimeMs) {
  if (uptimeMs >= WEARABLE_STABLE_UPTIME_MS) {
    wearableClearBootRecoveryCounters();
  }
}
#endif
