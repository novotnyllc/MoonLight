/**
    @title     MoonLight
    @file      ModuleEffects.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/effects/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleEffects_h
#define ModuleEffects_h

#if FT_MOONLIGHT

  #include <cstring>
  #include "FastLED.h"
  #include "MoonBase/NodeManager.h"
  #include "MoonLight/Layers/LayerManager.h"
  #include "MoonLight/Modules/ModuleLightsControl.h"

// #include "MoonBase/Nodes.h" //Nodes.h will include VirtualLayer.h which will include PhysicalLayer.h

class ModuleEffects : public NodeManager {
 public:
  ModuleLightsControl* _moduleLightsControl;
  LayerManager layerMgr;

 private:
  mutable bool presetReplacementBuilding = false;
  mutable bool presetBuildFailed = false;
  JsonObject presetSpareState;

  static bool presetNodeDocumentsValid(JsonObject data) {
    for (JsonPair item : data) {
      const char* key = item.key().c_str();
      if (strcmp(key, "nodes") != 0 && strncmp(key, "nodes_", 6) != 0) continue;
      if (!item.value().is<JsonArray>()) return false;
      for (JsonVariant node : item.value().as<JsonArray>()) {
        const char* name = node["name"];
        if (!name || !name[0]) return false;
      }
    }
    return true;
  }

  void activateDeferredLiveScripts() {
  #if FT_LIVESCRIPT
    for (VirtualLayer* layer : layerP.layers) {
      if (!layer) continue;
      for (Node* node : layer->nodes) {
        if (node && node->isLiveScriptNode()) static_cast<LiveScriptNode*>(node)->activateDeferredSetup();
      }
    }
  #endif
  }

 public:

  ModuleEffects(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager, ModuleLightsControl* moduleLightsControl) : NodeManager("effects", server, sveltekit, fileManager) {
    EXT_LOGV(ML_TAG, "constructor");
    _moduleLightsControl = moduleLightsControl;
  }

  void begin() override {
    defaultNodeName = safeModeMB ? getNameAndTags<SolidEffect>() : getNameAndTags<RandomEffect>();
    layerMgr.init(_state, nodes, requestUIUpdate);
    if (!layerMgr.selectLayer(0, false)) {  // initial setup, no state to swap yet
      EXT_LOGE(ML_TAG, "Failed to allocate required VirtualLayer 0; effects module disabled");
      return;
    }
    NodeManager::begin();
    layerMgr.scheduleRestore();

  #if FT_ENABLED(FT_MONITOR)
    _sveltekit->getSocket()->registerEvent("monitor");
    _sveltekit->getSocket()->onSubscribe("monitor", [this](const String&) { sendMonitorLayout(); });
    _server->on(
        "/rest/monitorLayout", HTTP_GET,
        _sveltekit->getSecurityManager()->wrapRequest(
            [this](PsychicRequest* request) {
              EXT_LOGV(ML_TAG, "rest monitor triggered");
              sendMonitorLayout();

              PsychicJsonResponse response = PsychicJsonResponse(request, false);
              return response.send();
            },
            AuthenticationPredicates::IS_AUTHENTICATED));
  #endif

    layerMgr.installReadHook();
  }

  #if FT_ENABLED(FT_MONITOR)
 private:
  void sendMonitorLayout() {
    LightsHeader header{};
    std::vector<uint8_t, VectorRAMAllocator<uint8_t>> positions;
    bool captured = false;
    {
      LayerMappingGuard guard(layerP.mappingMutex);
      layerP.pass = 1;
      layerP.monitorPass = true;
      layerP.mapLayout();
      layerP.monitorPass = false;

      extern SemaphoreHandle_t swapMutex;
      xSemaphoreTake(swapMutex, portMAX_DELAY);
      if (layerP.lights.header.isPositions == 2 && layerP.lights.channelsD && layerP.lights.header.nrOfLights) {
        header = layerP.lights.header;
        const size_t positionBytes = static_cast<size_t>(header.nrOfLights) * 3;
        positions.resize(positionBytes);
        memcpy(positions.data(), layerP.lights.channelsD, positionBytes);
        memset(layerP.lights.channelsD, 0, positionBytes);
        layerP.lights.header.isPositions = 3;
        captured = true;
      }
      xSemaphoreGive(swapMutex);
    }
    layerP.requestMapVirtual.store(true);
    if (captured) {
      _moduleLightsControl->storeMonitorLayout(header, positions.data(), positions.size());
    }
    _moduleLightsControl->emitStoredMonitorLayout();
  }

 public:
  #endif

  bool shouldLoadPersistedState() const override { return !safeModeMB; }

  StateUpdateResult replaceFullState(JsonObject& newData, const String& originId, bool persistedState = false) override {
    if (newData.size() == 0 || !presetNodeDocumentsValid(newData)) return StateUpdateResult::ERROR;
    return updateWithoutPropagation(
        [&](ModuleState& state) {
          LayerMappingGuard guard(layerP.mappingMutex);
          const int targetLayer = newData["layer"].isNull() ? 0 : newData["layer"].as<int>();
          if (targetLayer < 0 || static_cast<size_t>(targetLayer) >= layerP.layers.size() || !gModulesDoc) return StateUpdateResult::ERROR;

          JsonObject previousState = state.data;
          if (presetSpareState.isNull()) presetSpareState = gModulesDoc->as<JsonArray>().add<JsonObject>();
          JsonObject stagedState = presetSpareState;
          if (stagedState.isNull()) return StateUpdateResult::ERROR;
          stagedState.clear();
          state.data = stagedState;

          LayerManager::PresetReplacementBackup backup;
          presetBuildFailed = false;
          presetReplacementBuilding = true;
          if (!layerMgr.beginPresetReplacement(persistedState, backup) || !layerMgr.selectLayer(static_cast<uint8_t>(targetLayer), false)) {
            presetReplacementBuilding = false;
            layerMgr.rollbackPresetReplacement(backup);
            state.data = previousState;
            stagedState.clear();
            presetSpareState = stagedState;
            return StateUpdateResult::ERROR;
          }

          StateUpdateResult result = ModuleState::update(newData, state, originId);
          bool complete = result != StateUpdateResult::ERROR && !presetBuildFailed && layerMgr.finishPresetReplacement(*this);
          presetReplacementBuilding = false;
          if (!complete || presetBuildFailed) {
            layerMgr.rollbackPresetReplacement(backup);
            state.data = previousState;
            stagedState.clear();
            presetSpareState = stagedState;
            return StateUpdateResult::ERROR;
          }

          layerMgr.commitPresetReplacement(backup);
          // LiveScript uses a shared runtime keyed by animation name. Start a
          // staged script only after old nodes (including a same-name script)
          // have been destroyed, so rollback never mutates the active runtime.
          activateDeferredLiveScripts();
          previousState.clear();
          presetSpareState = previousState;
          return result;
        },
        originId);
  }

  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(ML_TAG, "");
    LayerManager::addLayerControls(*this, controls);
    NodeManager::setupDefinition(controls);
  }

  void addNodes(const JsonObject& control) override {
    // keep the order the same as in https://moonmodules.org/MoonLight/moonlight/effects

    // MoonLight effects, Solid first then alphabetically
    addNodeValue<SolidEffect>(control);
    addNodeValue<AudioRingsEffect>(control);
    addNodeValue<LinesEffect>(control);
    addNodeValue<FireEffect>(control);
    addNodeValue<FixedRectangleEffect>(control);
  #if USE_M5UNIFIED
    addNodeValue<MoonManEffect>(control);
  #endif
    addNodeValue<FreqSawsEffect>(control);
    addNodeValue<MarioTestEffect>(control);
    addNodeValue<ParticlesEffect>(control);
    addNodeValue<PixelMapEffect>(control);
    addNodeValue<PraxisEffect>(control);
    addNodeValue<RadarEffect>(control);
    addNodeValue<RandomEffect>(control);
    addNodeValue<RingRandomFlowEffect>(control);
    addNodeValue<RipplesEffect>(control);
    addNodeValue<RubiksCubeEffect>(control);
    addNodeValue<ScrollingTextEffect>(control);
    addNodeValue<SinusEffect>(control);
    addNodeValue<SphereMoveEffect>(control);
    addNodeValue<SpiralFireEffect>(control);
    addNodeValue<StarFieldEffect>(control);
    addNodeValue<StarSkyEffect>(control);
    addNodeValue<VUMeterEffect>(control);
    addNodeValue<WaveEffect>(control);
    addNodeValue<WraparoundRacersEffect>(control);
    addNodeValue<CrossingSpiralEffect>(control);
    addNodeValue<HorizonRingEffect>(control);

    // MoonModules effects, alphabetically
    addNodeValue<GameOfLifeEffect>(control);
    addNodeValue<GEQ3DEffect>(control);
    addNodeValue<PaintBrushEffect>(control);

    // WLED effects, alphabetically
    addNodeValue<BlackholeEffect>(control);
    addNodeValue<BlinkRainbowEffect>(control);
    addNodeValue<BlurzEffect>(control);
    addNodeValue<BouncingBallsEffect>(control);
    addNodeValue<ColorTwinkleEffect>(control);
    addNodeValue<DistortionWavesEffect>(control);
    addNodeValue<DJLightEffect>(control);
    addNodeValue<DNAEffect>(control);
    addNodeValue<DripEffect>(control);
    addNodeValue<FireworksEffect>(control);
    addNodeValue<FlowEffect>(control);
    addNodeValue<FrizzlesEffect>(control);
    addNodeValue<FunkyPlankEffect>(control);
    addNodeValue<GEQEffect>(control);
    addNodeValue<HeartBeatEffect>(control);
    addNodeValue<JuliaEffect>(control);
    addNodeValue<LissajousEffect>(control);
    addNodeValue<MeteorEffect>(control);
    addNodeValue<Noise2DEffect>(control);
    addNodeValue<NoisefireEffect>(control);
    addNodeValue<NoisemoveEffect>(control);
    addNodeValue<OctopusEffect>(control);
    addNodeValue<OscillateEffect>(control);
    addNodeValue<PacManEffect>(control);
    addNodeValue<PhasedNoiseEffect>(control);
    addNodeValue<PlasmaEffect>(control);
    addNodeValue<PoliceEffect>(control);
    addNodeValue<PopCornEffect>(control);
    addNodeValue<RainEffect>(control);
    addNodeValue<TetrixEffect>(control);
    addNodeValue<WaverlyEffect>(control);

    addNodeValue<FreqmapEffect>(control);
    addNodeValue<FreqMatrixEffect>(control);
    addNodeValue<FreqpixelsEffect>(control);
    addNodeValue<FreqwaveEffect>(control);
    addNodeValue<GravfreqEffect>(control);
    addNodeValue<GravimeterEffect>(control);
    addNodeValue<GravcenterEffect>(control);
    addNodeValue<GravcentricEffect>(control);
    addNodeValue<MidnoiseEffect>(control);
    addNodeValue<NoiseMeterEffect>(control);
    addNodeValue<PixelwaveEffect>(control);
    addNodeValue<PlasmoidEffect>(control);
    addNodeValue<PuddlepeakEffect>(control);
    addNodeValue<PuddlesEffect>(control);
    addNodeValue<RipplepeakEffect>(control);
    addNodeValue<RocktavesEffect>(control);
    addNodeValue<WaterfallEffect>(control);

    // FastLED effects
    addNodeValue<ColorTrailsEffect>(control);
    addNodeValue<RainbowEffect>(control);
    addNodeValue<FLAudioEffect>(control);
    addNodeValue<FixedPointCanvasDemoEffect>(control);

    // Moving head effects, alphabetically
    addNodeValue<AmbientMoveEffect>(control);
    addNodeValue<FreqColorsEffect>(control);
    addNodeValue<Troy1ColorEffect>(control);
    addNodeValue<Troy1MoveEffect>(control);
    addNodeValue<Troy2ColorEffect>(control);
    addNodeValue<Troy2MoveEffect>(control);
    addNodeValue<WowiMoveEffect>(control);

    // Modifiers, most used first
    addNodeValue<MultiplyModifier>(control);
    addNodeValue<MirrorModifier>(control);
    addNodeValue<TransposeModifier>(control);
    addNodeValue<CircleModifier>(control);
    addNodeValue<BlockModifier>(control);
    addNodeValue<RotateModifier>(control);
    addNodeValue<CheckerboardModifier>(control);
    addNodeValue<PinwheelModifier>(control);
    addNodeValue<RippleXZModifier>(control);

    // find all the .sc files on FS
    File rootFolder = ESPFS.open("/livescripts");
    walkThroughFiles(rootFolder, [&](File folder, File file) {
      const char* fname = file.name();
      size_t len = strlen(fname);
      bool isSc = (len >= 3) && strcmp(fname + (len - 3), ".sc") == 0;
      if (isSc && (strncmp(fname, "E_", 2) == 0 || strncmp(fname, "M_", 2) == 0)) {
        if (control["values"].isNull()) control["values"].to<JsonArray>();
        JsonObject entry = control["values"].as<JsonArray>().add<JsonObject>();
        entry["name"] = (const char*)file.path();
        entry["category"] = "LiveScript";
      }
    });
    rootFolder.close();
  }

  Node* addNode(const uint8_t index, char* name, const JsonArray& controls) const override {
    Node* node = nullptr;

    // MoonLight effects, Solid first then alphabetically
    // cppcheck-suppress knownConditionTrueFalse -- intentional: chain tries each type in order; first check is always true after node=nullptr
    if (!node) node = checkAndAlloc<SolidEffect>(name);
    if (!node) node = checkAndAlloc<AudioRingsEffect>(name);
    if (!node) node = checkAndAlloc<FireEffect>(name);
    if (!node) node = checkAndAlloc<FixedRectangleEffect>(name);
    if (!node) node = checkAndAlloc<FreqSawsEffect>(name);
    if (!node) node = checkAndAlloc<LinesEffect>(name);
    if (!node) node = checkAndAlloc<MarioTestEffect>(name);
  #if USE_M5UNIFIED
    if (!node) node = checkAndAlloc<MoonManEffect>(name);
  #endif
    if (!node) node = checkAndAlloc<ParticlesEffect>(name);
    if (!node) node = checkAndAlloc<PixelMapEffect>(name);
    if (!node) node = checkAndAlloc<PraxisEffect>(name);
    if (!node) node = checkAndAlloc<RadarEffect>(name);
    if (!node) node = checkAndAlloc<RandomEffect>(name);
    if (!node) node = checkAndAlloc<RingRandomFlowEffect>(name);
    if (!node) node = checkAndAlloc<RipplesEffect>(name);
    if (!node) node = checkAndAlloc<RubiksCubeEffect>(name);
    if (!node) node = checkAndAlloc<ScrollingTextEffect>(name);
    if (!node) node = checkAndAlloc<SinusEffect>(name);
    if (!node) node = checkAndAlloc<SphereMoveEffect>(name);
    if (!node) node = checkAndAlloc<SpiralFireEffect>(name);
    if (!node) node = checkAndAlloc<StarFieldEffect>(name);
    if (!node) node = checkAndAlloc<StarSkyEffect>(name);
    if (!node) node = checkAndAlloc<VUMeterEffect>(name);
    if (!node) node = checkAndAlloc<WaveEffect>(name);
    if (!node) node = checkAndAlloc<WraparoundRacersEffect>(name);
    if (!node) node = checkAndAlloc<CrossingSpiralEffect>(name);
    if (!node) node = checkAndAlloc<HorizonRingEffect>(name);

    // MoonModules effects, alphabetically
    if (!node) node = checkAndAlloc<GameOfLifeEffect>(name);
    if (!node) node = checkAndAlloc<GEQ3DEffect>(name);
    if (!node) node = checkAndAlloc<PaintBrushEffect>(name);

    // WLED effects, alphabetically
    if (!node) node = checkAndAlloc<BlackholeEffect>(name);
    if (!node) node = checkAndAlloc<BlinkRainbowEffect>(name);
    if (!node) node = checkAndAlloc<BlurzEffect>(name);
    if (!node) node = checkAndAlloc<BouncingBallsEffect>(name);
    if (!node) node = checkAndAlloc<ColorTwinkleEffect>(name);
    if (!node) node = checkAndAlloc<DistortionWavesEffect>(name);
    if (!node) node = checkAndAlloc<DJLightEffect>(name);
    if (!node) node = checkAndAlloc<DNAEffect>(name);
    if (!node) node = checkAndAlloc<DripEffect>(name);
    if (!node) node = checkAndAlloc<FireworksEffect>(name);
    if (!node) node = checkAndAlloc<FlowEffect>(name);
    if (!node) node = checkAndAlloc<FrizzlesEffect>(name);
    if (!node) node = checkAndAlloc<FunkyPlankEffect>(name);
    if (!node) node = checkAndAlloc<GEQEffect>(name);
    if (!node) node = checkAndAlloc<HeartBeatEffect>(name);
    if (!node) node = checkAndAlloc<JuliaEffect>(name);
    if (!node) node = checkAndAlloc<LissajousEffect>(name);
    if (!node) node = checkAndAlloc<MeteorEffect>(name);
    if (!node) node = checkAndAlloc<Noise2DEffect>(name);
    if (!node) node = checkAndAlloc<NoisefireEffect>(name);
    if (!node) node = checkAndAlloc<NoisemoveEffect>(name);
    if (!node) node = checkAndAlloc<OctopusEffect>(name);
    if (!node) node = checkAndAlloc<OscillateEffect>(name);
    if (!node) node = checkAndAlloc<PacManEffect>(name);
    if (!node) node = checkAndAlloc<PhasedNoiseEffect>(name);
    if (!node) node = checkAndAlloc<PlasmaEffect>(name);
    if (!node) node = checkAndAlloc<PoliceEffect>(name);
    if (!node) node = checkAndAlloc<PopCornEffect>(name);
    if (!node) node = checkAndAlloc<RainEffect>(name);
    if (!node) node = checkAndAlloc<TetrixEffect>(name);
    if (!node) node = checkAndAlloc<WaverlyEffect>(name);

    if (!node) node = checkAndAlloc<FreqmapEffect>(name);
    if (!node) node = checkAndAlloc<FreqMatrixEffect>(name);
    if (!node) node = checkAndAlloc<FreqpixelsEffect>(name);
    if (!node) node = checkAndAlloc<FreqwaveEffect>(name);
    if (!node) node = checkAndAlloc<GravfreqEffect>(name);
    if (!node) node = checkAndAlloc<GravimeterEffect>(name);
    if (!node) node = checkAndAlloc<GravcenterEffect>(name);
    if (!node) node = checkAndAlloc<GravcentricEffect>(name);
    if (!node) node = checkAndAlloc<MidnoiseEffect>(name);
    if (!node) node = checkAndAlloc<NoiseMeterEffect>(name);
    if (!node) node = checkAndAlloc<PixelwaveEffect>(name);
    if (!node) node = checkAndAlloc<PlasmoidEffect>(name);
    if (!node) node = checkAndAlloc<PuddlepeakEffect>(name);
    if (!node) node = checkAndAlloc<PuddlesEffect>(name);
    if (!node) node = checkAndAlloc<RipplepeakEffect>(name);
    if (!node) node = checkAndAlloc<RocktavesEffect>(name);
    if (!node) node = checkAndAlloc<WaterfallEffect>(name);

    // FastLED
    if (!node) node = checkAndAlloc<ColorTrailsEffect>(name);
    if (!node) node = checkAndAlloc<RainbowEffect>(name);
    if (!node) node = checkAndAlloc<FLAudioEffect>(name);
    if (!node) node = checkAndAlloc<FixedPointCanvasDemoEffect>(name);

    // Moving head effects, alphabetically

    if (!node) node = checkAndAlloc<AmbientMoveEffect>(name);
    if (!node) node = checkAndAlloc<FreqColorsEffect>(name);
    if (!node) node = checkAndAlloc<Troy1ColorEffect>(name);
    if (!node) node = checkAndAlloc<Troy1MoveEffect>(name);
    if (!node) node = checkAndAlloc<Troy2ColorEffect>(name);
    if (!node) node = checkAndAlloc<Troy2MoveEffect>(name);
    if (!node) node = checkAndAlloc<WowiMoveEffect>(name);

    // Modifiers, most used first

    if (!node) node = checkAndAlloc<MultiplyModifier>(name);
    if (!node) node = checkAndAlloc<MirrorModifier>(name);
    if (!node) node = checkAndAlloc<TransposeModifier>(name);
    if (!node) node = checkAndAlloc<CircleModifier>(name);
    if (!node) node = checkAndAlloc<BlockModifier>(name);
    if (!node) node = checkAndAlloc<RotateModifier>(name);
    if (!node) node = checkAndAlloc<CheckerboardModifier>(name);
    if (!node) node = checkAndAlloc<PinwheelModifier>(name);
    if (!node) node = checkAndAlloc<RippleXZModifier>(name);

  #if FT_LIVESCRIPT
    if (!node && !safeModeMB && ESPFS.exists(name)) {
      LiveScriptNode* liveScriptNode = allocMBObject<LiveScriptNode>();
      if (liveScriptNode) {
        liveScriptNode->animation = name;  // set the (file)name of the script
        node = liveScriptNode;
      }
    }
  #endif

    if (node) {
      EXT_LOGI(ML_TAG, "Add %s (p:%p pr:%d)", name, node, isInPSRAM(node));

      VirtualLayer* layer = layerP.ensureLayer(layerMgr.getSelectedLayer());
      if (!layer) {
        freeMBObject(node);
        if (presetReplacementBuilding) presetBuildFailed = true;
        return nullptr;
      }
      node->constructor(layer, controls, &layerP.effectsMutex);  // pass the selected layer to the node
      node->moduleControl = _moduleLightsControl;                // to access global lights control functions if needed
      // node->moduleIO = _moduleIO;                     // to get pin allocations
      node->moduleNodes = (Module*)this;  // cppcheck-suppress dangerousTypeCast -- upcast; to request UI update
    #if FT_LIVESCRIPT
      if (presetReplacementBuilding && node->isLiveScriptNode())
        static_cast<LiveScriptNode*>(node)->deferSetup();
      else
    #endif
        node->setup();                    // run the setup of the effect
      node->onSizeChanged(Coord3D());   // to init memory allocations

      // from here it runs concurrently in the effects task
      if (index >= layer->nodes.size())
        layer->nodes.push_back(node);
      else
        layer->nodes[index] = node;  // add the node to the layer
    }

    if (!node && presetReplacementBuilding) presetBuildFailed = true;

    return node;
  }

  void onNodeRemoved() override {
    layerMgr.onNodeRemoved();
  }

  void loop20ms() override {
    NodeManager::loop20ms();

    layerMgr.checkRestore(*this);

    if (triggerResetPreset) {
      triggerResetPreset = false;
      _moduleLightsControl->read(
          [&](ModuleState& state) {
            if (state.data["preset"]["selected"] != 255) {  // not needed if already 255
              JsonDocument doc;
              JsonObject newState = doc.to<JsonObject>();

              // EXT_LOGD(ML_TAG, "remove preset");
              newState["preset"] = state.data["preset"];
              newState["preset"]["select"] = 255;
              _moduleLightsControl->update(newState, ModuleState::update, _moduleName);  // Do not add server in the originID as that blocks updates, see execOnUpdate
            }
          },
          _moduleName);
    }

    if (pendingSyncBpm >= 0 || pendingSyncIntensity >= 0) {
      JsonDocument doc;
      JsonObject newState = doc.to<JsonObject>();
      if (pendingSyncBpm >= 0) {
        newState["bpm"] = (uint8_t)pendingSyncBpm;
        pendingSyncBpm = -1;
      }
      if (pendingSyncIntensity >= 0) {
        newState["intensity"] = (uint8_t)pendingSyncIntensity;
        pendingSyncIntensity = -1;
      }
      _moduleLightsControl->update(newState, ModuleState::update, _moduleName);
    }
  }

  void loop1s() override {
    // set shared data (eg used in scrolling text effect), every second
    sharedData.fps = esp32sveltekit.lps_all_snapshot; // 💫 read latched value, not live counter
    sharedData.connectionStatus = (uint8_t)esp32sveltekit.getConnectionStatus();
    sharedData.clientListSize = esp32sveltekit.getServer()->getClientList().size();
    sharedData.connectedClients = esp32sveltekit.getSocket()->getConnectedClients();
    sharedData.activeClients = esp32sveltekit.getSocket()->getActiveClients();
  }

  bool triggerResetPreset = false;
  int16_t pendingSyncBpm = -1;        // -1 = no pending sync; 0-255 = value to sync
  int16_t pendingSyncIntensity = -1;  // -1 = no pending sync; 0-255 = value to sync

  void syncControlToLightsControl(uint8_t nodeIndex, uint8_t controlIndex) {
    JsonObject control = _state.data["nodes"][nodeIndex]["controls"][controlIndex];
    const char* controlName = control["name"];
    if (controlName) {
      if (equal(controlName, "speed") || equal(controlName, "bpm")) {
        pendingSyncBpm = control["value"].as<uint8_t>();
      } else if (equal(controlName, "intensity")) {
        pendingSyncIntensity = control["value"].as<uint8_t>();
      }
    }
  }

  void onUpdate(const UpdatedItem& updatedItem) override {
    // delegate layer-related updates to LayerManager
    if (layerMgr.handleUpdate(updatedItem)) return;

    NodeManager::onUpdate(updatedItem);
    if (updatedItem.originId->toInt()) {  // UI triggered
      triggerResetPreset = true;
    }

    // sync effect speed/BPM/intensity back to LightsControl (UI changes and preset loads)
    // No loop risk: LightsControl's bpm/intensity handlers have toInt() guard, and
    // node->updateControl() modifies JSON directly without triggering ModuleEffects::onUpdate
    if (updatedItem.parent[1] == "controls" && updatedItem.name == "value" && updatedItem.index[1] != UINT8_MAX) {
      syncControlToLightsControl(updatedItem.index[0], updatedItem.index[1]);
    }

    // when a node name changes (new node created, e.g. preset load), sync its controls too
    // compareRecursive may not fire value changes if preset values match defaults
    if (updatedItem.parent[0] == "nodes" && updatedItem.name == "name" && updatedItem.parent[1] == "") {
      JsonArray controls = _state.data["nodes"][updatedItem.index[0]]["controls"];
      for (uint8_t j = 0; j < controls.size(); j++) {
        syncControlToLightsControl(updatedItem.index[0], j);
      }
    }
  }

};  // class ModuleEffects

#endif
#endif
