#ifndef RecoveryPolicy_h
#define RecoveryPolicy_h

#include <cstdint>

inline uint8_t recoveryEffectiveAffinity(uint8_t requestedAffinity, bool pdmForcesRmt) {
  return pdmForcesRmt ? 1 : requestedAffinity;
}

inline bool recoveryShouldRetryFastLedInitialization(uint32_t channelCount, uint32_t lightCount, uint8_t ledPinCount) {
  return channelCount == 0 && lightCount > 0 && ledPinCount > 0;
}

#endif
