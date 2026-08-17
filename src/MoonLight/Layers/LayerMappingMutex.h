#pragma once

#ifdef ARDUINO
  #include <freertos/FreeRTOS.h>
  #include <freertos/semphr.h>

class LayerMappingMutex {
 public:
  LayerMappingMutex() : handle(xSemaphoreCreateRecursiveMutex()) {}
  ~LayerMappingMutex() { if (handle) vSemaphoreDelete(handle); }
  LayerMappingMutex(const LayerMappingMutex&) = delete;
  LayerMappingMutex& operator=(const LayerMappingMutex&) = delete;
  void lock() { if (handle) xSemaphoreTakeRecursive(handle, portMAX_DELAY); }
  void unlock() { if (handle) xSemaphoreGiveRecursive(handle); }

 private:
  SemaphoreHandle_t handle;
};
#else
  #include <mutex>
using LayerMappingMutex = std::recursive_mutex;
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
