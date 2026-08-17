#ifndef RecoveryPolicy_h
#define RecoveryPolicy_h

#include <cstdint>
#include <atomic>

constexpr uint32_t RECOVERY_SOUND_FRESHNESS_MS = 5000;

inline bool recoveryConnectivityHealthy(bool stationOrEthernetConnected, bool apActive, bool clientConnected) {
  return stationOrEthernetConnected || (apActive && clientConnected);
}

inline bool recoverySoundSampleFresh(bool required, bool sampleSeen, uint32_t now, uint32_t lastSample) {
  return !required || (sampleSeen && static_cast<uint32_t>(now - lastSample) < RECOVERY_SOUND_FRESHNESS_MS);
}

class RecoverySoundState {
 public:
  void report(bool healthy, uint32_t now) {
    if (healthy) {
      lastSample.store(now, std::memory_order_relaxed);
      sampleSeen.store(true, std::memory_order_release);
    } else {
      sampleSeen.store(false, std::memory_order_release);
    }
  }

  bool fresh(bool required, uint32_t now) const {
    bool seen = sampleSeen.load(std::memory_order_acquire);
    uint32_t sample = lastSample.load(std::memory_order_relaxed);
    return recoverySoundSampleFresh(required, seen, now, sample);
  }

 private:
  std::atomic<uint32_t> lastSample{0};
  std::atomic<bool> sampleSeen{false};
};

inline bool recoveryShouldRestore(bool recoveryAvailable, bool failureReset, bool currentValid, bool fingerprintsMatch) {
  return recoveryAvailable && failureReset && (!currentValid || !fingerprintsMatch);
}

inline bool recoverySlotReady(bool manifestValid, bool configReadable, bool livescriptsReadable) {
  return manifestValid && configReadable && livescriptsReadable;
}

inline uint8_t recoveryEffectiveAffinity(uint8_t requestedAffinity, bool pdmForcesRmt) {
  return pdmForcesRmt ? 1 : requestedAffinity;
}

inline bool recoveryShouldRetryFastLedInitialization(uint32_t channelCount, uint32_t lightCount, uint8_t ledPinCount) {
  return channelCount == 0 && lightCount > 0 && ledPinCount > 0;
}

#endif
