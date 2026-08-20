#include "doctest.h"

#include <string>

#include "MoonLight/Modules/ModuleLightsControl.h"

TEST_CASE("preset display labels prefer an explicit trimmed label") {
  char label[32];

  extractPresetDisplayLabel(label, "  Counter-Rotating Spirals  ", "Legacy Node ");

  CHECK(std::string(label) == "Counter-Rotating Spirals");
}

TEST_CASE("preset display labels retain the legacy first-node fallback") {
  char label[32];

  extractPresetDisplayLabel(label, " \t ", "Frequency Wave   \xF0\x9F\x8C\x8A");

  CHECK(std::string(label) == "Frequency Wave");
}

TEST_CASE("preset scan accepts only exact presetNN.json basenames") {
  uint16_t sequence = 0;

  CHECK(parsePresetJsonBasename("preset01.json", sequence));
  CHECK_EQ(sequence, 1);
  CHECK(parsePresetJsonBasename("preset20.json", sequence));
  CHECK_EQ(sequence, 20);
  CHECK_FALSE(parsePresetJsonBasename("preset01.json.tmp", sequence));
  CHECK_FALSE(parsePresetJsonBasename("preset01.json.bak", sequence));
  CHECK_FALSE(parsePresetJsonBasename("preset1.json", sequence));
  CHECK_FALSE(parsePresetJsonBasename("/preset01.json", sequence));
}

TEST_CASE("pending preset eligibility and consume are one queue operation") {
  PresetCommandQueue<4> queue;
  PresetCommand command;
  command.mode = PresetCommandMode::Apply;
  command.select = 1;
  REQUIRE(queue.enqueue(command));
  command.select = 2;
  REQUIRE(queue.enqueue(command));
  CHECK_EQ(queue.size(), 1u);

  PresetCommand taken;
  CHECK_FALSE(queue.takeReadyNonSave(79, 0, 80, taken));
  command.select = 3;
  REQUIRE(queue.enqueue(command));
  CHECK(queue.takeReadyNonSave(80, 0, 80, taken));
  CHECK_EQ(taken.select, 3);
  CHECK_EQ(queue.size(), 0u);
}

TEST_CASE("pending save commands are never coalesced or overwritten") {
  PresetCommandQueue<4> queue;
  PresetCommand command;
  command.mode = PresetCommandMode::Save;
  command.select = 4;
  REQUIRE(queue.enqueue(command));
  command.select = 5;
  REQUIRE(queue.enqueue(command));

  PresetCommand taken;
  CHECK_FALSE(queue.takeReadyNonSave(0, 0, 80, taken));
  REQUIRE(queue.takeFirst(PresetCommandMode::Save, taken));
  CHECK_EQ(taken.select, 4);
  REQUIRE(queue.takeFirst(PresetCommandMode::Save, taken));
  CHECK_EQ(taken.select, 5);
}

TEST_CASE("queued saves do not block apply or delete processing") {
  PresetCommandQueue<4> queue;
  PresetCommand command;
  command.mode = PresetCommandMode::Save;
  command.select = 4;
  REQUIRE(queue.enqueue(command));
  command.mode = PresetCommandMode::Apply;
  command.select = 5;
  REQUIRE(queue.enqueue(command));
  command.mode = PresetCommandMode::Delete;
  command.select = 6;
  REQUIRE(queue.enqueue(command));

  PresetCommand taken;
  REQUIRE(queue.takeReadyNonSave(80, 0, 80, taken));
  CHECK_EQ(taken.mode, PresetCommandMode::Apply);
  CHECK_EQ(taken.select, 5);
  REQUIRE(queue.takeReadyNonSave(80, 80, 80, taken));
  CHECK_EQ(taken.mode, PresetCommandMode::Delete);
  CHECK_EQ(taken.select, 6);
  CHECK(queue.contains(PresetCommandMode::Save));
  REQUIRE(queue.takeFirst(PresetCommandMode::Save, taken));
  CHECK_EQ(taken.select, 4);
}

TEST_CASE("saved slot labels take precedence over builtin labels") {
  CHECK(std::string(presetSlotDisplayLabel("My Playa Look", builtinVestPresetLabel(12))) == "My Playa Look");
  CHECK(std::string(presetSlotDisplayLabel("", builtinVestPresetLabel(12))) == "🎵 Camp Pulse");
}

TEST_CASE("Camp Pulse uses the canonical audio-reactive rings node") {
  CHECK(std::string(builtinVestPresetNodeName(12)) == "Audio Rings");
}
