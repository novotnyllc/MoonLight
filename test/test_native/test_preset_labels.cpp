#include "doctest.h"

#include <string>

#include "MoonLight/Modules/ModuleLightsControl.h"

TEST_CASE("preset display labels prefer an explicit trimmed label") {
  char label[20];

  extractPresetDisplayLabel(label, "  Counter-Rotating Spirals  ", "Legacy Node ");

  CHECK(std::string(label) == "Counter-Rotating Sp");
}

TEST_CASE("preset display labels retain the legacy first-node fallback") {
  char label[20];

  extractPresetDisplayLabel(label, " \t ", "Frequency Wave   \xF0\x9F\x8C\x8A");

  CHECK(std::string(label) == "Frequency Wave");
}
