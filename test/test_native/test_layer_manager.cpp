/**
    @title     MoonLight Layer Manager Unit Tests
    @file      test_layer_manager.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for LayerManager.
    Tests call the real LayerManager class methods (selectLayer, beginPresetReplacement,
    onNodeRemoved) via minimal stubs for the ESP32/FreeRTOS dependencies.

    Run with: pio test -e native
**/

#include "doctest.h"

#define FT_MOONLIGHT 1  // activate LayerManager code

// ---------------------------------------------------------------------------
// Minimal stubs — native replacements for ESP32/FreeRTOS types
// ---------------------------------------------------------------------------

#include <ArduinoJson.h>
#include <array>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include <cstring>

#include "MoonLight/Layers/LayerMappingMutex.h"

// Logging macros — no-ops in native tests
#ifndef EXT_LOGD
  #define EXT_LOGD(tag, ...)
  #define EXT_LOGW(tag, ...)
  #define EXT_LOGE(tag, ...)
  #define ML_TAG "ML"
#endif

// Coord3D — pure C++ header, available natively
#include "MoonBase/utilities/Coord3D.h"

// VectorRAMAllocator stub — standard allocator on host
template <typename T>
using VectorRAMAllocator = std::allocator<T>;

// Node stub
static int destroyedOwnedNodes = 0;
struct VirtualLayer;
struct Node {
  explicit Node(bool owned = false) : owned(owned) {}
  ~Node() { if (owned) destroyedOwnedNodes++; }
  bool on = true;
  bool owned = false;
  VirtualLayer* layer = nullptr;
  void requestMappings() {}
};

// UpdatedItem stub
struct UpdatedItem {
  struct Parent {
    std::string values[2];
    const std::string& operator[](int index) const { return values[index]; }
  } parent;
  std::string name;
  JsonVariantConst value;
};

// VirtualLayer stub — only the fields LayerManager actually touches
struct VirtualLayer {
  Coord3D startPct{0, 0, 0};
  Coord3D endPct{100, 100, 100};
  uint8_t brightness = 255;
  std::vector<Node*, VectorRAMAllocator<Node*>> nodes;
  void* layerP = nullptr;
  void setup() {}
  ~VirtualLayer() {
    for (Node* node : nodes) {
      if (node && node->owned) delete node;
    }
  }
};

// PhysicalLayer stub
struct PhysicalLayer {
  std::array<VirtualLayer*, 16> layers{};
  std::vector<Node*> nodes;
  int activeLayerCount = 0;
  bool requestMapVirtual = false;
  LayerMappingMutex mappingMutex;

  PhysicalLayer() {
    layers.fill(nullptr);
  }

  VirtualLayer* ensureLayer(uint8_t i) {
    if (i >= layers.size()) return nullptr;
    if (!layers[i]) {
      layers[i] = new VirtualLayer();
      activeLayerCount++;
    }
    return layers[i];
  }

  void destroyLayer(VirtualLayer*& layer) {
    delete layer;
    layer = nullptr;
  }

  void rebindDriverNodes(VirtualLayer* layer) {
    for (Node* node : nodes) {
      if (node) node->layer = layer;
    }
  }

  void reset() {
    for (auto& l : layers) destroyLayer(l);
    nodes.clear();
    activeLayerCount = 0;
    ensureLayer(0);
    requestMapVirtual = false;
  }
} layerP;  // global singleton (matches PhysicalLayer.cpp)

// NodeManager stub (only addNode needed for restoreNonSelectedLayers)
struct NodeManager {
  Node* addNode(int /*index*/, const char* /*name*/, JsonArray /*controls*/) { return nullptr; }
};

// ModuleState stub — just wraps a JsonDocument
struct ModuleState {
  StaticJsonDocument<4096> _doc;
  JsonObject data;
  std::function<void(JsonObject)> readHook;
  ModuleState() : data(_doc.to<JsonObject>()) {}
};

// ---------------------------------------------------------------------------
// Now include the real LayerManager (FT_MOONLIGHT=1, ARDUINO not defined →
// #ifdef ARDUINO blocks are skipped, stubs above supply the missing types)
// ---------------------------------------------------------------------------
#include "MoonLight/Layers/LayerManager.h"

TEST_CASE("beginPresetReplacement waits for an active mapping reader") {
  layerP.reset();
  VirtualLayer* layer1 = layerP.ensureLayer(1);

  ModuleState state;
  std::vector<Node*, VectorRAMAllocator<Node*>>* selectedNodes = &layerP.layers[0]->nodes;
  bool requestUIUpdate = false;
  LayerManager manager;
  manager.init(state, selectedNodes, requestUIUpdate);

  std::promise<void> readerLocked;
  std::promise<void> releaseReader;
  std::shared_future<void> releaseFuture(releaseReader.get_future());
  std::thread reader([&]() {
    LayerMappingReadGuard guard(layerP.mappingMutex);
    readerLocked.set_value();
    releaseFuture.wait();
  });
  readerLocked.get_future().wait();

  std::promise<void> reconfigured;
  std::future<void> reconfiguredFuture = reconfigured.get_future();
  LayerManager::PresetReplacementBackup backup;
  std::thread writer([&]() {
    manager.beginPresetReplacement(false, backup);
    reconfigured.set_value();
  });

  CHECK(reconfiguredFuture.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  CHECK(layerP.layers[1] == layer1);

  releaseReader.set_value();
  reader.join();
  writer.join();
  CHECK(layerP.layers[1] == nullptr);
  manager.rollbackPresetReplacement(backup);
  CHECK(layerP.layers[1] == layer1);
}

TEST_CASE("shared lifetime readers overlap while writer waits and excludes new readers") {
  layerP.reset();
  std::atomic<int> activeReaders{0};
  std::atomic<bool> deleted{false};
  std::promise<void> releaseReaders;
  std::shared_future<void> releaseReadersFuture(releaseReaders.get_future());

  auto readerBody = [&]() {
    LayerMappingReadGuard guard(layerP.mappingMutex);
    CHECK_FALSE(deleted.load());
    activeReaders.fetch_add(1);
    releaseReadersFuture.wait();
    activeReaders.fetch_sub(1);
  };
  auto reader1 = std::async(std::launch::async, readerBody);
  auto reader2 = std::async(std::launch::async, readerBody);
  while (activeReaders.load() != 2) std::this_thread::yield();
  CHECK_EQ(activeReaders.load(), 2);

  std::promise<void> writerEntered;
  std::future<void> writerEnteredFuture = writerEntered.get_future();
  std::promise<void> releaseWriter;
  std::shared_future<void> releaseWriterFuture(releaseWriter.get_future());
  auto writer = std::async(std::launch::async, [&]() {
    LayerMappingGuard guard(layerP.mappingMutex);
    deleted.store(true);
    writerEntered.set_value();
    releaseWriterFuture.wait();
  });
  while (layerP.mappingMutex.waitingWriterCountForTest() == 0) std::this_thread::yield();

  std::promise<void> lateReaderEntered;
  auto lateReader = std::async(std::launch::async, [&]() {
    LayerMappingReadGuard guard(layerP.mappingMutex);
    lateReaderEntered.set_value();
    CHECK(deleted.load());
  });
  auto lateReaderFuture = lateReaderEntered.get_future();
  CHECK(lateReaderFuture.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  CHECK(writerEnteredFuture.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

  releaseReaders.set_value();
  CHECK(writerEnteredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(lateReaderFuture.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  releaseWriter.set_value();

  CHECK(lateReaderFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  reader1.get();
  reader2.get();
  writer.get();
  lateReader.get();
}

TEST_CASE("effect frame keeps exported buffer alive through async script and composite") {
  layerP.reset();
  int* exportedBuffer = new int(0);
  std::atomic<int> compositeValue{0};
  std::atomic<bool> deleted{false};
  std::promise<void> frameEntered;
  std::promise<void> runScript;
  std::shared_future<void> runScriptFuture(runScript.get_future());

  auto script = std::async(std::launch::async, [&]() {
    runScriptFuture.wait();
    *exportedBuffer = 42;
  });
  auto frame = std::async(std::launch::async, [&]() {
    LayerMappingReadGuard frameGuard(layerP.mappingMutex);
    frameEntered.set_value();
    script.get();
    compositeValue.store(*exportedBuffer);
  });
  frameEntered.get_future().wait();

  auto writer = std::async(std::launch::async, [&]() {
    LayerMappingGuard guard(layerP.mappingMutex);
    delete exportedBuffer;
    exportedBuffer = nullptr;
    deleted.store(true);
  });
  while (layerP.mappingMutex.waitingWriterCountForTest() == 0) std::this_thread::yield();
  CHECK(writer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

  runScript.set_value();
  CHECK(frame.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK_EQ(compositeValue.load(), 42);
  CHECK(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(deleted.load());
}

TEST_CASE("monitor reader keeps mapped buffer alive until its read completes") {
  layerP.reset();
  int* monitorBuffer = new int(95);
  std::promise<void> monitorEntered;
  std::promise<void> releaseSnapshot;
  std::shared_future<void> releaseSnapshotFuture(releaseSnapshot.get_future());
  std::promise<void> snapshotComplete;
  std::future<void> snapshotCompleteFuture = snapshotComplete.get_future();
  std::promise<void> releaseNetwork;
  std::shared_future<void> releaseNetworkFuture(releaseNetwork.get_future());

  auto monitor = std::async(std::launch::async, [&]() {
    int snapshot = 0;
    {
      LayerMappingReadGuard guard(layerP.mappingMutex);
      monitorEntered.set_value();
      releaseSnapshotFuture.wait();
      snapshot = *monitorBuffer;
    }
    snapshotComplete.set_value();
    releaseNetworkFuture.wait();  // network emission is outside lifetime ownership
    CHECK_EQ(snapshot, 95);
  });
  monitorEntered.get_future().wait();

  auto remap = std::async(std::launch::async, [&]() {
    LayerMappingGuard guard(layerP.mappingMutex);
    delete monitorBuffer;
    monitorBuffer = nullptr;
  });
  CHECK(remap.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  releaseSnapshot.set_value();
  CHECK(snapshotCompleteFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(remap.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(monitor.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
  releaseNetwork.set_value();
  CHECK(monitor.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
}

TEST_CASE("stuck script is quiesced before frame reader releases to writer") {
  layerP.reset();
  std::atomic<bool> scriptRunning{true};
  std::atomic<bool> scriptSchedulable{true};
  std::atomic<bool> writerObservedQuiesced{false};
  std::promise<void> frameEntered;
  std::promise<void> triggerTimeout;
  std::shared_future<void> timeoutFuture(triggerTimeout.get_future());

  auto frame = std::async(std::launch::async, [&]() {
    LayerMappingReadGuard frameGuard(layerP.mappingMutex);
    frameEntered.set_value();
    timeoutFuture.wait();
    scriptSchedulable.store(false);
    scriptRunning.store(false);  // synchronous runtime kill has returned
  });
  frameEntered.get_future().wait();

  auto writer = std::async(std::launch::async, [&]() {
    LayerMappingGuard guard(layerP.mappingMutex);
    writerObservedQuiesced.store(!scriptRunning.load() && !scriptSchedulable.load());
  });
  while (layerP.mappingMutex.waitingWriterCountForTest() == 0) std::this_thread::yield();
  CHECK(writer.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);

  triggerTimeout.set_value();
  CHECK(frame.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(writer.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(writerObservedQuiesced.load());

  int futureSchedules = 0;
  if (scriptSchedulable.load()) ++futureSchedules;
  CHECK_EQ(futureSchedules, 0);
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/// Add a named node object to a JsonArray (mimics what ModuleEffects stores).
static void addNode(JsonArray nodes, const char* name) {
  JsonObject n = nodes.add<JsonObject>();
  n["name"] = name;
  n["on"]   = true;
}

/// Returns true when data["nodes"] contains a node with the given name.
static bool nodesHas(JsonObject data, const char* name) {
  JsonArray nodes = data["nodes"].as<JsonArray>();
  if (nodes.isNull()) return false;
  for (JsonObject n : nodes) {
    if (strcmp(n["name"].as<const char*>(), name) == 0) return true;
  }
  return false;
}

/// Fixture: fresh LayerManager wired to a ModuleState and the layerP stub.
struct Fixture {
  ModuleState state;
  std::vector<Node*, VectorRAMAllocator<Node*>>* nodesVec = nullptr;
  bool uiUpdate = false;
  LayerManager lm;

  Fixture() {
    layerP.reset();
    nodesVec = &(layerP.layers[0]->nodes);
    lm.init(state, nodesVec, uiUpdate);
    // Start with an empty nodes array in state
    state.data["nodes"].to<JsonArray>();
  }
};

static size_t runtimeNodeCount() {
  size_t count = 0;
  for (VirtualLayer* layer : layerP.layers) {
    if (layer) count += layer->nodes.size();
  }
  return count;
}

TEST_CASE("full preset replacement detaches stale layers until commit") {
  Fixture f;
  destroyedOwnedNodes = 0;
  layerP.layers[0]->nodes.push_back(new Node(true));
  layerP.ensureLayer(1)->nodes.push_back(new Node(true));
  f.state.data["layer"] = 1;
  addNode(f.state.data["nodes"].as<JsonArray>(), "Fire");
  addNode(f.state.data["nodes_0"].to<JsonArray>(), "Rainbow");

  LayerManager::PresetReplacementBackup backup;
  REQUIRE(f.lm.beginPresetReplacement(false, backup));

  CHECK_EQ(layerP.activeLayerCount, 1);
  CHECK(layerP.layers[0] != nullptr);
  CHECK(layerP.layers[1] == nullptr);
  CHECK_EQ(runtimeNodeCount(), 0u);
  CHECK_EQ(destroyedOwnedNodes, 0);
  CHECK_EQ(f.state.data.size(), 0u);

  f.state.data["layer"] = 0;
  f.state.data["nodes"].to<JsonArray>();
  NodeManager nm;
  REQUIRE(f.lm.finishPresetReplacement(nm));
  f.lm.commitPresetReplacement(backup);
  CHECK_EQ(destroyedOwnedNodes, 2);
  CHECK_EQ(layerP.activeLayerCount, 1);
}

TEST_CASE("driver nodes rebind to committed layer zero and restored layer zero") {
  Fixture f;
  Node driver;
  VirtualLayer* oldLayer0 = layerP.layers[0];
  driver.layer = oldLayer0;
  layerP.nodes.push_back(&driver);

  LayerManager::PresetReplacementBackup backup;
  REQUIRE(f.lm.beginPresetReplacement(false, backup));
  VirtualLayer* stagedLayer0 = layerP.layers[0];
  REQUIRE(stagedLayer0 != oldLayer0);
  f.lm.commitPresetReplacement(backup);
  CHECK(driver.layer == stagedLayer0);

  LayerManager::PresetReplacementBackup rollbackBackup;
  REQUIRE(f.lm.beginPresetReplacement(false, rollbackBackup));
  VirtualLayer* stagedRollbackLayer0 = layerP.layers[0];
  REQUIRE(stagedRollbackLayer0 != stagedLayer0);
  f.lm.rollbackPresetReplacement(rollbackBackup);
  CHECK(layerP.layers[0] == stagedLayer0);
  CHECK(driver.layer == stagedLayer0);
}

TEST_CASE("failed preset replacement restores the exact prior topology") {
  Fixture f;
  destroyedOwnedNodes = 0;
  VirtualLayer* oldLayer0 = layerP.layers[0];
  VirtualLayer* oldLayer2 = layerP.ensureLayer(2);
  Node* oldNode0 = new Node(true);
  Node* oldNode2 = new Node(true);
  oldLayer0->nodes.push_back(oldNode0);
  oldLayer2->nodes.push_back(oldNode2);
  f.lm.selectLayer(2, false);

  LayerManager::PresetReplacementBackup backup;
  REQUIRE(f.lm.beginPresetReplacement(false, backup));
  layerP.layers[0]->nodes.push_back(new Node(true));
  addNode(f.state.data["nodes_1"].to<JsonArray>(), "Unknown");
  NodeManager nm;
  CHECK_FALSE(f.lm.finishPresetReplacement(nm));
  f.lm.rollbackPresetReplacement(backup);

  CHECK(layerP.layers[0] == oldLayer0);
  CHECK(layerP.layers[2] == oldLayer2);
  CHECK_EQ(f.lm.getSelectedLayer(), 2);
  CHECK_EQ(layerP.activeLayerCount, 2);
  CHECK((*f.nodesVec)[0] == oldNode2);
  CHECK_EQ(destroyedOwnedNodes, 1);
}

TEST_CASE("two catalog laps keep layer and node ownership at the current preset only") {
  Fixture f;
  NodeManager nm;
  const uint8_t layerOnePresets[] = {4, 6, 7, 17};
  destroyedOwnedNodes = 0;
  int createdNodes = 0;

  for (int lap = 0; lap < 2; lap++) {
    for (uint8_t preset = 1; preset <= 20; preset++) {
      bool layerOne = false;
      for (uint8_t candidate : layerOnePresets) layerOne = layerOne || candidate == preset;

      LayerManager::PresetReplacementBackup backup;
      REQUIRE(f.lm.beginPresetReplacement(false, backup));
      if (layerOne) f.lm.selectLayer(1, false);
      layerP.layers[layerOne ? 1 : 0]->nodes.push_back(new Node(true));
      createdNodes++;
      f.state.data["layer"] = layerOne ? 1 : 0;
      f.state.data["nodes"].to<JsonArray>();
      REQUIRE(f.lm.finishPresetReplacement(nm));
      f.lm.commitPresetReplacement(backup);

      CHECK_EQ(runtimeNodeCount(), 1u);
      CHECK_EQ(destroyedOwnedNodes, createdNodes - 1);
      CHECK_EQ(layerP.activeLayerCount, layerOne ? 2 : 1);
    }
    CHECK_EQ(f.lm.getSelectedLayer(), 0u);
    CHECK_EQ(layerP.activeLayerCount, 1);
    CHECK_EQ(runtimeNodeCount(), 1u);
  }
}

static UpdatedItem updateFrom(JsonDocument& doc, const char* name) {
  UpdatedItem item;
  item.name = name;
  item.value = doc.as<JsonVariantConst>();
  return item;
}

TEST_CASE("live bare layer controls apply to the selected layer and mirror canonical state") {
  Fixture f;
  f.lm.selectLayer(2, false);

  JsonDocument brightness;
  brightness.set(123);
  CHECK(f.lm.handleUpdate(updateFrom(brightness, "brightness")));
  CHECK_EQ(layerP.layers[2]->brightness, 123);
  CHECK_EQ(f.state.data["brightness_2"].as<int>(), 123);

  JsonDocument start;
  start["x"] = 10; start["y"] = 20; start["z"] = 30;
  CHECK(f.lm.handleUpdate(updateFrom(start, "start")));
  CHECK_EQ(layerP.layers[2]->startPct.x, 10);
  CHECK_EQ(f.state.data["start_2"]["y"].as<int>(), 20);

  JsonDocument end;
  end["x"] = 80; end["y"] = 90; end["z"] = 100;
  CHECK(f.lm.handleUpdate(updateFrom(end, "end")));
  CHECK_EQ(layerP.layers[2]->endPct.x, 80);
  CHECK_EQ(f.state.data["end_2"]["y"].as<int>(), 90);
  CHECK(layerP.requestMapVirtual);
}

TEST_CASE("legacy restore ignores bare controls only while canonical layer state is absent") {
  Fixture f;
  // A prior canonical preset must not make a following legacy preset look canonical.
  f.state.data["brightness_0"] = 77;
  f.state.data["start_0"]["x"] = 12;
  LayerManager::PresetReplacementBackup backup;
  REQUIRE(f.lm.beginPresetReplacement(true, backup));
  f.lm.commitPresetReplacement(backup);
  CHECK(f.state.data["brightness_0"].isNull());
  CHECK(f.state.data["start_0"].isNull());

  JsonDocument legacyBrightness;
  legacyBrightness.set(42);
  CHECK(f.lm.handleUpdate(updateFrom(legacyBrightness, "brightness")));
  CHECK_EQ(layerP.layers[0]->brightness, 255);
  CHECK(f.state.data["brightness_0"].isNull());

  JsonDocument legacyStart;
  legacyStart["x"] = 16; legacyStart["y"] = 16; legacyStart["z"] = 1;
  CHECK(f.lm.handleUpdate(updateFrom(legacyStart, "start")));
  CHECK_EQ(layerP.layers[0]->startPct.x, 0);
  CHECK(f.state.data["start_0"].isNull());

  JsonDocument legacyEnd;
  legacyEnd["x"] = 84; legacyEnd["y"] = 84; legacyEnd["z"] = 1;
  CHECK(f.lm.handleUpdate(updateFrom(legacyEnd, "end")));
  CHECK_EQ(layerP.layers[0]->endPct.x, 100);
  CHECK(f.state.data["end_0"].isNull());
}

TEST_CASE("canonical persisted controls still apply during the restore window") {
  Fixture f;
  f.state.data["brightness_0"] = 88;
  f.state.data["start_0"]["x"] = 7;
  f.state.data["start_0"]["y"] = 8;
  f.state.data["start_0"]["z"] = 9;
  f.lm.scheduleRestore();

  JsonDocument brightness;
  brightness.set(88);
  CHECK(f.lm.handleUpdate(updateFrom(brightness, "brightness")));
  CHECK_EQ(layerP.layers[0]->brightness, 88);

  JsonDocument start;
  start["x"] = 7; start["y"] = 8; start["z"] = 9;
  CHECK(f.lm.handleUpdate(updateFrom(start, "start")));
  CHECK_EQ(layerP.layers[0]->startPct.x, 7);
  CHECK_EQ(f.state.data["start_0"]["z"].as<int>(), 9);
}

// ---------------------------------------------------------------------------
// selectLayer — JSON state swap
// ---------------------------------------------------------------------------

TEST_CASE("selectLayer saves current layer and loads next layer") {
  Fixture f;

  // Layer 0: add Gradient
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");

  // Switch to layer 1 — saves layer 0's nodes, loads layer 1 (empty)
  f.lm.selectLayer(1);

  // nodes_0 must have been saved
  REQUIRE_FALSE(f.state.data["nodes_0"].isNull());
  CHECK_EQ(f.state.data["nodes_0"].as<JsonArray>().size(), 1u);
  CHECK_EQ(strcmp(f.state.data["nodes_0"][0]["name"].as<const char*>(), "Gradient"), 0);

  // data["nodes"] must now be empty (layer 1 has no saved state)
  CHECK_EQ(f.state.data["nodes"].as<JsonArray>().size(), 0u);
  CHECK_EQ(f.lm.getSelectedLayer(), 1u);
}

TEST_CASE("selectLayer round-trip: switch away and back restores nodes") {
  Fixture f;

  // Layer 0: Gradient
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");

  // Switch to layer 1, add Fire
  f.lm.selectLayer(1);
  addNode(f.state.data["nodes"].as<JsonArray>(), "Fire");

  // Switch back to layer 0
  f.lm.selectLayer(0);

  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_FALSE(nodesHas(f.state.data, "Fire"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("selectLayer(i, swapState=false) does not touch nodes JSON") {
  Fixture f;

  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");
  // Save a reference snapshot — after selectLayer(false) the JSON must be unchanged
  size_t before = f.state.data["nodes"].as<JsonArray>().size();

  f.lm.selectLayer(1, /*swapState=*/false);

  CHECK_EQ(f.state.data["nodes"].as<JsonArray>().size(), before);
  CHECK_EQ(f.lm.getSelectedLayer(), 1u);
}

// ---------------------------------------------------------------------------
// onNodeRemoved — destroy empty non-zero layer, reload layer 0
// ---------------------------------------------------------------------------

TEST_CASE("onNodeRemoved: non-zero empty layer is destroyed and state keys cleared") {
  Fixture f;

  // Switch to layer 1
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");
  f.lm.selectLayer(1);

  // Save layer 0 state and populate layer 1 state keys
  addNode(f.state.data["nodes_1"].to<JsonArray>(), "Fire");
  f.state.data["start_1"]["x"]  = 5;
  f.state.data["end_1"]["x"]    = 95;
  f.state.data["brightness_1"]  = 200;
  // Layer 1's node vector is empty (node was just removed — that's the trigger)
  // layerP.layers[1] exists but nodes.empty() == true

  f.lm.onNodeRemoved();

  // Layer 1 must be destroyed and its JSON keys cleared
  CHECK(layerP.layers[1] == nullptr);
  CHECK(f.state.data["nodes_1"].isNull());
  CHECK(f.state.data["start_1"].isNull());
  CHECK(f.state.data["end_1"].isNull());
  CHECK(f.state.data["brightness_1"].isNull());

  // Selected layer must fall back to 0
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("onNodeRemoved: data[nodes] restored to layer 0 after layer 1 destroyed") {
  Fixture f;

  // Layer 0 has Gradient saved
  addNode(f.state.data["nodes_0"].to<JsonArray>(), "Gradient");

  // Switch to layer 1, then remove its only node
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");  // needed for save
  f.lm.selectLayer(1);
  // data["nodes"] is now empty (layer 1 had nothing saved yet)
  // Simulate: layer 1 node was just removed → nodes.empty() == true

  f.lm.onNodeRemoved();

  // data["nodes"] must reflect layer 0
  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
}

TEST_CASE("onNodeRemoved: no-op when selectedLayer is 0") {
  Fixture f;

  // selectedLayer is 0 — onNodeRemoved must not touch anything
  addNode(f.state.data["nodes"].as<JsonArray>(), "Gradient");
  f.lm.onNodeRemoved();

  // Nothing changed
  CHECK(nodesHas(f.state.data, "Gradient"));
  CHECK_EQ(f.lm.getSelectedLayer(), 0u);
  CHECK(layerP.layers[0] != nullptr);
}

TEST_CASE("onNodeRemoved: no-op when selected layer still has nodes") {
  Fixture f;

  f.lm.selectLayer(1);
  // Add a real Node so nodes.empty() == false
  Node n;
  layerP.layers[1]->nodes.push_back(&n);

  f.lm.onNodeRemoved();

  // Layer 1 must still exist (not empty)
  CHECK(layerP.layers[1] != nullptr);
  CHECK_EQ(f.lm.getSelectedLayer(), 1u);
}
