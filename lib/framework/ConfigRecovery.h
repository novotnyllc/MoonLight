#ifndef ConfigRecovery_h
#define ConfigRecovery_h

#include <Arduino.h>
#include <FS.h>
#include <esp_system.h>

class ConfigRecovery {
 public:
  static void begin(FS* fs, esp_reset_reason_t resetReason);
  static void loop(bool healthy);
  static void clear();
  static void requestRestore();
  static void requireSound(bool required);
  static void reportSoundHealthy(bool healthy);

  static bool available();
  static bool pending();
  static uint32_t secondsUntilConfirmation();
  static const char* lastAction();
  static bool soundHealthy();
  static bool restoredThisBoot();
};

#endif
