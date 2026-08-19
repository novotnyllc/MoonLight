/**
 * Native checks for the Dig-Next-2 button policy and preset ordering.
 */

#include "doctest.h"

#include <array>

#include "MoonLight/Modules/DigNext2ButtonPolicy.h"

TEST_CASE("Dig-Next-2 short preset taps while lights on") {
  DigNext2ButtonRuntime rt;
  DigNext2ButtonInputs in;
  in.lightsOn = true;

  in.nowMs = 0;
  in.button1 = true;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::None);
  in.nowMs = 300;
  in.button1 = false;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::NextPreset);

  DigNext2ButtonRuntime rt2;
  in = {};
  in.lightsOn = true;
  in.button2 = true;
  CHECK(updateDigNext2ButtonsPolicy(rt2, in) == DigNext2ButtonAction::None);
  in.nowMs = 300;
  in.button2 = false;
  CHECK(updateDigNext2ButtonsPolicy(rt2, in) == DigNext2ButtonAction::PreviousPreset);
}

TEST_CASE("Dig-Next-2 short taps ignored while lights off") {
  DigNext2ButtonRuntime rt;
  DigNext2ButtonInputs in;
  in.lightsOn = false;
  in.button1 = true;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::None);
  in.nowMs = 300;
  in.button1 = false;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::None);
}

TEST_CASE("Dig-Next-2 both-hold toggles power") {
  DigNext2ButtonRuntime rt;
  DigNext2ButtonInputs in;
  in.lightsOn = true;
  in.button1 = true;
  in.button2 = true;
  in.nowMs = 0;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::None);
  in.nowMs = DIG_NEXT2_BOTH_POWER_MS;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::TogglePower);
  in.nowMs = DIG_NEXT2_BOTH_POWER_MS + 100;
  in.button1 = false;
  in.button2 = false;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::None);
}

TEST_CASE("Dig-Next-2 long single hold ramps brightness") {
  DigNext2ButtonRuntime rt;
  DigNext2ButtonInputs in;
  in.lightsOn = true;
  in.button1 = true;
  in.nowMs = 0;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::None);
  in.nowMs = DIG_NEXT2_RAMP_START_MS + DIG_NEXT2_RAMP_INTERVAL_MS;
  CHECK(updateDigNext2ButtonsPolicy(rt, in) == DigNext2ButtonAction::BrightnessStepUp);
}

TEST_CASE("Dig-Next-2 preset order ignores input order and missing slots") {
  const std::array<int, 5> presets = {9, 2, 7, 64, 1};
  auto valueAt = [&](size_t index) { return presets[index]; };

  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 2, 2, 9, false) == 7);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 7, 2, 9, false) == 9);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 9, 2, 9, false) == 2);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 7, 2, 9, true) == 2);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 2, 2, 9, true) == 9);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 255, 2, 9, false) == 2);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 255, 2, 9, true) == 9);

  CHECK(chooseDigNext2Preset(0, valueAt, 2, 2, 9, false) == -1);
  CHECK(chooseDigNext2Preset(presets.size(), valueAt, 2, 10, 20, false) == -1);
}
