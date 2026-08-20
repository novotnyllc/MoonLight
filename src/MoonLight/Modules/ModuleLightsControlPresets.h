#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

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

inline bool parsePresetJsonBasename(const char* name, uint16_t& sequence) {
  if (!name || std::strlen(name) != 13 || std::strncmp(name, "preset", 6) != 0 ||
      name[6] < '0' || name[6] > '9' || name[7] < '0' || name[7] > '9' ||
      std::strcmp(name + 8, ".json") != 0) {
    return false;
  }
  sequence = static_cast<uint16_t>((name[6] - '0') * 10 + (name[7] - '0'));
  return true;
}

inline const char* builtinVestPresetLabel(uint8_t sequence) {
  static const char* labels[] = {
      "💫 Horizon Ring",      "🌀 Crossing Spiral", "✨ Particle Sphere", "⭐ Star Wave",
      "💨 Wraparound Racers", "🌊 Ripple Stars",    "🎵 Audio Paintbrush", "🎵 Audio GEQ",
      "🎵 Bass Rings",        "🎵 Freq Wave",       "🎵 Noise Pulse",      "🎵 Camp Pulse",
      "🎵 DJ Strobe",         "🎵 Bass Puddles",    "🎵 Grav Meter",       "☄️ Meteor Rain",
      "🔥 Camp Fire",         "💡 White Out",       "✨ Twinkle Night",    "🌈 Rainbow Walk"};
  return sequence >= 1 && sequence <= sizeof(labels) / sizeof(labels[0]) ? labels[sequence - 1] : "";
}

inline const char* builtinVestPresetNodeName(uint8_t sequence) {
  static const char* names[] = {
      "Horizon Ring", "Crossing Spiral", "Sphere Move", "Star Sky", "Wraparound Racers", "Ripples", "Paintbrush",
      "GEQ 3D", "Audio Rings", "Freq Wave", "Noise Meter", "Audio Rings", "DJ Light", "Puddle Peak", "Gravimeter",
      "Meteor", "Fire", "Solid", "Color Twinkle", "Rainbow"};
  return sequence >= 1 && sequence <= sizeof(names) / sizeof(names[0]) ? names[sequence - 1] : nullptr;
}

inline const char* presetSlotDisplayLabel(const char* savedLabel, const char* builtinLabel) {
  return savedLabel && savedLabel[0] ? savedLabel : builtinLabel;
}

enum class PresetCommandMode : uint8_t { Apply, Save, Delete };

struct PresetCommand {
  PresetCommandMode mode = PresetCommandMode::Apply;
  uint16_t select = 255;
  char origin[32] = "";
};

template <size_t Capacity>
class PresetCommandQueue {
 public:
  bool enqueue(const PresetCommand& command) {
    if (command.mode == PresetCommandMode::Apply && _size && _items[_size - 1].mode == PresetCommandMode::Apply) {
      _items[_size - 1] = command;
      return true;
    }
    if (_size == Capacity) return false;
    _items[_size++] = command;
    return true;
  }

  bool takeReadyNonSave(uint32_t now, uint32_t lastApply, uint32_t applyGap, PresetCommand& command) {
    for (size_t i = 0; i < _size; i++) {
      if (_items[i].mode == PresetCommandMode::Save) continue;
      if (_items[i].mode == PresetCommandMode::Apply && now - lastApply < applyGap) return false;
      command = _items[i];
      erase(i);
      return true;
    }
    return false;
  }

  bool takeFirst(PresetCommandMode mode, PresetCommand& command) {
    for (size_t i = 0; i < _size; i++) {
      if (_items[i].mode != mode) continue;
      command = _items[i];
      erase(i);
      return true;
    }
    return false;
  }

  size_t discard(PresetCommandMode mode) {
    size_t removed = 0;
    for (size_t i = 0; i < _size;) {
      if (_items[i].mode == mode) {
        erase(i);
        removed++;
      } else {
        i++;
      }
    }
    return removed;
  }

  bool contains(PresetCommandMode mode) const {
    for (size_t i = 0; i < _size; i++) {
      if (_items[i].mode == mode) return true;
    }
    return false;
  }

  size_t size() const { return _size; }

 private:
  void erase(size_t index) {
    for (size_t i = index + 1; i < _size; i++) _items[i - 1] = _items[i];
    _size--;
  }

  std::array<PresetCommand, Capacity> _items{};
  size_t _size = 0;
};

#if FT_MOONLIGHT

  #include <esp_heap_caps.h>
  #include <freertos/semphr.h>
  #include <vector>

  #include "MoonBase/Module.h"
  #include "MoonBase/SharedFSPersistence.h"
  #include "MoonBase/utilities/PlatformFunctions.h"
  #include "MoonLight/Modules/DigNext2ButtonPolicy.h"

class ModuleLightsControlPresetController {
 public:
#ifdef ML_WEARABLE_FIELD_BOOT
  static constexpr uint8_t kBuiltinVestPresetCount = 20;
  static constexpr uint8_t kPresetCount = kBuiltinVestPresetCount;
#else
  static constexpr uint8_t kPresetCount = 64;
#endif

  ModuleLightsControlPresetController(Module& owner, ModuleState& state) : _owner(owner), _state(state) {
    _cacheMutex = xSemaphoreCreateMutex();
    if (!_cacheMutex) abort();
  }

  ~ModuleLightsControlPresetController() {
    clearCache(_cached);
    vSemaphoreDelete(_cacheMutex);
  }

  void begin() {
    _selected = _state.data["preset"]["selected"] | 255;
    refreshFromFolder();
  }

  void requestRefresh() {
    portENTER_CRITICAL(&_mux);
    _refreshRequested = true;
    portEXIT_CRITICAL(&_mux);
  }

  void handleUpdate(const UpdatedItem& updatedItem) {
    char action[12] = "";
    strlcpy(action, updatedItem.value["action"] | "", sizeof(action));
    const uint16_t select = updatedItem.value["select"] | 255;
    const String origin = updatedItem.originId ? *updatedItem.originId : String(_owner._moduleName);

    _owner.updateWithoutPropagation(
        [](ModuleState& state) {
          state.data["preset"].remove("action");
          state.data["preset"].remove("select");
          return StateUpdateResult::CHANGED;
        },
        origin);
    syncStateCatalog(false, origin);
    if (!action[0] || select == 255) return;

    PresetCommandMode mode;
    if (!std::strcmp(action, "click")) {
      mode = hasPreset(select) ? PresetCommandMode::Apply : PresetCommandMode::Save;
    } else if (!std::strcmp(action, "dblclick")) {
#ifdef ML_WEARABLE_FIELD_BOOT
      if (select < 1 || select > kBuiltinVestPresetCount) return;
      mode = PresetCommandMode::Save;
#else
      mode = PresetCommandMode::Delete;
#endif
    } else {
      return;
    }

    if (!enqueue(mode, select, origin)) {
      EXT_LOGE(ML_TAG, "Preset command queue full; rejected %u for slot %u", static_cast<unsigned>(mode), select);
    }
  }

  void requestPreset(int select) {
    if (select < 0) return;
    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject newState = doc.to<JsonObject>();
    _owner.read([&](ModuleState& state) { newState["preset"] = state.data["preset"]; }, _owner._moduleName);
    newState["preset"]["action"] = "click";
    newState["preset"]["select"] = select;
    _owner.update(newState, ModuleState::update, _owner._moduleName);
  }

  int selectNext(bool backwards) const {
    lockCache();
    const uint16_t selected = _selected;
    unlockCache();
    int result = -1;
    _owner.read(
        [&](ModuleState& state) {
          JsonArray list = state.data["preset"]["list"];
          const int firstPreset = state.data["firstPreset"] | 1;
          const int lastPreset = state.data["lastPreset"] | kPresetCount;
          result = chooseDigNext2Preset(
              list.size(), [&](size_t index) { return list[index].as<int>(); }, selected, firstPreset, lastPreset, backwards);
        },
        _owner._moduleName);
    return result;
  }

  void process(uint32_t now) {
    bool refresh = false;
    portENTER_CRITICAL(&_mux);
    refresh = _refreshRequested;
    _refreshRequested = false;
    portEXIT_CRITICAL(&_mux);
    if (refresh) refreshFromFolder();

    PresetCommand command;
    portENTER_CRITICAL(&_mux);
    const bool ready = _commands.takeReadyNonSave(now, _lastApplyMs, kMinPresetApplyGapMs, command);
    portEXIT_CRITICAL(&_mux);
    if (!ready) return;

    const String origin(command.origin);
    switch (command.mode) {
      case PresetCommandMode::Apply: {
        EXT_LOGI(ML_TAG, "preset apply select=%u largestDMA=%u", command.select,
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
        bool applied = applyCached(command.select, origin);
#ifdef ML_WEARABLE_FIELD_BOOT
        if (!applied) applied = applyBuiltin(command.select, origin);
#endif
        _lastApplyMs = millis();
        if (applied) {
          commitSelected(command.select, origin);
        } else {
          EXT_LOGE(ML_TAG, "Preset %u unavailable; selection unchanged", command.select);
        }
        break;
      }
      case PresetCommandMode::Save:
        break;
      case PresetCommandMode::Delete:
        deleteSlot(command.select);
        break;
    }
  }

#ifdef ML_WEARABLE_FIELD_BOOT
  bool applyBuiltin(uint16_t select, const String& origin) {
    const char* name = builtinVestPresetNodeName(select);
    if (!name) return false;

    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject fx = doc.to<JsonObject>();
    fx["layer"] = 0;
    fx["brightness"] = 220;
    fx["start"]["x"] = 0;
    fx["start"]["y"] = 0;
    fx["start"]["z"] = 0;
    fx["end"]["x"] = 100;
    fx["end"]["y"] = 100;
    fx["end"]["z"] = 100;
    JsonObject node = fx["nodes"].to<JsonArray>().add<JsonObject>();
    node["on"] = true;
    switch (select) {
      case 1: node["controls"][0]["name"] = "bpm"; node["controls"][0]["value"] = 20; node["controls"][1]["name"] = "fade"; node["controls"][1]["value"] = 64; node["controls"][2]["name"] = "thickness"; node["controls"][2]["value"] = 2; break;
      case 2: node["controls"][0]["name"] = "bpm"; node["controls"][0]["value"] = 32; node["controls"][1]["name"] = "fade"; node["controls"][1]["value"] = 34; node["controls"][2]["name"] = "turns"; node["controls"][2]["value"] = 2; node["controls"][3]["name"] = "width"; node["controls"][3]["value"] = 40; break;
      case 3: fx["label"] = "Particle Sphere"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 46; break;
      case 4: fx["layer"] = 1; fx["label"] = "Star Wave"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 4; node["controls"][1]["name"] = "star fill"; node["controls"][1]["value"] = 180; node["controls"][2]["name"] = "usePalette"; node["controls"][2]["value"] = true; break;
      case 5: node["controls"][0]["name"] = "bpm"; node["controls"][0]["value"] = 58; node["controls"][1]["name"] = "fade"; node["controls"][1]["value"] = 38; node["controls"][2]["name"] = "racers"; node["controls"][2]["value"] = 6; node["controls"][3]["name"] = "trail"; node["controls"][3]["value"] = 52; break;
      case 6: fx["layer"] = 1; fx["label"] = "Ripple Stars"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 50; node["controls"][1]["name"] = "interval"; node["controls"][1]["value"] = 128; break;
      case 7: fx["layer"] = 1; fx["label"] = "Audio Paintbrush"; break;
      case 8: fx["label"] = "Audio GEQ"; break;
      case 9: fx["label"] = "Bass Rings"; node["controls"][0]["name"] = "inWards"; node["controls"][0]["value"] = true; break;
      case 10: node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 140; break;
      case 11: fx["label"] = "Noise Pulse"; node["controls"][0]["name"] = "fadeRate"; node["controls"][0]["value"] = 248; node["controls"][1]["name"] = "width"; node["controls"][1]["value"] = 180; break;
      case 12: fx["label"] = "Camp Pulse"; node["controls"][0]["name"] = "inWards"; node["controls"][0]["value"] = false; break;
      case 13: fx["label"] = "DJ Strobe"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 220; node["controls"][1]["name"] = "candyFactory"; node["controls"][1]["value"] = true; node["controls"][2]["name"] = "fade"; node["controls"][2]["value"] = 4; break;
      case 14: fx["label"] = "Bass Puddles"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 128; node["controls"][1]["name"] = "intensity"; node["controls"][1]["value"] = 180; break;
      case 15: fx["label"] = "Grav Meter"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 128; node["controls"][1]["name"] = "intensity"; node["controls"][1]["value"] = 160; break;
      case 16: fx["label"] = "Meteor Rain"; node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 128; node["controls"][1]["name"] = "trail"; node["controls"][1]["value"] = 160; break;
      case 17: fx["layer"] = 1; fx["label"] = "Camp Fire"; node["controls"][0]["name"] = "usePalette"; node["controls"][0]["value"] = true; node["controls"][1]["name"] = "flareDecay"; node["controls"][1]["value"] = 14; break;
      case 18: fx["label"] = "White Out"; node["controls"][0]["name"] = "colorMode"; node["controls"][0]["value"] = 0; node["controls"][1]["name"] = "red"; node["controls"][1]["value"] = 255; node["controls"][2]["name"] = "green"; node["controls"][2]["value"] = 200; node["controls"][3]["name"] = "blue"; node["controls"][3]["value"] = 180; node["controls"][4]["name"] = "brightness"; node["controls"][4]["value"] = 220; break;
      case 19: fx["label"] = "Twinkle Night"; node["controls"][0]["name"] = "fadeSpeed"; node["controls"][0]["value"] = 140; node["controls"][1]["name"] = "spawnSpeed"; node["controls"][1]["value"] = 120; break;
      case 20: node["controls"][0]["name"] = "speed"; node["controls"][0]["value"] = 10; node["controls"][1]["name"] = "deltaHue"; node["controls"][1]["value"] = 7; node["controls"][2]["name"] = "usePalette"; node["controls"][2]["value"] = true; break;
      default: return false;
    }
    node["name"] = name;
    if (fx["label"].isNull()) fx["label"] = name;
    return !doc.overflowed() && applyEffects(fx, origin);
  }
#endif

 private:
  struct CachedPreset {
    char label[32] = "";
    char* json = nullptr;
    size_t jsonLen = 0;
  };

  static constexpr size_t kMaxPresetBytes = 12288;
  static constexpr size_t kPresetDmaMinBytes = 8192;
  static constexpr uint32_t kMinPresetApplyGapMs = 80;
  static constexpr size_t kCommandCapacity = kPresetCount + 4;

  bool dmaCanOpenFile() const {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >= kPresetDmaMinBytes;
  }

  bool enqueue(PresetCommandMode mode, uint16_t select, const String& origin) {
    PresetCommand command;
    command.mode = mode;
    command.select = select;
    origin.toCharArray(command.origin, sizeof(command.origin));

    bool queueFlush = false;
    portENTER_CRITICAL(&_mux);
    const bool queued = _commands.enqueue(command);
    if (queued && mode == PresetCommandMode::Save && !_saveFlushQueued) {
      _saveFlushQueued = true;
      queueFlush = true;
    }
    portEXIT_CRITICAL(&_mux);
    if (queueFlush) {
      SharedFSPersistence::enqueueDelayedWrite([this](char action) { return flushPendingSaves(action); });
    }
    return queued;
  }

  bool flushPendingSaves(char action) {
    portENTER_CRITICAL(&_mux);
    _saveFlushQueued = false;
    if (action != 'W') {
      _commands.discard(PresetCommandMode::Save);
      portEXIT_CRITICAL(&_mux);
      return true;
    }
    portEXIT_CRITICAL(&_mux);

    bool ok = true;
    while (true) {
      PresetCommand command;
      portENTER_CRITICAL(&_mux);
      const bool found = _commands.takeFirst(PresetCommandMode::Save, command);
      portEXIT_CRITICAL(&_mux);
      if (!found) break;
      const bool saved = saveSlot(command.select);
      if (!saved) EXT_LOGE(ML_TAG, "Preset save failed for slot %u", command.select);
      ok = saved && ok;
    }
    return ok;
  }

  bool makeCachedPreset(uint16_t select, File& file, CachedPreset& cached) {
    const size_t size = file.size();
    if (!size || size > kMaxPresetBytes || !file.seek(0)) return false;
    cached.json = allocMB<char>(size + 1, "presetCache");
    if (!cached.json) return false;
    const size_t got = file.read(reinterpret_cast<uint8_t*>(cached.json), size);
    if (got != size) {
      clearCached(cached);
      return false;
    }
    cached.json[size] = '\0';
    cached.jsonLen = size;

    JsonDocument doc(JsonRAMAllocator::instance());
    if (deserializeJson(doc, cached.json, cached.jsonLen) || doc.overflowed() || !doc.is<JsonObject>()) {
      clearCached(cached);
      return false;
    }
    JsonArray nodes = doc["nodes"];
    const char* nodeName = nodes.size() ? nodes[0]["name"].as<const char*>() : nullptr;
    extractPresetDisplayLabel(cached.label, doc["label"].as<const char*>(), nodeName);
    if (!cached.label[0]) snprintf(cached.label, sizeof(cached.label), "Preset %02u", select);
    return true;
  }

  bool makeLivePreset(uint16_t select, CachedPreset& cached) {
    Module* effects = effectsModule();
    if (!effects) return false;
    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject root = doc.to<JsonObject>();
    effects->read(root, ModuleState::read, effects->_moduleName);
    if (doc.overflowed() || !root.size()) return false;
    const size_t needed = measureJson(root);
    if (!needed || needed > kMaxPresetBytes) return false;
    cached.json = allocMB<char>(needed + 1, "presetCache");
    if (!cached.json) return false;
    const size_t wrote = serializeJson(root, cached.json, needed + 1);
    if (wrote != needed) {
      clearCached(cached);
      return false;
    }
    cached.json[wrote] = '\0';
    cached.jsonLen = wrote;
    const char* nodeName = root["nodes"].is<JsonArray>() && root["nodes"].size()
                               ? root["nodes"][0]["name"].as<const char*>()
                               : nullptr;
    extractPresetDisplayLabel(cached.label, root["label"].as<const char*>(), nodeName);
    if (!cached.label[0]) snprintf(cached.label, sizeof(cached.label), "Preset %02u", select);
    return true;
  }

  bool refreshFromFolder() {
    if (!dmaCanOpenFile()) {
      EXT_LOGW(ML_TAG, "Preset folder scan skipped (largest DMA %u < %u); keeping last-known-good cache",
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
               static_cast<unsigned>(kPresetDmaMinBytes));
      return false;
    }

    std::array<CachedPreset, kPresetCount> staged{};
    bool valid = true;
    const bool folderExists = ESPFS.exists("/.config/presets");
    File root;
    if (folderExists) {
      root = ESPFS.open("/.config/presets/");
      if (!root || !root.isDirectory()) valid = false;
    }
    if (valid && root) {
      walkThroughFiles(root, [&](File, File file) {
        if (!valid || file.isDirectory()) return;
        uint16_t sequence = 0;
        if (!parsePresetJsonBasename(file.name(), sequence) || sequence < 1 || sequence > kPresetCount) return;
        CachedPreset& slot = staged[sequence - 1];
        if (slot.json || !makeCachedPreset(sequence, file, slot)) valid = false;
      });
      root.close();
    }
    if (!valid) {
      clearCache(staged);
      EXT_LOGW(ML_TAG, "Preset folder scan incomplete; keeping last-known-good cache and labels");
      return false;
    }

    lockCache();
    for (size_t i = 0; i < _cached.size(); i++) std::swap(_cached[i], staged[i]);
    clearCache(staged);
    unlockCache();
    syncStateCatalog(true, _owner._moduleName);
    return true;
  }

  bool saveSlot(uint16_t select) {
    if (select < 1 || select > kPresetCount) return false;
    CachedPreset staged;
    if (!makeLivePreset(select, staged)) return false;
    if (!ESPFS.exists("/.config/presets") && !ESPFS.mkdir("/.config/presets")) {
      clearCached(staged);
      return false;
    }
    Char<32> path;
    path.format("/.config/presets/preset%02u.json", select);
    if (!SharedFSPersistence::writeJsonPath(path.c_str(), staged.json, staged.jsonLen)) {
      clearCached(staged);
      return false;
    }

    lockCache();
    clearCached(_cached[select - 1]);
    _cached[select - 1] = staged;
    staged.json = nullptr;
    const size_t savedLength = _cached[select - 1].jsonLen;
    unlockCache();
    syncStateCatalog(true, _owner._moduleName);
    EXT_LOGI(ML_TAG, "Wrote preset slot %u (%u bytes)", select, static_cast<unsigned>(savedLength));
    return true;
  }

  void deleteSlot(uint16_t select) {
    if (select < 1 || select > kPresetCount) return;
    Char<32> path;
    path.format("/.config/presets/preset%02u.json", select);
    if (!ESPFS.remove(path.c_str())) {
      EXT_LOGW(ML_TAG, "Failed to delete preset slot %u", select);
      return;
    }
    requestRefresh();
  }

  bool applyCached(uint16_t select, const String& origin) {
    if (select < 1 || select > kPresetCount) return false;
    lockCache();
    const CachedPreset& cached = _cached[select - 1];
    if (!cached.json) {
      unlockCache();
      return false;
    }
    JsonDocument doc(JsonRAMAllocator::instance());
    const bool invalid = deserializeJson(doc, cached.json, cached.jsonLen) || doc.overflowed() || !doc.is<JsonObject>();
    unlockCache();
    if (invalid) return false;
    return applyEffects(doc.as<JsonObject>(), origin, true);
  }

  bool applyEffects(JsonObject object, const String& origin, bool persistedState = false) {
    Module* effects = effectsModule();
    if (!effects || effects->replaceFullState(object, origin, persistedState) == StateUpdateResult::ERROR) return false;
    effects->update([](ModuleState&) { return StateUpdateResult::CHANGED; }, origin);
    return true;
  }

  Module* effectsModule() const {
    extern std::vector<Module*> modules;
    for (Module* module : modules) {
      if (module && !std::strcmp(module->_moduleName, "effects")) return module;
    }
    return nullptr;
  }

  bool hasPreset(uint16_t select) const {
    if (select < 1 || select > kPresetCount) return false;
    lockCache();
    const bool cached = _cached[select - 1].json;
    unlockCache();
    if (cached) return true;
#ifdef ML_WEARABLE_FIELD_BOOT
    return select <= kBuiltinVestPresetCount;
#else
    return false;
#endif
  }

  void syncStateCatalog(bool publish, const String& origin) {
    std::array<std::array<char, 32>, kPresetCount> savedLabels{};
    std::array<bool, kPresetCount> cachedSlots{};
    lockCache();
    for (uint16_t select = 1; select <= kPresetCount; select++) {
      const CachedPreset& cached = _cached[select - 1];
      cachedSlots[select - 1] = cached.json;
      strlcpy(savedLabels[select - 1].data(), cached.label, savedLabels[select - 1].size());
    }
    const uint16_t selected = _selected;
    unlockCache();

    auto updateCatalog = [&](ModuleState& state) {
      JsonObject preset = state.data["preset"];
      JsonArray list = preset["list"].to<JsonArray>();
      JsonArray labels = preset["labels"].to<JsonArray>();
      list.clear();
      labels.clear();
      for (uint16_t select = 1; select <= kPresetCount; select++) {
#ifdef ML_WEARABLE_FIELD_BOOT
        list.add(select);
        labels.add(presetSlotDisplayLabel(savedLabels[select - 1].data(), builtinVestPresetLabel(select)));
#else
        if (!cachedSlots[select - 1]) continue;
        list.add(select);
        labels.add(savedLabels[select - 1].data());
#endif
      }
      preset["count"] = kPresetCount;
      if (selected == 255)
        preset.remove("selected");
      else
        preset["selected"] = selected;
      return StateUpdateResult::CHANGED;
    };
    if (publish)
      _owner.update(updateCatalog, origin);
    else
      _owner.updateWithoutPropagation(updateCatalog, origin);
  }

  void commitSelected(uint16_t select, const String& origin) {
    lockCache();
    _selected = select;
    unlockCache();
    _owner.update(
        [select](ModuleState& state) {
          state.data["preset"]["selected"] = select;
          return StateUpdateResult::CHANGED;
        },
        origin);
  }

  void lockCache() const { xSemaphoreTake(_cacheMutex, portMAX_DELAY); }
  void unlockCache() const { xSemaphoreGive(_cacheMutex); }

  static void clearCached(CachedPreset& cached) {
    if (cached.json) freeMB(cached.json, "presetCache");
    cached.jsonLen = 0;
    cached.label[0] = '\0';
  }

  static void clearCache(std::array<CachedPreset, kPresetCount>& cache) {
    for (CachedPreset& cached : cache) clearCached(cached);
  }

  Module& _owner;
  ModuleState& _state;
  std::array<CachedPreset, kPresetCount> _cached{};
  PresetCommandQueue<kCommandCapacity> _commands;
  SemaphoreHandle_t _cacheMutex = nullptr;
  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  uint16_t _selected = 255;
  uint32_t _lastApplyMs = 0;
  bool _refreshRequested = false;
  bool _saveFlushQueued = false;
};

#endif
