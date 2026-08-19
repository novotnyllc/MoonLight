#pragma once

#include <cstddef>
#include <cstdint>

enum class DigNext2ButtonAction : uint8_t {
  None,
  NextPreset,
  PreviousPreset,
  BrightnessStepUp,
  BrightnessStepDown,
  TogglePower,
};

struct DigNext2ButtonInputs {
  bool button1 = false;
  bool button2 = false;
  bool lightsOn = true;
  uint32_t nowMs = 0;
};

struct DigNext2ButtonRuntime {
  bool b1Down = false;
  bool b2Down = false;
  uint32_t b1DownAt = 0;
  uint32_t b2DownAt = 0;
  uint32_t bothDownAt = 0;
  bool bothPowerDone = false;
  bool suppressShortUntilRelease = false;
  uint32_t lastRampAt = 0;
};

inline constexpr uint32_t DIG_NEXT2_SHORT_PRESS_MS = 400;
inline constexpr uint32_t DIG_NEXT2_RAMP_START_MS = 450;
inline constexpr uint32_t DIG_NEXT2_RAMP_INTERVAL_MS = 180;
inline constexpr uint32_t DIG_NEXT2_BOTH_POWER_MS = 2000;
inline constexpr uint32_t DIG_NEXT2_BOOT_HOLD_MS = 5000;

/// Returns the next/previous available numeric preset in the configured range.
template <typename GetValue>
int chooseDigNext2Preset(size_t count, GetValue&& getValue, int selected, int firstPreset, int lastPreset, bool backwards) {
  if (count == 0 || firstPreset > lastPreset) return -1;

  int boundary = -1;
  bool selectedAvailable = false;
  for (size_t i = 0; i < count; i++) {
    int value = getValue(i);
    if (value < firstPreset || value > lastPreset) continue;
    if (boundary < 0 || (backwards ? value > boundary : value < boundary)) boundary = value;
    if (value == selected) selectedAvailable = true;
  }
  if (boundary < 0 || !selectedAvailable) return boundary;

  int candidate = -1;
  for (size_t i = 0; i < count; i++) {
    int value = getValue(i);
    if (value < firstPreset || value > lastPreset) continue;
    if ((!backwards && value > selected) || (backwards && value < selected)) {
      if (candidate < 0 || (backwards ? value > candidate : value < candidate)) candidate = value;
    }
  }
  return candidate < 0 ? boundary : candidate;
}

/// Combined Dig-Next-2 button policy: short taps change presets (on only), long single holds ramp brightness, both-hold toggles relay power.
inline DigNext2ButtonAction updateDigNext2ButtonsPolicy(DigNext2ButtonRuntime& rt, const DigNext2ButtonInputs& in) {
  DigNext2ButtonAction action = DigNext2ButtonAction::None;
  const bool both = in.button1 && in.button2;
  const bool bothWas = rt.b1Down && rt.b2Down;

  if (both) {
    if (!bothWas) {
      rt.bothDownAt = in.nowMs;
      rt.bothPowerDone = false;
    } else if (!rt.bothPowerDone && in.nowMs - rt.bothDownAt >= DIG_NEXT2_BOTH_POWER_MS) {
      rt.bothPowerDone = true;
      rt.suppressShortUntilRelease = true;
      action = DigNext2ButtonAction::TogglePower;
    }
    rt.b1Down = true;
    rt.b2Down = true;
    return action;
  }

  if (bothWas) {
    rt.bothDownAt = 0;
    rt.bothPowerDone = false;
    rt.suppressShortUntilRelease = false;
  }

  if (!rt.suppressShortUntilRelease) {
    if (rt.b1Down && !in.button1) {
      const uint32_t duration = in.nowMs - rt.b1DownAt;
      if (!rt.b2Down && in.lightsOn && duration <= DIG_NEXT2_SHORT_PRESS_MS) {
        action = DigNext2ButtonAction::NextPreset;
      }
    }
    if (rt.b2Down && !in.button2 && action == DigNext2ButtonAction::None) {
      const uint32_t duration = in.nowMs - rt.b2DownAt;
      if (!rt.b1Down && in.lightsOn && duration <= DIG_NEXT2_SHORT_PRESS_MS) {
        action = DigNext2ButtonAction::PreviousPreset;
      }
    }
  }

  if (in.button1 && !in.button2 && in.lightsOn) {
    if (!rt.b1Down) rt.b1DownAt = in.nowMs;
    if (in.nowMs - rt.b1DownAt >= DIG_NEXT2_RAMP_START_MS && in.nowMs - rt.lastRampAt >= DIG_NEXT2_RAMP_INTERVAL_MS) {
      rt.lastRampAt = in.nowMs;
      if (action == DigNext2ButtonAction::None) action = DigNext2ButtonAction::BrightnessStepUp;
    }
  }

  if (in.button2 && !in.button1 && in.lightsOn) {
    if (!rt.b2Down) rt.b2DownAt = in.nowMs;
    if (in.nowMs - rt.b2DownAt >= DIG_NEXT2_RAMP_START_MS && in.nowMs - rt.lastRampAt >= DIG_NEXT2_RAMP_INTERVAL_MS) {
      rt.lastRampAt = in.nowMs;
      if (action == DigNext2ButtonAction::None) action = DigNext2ButtonAction::BrightnessStepDown;
    }
  }

  rt.b1Down = in.button1;
  rt.b2Down = in.button2;
  if (!in.button1) rt.b1DownAt = 0;
  if (!in.button2) rt.b2DownAt = 0;
  return action;
}
