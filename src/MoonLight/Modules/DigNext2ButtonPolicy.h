#pragma once

#include <cstddef>
#include <cstdint>

enum class DigNext2ButtonAction : uint8_t {
  None,
  NextPreset,
  PreviousPreset,
  ToggleSoftBlackout,
  WakeSoftBlackout,
};

struct DigNext2ButtonState {
  bool initialized = false;
  bool pressed = false;
  bool longActionDone = false;
  uint32_t pressedAt = 0;
};

inline constexpr uint32_t DIG_NEXT2_SHORT_PRESS_MS = 500;
inline constexpr uint32_t DIG_NEXT2_BLACKOUT_HOLD_MS = 8000;

/// Returns the next/previous available numeric preset in the configured range.
/// The input order is irrelevant; -1 means the range has no available preset.
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

/// Advances one active button state. Buttons are active-low at the hardware boundary.
inline DigNext2ButtonAction updateDigNext2Button(DigNext2ButtonState& state, bool pressed, uint32_t now, bool nextButton, bool softBlackout) {
  if (!state.initialized) {
    state.initialized = true;
    state.pressed = pressed;
    state.pressedAt = now;
    return DigNext2ButtonAction::None;
  }

  if (pressed) {
    if (!state.pressed) {
      state.pressed = true;
      state.pressedAt = now;
      state.longActionDone = false;
    } else if (!nextButton && !state.longActionDone && now - state.pressedAt >= DIG_NEXT2_BLACKOUT_HOLD_MS) {
      state.longActionDone = true;
      return DigNext2ButtonAction::ToggleSoftBlackout;
    }
    return DigNext2ButtonAction::None;
  }

  if (!state.pressed) return DigNext2ButtonAction::None;

  uint32_t duration = now - state.pressedAt;
  state.pressed = false;
  if (!nextButton && !state.longActionDone && duration >= DIG_NEXT2_BLACKOUT_HOLD_MS) {
    state.longActionDone = false;
    return DigNext2ButtonAction::ToggleSoftBlackout;
  }

  bool longActionDone = state.longActionDone;
  state.longActionDone = false;
  if (longActionDone) return DigNext2ButtonAction::None;
  if (duration <= DIG_NEXT2_SHORT_PRESS_MS) {
    return softBlackout ? DigNext2ButtonAction::WakeSoftBlackout
                        : (nextButton ? DigNext2ButtonAction::NextPreset : DigNext2ButtonAction::PreviousPreset);
  }
  return DigNext2ButtonAction::None;
}
