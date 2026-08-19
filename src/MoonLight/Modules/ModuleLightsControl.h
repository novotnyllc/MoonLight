/**
    @title     MoonLight
    @file      ModuleLightsControl.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonlight/lightscontrol/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef ModuleLightsControl_h
#define ModuleLightsControl_h

#include <cstddef>
#include <cstring>

inline bool isPresetLabelWhitespace(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

inline void extractPresetDisplayLabel(char (&label)[32], const char* explicitLabel, const char* nodeName) {
  label[0] = '\0';

  if (explicitLabel) {
    size_t begin = 0;
    size_t end = std::strlen(explicitLabel);
    while (begin < end && isPresetLabelWhitespace(explicitLabel[begin])) begin++;
    while (end > begin && isPresetLabelWhitespace(explicitLabel[end - 1])) end--;
    if (end > begin) {
      size_t length = end - begin;
      if (length > sizeof(label) - 1) length = sizeof(label) - 1;
      std::memcpy(label, explicitLabel + begin, length);
      label[length] = '\0';
      return;
    }
  }

  if (!nodeName) return;

  size_t length = 0;
  while (nodeName[length] && length < sizeof(label) - 1 && static_cast<unsigned char>(nodeName[length]) < 0x80) {
    label[length] = nodeName[length];
    length++;
  }
  while (length > 0 && label[length - 1] == ' ') length--;
  label[length] = '\0';
}

#if FT_MOONLIGHT


  #include <vector>

  #include "FastLED.h"
  #include "MoonBase/Module.h"
  #include "MoonBase/Modules/FileManager.h"
  #include "MoonBase/SharedFSPersistence.h"
  #include "MoonBase/utilities/MemAlloc.h"
  #include "MoonBase/Nodes.h"                // for Node::updateControl
  #include "MoonBase/utilities/PlatformFunctions.h"  //for isInPSRAM
  #include "MoonLight/Modules/DigNext2ButtonPolicy.h"
  #include "palettes.h"
  #if FT_LIVESCRIPT
    #include "MoonBase/LiveScriptNode.h"
  #endif

// Convert ModuleLightsControl state -> Home Assistant JSON
void readMQTT(ModuleState& state, JsonObject& root) {
  root["state"] = state.data["lightsOn"].as<bool>() ? "ON" : "OFF";
  root["brightness"] = state.data["brightness"].as<uint8_t>();
  String js;
  serializeJson(root, js);
  EXT_LOGD(ML_TAG, "Read HA %s", js.c_str());
}

// Convert Home Assistant JSON -> ModuleLightsControl state updates
StateUpdateResult updateMQTT(JsonObject& root, ModuleState& state, const String& originId) {
  String js;
  serializeJson(root, js);
  EXT_LOGD(ML_TAG, "Update HA %s", js.c_str());
  JsonDocument doc;
  JsonObject newState = doc.to<JsonObject>();

  if (!root["state"].isNull()) newState["lightsOn"] = (root["state"] == "ON");
  if (!root["brightness"].isNull()) newState["brightness"] = root["brightness"].as<uint8_t>();

  return ModuleState::update(newState, state, originId);
}

class ModuleLightsControl : public Module {
 private:
  MqttEndpoint<ModuleState>* _mqttEndpoint = nullptr;  // pointer, dynamically allocated
  #if FT_ENABLED(FT_MQTT)
  PsychicMqttClient* _mqttClient;
  MqttSettingsService* _mqttSettingsService;
  update_handler_id_t _mqttSettingsUpdateHandlerId = 0;  // track handler ID
  #endif
 public:
  FileManager* _fileManager;
  ModuleIO* _moduleIO;
  uint8_t pinRelayLightsOn = UINT8_MAX;
  uint8_t pinPushButtonLightsOn = UINT8_MAX;
  uint8_t pinToggleButtonLightsOn = UINT8_MAX;
  uint8_t pinDigNext2Button1 = UINT8_MAX;
  uint8_t pinDigNext2Button2 = UINT8_MAX;
  uint8_t pinPIR = UINT8_MAX;

  ModuleLightsControl(PsychicHttpServer* server, ESP32SvelteKit* sveltekit, FileManager* fileManager, ModuleIO* moduleIO)
      : Module("lightscontrol", server, sveltekit)
  #if FT_ENABLED(FT_MQTT)
        ,
        _mqttClient(sveltekit->getMqttClient()),
        _mqttSettingsService(sveltekit->getMqttSettingsService())
  #endif
  {
    EXT_LOGV(ML_TAG, "constructor");
    _fileManager = fileManager;
    _moduleIO = moduleIO;

    _moduleIO->addUpdateHandler([this](const String& originId) { readPins(); }, false);
  }

  void begin() override {
    Module::begin();

    EXT_LOGD(ML_TAG, "Lights:%d(Header:%d) L-H:%d Node:%d PL:%d(PL-L:%d) VL:%d PM:%d C3D:%d", sizeof(Lights), sizeof(LightsHeader), sizeof(Lights) - sizeof(LightsHeader), sizeof(Node), sizeof(PhysicalLayer), sizeof(PhysicalLayer) - sizeof(Lights), sizeof(VirtualLayer), sizeof(PhysMap), sizeof(Coord3D));

    EXT_LOGD(ML_TAG, "isInPSRAM: mt:%d mti:%d ch:%d", isInPSRAM(layerP.layers[0]->mappingTable), isInPSRAM(layerP.layers[0]->mappingTableIndexes.data()), isInPSRAM(layerP.lights.channelsD));

  #ifdef BOARD_HAS_PSRAM
    if (!psramFound) EXT_LOGE(ML_TAG, "Board has PSRAM but not found !!");
  #endif
    // Preset list/labels are populated in afterPersistenceLoaded() once lightscontrol.json is read.

    // update presets if files changed in presets folder
    _fileManager->addUpdateHandler([this](const String& originId) {
      EXT_LOGV(ML_TAG, "FileManager::updateHandler %s", originId.c_str());
      // read the file state (read all files and folders on FS and collect changes)
      _fileManager->read(
          [&](FilesState& filesState) {
            // loop over all changed files (normally only one)
            bool presetChanged = false;
            for (const auto& updatedItem : filesState.updatedItems) {
              // if file is the current live script, recompile it (to do: multiple live effects)
              EXT_LOGV(ML_TAG, "updateHandler updatedItem %s", updatedItem.c_str());
              if (strstr(updatedItem.c_str(), "/.config/presets")) {
                EXT_LOGV(ML_TAG, " preset.json updated -> call update %s", updatedItem.c_str());
                presetChanged = true;
              }
            }
            if (presetChanged) {
              EXT_LOGV(ML_TAG, "setPresetsFromFolder");
              setPresetsFromFolder();  // update the presets from the folder
            }
          },
          originId);
    });

  #if FT_ENABLED(FT_MQTT)
    // Register handler to react to MQTT settings changes (including enable/disable)
    if (_mqttSettingsService) {
      _mqttSettingsUpdateHandlerId = _mqttSettingsService->addUpdateHandler([this](const String& originId) { onMqttSettingsChanged(); },
                                                                            false  // don't allow removal by others
      );
      onMqttSettingsChanged();  // Initialize MQTT if enabled at boot
    }
  #endif
  }

  // Call after SharedFSPersistence::begin() so persisted lightscontrol.json does not wipe preset list.
  void afterPersistenceLoaded() { setPresetsFromFolder(); }

  #if FT_ENABLED(FT_MQTT)
  void onMqttSettingsChanged() {
    if (!_mqttSettingsService) return;

    bool shouldBeEnabled = _mqttSettingsService->isEnabled() && _mqttClient;
    bool isCurrentlyEnabled = (_mqttEndpoint != nullptr);

    // Handle enable/disable transitions
    if (shouldBeEnabled && !isCurrentlyEnabled) {
      // Transition: disabled → enabled
      EXT_LOGD(ML_TAG, "Enabling MQTT for Home Assistant");
      initializeMqtt();
    } else if (!shouldBeEnabled && isCurrentlyEnabled) {
      // Transition: enabled → disabled
      EXT_LOGD(ML_TAG, "Disabling MQTT for Home Assistant");
      cleanupMqtt();
    }
    // If both true or both false, no transition needed (already in correct state)
  }

  void initializeMqtt() {
    if (_mqttEndpoint) return;  // already initialized

    _mqttEndpoint = new MqttEndpoint<ModuleState>(readMQTT, updateMQTT, this, _mqttClient);
    _mqttClient->onConnect(std::bind(&ModuleLightsControl::registerConfig, this));

    // If already connected, register config immediately
    if (_mqttClient->connected()) {
      registerConfig();
    }
  }

  void cleanupMqtt() {
    if (_mqttEndpoint) {
      delete _mqttEndpoint;
      _mqttEndpoint = nullptr;
    }
    // Note: We can't easily remove the onConnect callback, but registerConfig checks enabled state
  }

  void registerConfig() {
    // Defense in depth: check enabled state even though we only create endpoint when enabled
    if (!_mqttSettingsService || !_mqttSettingsService->isEnabled()) {
      EXT_LOGD(ML_TAG, "MQTT not enabled, skipping HA config");
      return;
    }
    if (!_mqttClient || !_mqttClient->connected()) {
      EXT_LOGE(ML_TAG, "MQTT client not connected");
      return;
    }

    String configTopic;
    String subTopic;
    String pubTopic;

    String settingsUniqueId = _mqttSettingsService->getClientId() ? _mqttSettingsService->getClientId() : SettingValue::format("#{platform}-#{unique_id}");
    String settingsMqttPath = "homeassistant/light/" + esp32sveltekit.getSystemHostname();  // currently configured as a homeassistent light type
    String settingsName = esp32sveltekit.getSystemHostname();
    String settingsStateTopic = SettingValue::format(FACTORY_MQTT_STATUS_TOPIC);

    JsonDocument doc;
    configTopic = settingsMqttPath + "/config";
    subTopic = settingsMqttPath + "/set";
    pubTopic = settingsMqttPath + "/state";
    doc["~"] = settingsMqttPath;
    doc["name"] = "🌙💡";  // so HA displays Entity name → function of that device as light, instead of showing device name twice
    doc["unique_id"] = settingsUniqueId;

    doc["cmd_t"] = "~/set";
    doc["stat_t"] = "~/state";
    doc["schema"] = "json";
    doc["brightness"] = true;

    // Add availability topic for online/offline status
    doc["availability_topic"] = settingsStateTopic;
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";

    // Add device info for grouping in HA
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"].to<JsonArray>().add(settingsUniqueId);
    device["name"] = settingsName;
    device["manufacturer"] = "MoonModules";
    device["model"] = "MoonLight";

    _mqttSettingsService->setStatusTopic(settingsStateTopic);

    String payload;
    serializeJson(doc, payload);
    if (!_mqttClient->publish(configTopic.c_str(), 1, false, payload.c_str())) {  // QoS 1
      EXT_LOGE(ML_TAG, "Failed to publish HA discovery config");
      return;
    }

    _mqttEndpoint->configureTopics(pubTopic, subTopic);

    EXT_LOGI(ML_TAG, "Published HA discovery to %s", configTopic.c_str());
  }
  #endif

  void readPins() {
    if (safeModeMB) {
      EXT_LOGW(ML_TAG, "Safe mode enabled, not adding pins");
      return;
    }

    // get board preset info
    moduleIO.read(
        [&](ModuleState& state) {
          pinRelayLightsOn = UINT8_MAX;
          pinPushButtonLightsOn = UINT8_MAX;
          pinToggleButtonLightsOn = UINT8_MAX;
          pinDigNext2Button1 = UINT8_MAX;
          pinDigNext2Button2 = UINT8_MAX;
          pinPIR = UINT8_MAX;
          for (JsonObject pinObject : state.data["pins"].as<JsonArray>()) {
            uint8_t usage = pinObject["usage"];
            uint8_t gpio = pinObject["GPIO"];

            if (usage == pin_Relay_LightsOn) {
              if (GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
                pinRelayLightsOn = gpio;
                pinMode(pinRelayLightsOn, OUTPUT);
                uint8_t newBri = _state.data["lightsOn"] ? _state.data["brightness"] : 0;
                digitalWrite(pinRelayLightsOn, newBri > 0 ? HIGH : LOW);
                EXT_LOGD(ML_TAG, "pinRelayLightsOn found %d", pinRelayLightsOn);
              } else
                EXT_LOGE(MB_TAG, "gpio %d not valid", pinRelayLightsOn);
            } else if (usage == pin_Button_Push_LightsOn) {
              if (GPIO_IS_VALID_GPIO(gpio)) {
                pinPushButtonLightsOn = gpio;
#if CONFIG_IDF_TARGET_ESP32
                pinMode(pinPushButtonLightsOn, gpio >= 34 && gpio <= 39 ? INPUT : INPUT_PULLUP);
#else
                pinMode(pinPushButtonLightsOn, INPUT_PULLUP);
#endif
                EXT_LOGD(ML_TAG, "pinPushButtonLightsOn found %d", pinPushButtonLightsOn);
              } else
                EXT_LOGE(MB_TAG, "gpio %d not valid", pinPushButtonLightsOn);
            } else if (usage == pin_Button_Toggle_LightsOn) {
              if (GPIO_IS_VALID_GPIO(gpio)) {
                pinToggleButtonLightsOn = gpio;
                pinMode(pinToggleButtonLightsOn, INPUT_PULLUP);
                EXT_LOGD(ML_TAG, "pinToggleButtonLightsOn found %d", pinToggleButtonLightsOn);
              } else
                EXT_LOGE(MB_TAG, "gpio %d not valid", pinToggleButtonLightsOn);
            } else if (usage == pin_DigNext2_Button1) {
              if (GPIO_IS_VALID_GPIO(gpio)) {
                pinDigNext2Button1 = gpio;
                pinMode(pinDigNext2Button1, INPUT);
                EXT_LOGD(ML_TAG, "pinDigNext2Button1 found %d", pinDigNext2Button1);
              } else
                EXT_LOGE(MB_TAG, "gpio %d not valid", pinDigNext2Button1);
            } else if (usage == pin_DigNext2_Button2) {
              if (GPIO_IS_VALID_GPIO(gpio)) {
                pinDigNext2Button2 = gpio;
                pinMode(pinDigNext2Button2, INPUT);
                EXT_LOGD(ML_TAG, "pinDigNext2Button2 found %d", pinDigNext2Button2);
              } else
                EXT_LOGE(MB_TAG, "gpio %d not valid", pinDigNext2Button2);
            } else if (usage == pin_PIR) {
              if (GPIO_IS_VALID_GPIO(gpio)) {
                pinPIR = gpio;
                pinMode(pinPIR, INPUT);
                EXT_LOGD(ML_TAG, "pinPIR found %d", pinPIR);
              } else
                EXT_LOGE(MB_TAG, "gpio %d not valid", pinPIR);
            }
          }
          // for (int i = 0; i < sizeof(pins); i++) EXT_LOGD(ML_TAG, "pin %d = %d", i, pins[i]);
        },
        _moduleName);
  }

  // define the data model
  void setupDefinition(const JsonArray& controls) override {
    EXT_LOGV(ML_TAG, "");
    JsonObject control;  // state.data has one or more properties

    control = addControl(controls, "lightsOn", "checkbox");
    control["default"] = true;
    control = addControl(controls, "bootLightsOn", "checkbox");
    control["default"] = true;
    control = addControl(controls, "bootBrightness", "slider", 1, 255, false, "At power-on");
    control["default"] = 128;
    control = addControl(controls, "brightness", "slider");
    control["default"] = 20;
    control = addControl(controls, "red", "slider");
    control["default"] = 255;
    control["color"] = "Red";
    control = addControl(controls, "green", "slider");
    control["default"] = 255;
    control["color"] = "Green";
    control = addControl(controls, "blue", "slider");
    control["default"] = 255;
    control["color"] = "Blue";
    control = addControl(controls, "palette", "palette");  // palette type
    control["default"] = 8;

    control["values"].to<JsonArray>();

    // add palettes from palettes.h
    for (int i = 0; i < sizeof(palette_names) / sizeof(char*); i++) {
      JsonArray values = control["values"];
      JsonObject object = values.add<JsonObject>();
      object["name"] = palette_names[i];
      object["colors"] = getPaletteHexString(i);
      const char* n = palette_names[i];
      if (strstr(n, "⚡️"))
        object["category"] = "FastLED";
      else if (strstr(n, "🌙"))
        object["category"] = "MoonModules";
      else if (strstr(n, "💫"))
        object["category"] = "MoonLight";
      else
        object["category"] = "WLED";
    }

  #if FT_LIVESCRIPT
    // find palette live scripts (P_*.sc files) on FS
    {
      File rootFolder = ESPFS.open("/livescripts");
      walkThroughFiles(rootFolder, [&](File folder, File file) {
        const char* fname = file.name();
        size_t len = strlen(fname);
        bool isSc = (len >= 3) && strcmp(fname + (len - 3), ".sc") == 0;
        if (isSc && strncmp(fname, "P_", 2) == 0) {
          JsonObject entry = control["values"].as<JsonArray>().add<JsonObject>();
          entry["name"] = (const char*)file.path();
          entry["category"] = "LiveScript";
        }
      });
      rootFolder.close();
    }
  #endif

    control = addControl(controls, "bpm", "slider");
    control["default"] = 60;

    control = addControl(controls, "intensity", "slider");
    control["default"] = 128;

    control = addControl(controls, "preset", "preset");
    control["width"] = 8;
    control["size"] = 18;
    control["wrap"] = true;
    control["default"].to<JsonObject>();  // clear the preset array before adding new presets
    control["default"]["list"].to<JsonArray>();
    control["default"]["labels"].to<JsonArray>();
    control["default"]["count"] = 64;

    control = addControl(controls, "presetLoop", "slider");
    control["default"] = 0;
    control = addControl(controls, "firstPreset", "slider", 1, 64);
    control["default"] = 1;
    control = addControl(controls, "lastPreset", "slider", 1, 64);
    control["default"] = 20;

  #if FT_ENABLED(FT_MONITOR)
    control = addControl(controls, "monitorOn", "checkbox");
    control["default"] = true;
  #endif
  }

  // implement business logic
  void onUpdate(const UpdatedItem& updatedItem) override {
    // EXT_LOGD(ML_TAG, "handle %s[%d]%s[%d].%s = %s -> %s", updatedItem.parent[0].c_str(), updatedItem.index[0], updatedItem.parent[1].c_str(), updatedItem.index[1], updatedItem.name.c_str(), updatedItem.oldValue.c_str(), updatedItem.value.as<String>().c_str());
    if (updatedItem.name == "red") {
      layerP.lights.header.red = _state.data["red"];
    } else if (updatedItem.name == "green") {
      layerP.lights.header.green = _state.data["green"];
    } else if (updatedItem.name == "blue") {
      layerP.lights.header.blue = _state.data["blue"];
    } else if (updatedItem.name == "lightsOn" || updatedItem.name == "brightness") {
      uint8_t newBri = _state.data["lightsOn"] ? _state.data["brightness"] : 0;
      if (!!layerP.lights.header.brightness != !!newBri && pinRelayLightsOn != UINT8_MAX) {  // !! is intentional!
        EXT_LOGD(ML_TAG, "pinRelayLightsOn %s", !!newBri ? "On" : "Off");
        digitalWrite(pinRelayLightsOn, newBri > 0 ? HIGH : LOW);
      };
      layerP.lights.header.brightness = newBri;
    } else if (updatedItem.name == "palette") {
      #if FT_LIVESCRIPT
      if (paletteNode) {
        delete paletteNode;
        paletteNode = nullptr;
      }
      #endif

      uint8_t index = updatedItem.value;
      uint8_t nrOfHardcodedPalettes = sizeof(palette_names) / sizeof(palette_names[0]);

      if (index < nrOfHardcodedPalettes) {
        layerP.palette = getGradientPalette(index);
      }
      #if FT_LIVESCRIPT
      else {
        // LiveScript palette — find the P_ script by index offset
        uint8_t palScriptIndex = index - nrOfHardcodedPalettes;
        uint8_t count = 0;
        File rootFolder = ESPFS.open("/livescripts");
        walkThroughFiles(rootFolder, [&](File folder, File file) {
          const char* fname = file.name();
          size_t len = strlen(fname);
          bool isSc = (len >= 3) && strcmp(fname + (len - 3), ".sc") == 0;
          if (isSc && strncmp(fname, "P_", 2) == 0) {
            if (count == palScriptIndex) {
              EXT_LOGI(ML_TAG, "Palette LiveScript: %s", file.path());
              paletteNode = allocMBObject<LiveScriptNode>();
              paletteNode->animation = file.path();
              paletteNode->constructor(layerP.layers[0], JsonArray(), &layerP.driversMutex);
              paletteNode->setup();
            }
            count++;
          }
        });
        rootFolder.close();
      }
      #endif
    } else if (updatedItem.name == "bpm") {
      if (updatedItem.originId->toInt()) {  // only propagate UI-initiated changes to nodes
        uint8_t bpm = _state.data["bpm"];
        for (auto* node : layerP.layers[0]->nodes) {
          if (node && node->on) {
            node->updateControl("speed", bpm);
            node->updateControl("bpm", bpm);
          }
        }
      }
    } else if (updatedItem.name == "intensity") {
      if (updatedItem.originId->toInt()) {  // only propagate UI-initiated changes to nodes
        uint8_t intensity = _state.data["intensity"];
        for (auto* node : layerP.layers[0]->nodes) {
          if (node && node->on) {
            node->updateControl("intensity", intensity);
          }
        }
      }
    } else if (updatedItem.name == "preset") {
      // copy /.config/effects.json to the hidden folder /.config/presets/preset[x].json
      // do not set preset at boot...
      if (updatedItem.oldValue != "" && !updatedItem.value["action"].isNull()) {
        uint16_t select = updatedItem.value["select"];
        Char<32> presetFile;
        presetFile.format("/.config/presets/preset%02d.json", select);

        if (updatedItem.value["action"] == "click") {
          updatedItem.value["selected"] = select;  // store the selected preset
          if (select != 255) {
            // ponytail: coalesce one pending copy; latest click wins if another arrives before loop20ms
            pendingPresetCopy = true;
            pendingPresetCopyToEffects = arrayContainsValue(updatedItem.value["list"], select);
            pendingPresetSelect = select;
            pendingPresetOrigin = updatedItem.originId ? *updatedItem.originId : String(_moduleName);
            if (!pendingPresetCopyToEffects) cacheLiveEffectsAsPreset(select);
          }
        } else if (updatedItem.value["action"] == "dblclick") {
          ESPFS.remove(presetFile.c_str());
          setPresetsFromFolder();  // update presets in UI
        }
        // Clear transient action/select fields after processing to prevent stale UI echoes.
        // The UI sends the full state on every change (e.g. slider drag), which may include
        // old action/select values that would re-trigger preset operations.
        _state.data["preset"].remove("action");
        _state.data["preset"].remove("select");
        // A click/POST can replace the whole preset object and drop labels.
        if (_state.data["preset"]["labels"].isNull() || !_state.data["preset"]["labels"].size()) {
          refreshBuiltinPresetLabels();
        }
      }
    }
  }

  // update _state.data["preset"]["list"] and send update to endpoints
  struct CachedPreset {
    uint16_t select = 255;
    char label[32] = "";
    char* json = nullptr;
    size_t jsonLen = 0;
  };
  std::vector<CachedPreset, VectorRAMAllocator<CachedPreset>> cachedPresets;

  void clearCachedPresets() {
    for (CachedPreset& cached : cachedPresets) freeMB(cached.json, "presetCache");
    cachedPresets.clear();
  }

  static constexpr size_t kPresetDmaMinBytes = 8192;

  bool dmaCanOpenFile() const {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= kPresetDmaMinBytes;
  }

  bool cachePresetFile(uint16_t select, File& file, char (&label)[32]) {
    CachedPreset cached;
    cached.select = select;
    std::memcpy(cached.label, label, sizeof(cached.label));
    const size_t size = file.size();
    if (!size || size > 12288) return false;
    cached.json = allocMB<char>(size + 1, "presetCache");
    if (!cached.json) return false;
    file.seek(0);
    const size_t got = file.read(reinterpret_cast<uint8_t*>(cached.json), size);
    cached.json[got] = '\0';
    cached.jsonLen = got;
    cachedPresets.push_back(cached);
    return true;
  }

  bool applyCachedPreset(uint16_t select, const String& originId) {
    for (const CachedPreset& cached : cachedPresets) {
      if (cached.select != select || !cached.json) continue;
      JsonDocument doc(JsonRAMAllocator::instance());
      if (deserializeJson(doc, cached.json, cached.jsonLen) || !doc.is<JsonObject>()) return false;
      applyEffectsObject(doc.as<JsonObject>(), originId);
      return true;
    }
    return false;
  }

  void cacheLiveEffectsAsPreset(uint16_t select) {
    extern std::vector<Module*> modules;
    Module* effects = nullptr;
    for (Module* module : modules) {
      if (module && strcmp(module->_moduleName, "effects") == 0) {
        effects = module;
        break;
      }
    }
    if (!effects) return;
    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject root = doc.to<JsonObject>();
    effects->read(root, ModuleState::read, effects->_moduleName);
    const size_t needed = measureJson(root);
    if (!needed || needed > 12288) return;
    char* json = allocMB<char>(needed + 1, "presetCache");
    if (!json) return;
    const size_t wrote = serializeJson(root, json, needed + 1);
    json[wrote] = '\0';
    char label[32] = "";
    const char* explicitLabel = root["label"].as<const char*>();
    const char* nodeName = nullptr;
    if (root["nodes"].is<JsonArray>() && root["nodes"].size()) nodeName = root["nodes"][0]["name"].as<const char*>();
    extractPresetDisplayLabel(label, explicitLabel, nodeName);
    if (!label[0]) snprintf(label, sizeof(label), "Preset %02u", select);
    for (CachedPreset& cached : cachedPresets) {
      if (cached.select == select) {
        freeMB(cached.json, "presetCache");
        cached.json = json;
        cached.jsonLen = wrote;
        std::memcpy(cached.label, label, sizeof(cached.label));
        return;
      }
    }
    CachedPreset cached;
    cached.select = select;
    std::memcpy(cached.label, label, sizeof(cached.label));
    cached.json = json;
    cached.jsonLen = wrote;
    cachedPresets.push_back(cached);
    JsonArray list = _state.data["preset"]["list"];
    JsonArray labels = _state.data["preset"]["labels"];
    bool found = false;
    for (size_t i = 0; i < list.size(); i++) {
      if ((list[i] | 0) == select) {
        if (i < labels.size()) labels[i] = cached.label;
        found = true;
        break;
      }
    }
    if (!found) {
      list.add(select);
      labels.add(cached.label);
    }
  }

  bool writePresetSlotFromCache(uint16_t select) {
    for (const CachedPreset& cached : cachedPresets) {
      if (cached.select != select || !cached.json || !cached.jsonLen) continue;
      if (!ESPFS.exists("/.config/presets")) ESPFS.mkdir("/.config/presets");
      Char<32> presetPath;
      presetPath.format("/.config/presets/preset%02d.json", select);
      if (SharedFSPersistence::writeJsonPath(presetPath.c_str(), cached.json, cached.jsonLen)) {
        EXT_LOGI(ML_TAG, "Wrote preset slot %u (%u bytes)", select, static_cast<unsigned>(cached.jsonLen));
        return true;
      }
      EXT_LOGW(ML_TAG, "Failed to write preset slot %u", select);
      return false;
    }
    EXT_LOGW(ML_TAG, "Preset %u not in RAM cache; cannot write slot file", select);
    return false;
  }

  static constexpr uint8_t kBuiltinVestPresetCount = 20;

  static const char* builtinVestPresetLabel(uint8_t seq) {
    static const char* kVestPresetLabels[kBuiltinVestPresetCount] = {
      "Horizon Ring", "Crossing Spiral", "Particle Sphere", "Star Wave",
      "Wraparound Racers", "Ripple Stars", "Audio Paintbrush", "Audio GEQ",
      "Bass Rings", "Freq Wave", "Meteor Rain", "Camp Fire",
      "Noise Pulse", "Camp Pulse", "DJ Strobe", "Bass Puddles",
      "White Out", "Twinkle Night", "Grav Meter", "Rainbow Walk"};
    if (seq < 1 || seq > kBuiltinVestPresetCount) return "";
    return kVestPresetLabels[seq - 1];
  }

  static bool isStaleBuiltinPresetLabel(const char* current) {
    return !current || !current[0] || !std::strcmp(current, "Radar") || !std::strcmp(current, "Lines");
  }

  bool refreshBuiltinPresetLabels() {
    JsonArray list = _state.data["preset"]["list"];
    JsonArray labels = _state.data["preset"]["labels"];
    bool changed = false;
    if (!list.size()) {
      for (uint8_t seq = 1; seq <= kBuiltinVestPresetCount; seq++) {
        list.add(seq);
        labels.add(builtinVestPresetLabel(seq));
      }
      changed = true;
    } else {
      while (labels.size() < list.size()) {
        labels.add("");
        changed = true;
      }
      for (size_t i = 0; i < list.size() && i < labels.size(); i++) {
        int seq = list[i] | 0;
        const char* current = labels[i].as<const char*>();
        if (seq >= 1 && seq <= static_cast<int>(kBuiltinVestPresetCount) && isStaleBuiltinPresetLabel(current)) {
          labels[i] = builtinVestPresetLabel(static_cast<uint8_t>(seq));
          changed = true;
        }
      }
      for (uint8_t seq = 1; seq <= kBuiltinVestPresetCount; seq++) {
        bool found = false;
        for (size_t i = 0; i < list.size(); i++) {
          if ((list[i] | 0) == seq) {
            found = true;
            break;
          }
        }
        if (!found) {
          list.add(seq);
          labels.add(builtinVestPresetLabel(seq));
          changed = true;
        }
      }
    }
    if (changed) {
      update([&](ModuleState& state) { return StateUpdateResult::CHANGED; }, _moduleName);
    }
    return changed;
  }

  void setPresetsFromFolder() {
    const bool canScan = dmaCanOpenFile();
    if (!canScan) {
      EXT_LOGW(ML_TAG, "Preset folder scan skipped (largest internal %u < %u); keeping RAM cache",
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(kPresetDmaMinBytes));
      refreshBuiltinPresetLabels();
      return;
    }

    File rootFolder = ESPFS.open("/.config/presets/");
    const bool hadPresets = _state.data["preset"]["list"].size() || _state.data["preset"]["labels"].size();
    _state.data["preset"]["list"].clear();
    _state.data["preset"]["labels"].clear();
    clearCachedPresets();
    bool changed = hadPresets;
    walkThroughFiles(rootFolder, [&](File folder, File file) {
      int seq = -1;
      if (sscanf(file.name(), "preset%02d.json", &seq) != 1) return;
      _state.data["preset"]["list"].add(seq);
      char label[32] = "";
      file.seek(0);
      JsonDocument doc(JsonRAMAllocator::instance());
      if (!deserializeJson(doc, file)) {
        JsonArray nodes = doc["nodes"];
        const char* nodeName = nodes.size() > 0 ? nodes[0]["name"].as<const char*>() : nullptr;
        extractPresetDisplayLabel(label, doc["label"].as<const char*>(), nodeName);
        cachePresetFile(seq, file, label);
      }
      _state.data["preset"]["labels"].add((const char*)label);
      changed = true;
    });
    if (refreshBuiltinPresetLabels()) changed = true;
    if (changed) {
      update([&](ModuleState& state) { return StateUpdateResult::CHANGED; }, _moduleName);
    }
  }

  #if FT_LIVESCRIPT
  LiveScriptNode* paletteNode = nullptr;
  #endif

  int selectDigNext2Preset(bool backwards) {
    JsonArray presetList = _state.data["preset"]["list"];
    int selected = _state.data["preset"]["selected"] | 255;
    int firstPreset = _state.data["firstPreset"] | 1;
    int lastPreset = _state.data["lastPreset"] | 64;
    return chooseDigNext2Preset(
        presetList.size(), [&](size_t index) { return presetList[index].as<int>(); }, selected, firstPreset, lastPreset, backwards);
  }

  void forceWearablePowerOn() {
    // Empty effect layers skip virtual mapping, so the compiled White Vest 95 layout stays unused.
    if (_state.data["bootLightsOn"] | true) {
      JsonDocument powerDoc;
      JsonObject power = powerDoc.to<JsonObject>();
      power["lightsOn"] = true;
      power["brightness"] = (uint8_t)(_state.data["bootBrightness"] | 128);
      update(power, ModuleState::update, String("1"));  // numeric origin persists lightsOn + boot brightness
    }

    bool hasEffect = false;
    for (VirtualLayer* layer : layerP.layers) {
      if (layer && !layer->nodes.empty()) {
        hasEffect = true;
        break;
      }
    }
    if (!hasEffect) {
      extern std::vector<Module*> modules;
      JsonDocument fxDoc;
      JsonObject fx = fxDoc.to<JsonObject>();
      fx["layer"] = 0;
      fx["brightness"] = 220;
      fx["label"] = "Wraparound Racers";
      JsonObject node = fx["nodes"].to<JsonArray>().add<JsonObject>();
      node["name"] = "Wraparound Racers";
      node["on"] = true;
      for (Module* module : modules) {
        if (module && strcmp(module->_moduleName, "effects") == 0) {
          module->update(fx, ModuleState::update, String("1"));
          break;
        }
      }
    }
    setPresetsFromFolder();
    layerP.requestMapPhysical.store(true);
    layerP.requestMapVirtual.store(true);
  }

  void applyEffectsObject(JsonObject obj, const String& originId) {
    extern std::vector<Module*> modules;
    for (Module* module : modules) {
      if (module && strcmp(module->_moduleName, "effects") == 0) {
        module->updateWithoutPropagation(obj, ModuleState::update, originId);
        module->update([](ModuleState&) { return StateUpdateResult::CHANGED; }, originId);
        break;
      }
    }
  }

  bool applyBuiltInVestPreset(uint16_t select, const String& originId) {
    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject fx = doc.to<JsonObject>();
    fx["layer"] = 0;
    fx["brightness"] = 220;
    fx["start"]["x"] = 0; fx["start"]["y"] = 0; fx["start"]["z"] = 0;
    fx["end"]["x"] = 100; fx["end"]["y"] = 100; fx["end"]["z"] = 100;
    JsonObject node = fx["nodes"].to<JsonArray>().add<JsonObject>();
    node["on"] = true;
    const char* name = nullptr;
    switch (select) {
    case 1: name = "Horizon Ring"; node["controls"][0]["name"] = "bpm"; node["controls"][0]["value"] = 20; node["controls"][1]["name"] = "fade"; node["controls"][1]["value"] = 64; node["controls"][2]["name"] = "thickness"; node["controls"][2]["value"] = 2; break;
    case 2: name = "Crossing Spiral"; node["controls"][0]["name"] = "bpm"; node["controls"][0]["value"] = 32; node["controls"][1]["name"] = "fade"; node["controls"][1]["value"] = 34; node["controls"][2]["name"] = "turns"; node["controls"][2]["value"] = 2; node["controls"][3]["name"] = "width"; node["controls"][3]["value"] = 40; break;
    case 3: name = "Sphere Move"; fx["label"] = "Particle Sphere"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 46; break;
    case 4: name = "Star Sky"; fx["layer"] = 1; fx["label"] = "Star Wave"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 4; node["controls"][1]["name"] = "star fill"; node["controls"][1]["value"] = 180; node["controls"][2]["name"] = "usePalette"; node["controls"][2]["value"] = true; break;
    case 5: name = "Wraparound Racers"; node["controls"][0]["name"] = "bpm"; node["controls"][0]["value"] = 58; node["controls"][1]["name"] = "fade"; node["controls"][1]["value"] = 38; node["controls"][2]["name"] = "racers"; node["controls"][2]["value"] = 6; node["controls"][3]["name"] = "trail"; node["controls"][3]["value"] = 52; break;
    case 6: name = "Ripples"; fx["layer"] = 1; fx["label"] = "Ripple Stars"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 50; node["controls"][1]["name"] = "interval"; node["controls"][1]["value"] = 128; break;
    case 7: name = "Paintbrush"; fx["layer"] = 1; fx["label"] = "Audio Paintbrush"; break;
    case 8: name = "GEQ 3D"; fx["label"] = "Audio GEQ"; break;
    case 9: name = "Audio Rings"; fx["label"] = "Bass Rings"; node["controls"][0]["name"] = "inWards"; node["controls"][0]["value"] = true; break;
    case 10: name = "Freq Wave"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 140; break;
    case 11: name = "Meteor"; fx["label"] = "Meteor Rain"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 128; node["controls"][1]["name"] = "trail"; node["controls"][1]["value"] = 160; break;
    case 12: name = "Fire"; fx["layer"] = 1; fx["label"] = "Camp Fire"; node["controls"][0]["name"] = "usePalette"; node["controls"][0]["value"] = true; node["controls"][1]["name"] = "flareDecay"; node["controls"][1]["value"] = 14; break;
    case 13: name = "Noise Meter"; fx["label"] = "Noise Pulse"; node["controls"][0]["name"] = "fadeRate"; node["controls"][0]["value"] = 248; node["controls"][1]["name"] = "width"; node["controls"][1]["value"] = 180; break;
    case 14: name = "Heartbeat"; fx["label"] = "Camp Pulse"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 18; node["controls"][1]["name"] = "intensity"; node["controls"][1]["value"] = 160; break;
    case 15: name = "DJ Light"; fx["label"] = "DJ Strobe"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 220; node["controls"][1]["name"] = "candyFactory"; node["controls"][1]["value"] = true; node["controls"][2]["name"] = "fade"; node["controls"][2]["value"] = 4; break;
    case 16: name = "Puddle Peak"; fx["label"] = "Bass Puddles"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 128; node["controls"][1]["name"] = "intensity"; node["controls"][1]["value"] = 180; break;
    case 17: name = "Solid"; fx["label"] = "White Out"; node["controls"][0]["name"] = "colorMode"; node["controls"][0]["value"] = 0; node["controls"][1]["name"] = "red"; node["controls"][1]["value"] = 255; node["controls"][2]["name"] = "green"; node["controls"][2]["value"] = 200; node["controls"][3]["name"] = "blue"; node["controls"][3]["value"] = 180; node["controls"][4]["name"] = "brightness"; node["controls"][4]["value"] = 220; break;
    case 18: name = "Color Twinkle"; fx["label"] = "Twinkle Night"; node["controls"][0]["name"] = "fadeSpeed"; node["controls"][0]["value"] = 140; node["controls"][1]["name"] = "spawnSpeed"; node["controls"][1]["value"] = 120; break;
    case 19: name = "Gravimeter"; fx["label"] = "Grav Meter"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 128; node["controls"][1]["name"] = "intensity"; node["controls"][1]["value"] = 160; break;
    case 20: name = "Rainbow"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 10; node["controls"][1]["name"] = "deltaHue"; node["controls"][1]["value"] = 7; node["controls"][2]["name"] = "usePalette"; node["controls"][2]["value"] = true; break;
    default: return false;
    }
    node["name"] = name;
    if (fx["label"].isNull()) fx["label"] = name;
    applyEffectsObject(fx, originId);
    return true;
  }

  void requestPreset(int select) {
    if (select < 0) return;

    JsonDocument doc;
    JsonObject newState = doc.to<JsonObject>();
    newState["preset"] = _state.data["preset"];
    newState["preset"]["action"] = "click";
    newState["preset"]["select"] = select;
    update(newState, ModuleState::update, _moduleName);
  }

  void stepDigNext2Brightness(int delta) {
    int brightness = _state.data["brightness"] | 128;
    brightness += delta;
    if (brightness < 1) brightness = 1;
    if (brightness > 255) brightness = 255;
    JsonDocument doc;
    JsonObject newState = doc.to<JsonObject>();
    newState["brightness"] = static_cast<uint8_t>(brightness);
    update(newState, ModuleState::update, _moduleName);
  }

  void toggleDigNext2Power() {
    JsonDocument doc;
    JsonObject newState = doc.to<JsonObject>();
    newState["lightsOn"] = !_state.data["lightsOn"].as<bool>();
    update(newState, ModuleState::update, _moduleName);
  }

  void handleDigNext2ButtonAction(DigNext2ButtonAction action) {
    switch (action) {
    case DigNext2ButtonAction::NextPreset:
      requestPreset(selectDigNext2Preset(false));
      break;
    case DigNext2ButtonAction::PreviousPreset:
      requestPreset(selectDigNext2Preset(true));
      break;
    case DigNext2ButtonAction::BrightnessStepUp:
      stepDigNext2Brightness(8);
      break;
    case DigNext2ButtonAction::BrightnessStepDown:
      stepDigNext2Brightness(-8);
      break;
    case DigNext2ButtonAction::TogglePower:
      toggleDigNext2Power();
      break;
    case DigNext2ButtonAction::None:
      break;
    }
  }

  void pollDigNext2Buttons(uint32_t now) {
    if (pinDigNext2Button1 == UINT8_MAX && pinDigNext2Button2 == UINT8_MAX) return;
    DigNext2ButtonInputs inputs;
    inputs.nowMs = now;
    inputs.lightsOn = _state.data["lightsOn"] | false;
    if (pinDigNext2Button1 != UINT8_MAX) inputs.button1 = digitalRead(pinDigNext2Button1) == LOW;
    if (pinDigNext2Button2 != UINT8_MAX) inputs.button2 = digitalRead(pinDigNext2Button2) == LOW;
    handleDigNext2ButtonAction(updateDigNext2ButtonsPolicy(digNext2Buttons, inputs));
  }

  unsigned long lastPresetTime = 0;
  // see pinPushButtonLightsOn
  static constexpr unsigned long debounceDelay = 50;  // 50ms debounce
  bool pendingPresetCopy = false;
  bool pendingPresetCopyToEffects = false;
  uint16_t pendingPresetSelect = 255;
  String pendingPresetOrigin;
  unsigned long lastPushDebounceTime = 0;
  unsigned long lastToggleDebounceTime = 0;
  unsigned long lastPIRDebounceTime = 0;
  int lastPushPinState = HIGH;
  int lastTogglePinState = HIGH;
  int lastPIRPinState = LOW;
  DigNext2ButtonRuntime digNext2Buttons;
#if FT_ENABLED(FT_MONITOR)
  LightsHeader lastMonitorHeader{};
  std::vector<uint8_t, VectorRAMAllocator<uint8_t>> lastMonitorPositions;
  bool lastMonitorReady = false;

 public:
  void emitStoredMonitorLayout() {
    if (!lastMonitorReady) return;
    _sveltekit->getSocket()->emitEvent("monitor", reinterpret_cast<char*>(&lastMonitorHeader), 47, _moduleName);
    if (!lastMonitorPositions.empty()) {
      _sveltekit->getSocket()->emitEvent("monitor", reinterpret_cast<char*>(lastMonitorPositions.data()), lastMonitorPositions.size(), _moduleName);
    }
  }

  void storeMonitorLayout(const LightsHeader& header, const uint8_t* positions, size_t positionBytes) {
    lastMonitorHeader = header;
    lastMonitorPositions.assign(positions, positions + positionBytes);
    lastMonitorReady = header.nrOfLights > 0 && positionBytes == static_cast<size_t>(header.nrOfLights) * 3;
  }
#endif

  void loop20ms() override {
    Module::loop20ms();  // requestUIUpdate

    if (pendingPresetCopy) {
      pendingPresetCopy = false;
      Char<32> presetFile;
      presetFile.format("/.config/presets/preset%02d.json", pendingPresetSelect);
      const String originId = pendingPresetOrigin;
      if (pendingPresetCopyToEffects) {
        if (!applyCachedPreset(pendingPresetSelect, originId) && !applyBuiltInVestPreset(pendingPresetSelect, originId)) {
          EXT_LOGE(ML_TAG, "Preset %u not in RAM cache; skipped fopen", pendingPresetSelect);
        }
      } else if (!writePresetSlotFromCache(pendingPresetSelect)) {
        EXT_LOGW(ML_TAG, "Preset save to %s failed", presetFile.c_str());
      }
    }

    // process presetLoop
    uint8_t presetLoop = _state.data["presetLoop"];
    if (presetLoop && millis() - lastPresetTime > presetLoop * 1000) {  // every presetLoop seconds
      lastPresetTime = millis();

      // bugfix;
      //  runInAppTask.push_back([&]() {
      //  load the xth preset from FS
      requestPreset(selectDigNext2Preset(false));
    }

    pollDigNext2Buttons(millis());

    if (pinPushButtonLightsOn != UINT8_MAX) {
      if ((millis() - lastPushDebounceTime) > debounceDelay) {
        lastPushDebounceTime = millis();
        int state = digitalRead(pinPushButtonLightsOn);
        if (state != lastPushPinState) {
          lastPushPinState = state;
          // Trigger only on button press (HIGH to LOW transition for INPUT_PULLUP)
          if (state == LOW) {
            JsonDocument doc;
            JsonObject newState = doc.to<JsonObject>();
            newState["lightsOn"] = !_state.data["lightsOn"];
            update(newState, ModuleState::update, _moduleName);
          }
        }
      }
    }

    if (pinToggleButtonLightsOn != UINT8_MAX) {
      if (((millis() - lastToggleDebounceTime) > debounceDelay)) {
        lastToggleDebounceTime = millis();
        int state = digitalRead(pinToggleButtonLightsOn);
        if (state != lastTogglePinState) {
          lastTogglePinState = state;
          JsonDocument doc;
          JsonObject newState = doc.to<JsonObject>();
          newState["lightsOn"] = !_state.data["lightsOn"];
          update(newState, ModuleState::update, _moduleName);
        }
      }
    }

    if (pinPIR != UINT8_MAX) {
      if ((millis() - lastPIRDebounceTime) > debounceDelay) {
        lastPIRDebounceTime = millis();
        int state = digitalRead(pinPIR);
        // EXT_LOGD(ML_TAG, "PIR  %d -> %d", lastPIRPinState, state);
        if (state != lastPIRPinState) {
          EXT_LOGD(ML_TAG, "PIR toggle %d -> %d", lastPIRPinState, state);
          lastPIRPinState = state;
          JsonDocument doc;
          JsonObject newState = doc.to<JsonObject>();
          newState["lightsOn"] = state == HIGH;
          update(newState, ModuleState::update, _moduleName);
        }
      }
    }

  #define headerPrimeNumber 47  // prime number so nrOfChannels is not likely to be the same so monitor can recognize a header

  #if FT_ENABLED(FT_MONITOR)
    extern SemaphoreHandle_t swapMutex;
    static std::vector<uint8_t, VectorRAMAllocator<uint8_t>> monitorSnapshot;
    static LightsHeader monitorHeaderSnapshot;
    bool emitMonitorHeader = false;
    bool emitMonitorData = false;
    const bool monitorOn = _state.data["monitorOn"];
    // The header is one-shot. Do not wait for client_info/visibility: the in-app
    // browser can stay document.hidden, and subscribe can land after /rest/monitorLayout.
    // emitEvent already no-ops when nobody is subscribed.
    const bool streamMonitor = monitorOn && _sveltekit->getSocket()->getConnectedClients();

    {
      LayerMappingReadGuard monitorGuard(layerP.mappingMutex);
      xSemaphoreTake(swapMutex, portMAX_DELAY);
      uint8_t isPositions = layerP.lights.header.isPositions;

      if (isPositions == 2) {
        if (layerP.lights.channelsD) {
          size_t positionBytes = layerP.lights.header.nrOfLights * 3;
          if (monitorOn) {
            monitorHeaderSnapshot = layerP.lights.header;
            monitorSnapshot.resize(positionBytes);
            memcpy(monitorSnapshot.data(), layerP.lights.channelsD, positionBytes);
            storeMonitorLayout(monitorHeaderSnapshot, monitorSnapshot.data(), positionBytes);
            emitMonitorHeader = true;
            emitMonitorData = true;
          }
          memset(layerP.lights.channelsD, 0, positionBytes);
        }
        EXT_LOGD(ML_TAG, "positions copied for monitor (2 -> 3)");
        layerP.lights.header.isPositions = 3;
      } else if (isPositions == 0 && layerP.lights.header.nrOfLights) {
        static unsigned long monitorMillis = 0;
        if (millis() - monitorMillis >= MAX(20, layerP.lights.header.nrOfLights / 300)) {
          monitorMillis = millis();
          if (layerP.lights.channelsD && streamMonitor) {
            size_t channelBytes = layerP.lights.header.nrOfChannels;
            monitorSnapshot.resize(channelBytes);
            memcpy(monitorSnapshot.data(), layerP.lights.channelsD, channelBytes);
            emitMonitorData = true;
          }
        }
      }
      xSemaphoreGive(swapMutex);
    }

    if (emitMonitorHeader) {
      static_assert(sizeof(LightsHeader) > headerPrimeNumber, "LightsHeader size nog large enough for Monitor protocol");
      _sveltekit->getSocket()->emitEvent("monitor", (char*)&monitorHeaderSnapshot, headerPrimeNumber, _moduleName);
    }
    if (emitMonitorData) {
      _sveltekit->getSocket()->emitEvent("monitor", (char*)monitorSnapshot.data(), monitorSnapshot.size(), _moduleName);
    }
  #endif
  }

};  // class ModuleLightsControl

#endif
#endif
