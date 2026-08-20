/**
    @title     MoonBase Unit Tests — Module
    @file      test_module.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for pure ArduinoJson logic used by the Module system.
    Functions are copied here to avoid ESP32 header dependencies.
    Run with: pio test -e native
**/

#include "doctest.h"

#include <ArduinoJson.h>

#include <functional>

#include "MoonBase/utilities/JsonRowRemoval.h"

// ============================================================
// Copied from Module.cpp — keep in sync with the original
// ============================================================

/// Populates a controls object with default values from a definition array.
/// Non-"rows" controls get their "default" value; "rows" controls are skipped.
void setDefaults(JsonObject controls, JsonArray definition) {
  for (JsonObject control : definition) {
    if (control["type"] != "rows") {
      controls[control["name"]] = control["default"];
    } else {
      // rows are not given defaults (they start empty)
    }
  }
}

// ============================================================
// Tests
// ============================================================

TEST_CASE("setDefaults: text control gets default value") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "brightness";
  ctrl["type"] = "number";
  ctrl["default"] = 128;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["brightness"] == 128);
}

TEST_CASE("setDefaults: multiple controls") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonObject c1 = definition.add<JsonObject>();
  c1["name"] = "name";
  c1["type"] = "text";
  c1["default"] = "MoonLight";

  JsonObject c2 = definition.add<JsonObject>();
  c2["name"] = "speed";
  c2["type"] = "number";
  c2["default"] = 50;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["name"] == "MoonLight");
  CHECK(controls["speed"] == 50);
}

TEST_CASE("setDefaults: rows type is skipped") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonObject c1 = definition.add<JsonObject>();
  c1["name"] = "items";
  c1["type"] = "rows";
  c1["default"] = "should_not_appear";

  JsonObject c2 = definition.add<JsonObject>();
  c2["name"] = "color";
  c2["type"] = "color";
  c2["default"] = "#FF0000";

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["items"].isNull());
  CHECK(controls["color"] == "#FF0000");
}

TEST_CASE("setDefaults: control with no default sets null") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "optional";
  ctrl["type"] = "text";
  // no "default" key

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["optional"].isNull());
}

TEST_CASE("setDefaults: empty definition produces empty controls") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls.size() == 0);
}

TEST_CASE("setDefaults: boolean default") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "enabled";
  ctrl["type"] = "checkbox";
  ctrl["default"] = true;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["enabled"] == true);
}

TEST_CASE("setDefaults: overwrites existing value in controls") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "volume";
  ctrl["type"] = "number";
  ctrl["default"] = 75;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  controls["volume"] = 42;  // pre-existing value
  setDefaults(controls, definition);

  CHECK(controls["volume"] == 75);  // overwritten by default
}

TEST_CASE("setDefaults: string default value") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "hostname";
  ctrl["type"] = "text";
  ctrl["default"] = "moon-device";

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["hostname"] == "moon-device");
}

TEST_CASE("setDefaults: float default value") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "gain";
  ctrl["type"] = "number";
  ctrl["default"] = 1.5f;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["gain"].as<float>() == doctest::Approx(1.5f));
}

TEST_CASE("setDefaults: mixed types with rows skipped") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonObject c1 = definition.add<JsonObject>();
  c1["name"] = "speed";
  c1["type"] = "number";
  c1["default"] = 200;

  JsonObject c2 = definition.add<JsonObject>();
  c2["name"] = "label";
  c2["type"] = "text";
  c2["default"] = "hello";

  JsonObject c3 = definition.add<JsonObject>();
  c3["name"] = "nodes";
  c3["type"] = "rows";
  c3["default"] = "ignored";

  JsonObject c4 = definition.add<JsonObject>();
  c4["name"] = "active";
  c4["type"] = "checkbox";
  c4["default"] = false;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["speed"] == 200);
  CHECK(controls["label"] == "hello");
  CHECK(controls["nodes"].isNull());
  CHECK(controls["active"] == false);
  CHECK(controls.size() == 3);  // rows not included
}

TEST_CASE("settled IO state is published once after persistence loads") {
  struct FakeInputOutput {
    int persistedLedPins = 0;
    int publishedLedPins = 0;
    int publishCount = 0;
    std::function<void()> subscriber;

    bool updateWithoutPropagation(int ledPins) {
      if (persistedLedPins == ledPins) return false;
      persistedLedPins = ledPins;
      return true;
    }

    void callUpdateHandlers() {
      ++publishCount;
      subscriber();
    }
  } inputOutput;

  inputOutput.subscriber = [&] { inputOutput.publishedLedPins = inputOutput.persistedLedPins; };

  CHECK(inputOutput.updateWithoutPropagation(2));
  CHECK_EQ(inputOutput.publishCount, 0);
  inputOutput.callUpdateHandlers();  // SharedFSPersistence post-load publish
  CHECK_EQ(inputOutput.publishCount, 1);
  CHECK_EQ(inputOutput.publishedLedPins, 2);

  CHECK_FALSE(inputOutput.updateWithoutPropagation(2));  // unchanged board-default pass
  CHECK_EQ(inputOutput.publishCount, 1);
  CHECK_EQ(inputOutput.publishedLedPins, 2);
}

TEST_CASE("row shrink removes every surplus row and reports every member") {
  JsonDocument doc;
  JsonArray rows = doc["nodes"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    JsonObject row = rows.add<JsonObject>();
    row["name"] = i;
    row["on"] = true;
  }

  int removedNames = 0;
  int removedMembers = 0;
  std::string lastRemoved;
  while (rows.size() > 1) {
    REQUIRE(removeJsonObjectRow(rows, 1, [&](const char* key, const char*) {
      removedMembers++;
      lastRemoved = key;
      if (strcmp(key, "name") == 0) removedNames++;
    }));
    CHECK_EQ(lastRemoved, "name");
  }

  CHECK_EQ(rows.size(), 1u);
  CHECK_EQ(rows[0]["name"].as<int>(), 0);
  CHECK_EQ(removedNames, 3);
  CHECK_EQ(removedMembers, 6);
}

TEST_CASE("row shrink preserves runtime-valid rows") {
  JsonDocument doc;
  JsonArray rows = doc.to<JsonArray>();
  JsonObject row = rows.add<JsonObject>();
  row["name"] = "runtime control";
  row["valid"] = true;

  int callbacks = 0;
  CHECK_FALSE(removeJsonObjectRow(rows, 0, [&](const char*, const char*) { callbacks++; }));
  CHECK_EQ(rows.size(), 1u);
  CHECK_EQ(callbacks, 0);
}

TEST_CASE("staged module state reuses one bounded spare slot") {
  JsonDocument modulesDoc;
  JsonArray moduleStates = modulesDoc.to<JsonArray>();
  JsonObject activeState = moduleStates.add<JsonObject>();
  JsonObject spareState = moduleStates.add<JsonObject>();

  for (int selected = 1; selected <= 20; selected++) {
    JsonObject previousState = activeState;
    JsonObject stagedState = spareState;
    stagedState.clear();
    stagedState["selected"] = selected;

    activeState = stagedState;
    previousState.clear();
    spareState = previousState;

    REQUIRE_EQ(moduleStates.size(), 2u);
    CHECK_EQ(activeState["selected"].as<int>(), selected);
    CHECK_EQ(spareState.size(), 0u);
  }
}
