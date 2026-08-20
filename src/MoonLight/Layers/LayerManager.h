/**
    @title     MoonLight
    @file      LayerManager.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.

    Manages multi-layer selection, persistence, and lifecycle for virtual layers.
    Used by ModuleEffects to keep layer logic separate from effect-specific code.
**/

#pragma once

#if FT_MOONLIGHT

  #include <ArduinoJson.h>
  #include "MoonBase/utilities/Char.h"
  #include "LayerMappingMutex.h"

  #ifdef ARDUINO
    #include "MoonBase/Module.h"
    #include "MoonBase/NodeManager.h"
    #include "PhysicalLayer.h"
  #endif

  /// Copy data["nodes"] → data["nodes_<layer>"]. Called when leaving a layer.
  inline void layerStateSave(JsonObject data, uint8_t layer) {
    Char<16> key;
    key.format("nodes_%d", layer);
    data[key.c_str()].to<JsonArray>().set(data["nodes"].as<JsonArray>());
  }

  /// Copy data["nodes_<layer>"] → data["nodes"], or clear data["nodes"] if absent/empty.
  /// Called when entering a layer, or when restoring layer 0 after a preset load.
  inline void layerStateLoad(JsonObject data, uint8_t layer) {
    Char<16> key;
    key.format("nodes_%d", layer);
    if (!data[key.c_str()].isNull() && data[key.c_str()].as<JsonArray>().size() > 0) {
      data["nodes"].to<JsonArray>().set(data[key.c_str()].as<JsonArray>());
    } else {
      data["nodes"].to<JsonArray>();
    }
  }

  /// Remove the four per-layer state keys (nodes_N, start_N, end_N, brightness_N).
  /// Called when a layer is destroyed so compareRecursive treats the keys as absent.
  inline void layerStateClearKeys(JsonObject data, uint8_t layer) {
    Char<16> key;
    key.format("nodes_%d",      layer); data.remove(key.c_str());
    key.format("start_%d",      layer); data.remove(key.c_str());
    key.format("end_%d",        layer); data.remove(key.c_str());
    key.format("brightness_%d", layer); data.remove(key.c_str());
  }

class LayerManager {
 public:
  struct PresetReplacementBackup {
    VirtualLayer* layers[16]{};
    size_t layerSlots = 0;
    uint8_t activeLayerCount = 0;
    uint8_t selectedLayer = 0;
    bool needsRestore = false;
    bool requestMapVirtual = false;
    bool valid = false;
  };

 private:
  uint8_t selectedLayer = 0;
  bool needsRestore = false;

  // Stored references — set once via init(), valid for the lifetime of the owning module.
  ModuleState* state = nullptr;
  std::vector<Node*, VectorRAMAllocator<Node*>>** nodesPtr = nullptr;
  bool* requestUIUpdatePtr = nullptr;

 public:
  uint8_t getSelectedLayer() const { return selectedLayer; }

  /// Call once from Module::begin() before any other LayerManager methods.
  void init(ModuleState& moduleState, std::vector<Node*, VectorRAMAllocator<Node*>>*& nodes, bool& requestUIUpdate) {
    state = &moduleState;
    nodesPtr = &nodes;
    requestUIUpdatePtr = &requestUIUpdate;
  }

  /// Switch the active layer, swapping per-layer JSON state (nodes, start/end/brightness).
  bool selectLayer(uint8_t index, bool swapState = true) {
    LayerMappingGuard guard(layerP.mappingMutex);
    if (index >= layerP.layers.size()) return false;

    if (swapState && !state->data["nodes"].isNull()) {
      layerStateSave(state->data, selectedLayer);  // save current layer's node state

      // save current layer's per-layer properties
      Char<16> key;
      VirtualLayer* curLayer = layerP.layers[selectedLayer];
      if (curLayer) {
        key.format("start_%d", selectedLayer);
        state->data[key.c_str()]["x"] = curLayer->startPct.x;
        state->data[key.c_str()]["y"] = curLayer->startPct.y;
        state->data[key.c_str()]["z"] = curLayer->startPct.z;
        key.format("end_%d", selectedLayer);
        state->data[key.c_str()]["x"] = curLayer->endPct.x;
        state->data[key.c_str()]["y"] = curLayer->endPct.y;
        state->data[key.c_str()]["z"] = curLayer->endPct.z;
        key.format("brightness_%d", selectedLayer);
        state->data[key.c_str()] = curLayer->brightness;
      }

      layerStateLoad(state->data, index);  // load new layer's node state
    }

    selectedLayer = index;
    VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
    if (!layer) return false;  // allocation failed
    *nodesPtr = &(layer->nodes);
    return true;
  }

  /// Detach the current runtime topology without destroying it, then create an
  /// empty topology for a complete preset. The caller keeps mappingMutex held
  /// through finishPresetReplacement() and commit/rollback.
  bool beginPresetReplacement(bool persistedState, PresetReplacementBackup& backup) {
    LayerMappingGuard guard(layerP.mappingMutex);
    if (backup.valid || layerP.layers.size() > sizeof(backup.layers) / sizeof(backup.layers[0])) return false;

    backup.layerSlots = layerP.layers.size();
    backup.activeLayerCount = layerP.activeLayerCount;
    backup.selectedLayer = selectedLayer;
    backup.needsRestore = needsRestore;
    backup.requestMapVirtual = layerP.requestMapVirtual;
    for (size_t i = 0; i < backup.layerSlots; i++) {
      backup.layers[i] = layerP.layers[i];
      layerP.layers[i] = nullptr;
    }
    backup.valid = true;

    needsRestore = persistedState;
    layerP.activeLayerCount = 0;

    selectedLayer = 0;
    VirtualLayer* layer0 = layerP.ensureLayer(0);
    if (!layer0) {
      rollbackPresetReplacement(backup);
      return false;
    }
    *nodesPtr = &(layer0->nodes);

    state->data.clear();
    return true;
  }

  /// Rebuild only non-selected layers explicitly present in the incoming
  /// document, then close the persisted-state migration window.
  bool finishPresetReplacement(NodeManager& nm) {
    LayerMappingGuard guard(layerP.mappingMutex);
    if (!restoreNonSelectedLayers(nm)) return false;
    needsRestore = false;
    layerP.requestMapVirtual = true;
    *requestUIUpdatePtr = true;
    return true;
  }

  void commitPresetReplacement(PresetReplacementBackup& backup) {
    LayerMappingGuard guard(layerP.mappingMutex);
    if (!backup.valid) return;
    // Physical driver/layout nodes survive preset replacement. Rebind them to
    // the staged layer 0 before retiring the old topology they referenced.
    layerP.rebindDriverNodes(layerP.layers[0]);
    for (size_t i = 0; i < backup.layerSlots; i++) {
      layerP.destroyLayer(backup.layers[i]);
    }
    backup.valid = false;
  }

  void rollbackPresetReplacement(PresetReplacementBackup& backup) {
    LayerMappingGuard guard(layerP.mappingMutex);
    if (!backup.valid) return;
    // Keep surviving drivers valid while the staged topology is retired.
    layerP.rebindDriverNodes(backup.layers[0]);
    for (VirtualLayer*& layer : layerP.layers) {
      layerP.destroyLayer(layer);
    }
    for (size_t i = 0; i < backup.layerSlots; i++) {
      layerP.layers[i] = backup.layers[i];
      backup.layers[i] = nullptr;
    }
    layerP.activeLayerCount = backup.activeLayerCount;
    selectedLayer = backup.selectedLayer;
    needsRestore = backup.needsRestore;
    layerP.requestMapVirtual = backup.requestMapVirtual;
    VirtualLayer* layer = selectedLayer < layerP.layers.size() ? layerP.layers[selectedLayer] : nullptr;
    *nodesPtr = layer ? &(layer->nodes) : nullptr;
    backup.valid = false;
  }

  /// Schedule restoration of non-selected layers. Call from begin() after NodeManager::begin().
  void scheduleRestore() { needsRestore = true; }

  /// Call from loop20ms(). If a restore is pending (after FS state has been loaded), instantiate
  /// nodes for non-selected layers and apply their persisted bounds.
  void checkRestore(NodeManager& nm) {
    if (!needsRestore) return;
    needsRestore = false;
    restoreNonSelectedLayers(nm);
  }

  /// Called after a node is removed. If the current layer is empty, destroy it and switch to layer 0.
  void onNodeRemoved() {
    LayerMappingGuard guard(layerP.mappingMutex);
    if (selectedLayer > 0 && layerP.layers[selectedLayer] && layerP.layers[selectedLayer]->nodes.empty()) {
      uint8_t destroyedLayer = selectedLayer;
      EXT_LOGD(ML_TAG, "Destroying empty VirtualLayer %d", destroyedLayer);
      layerP.destroyLayer(layerP.layers[destroyedLayer]);
      layerP.activeLayerCount--;
      // clean up JSON state for the destroyed layer
      layerStateClearKeys(state->data, destroyedLayer);

      // switch UI back to layer 0: reload layer 0's nodes and properties from state
      selectLayer(0, false);
      state->data["layer"] = 0;

      // Reload layer 0's node array into state->data["nodes"] (selectLayer(false) skipped state swap)
      layerStateLoad(state->data, 0);

      // Reload layer 0's per-layer properties from state
      Char<16> key;
      VirtualLayer* layer0 = layerP.layers[0];
      if (layer0) {
        key.format("start_%d", 0);
        if (!state->data[key.c_str()].isNull()) {
          layer0->startPct = {state->data[key.c_str()]["x"].as<int>(), state->data[key.c_str()]["y"].as<int>(), state->data[key.c_str()]["z"].as<int>()};
        }
        key.format("end_%d", 0);
        if (!state->data[key.c_str()].isNull()) {
          layer0->endPct = {state->data[key.c_str()]["x"] | 100, state->data[key.c_str()]["y"] | 100, state->data[key.c_str()]["z"] | 100};
        }
        key.format("brightness_%d", 0);
        if (!state->data[key.c_str()].isNull()) {
          layer0->brightness = state->data[key.c_str()] | 255;
        }
      }

      layerP.requestMapVirtual = true;
      *requestUIUpdatePtr = true;
    }
  }

  /// Handle layer-related onUpdate events. Returns true if the event was consumed.
  bool handleUpdate(const UpdatedItem& updatedItem) {
    if (updatedItem.parent[0] != "") return false;
    LayerMappingGuard guard(layerP.mappingMutex);

    if (updatedItem.name == "layer") {
      selectLayer(updatedItem.value.as<uint8_t>());
      *requestUIUpdatePtr = true;
      return true;
    }
    if (updatedItem.name == "brightness") {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return true;
      Char<16> key;
      key.format("brightness_%d", selectedLayer);
      // Old presets stored the global brightness under bare "brightness"; new presets use "brightness_0".
      // Suppress that legacy field only while a persisted restore is pending. A live bare field is
      // the public control update and must apply to the selected layer.
      if (needsRestore && state->data[key.c_str()].isNull()) {
        EXT_LOGD(ML_TAG, "Old preset: ignoring bare 'brightness' (was global, not layer), using default 255");
        return true;
      }
      layer->brightness = updatedItem.value.as<uint8_t>();
      state->data[key.c_str()] = layer->brightness;
      return true;
    }
    if (updatedItem.name == "start") {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return true;
      Char<16> key;
      key.format("start_%d", selectedLayer);
      // Old presets stored pixel coordinates under bare "start"/"end"; new presets use "start_0".
      // Ignore absent per-layer state only during legacy persisted restore, not during live edits.
      if (needsRestore && state->data[key.c_str()].isNull()) {
        EXT_LOGD(ML_TAG, "Old preset: ignoring bare 'start' pixel coords, using default {0,0,0}");
        return true;
      }
      layer->startPct = {updatedItem.value["x"].as<int>(), updatedItem.value["y"].as<int>(), updatedItem.value["z"].as<int>()};
      state->data[key.c_str()].set(updatedItem.value);
      layerP.requestMapVirtual = true;
      return true;
    }
    if (updatedItem.name == "end") {
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return true;
      Char<16> key;
      key.format("end_%d", selectedLayer);
      // Old presets stored pixel coordinates under bare "start"/"end"; new presets use "end_0".
      // Ignore absent per-layer state only during legacy persisted restore, not during live edits.
      if (needsRestore && state->data[key.c_str()].isNull()) {
        EXT_LOGD(ML_TAG, "Old preset: ignoring bare 'end' pixel coords, using default {100,100,100}");
        return true;
      }
      layer->endPct = {updatedItem.value["x"] | 100, updatedItem.value["y"] | 100, updatedItem.value["z"] | 100};
      state->data[key.c_str()].set(updatedItem.value);
      layerP.requestMapVirtual = true;
      return true;
    }
    return false;
  }

  /// Install a readHook on the module state that injects per-layer bounds into the JSON sent to the UI.
  void installReadHook() {
    state->readHook = [this](JsonObject data) {
      LayerMappingGuard guard(layerP.mappingMutex);
      VirtualLayer* layer = layerP.ensureLayer(selectedLayer);
      if (!layer) return;
      data["start"]["x"] = layer->startPct.x;
      data["start"]["y"] = layer->startPct.y;
      data["start"]["z"] = layer->startPct.z;
      data["end"]["x"] = layer->endPct.x;
      data["end"]["y"] = layer->endPct.y;
      data["end"]["z"] = layer->endPct.z;
      data["brightness"] = layer->brightness;
    };
  }

  /// Add layer selection dropdown and per-layer bound controls to setupDefinition.
  #ifdef ARDUINO // Because addLayerControls takes a Module& parameter, and Module is only defined when #include "MoonBase/Module.h" is processed — which is guarded by #ifdef ARDUINO. Without the method-level guard, native compilation fails with "unknown type name 'Module'". It's a consequence of the include guard, not a deliberate design choice. It's not testable anyway (it's pure UI setup), so the guard is harmless. If you'd prefer to avoid it, the alternative is to add a Module stub to the test stubs — but Module is complex enough that guarding the one method is simpler.
  static void addLayerControls(Module& module, const JsonArray& controls) {
    LayerMappingGuard guard(layerP.mappingMutex);
    JsonObject control = module.addControl(controls, "layer", "select");
    control["default"] = 0;
    // Find highest active index so that a "add new" slot always appears beyond it,
    // even when middle layers have been deleted and activeLayerCount has holes.
    uint8_t highestActive = 0;
    for (uint8_t i = 0; i < layerP.layers.size(); i++) {
      if (layerP.layers[i]) highestActive = i;
    }
    for (uint8_t i = 0; i <= highestActive + 1 && i < layerP.layers.size(); i++) {
      Char<12> layerName;
      layerName.format("Layer %d", i + 1);
      module.addControlValue(control, layerName.c_str());
    }

    control = module.addControl(controls, "start", "coord3D", 0, 100, false, "%");
    control["default"]["x"] = 0; control["default"]["y"] = 0; control["default"]["z"] = 0;
    control = module.addControl(controls, "end", "coord3D", 0, 100, false, "%");
    control["default"]["x"] = 100; control["default"]["y"] = 100; control["default"]["z"] = 100;
    control = module.addControl(controls, "brightness", "slider", 0, 255);
    control["default"] = 255;
  }
  #endif  // ARDUINO

 private:
  /// Instantiate nodes for non-selected layers and restore their per-layer bounds from JSON state.
  bool restoreNonSelectedLayers(NodeManager& nm) {
    LayerMappingGuard guard(layerP.mappingMutex);
    uint8_t savedSelectedLayer = selectedLayer;
    Char<16> key;
    bool restoredAny = false;

    // Migrate old-format state: old firmware stored 'end' as pixel coordinates (e.g. {16,16,1}
    // for a 16×16 panel) rather than percentages. New firmware uses per-layer keys (end_0, …).
    // If end_0 is absent, reset layer 0 bounds to defaults now — after all deferred compareRecursive
    // updates have settled — so the stale pixel values cannot win by arriving after scheduleRestore().
    key.format("end_%d", savedSelectedLayer);
    if (state->data[key.c_str()].isNull() && layerP.layers[savedSelectedLayer]) {
      layerP.layers[savedSelectedLayer]->startPct  = {0, 0, 0};
      layerP.layers[savedSelectedLayer]->endPct    = {100, 100, 100};
      layerP.layers[savedSelectedLayer]->brightness = 255;
      state->data.remove("start");
      state->data.remove("end");
      state->data.remove("brightness");
      EXT_LOGD(ML_TAG, "Migrated old-format state: reset layer %d bounds to defaults", savedSelectedLayer);
    }

    for (uint8_t i = 0; i < layerP.layers.size(); i++) {
      if (i == savedSelectedLayer) continue;

      key.format("nodes_%d", i);
      JsonArray layerNodes = state->data[key.c_str()];
      if (layerNodes.isNull() || layerNodes.size() == 0) continue;

      VirtualLayer* layer = layerP.ensureLayer(i);
      if (!layer) { EXT_LOGW(ML_TAG, "ensureLayer(%d) failed", i); return false; }
      selectedLayer = i;
      *nodesPtr = &(layer->nodes);

      for (uint8_t j = 0; j < layerNodes.size(); j++) {
        JsonObject nodeState = layerNodes[j];
        if (nodeState["name"].isNull()) return false;
        char name[32];
        strlcpy(name, nodeState["name"].as<const char*>(), 32);
        Node* node = nm.addNode(j, name, nodeState["controls"]);
        if (!node) return false;
        node->on = nodeState["on"];
        node->requestMappings();
      }

      // restore per-layer bounds
      key.format("start_%d", i);
      if (!state->data[key.c_str()].isNull()) {
        layer->startPct = {state->data[key.c_str()]["x"].as<int>(), state->data[key.c_str()]["y"].as<int>(), state->data[key.c_str()]["z"].as<int>()};
      }
      key.format("end_%d", i);
      if (!state->data[key.c_str()].isNull()) {
        layer->endPct = {state->data[key.c_str()]["x"] | 100, state->data[key.c_str()]["y"] | 100, state->data[key.c_str()]["z"] | 100};
      }
      key.format("brightness_%d", i);
      if (!state->data[key.c_str()].isNull()) {
        layer->brightness = state->data[key.c_str()] | 255;
      }

      EXT_LOGD(ML_TAG, "Restored layer %d: %d nodes, start:%d,%d,%d end:%d,%d,%d brightness:%d",
               i, layer->nodes.size(), layer->startPct.x, layer->startPct.y, layer->startPct.z,
               layer->endPct.x, layer->endPct.y, layer->endPct.z, layer->brightness);
      restoredAny = true;
    }
    selectedLayer = savedSelectedLayer;
    VirtualLayer* layer = layerP.layers[selectedLayer];
    if (layer) *nodesPtr = &(layer->nodes);


    if (restoredAny) layerP.requestMapVirtual = true;
    return true;
  }
};

#endif  // FT_MOONLIGHT
