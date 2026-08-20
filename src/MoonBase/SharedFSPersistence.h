/**
    @title     MoonBase
    @file      SharedFSPersistence.h
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/overview/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#ifndef SharedFSPersistence_h
#define SharedFSPersistence_h

#include <FS.h>
#include <StatefulService.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>
#include <utility>

#include "Module.h"

// ADDED: Global delayed writes queue (matches templated version)
inline std::vector<std::function<bool(char)>> sharedDelayedWrites;
inline portMUX_TYPE sharedDelayedWritesMux = portMUX_INITIALIZER_UNLOCKED;

class SharedFSPersistence {
 private:
  FS* _fs;
  struct ModuleInfo {
    Module* module;
    String filePath;
    bool delayedWriting;
    bool hasDelayedWrite;
    update_handler_id_t updateHandlerId;

    ModuleInfo() : module(nullptr), delayedWriting(false), hasDelayedWrite(false), updateHandlerId(0) {}
  };
  struct CStrComparator {
    bool operator()(const char* a, const char* b) const { return strcmp(a, b) < 0; }
  };
  std::map<const char*, ModuleInfo, CStrComparator> _modules;  // moduleName -> info

 public:
  SharedFSPersistence(FS* fs) : _fs(fs) {}

  // ADDED: Support for delayed writing parameter
  void registerModule(Module* module, bool delayedWriting = false) {
    ModuleInfo info;
    info.module = module;
    info.filePath = String("/.config/") + module->_moduleName + ".json";
    info.delayedWriting = delayedWriting;
    info.hasDelayedWrite = false;

    _modules[module->_moduleName] = info;
  }

  void begin() {
    // Pin assignments are a dependency of LED, audio, button, and sensor nodes.
    // The map is alphabetically ordered, so load the foundational IO module
    // explicitly before node managers construct their persisted drivers.
    auto inputOutput = _modules.find("inputoutput");
    if (inputOutput != _modules.end()) readFromFS(inputOutput->first);

    // Read initial state from filesystem
    for (const auto& pair : _modules) {
      if (inputOutput != _modules.end() && pair.first == inputOutput->first) continue;
      readFromFS(pair.first);
    }

    // Persisted reads deliberately suppress propagation. Publish the settled IO
    // state once, after every pin consumer has restored its own state, so drivers
    // initialize even when the later board-default pass is unchanged.
    if (inputOutput != _modules.end()) inputOutput->second.module->callUpdateHandlers(inputOutput->first);

    // Register update handlers for modules that requested delayed writing
    for (const auto& pair : _modules) {
      if (pair.second.delayedWriting) {
        enableUpdateHandler(pair.first);
        EXT_LOGD(MB_TAG, "Enabled update handler for %s after file read", pair.first);
      }
    }

    EXT_LOGI(MB_TAG, "SharedFSPersistence initialization complete");
  }

  // ADDED: Enable/disable update handler for specific module
  void disableUpdateHandler(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it != _modules.end() && it->second.updateHandlerId) {
      it->second.module->removeUpdateHandler(it->second.updateHandlerId);
      it->second.updateHandlerId = 0;
    }
  }

  void enableUpdateHandler(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it != _modules.end() && !it->second.updateHandlerId) {
      it->second.updateHandlerId = it->second.module->addUpdateHandler([this, module = it->second.module](const String& originId) { writeToFS(module->_moduleName); }, false);
    }
  }

  void readFromFS(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it == _modules.end()) return;

    ModuleInfo& info = it->second;
    if (!info.module->shouldLoadPersistedState()) {
      EXT_LOGW(MB_TAG, "Using in-memory defaults for %s; leaving %s unchanged", moduleName, info.filePath.c_str());
      return;
    }
    File file = _fs->open(info.filePath.c_str(), "r");

    if (file) {
      JsonDocument doc(JsonRAMAllocator::instance());
      DeserializationError error = deserializeJson(doc, file);
      file.close();

      if (!error && !doc.overflowed() && doc.is<JsonObject>()) {
        JsonObject obj = doc.as<JsonObject>();
        if (info.module->replaceFullState(obj, moduleName, true) != StateUpdateResult::ERROR) return;
        EXT_LOGE(MB_TAG, "Rejected invalid persisted state for %s; leaving %s unchanged", moduleName,
                 info.filePath.c_str());
        return;
      }
    }

    // ADDED: Apply defaults if file doesn't exist or is corrupted
    applyDefaults(info, moduleName);
    writeToFSNow(moduleName);
  }

  void writeToFS(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it == _modules.end()) return;

    ModuleInfo& info = it->second;
    if (!info.module->shouldLoadPersistedState()) return;

    // ADDED: Delayed write support
    if (info.delayedWriting) {
      if (!info.hasDelayedWrite) {
        ESP_LOGD(SVK_TAG, "delayedWrites: Add %s", info.filePath.c_str());

        portENTER_CRITICAL(&sharedDelayedWritesMux);
        sharedDelayedWrites.push_back([this, module = info.module](char writeOrCancel) {
          auto it = _modules.find(module->_moduleName);
          if (it == _modules.end()) return false;

          ESP_LOGD(SVK_TAG, "delayedWrites: %c %s", writeOrCancel, it->second.filePath.c_str());

          // Allow updates that arrive during this write to enqueue their own
          // follow-up instead of being hidden by the old pending flag.
          it->second.hasDelayedWrite = false;
          bool ok = true;
          if (writeOrCancel == 'W') {
            ok = this->writeToFSNow(module->_moduleName);
            if (!ok) this->writeToFS(module->_moduleName);
          } else {
            // Cancel: read old state back from FS
            this->readFromFS(module->_moduleName);
            // Update UI with restored state
            it->second.module->update([](ModuleState& state) { return StateUpdateResult::CHANGED; }, SVK_TAG);
          }
          return ok;
        });

        info.hasDelayedWrite = true;
        portEXIT_CRITICAL(&sharedDelayedWritesMux);
      }
    } else {
      writeToFSNow(moduleName);
    }
  }

  bool writeToFSNow(const char* moduleName) {
    auto it = _modules.find(moduleName);
    if (it == _modules.end()) return false;

    ModuleInfo& info = it->second;
    if (!info.module->shouldLoadPersistedState()) return true;

    // ADDED: Create directories if needed
    mkdirs(info.filePath);

    // Create JSON document
    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject root = doc.to<JsonObject>();
    info.module->read(root, ModuleState::read, moduleName);
    if (doc.overflowed() || !root.size()) return false;

    // Arduino File -> fopen -> FILE lock in INTERNAL/DMA. Floppy must not do that.
    const size_t needed = measureJson(root);
    if (!needed) return false;
    char* json = static_cast<char*>(heap_caps_malloc(needed + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!json) json = static_cast<char*>(heap_caps_malloc(needed + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!json) return false;
    const size_t wrote = serializeJson(root, json, needed + 1);
    const bool serialized = wrote == needed;
    if (serialized) json[wrote] = '\0';
    const bool ok = serialized && writeConfigFd(info.filePath.c_str(), json, needed);
    heap_caps_free(json);
    return ok;
  }

  // Atomic JSON write without Arduino File/fopen (safe when DMA heap is tight).
  static bool writeJsonPath(const char* modulePath, const char* json, size_t len) { return writeConfigFd(modulePath, json, len); }

  static void enqueueDelayedWrite(std::function<bool(char)> write) {
    portENTER_CRITICAL(&sharedDelayedWritesMux);
    sharedDelayedWrites.push_back(std::move(write));
    portEXIT_CRITICAL(&sharedDelayedWritesMux);
  }

  // ADDED: Static method to process all delayed writes
  static bool writeToFSDelayed(char writeOrCancel) {
    while (true) {
      std::vector<std::function<bool(char)>> pending;
      portENTER_CRITICAL(&sharedDelayedWritesMux);
      pending = std::move(sharedDelayedWrites);
      sharedDelayedWrites.clear();
      portEXIT_CRITICAL(&sharedDelayedWritesMux);
      if (pending.empty()) return true;

      ESP_LOGD(SVK_TAG, "calling %u writeFuncs from delayedWrites", pending.size());
      bool ok = true;
      for (const auto& writeFunc : pending) ok = writeFunc(writeOrCancel) && ok;
      if (!ok) return false;
    }
  }

 private:
  static bool writeConfigFd(const char* modulePath, const char* json, size_t len) {
    if (!modulePath || !json || !len) return false;
    char vfsPath[96];
    const int vfsLen = snprintf(vfsPath, sizeof(vfsPath), "/littlefs%s", modulePath);
    char tmpPath[100];
    const int tmpLen = snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", vfsPath);
    if (vfsLen < 0 || static_cast<size_t>(vfsLen) >= sizeof(vfsPath) || tmpLen < 0 ||
        static_cast<size_t>(tmpLen) >= sizeof(tmpPath))
      return false;
    const int fd = ::open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
      const ssize_t n = ::write(fd, json + off, len - off);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        ::close(fd);
        ::unlink(tmpPath);
        return false;
      }
      off += static_cast<size_t>(n);
    }

    // Commit the complete temporary file before atomically replacing the
    // destination. Never unlink the destination as a rename fallback: if the
    // replacement fails, the previous complete configuration must survive.
    const bool synced = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!synced || !closed) {
      ::unlink(tmpPath);
      return false;
    }
    if (::rename(tmpPath, vfsPath) != 0) {
      ::unlink(tmpPath);
      return false;
    }
    return true;
  }

  // ADDED: Create directories if they don't exist
  void mkdirs(const String& filePath) {
    int index = 0;
    while ((index = filePath.indexOf('/', index + 1)) != -1) {
      String segment = filePath.substring(0, index);
      if (!_fs->exists(segment)) {
        _fs->mkdir(segment);
      }
    }
  }

  // ADDED: Apply defaults from empty object
  void applyDefaults(ModuleInfo& info, const char* moduleName) {
    JsonDocument doc(JsonRAMAllocator::instance());
    JsonObject obj = doc.to<JsonObject>();
    info.module->updateWithoutPropagation(obj, ModuleState::update, moduleName);
  }
};

#endif
