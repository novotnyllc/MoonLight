#pragma once

#ifdef ARDUINO
  #include <atomic>
  #include <freertos/FreeRTOS.h>
  #include <freertos/event_groups.h>
  #include <freertos/semphr.h>
  #include <freertos/task.h>

// Writer-preferring shared lifetime lock. Uncontended readers use one atomic
// increment/decrement; writers close the event gate, then wait for active readers.
class LayerMappingMutex {
 public:
  LayerMappingMutex()
      : writerMutex(xSemaphoreCreateMutex()),
        readersDrained(xSemaphoreCreateBinary()),
        readerGate(xEventGroupCreate()) {
    if (readerGate) xEventGroupSetBits(readerGate, READERS_ALLOWED);
  }

  ~LayerMappingMutex() {
    if (writerMutex) vSemaphoreDelete(writerMutex);
    if (readersDrained) vSemaphoreDelete(readersDrained);
    if (readerGate) vEventGroupDelete(readerGate);
  }

  LayerMappingMutex(const LayerMappingMutex&) = delete;
  LayerMappingMutex& operator=(const LayerMappingMutex&) = delete;

  void lock() {
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (writerOwner.load(std::memory_order_acquire) == current) {
      ++writerDepth;
      return;
    }
    waitingWriters.fetch_add(1, std::memory_order_acq_rel);
    xSemaphoreTake(writerMutex, portMAX_DELAY);
    while (xSemaphoreTake(readersDrained, 0) == pdTRUE) {}
    if ((state.load(std::memory_order_acquire) & WRITER_ACTIVE) == 0) {
      xEventGroupClearBits(readerGate, READERS_ALLOWED);
      state.fetch_or(WRITER_ACTIVE, std::memory_order_acq_rel);
    }
    if ((state.load(std::memory_order_acquire) & READER_COUNT_MASK) != 0) {
      xSemaphoreTake(readersDrained, portMAX_DELAY);
    }
    writerOwner.store(current, std::memory_order_release);
    writerDepth = 1;
  }

  void unlock() {
    if (writerOwner.load(std::memory_order_acquire) != xTaskGetCurrentTaskHandle() || writerDepth == 0) return;
    if (--writerDepth > 0) return;
    writerOwner.store(nullptr, std::memory_order_release);
    if (waitingWriters.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      state.fetch_and(READER_COUNT_MASK, std::memory_order_release);
      xEventGroupSetBits(readerGate, READERS_ALLOWED);
    }
    xSemaphoreGive(writerMutex);
  }

  bool lockShared() {
    if (writerOwner.load(std::memory_order_acquire) == xTaskGetCurrentTaskHandle()) return false;
    while (true) {
      uint32_t observed = state.load(std::memory_order_acquire);
      if (observed & WRITER_ACTIVE) {
        xEventGroupWaitBits(readerGate, READERS_ALLOWED, pdFALSE, pdTRUE, portMAX_DELAY);
        continue;
      }
      if (state.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel, std::memory_order_acquire)) return true;
    }
  }

  void unlockShared(bool acquired) {
    if (!acquired) return;
    uint32_t previous = state.fetch_sub(1, std::memory_order_acq_rel);
    if ((previous & WRITER_ACTIVE) && (previous & READER_COUNT_MASK) == 1) {
      xSemaphoreGive(readersDrained);
    }
  }

 private:
  static constexpr uint32_t WRITER_ACTIVE = 0x80000000UL;
  static constexpr uint32_t READER_COUNT_MASK = ~WRITER_ACTIVE;
  static constexpr EventBits_t READERS_ALLOWED = BIT0;
  SemaphoreHandle_t writerMutex;
  SemaphoreHandle_t readersDrained;
  EventGroupHandle_t readerGate;
  std::atomic<uint32_t> state{0};
  std::atomic<uint32_t> waitingWriters{0};
  std::atomic<TaskHandle_t> writerOwner{nullptr};
  uint32_t writerDepth = 0;
};
#else
  #include <condition_variable>
  #include <mutex>
  #include <thread>

class LayerMappingMutex {
 public:
  LayerMappingMutex() = default;
  LayerMappingMutex(const LayerMappingMutex&) = delete;
  LayerMappingMutex& operator=(const LayerMappingMutex&) = delete;

  void lock() {
    std::unique_lock<std::mutex> lock(stateMutex);
    std::thread::id current = std::this_thread::get_id();
    if (writerOwner == current) {
      ++writerDepth;
      return;
    }
    ++waitingWriters;
    stateChanged.wait(lock, [&]() { return writerDepth == 0 && readers == 0; });
    --waitingWriters;
    writerOwner = current;
    writerDepth = 1;
  }

  void unlock() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (writerOwner != std::this_thread::get_id() || writerDepth == 0) return;
    if (--writerDepth > 0) return;
    writerOwner = std::thread::id();
    stateChanged.notify_all();
  }

  bool lockShared() {
    std::unique_lock<std::mutex> lock(stateMutex);
    if (writerOwner == std::this_thread::get_id()) return false;
    stateChanged.wait(lock, [&]() { return writerDepth == 0 && waitingWriters == 0; });
    ++readers;
    return true;
  }

  void unlockShared(bool acquired) {
    if (!acquired) return;
    std::lock_guard<std::mutex> lock(stateMutex);
    if (--readers == 0) stateChanged.notify_all();
  }

  uint32_t waitingWriterCountForTest() {
    std::lock_guard<std::mutex> lock(stateMutex);
    return waitingWriters;
  }

 private:
  std::mutex stateMutex;
  std::condition_variable stateChanged;
  uint32_t readers = 0;
  uint32_t waitingWriters = 0;
  std::thread::id writerOwner;
  uint32_t writerDepth = 0;
};
#endif

class LayerMappingGuard {
 public:
  explicit LayerMappingGuard(LayerMappingMutex& mutex) : mutex(mutex) { mutex.lock(); }
  ~LayerMappingGuard() { mutex.unlock(); }
  LayerMappingGuard(const LayerMappingGuard&) = delete;
  LayerMappingGuard& operator=(const LayerMappingGuard&) = delete;

 private:
  LayerMappingMutex& mutex;
};

class LayerMappingReadGuard {
 public:
  explicit LayerMappingReadGuard(LayerMappingMutex& mutex) : mutex(mutex), acquired(mutex.lockShared()) {}
  ~LayerMappingReadGuard() { mutex.unlockShared(acquired); }
  LayerMappingReadGuard(const LayerMappingReadGuard&) = delete;
  LayerMappingReadGuard& operator=(const LayerMappingReadGuard&) = delete;

 private:
  LayerMappingMutex& mutex;
  bool acquired;
};
