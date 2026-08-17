/**
 * Native checks for the Dig-Next-2 button policy and preset ordering.
 */

#include "doctest.h"

#include <array>

#include "MoonLight/Modules/DigNext2ButtonPolicy.h"

TEST_CASE("Dig-Next-2 button hold policy") {
  DigNext2ButtonState button1;
  CHECK(updateDigNext2Button(button1, false, 0, true, false) == DigNext2ButtonAction::None);
  CHECK(updateDigNext2Button(button1, true, 100, true, false) == DigNext2ButtonAction::None);
  CHECK(updateDigNext2Button(button1, false, 600, true, false) == DigNext2ButtonAction::NextPreset);

  DigNext2ButtonState button1Medium;
  updateDigNext2Button(button1Medium, false, 0, true, false);
  updateDigNext2Button(button1Medium, true, 100, true, false);
  CHECK(updateDigNext2Button(button1Medium, false, 601, true, false) == DigNext2ButtonAction::None);

  DigNext2ButtonState button1Long;
  updateDigNext2Button(button1Long, false, 0, true, false);
  updateDigNext2Button(button1Long, true, 100, true, false);
  CHECK(updateDigNext2Button(button1Long, true, 3100, true, false) == DigNext2ButtonAction::None);
  CHECK(updateDigNext2Button(button1Long, false, 3101, true, false) == DigNext2ButtonAction::None);

  DigNext2ButtonState button2;
  updateDigNext2Button(button2, false, 0, false, false);
  updateDigNext2Button(button2, true, 100, false, false);
  CHECK(updateDigNext2Button(button2, false, 600, false, false) == DigNext2ButtonAction::PreviousPreset);

  DigNext2ButtonState button2Medium;
  updateDigNext2Button(button2Medium, false, 0, false, false);
  updateDigNext2Button(button2Medium, true, 100, false, false);
  CHECK(updateDigNext2Button(button2Medium, false, 7999, false, false) == DigNext2ButtonAction::None);

  DigNext2ButtonState button2Long;
  updateDigNext2Button(button2Long, false, 0, false, false);
  updateDigNext2Button(button2Long, true, 100, false, false);
  CHECK(updateDigNext2Button(button2Long, true, 8099, false, false) == DigNext2ButtonAction::None);
  CHECK(updateDigNext2Button(button2Long, true, 8100, false, false) == DigNext2ButtonAction::ToggleSoftBlackout);
  CHECK(updateDigNext2Button(button2Long, true, 9000, false, true) == DigNext2ButtonAction::None);
  CHECK(updateDigNext2Button(button2Long, false, 9001, false, true) == DigNext2ButtonAction::None);

  DigNext2ButtonState button2ReleaseAtThreshold;
  updateDigNext2Button(button2ReleaseAtThreshold, false, 0, false, false);
  updateDigNext2Button(button2ReleaseAtThreshold, true, 100, false, false);
  CHECK(updateDigNext2Button(button2ReleaseAtThreshold, false, 8100, false, false) == DigNext2ButtonAction::ToggleSoftBlackout);
}

TEST_CASE("Dig-Next-2 blackout wake consumes short releases") {
  DigNext2ButtonState button1;
  updateDigNext2Button(button1, false, 0, true, true);
  updateDigNext2Button(button1, true, 100, true, true);
  CHECK(updateDigNext2Button(button1, false, 500, true, true) == DigNext2ButtonAction::WakeSoftBlackout);

  DigNext2ButtonState button2;
  updateDigNext2Button(button2, false, 0, false, true);
  updateDigNext2Button(button2, true, 100, false, true);
  CHECK(updateDigNext2Button(button2, false, 500, false, true) == DigNext2ButtonAction::WakeSoftBlackout);
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
