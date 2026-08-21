/**
    @title     MoonBase
    @file      FileManager.cpp
    @repo      https://github.com/MoonModules/MoonLight, submit changes to this file as PRs
    @Authors   https://github.com/MoonModules/MoonLight/commits/main
    @Doc       https://moonmodules.org/MoonLight/moonbase/FileManager/
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007
    @license   For non GPL-v3 usage, commercial licenses must be purchased. Contact us for more information.
**/

#if FT_MOONBASE == 1

  #include "FileManager.h"

  #include "MoonBase/GoldenConfig.h"
  #include "MoonBase/SharedFSPersistence.h"
  #include "MoonBase/utilities/PlatformFunctions.h"

inline bool isProtectedFileManagerPath(const char* path) {
  return isProtectedRecoveryPath(path) || isProtectedGoldenPath(path);
}

// recursively fill a fileArray with all files and folders on the FS
void addFolder(File folder, bool showHidden, const JsonArray& fileArray) {
  folder.rewindDirectory();
  while (true) {
    File file = folder.openNextFile();
    if (!file) {
      break;
    } else {
      if (!isProtectedFileManagerPath(file.path()) && (showHidden || file.name()[0] != '.')) {
        JsonObject fileObject = fileArray.add<JsonObject>();
        fileObject["name"] = (char*)file.name();  // enforces copy, solved in latest arduinojson!, see https://arduinojson.org/news/2024/12/29/arduinojson-7-3/
        fileObject["path"] = (char*)file.path();  // enforces copy, solved in latest arduinojson!, see https://arduinojson.org/news/2024/12/29/arduinojson-7-3/
        fileObject["isFile"] = !file.isDirectory();
        // EXT_LOGI(MB_TAG, "file %s (%d)", file.path(), file.size());
        if (file.isDirectory()) {
          addFolder(file, showHidden, fileObject["files"].to<JsonArray>());
        } else {
          fileObject["size"] = file.size();
          fileObject["time"] = file.getLastWrite();
        }
        // serializeJson(fileObject, Serial);
      }
      file.close();
      vTaskDelay(1);
    }
  }
}

void FilesState::read(FilesState& state, JsonObject& stateJson) {
  stateJson["name"] = "/";
  // crashes for some reason: ???
  stateJson["fs_total"] = ESPFS.totalBytes();
  stateJson["fs_used"] = ESPFS.usedBytes();
  stateJson["showHidden"] = state.showHidden;
  File folder = ESPFS.open("/");
  addFolder(folder, state.showHidden, stateJson["files"].to<JsonArray>());
  folder.close();
  // print->printJson("FilesState::read", stateJson);
  EXT_LOGI(MB_TAG, "");
}

StateUpdateResult FilesState::update(JsonObject& newData, FilesState& state, const String& originId) {
  bool changed = false;

  if (newData["showHidden"] != state.showHidden) {
    state.showHidden = newData["showHidden"];
    EXT_LOGD(MB_TAG, "showHidden %d", state.showHidden);
    changed = true;
  }

  state.updatedItems.clear();

  JsonArray deletes = newData["deletes"].as<JsonArray>();
  if (!deletes.isNull()) {
    for (JsonObject var : deletes) {
      const char* path = var["path"].as<const char*>();
      if (!path || isProtectedFileManagerPath(path)) {
        EXT_LOGW(MB_TAG, "Rejected invalid or protected recovery delete path");
        continue;
      }
      EXT_LOGI(MB_TAG, "delete %s %s", path, var["isFile"] ? "File" : "Folder");
      // print->printJson("new file", var);
      if (var["isFile"])
        ESPFS.remove(path);
      else
        ESPFS.rmdir(path);

      state.updatedItems.push_back(path);
    }
  }

  JsonArray news = newData["news"].as<JsonArray>();
  if (!news.isNull()) {
    for (JsonObject var : news) {
      const char* path = var["path"].as<const char*>();
      if (!path || isProtectedFileManagerPath(path)) {
        EXT_LOGW(MB_TAG, "Rejected invalid or protected recovery create path");
        continue;
      }
      EXT_LOGI(MB_TAG, "new %s %s", path, var["isFile"] ? "File" : "Folder");
      // print->printJson("new file", var);
      if (var["isFile"]) {
        File file = ESPFS.open(path, FILE_WRITE);
        const char* contents = var["contents"];
        if (strlen(contents)) {
          if (!file.write(reinterpret_cast<const byte*>(contents), strlen(contents))) {  // changed not true as contents is not part of the state
            EXT_LOGE(MB_TAG, "Write failed");
          }
        }
        file.close();
      } else {
        ESPFS.mkdir(path);
      }
      state.updatedItems.push_back(path);
    }
  }

  JsonArray updates = newData["updates"].as<JsonArray>();
  if (!updates.isNull()) {
    for (JsonObject var : updates) {
      const char* source = var["path"].as<const char*>();
      const char* name = var["name"].as<const char*>();
      if (!source || !name) {
        EXT_LOGW(MB_TAG, "Rejected invalid update path");
        continue;
      }
      String sourcePath = source;
      int lastSlash = sourcePath.lastIndexOf('/');
      String destinationPath = (lastSlash >= 0 ? sourcePath.substring(0, lastSlash + 1) : String("/")) + name;
      if (isProtectedFileManagerPath(sourcePath.c_str()) || isProtectedFileManagerPath(destinationPath.c_str())) {
        EXT_LOGW(MB_TAG, "Rejected protected recovery update path");
        continue;
      }
      EXT_LOGI(MB_TAG, "update %s %s", var["path"].as<const char*>(), var["isFile"] ? "File" : "Folder");
      // print->printJson("update file", var);
      File file = ESPFS.open(sourcePath.c_str(), FILE_WRITE);
      if (!file) {
        EXT_LOGE(MB_TAG, "Failed to open file");
      } else {
        const char* contents = var["contents"];
        if (!file.write(reinterpret_cast<const byte*>(contents), strlen(contents))) {  // changed not true as contents is not part of the state
          EXT_LOGE(MB_TAG, "Write failed");
        }
        file.close();

        EXT_LOGI(MB_TAG, "rename %s to %s", sourcePath.c_str(), destinationPath.c_str());

        if (sourcePath != destinationPath) {
          ESPFS.rename(sourcePath.c_str(), destinationPath.c_str());
        }
        state.updatedItems.push_back(sourcePath);
      }
    }
  }

  changed = changed || !state.updatedItems.empty();

  if (changed && state.updatedItems.size()) EXT_LOGV(MB_TAG, "first item %s", state.updatedItems.front().c_str());

  return changed ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

FileManager::FileManager(PsychicHttpServer* server, ESP32SvelteKit* sveltekit)
    : _httpEndpoint(FilesState::read, FilesState::update, this, server, "/rest/FileManager",  //
                    sveltekit->getSecurityManager(), AuthenticationPredicates::IS_AUTHENTICATED, false),
      _eventEndpoint(FilesState::read, FilesState::update, this, sveltekit->getSocket(), "FileManager"),
      _webSocketServer(FilesState::read, FilesState::update, this, server, "/ws/FileManager", sveltekit->getSecurityManager(), AuthenticationPredicates::IS_AUTHENTICATED),
      _socket(sveltekit->getSocket()),
      _server(server),
      _sveltekit(sveltekit) {}

void FileManager::begin() {
  _httpEndpoint.begin();
  _eventEndpoint.begin();
  _webSocketServer.begin();

  // setup the file server
  _server->serveStatic("/rest/file", ESPFS, "/")->setFilter([](PsychicRequest* request) {
    return !isProtectedFileManagerPath(request->uri().c_str());
  });

  _server->on("/rest/saveConfig", HTTP_POST,
              _sveltekit->getSecurityManager()->wrapRequest(
                  [this](PsychicRequest* request) {
                    if (safeModeMB) {
                      request->reply(409, "text/plain", "Configuration save disabled in safe mode");
                      return ESP_OK;
                    }
                    if (!SharedFSPersistence::writeToFSDelayed('W')) {
                      request->reply(500, "text/plain", "Configuration save failed");
                      return ESP_OK;
                    }

                    saveNeeded = false;
                    request->reply(200);
                    return ESP_OK;
                  },
                  AuthenticationPredicates::IS_AUTHENTICATED));

  _server->on("/rest/cancelConfig", HTTP_POST,
              _sveltekit->getSecurityManager()->wrapRequest(
                  [this](PsychicRequest* request) {
                    request->reply(200);

                    SharedFSPersistence::writeToFSDelayed('C');  // read back from FS

                    // update UI...

                    saveNeeded = false;

                    return ESP_OK;
                  },
                  AuthenticationPredicates::IS_AUTHENTICATED));

  _server->on("/rest/saveGolden", HTTP_POST,
              _sveltekit->getSecurityManager()->wrapRequest(
                  [](PsychicRequest* request) {
                    if (safeModeMB) {
                      request->reply(409, "text/plain", "Configuration save disabled in safe mode");
                      return ESP_OK;
                    }
                    if (!goldenDmaReady()) {
                      request->reply(503, "text/plain", "Insufficient internal RAM for golden snapshot");
                      return ESP_OK;
                    }
                    if (!SharedFSPersistence::writeToFSDelayed('W')) {
                      request->reply(500, "text/plain", "Configuration save failed");
                      return ESP_OK;
                    }
                    if (!goldenSaveSnapshot()) {
                      request->reply(500, "text/plain", "Golden snapshot failed");
                      return ESP_OK;
                    }
                    saveNeeded = false;
                    request->reply(200, "application/json", "{\"ok\":true,\"golden\":true}");
                    return ESP_OK;
                  },
                  AuthenticationPredicates::IS_ADMIN));

  _server->on("/rest/restoreGolden", HTTP_POST,
              _sveltekit->getSecurityManager()->wrapRequest(
                  [](PsychicRequest* request) {
                    if (!goldenConfigPresent()) {
                      request->reply(404, "text/plain", "No golden snapshot present");
                      return ESP_OK;
                    }
                    if (!goldenDmaReady()) {
                      request->reply(503, "text/plain", "Insufficient internal RAM for golden restore");
                      return ESP_OK;
                    }
                    if (!goldenRestoreSnapshot()) {
                      request->reply(500, "text/plain", "Golden restore failed; rebooting for filesystem recovery");
                      delay(250);
                      ESP.restart();
                      return ESP_OK;
                    }
                    request->reply(200, "application/json",
                                   "{\"ok\":true,\"restart\":true,\"liveScripts\":\"disabled\"}");
                    delay(250);
                    ESP.restart();
                    return ESP_OK;
                  },
                  AuthenticationPredicates::IS_ADMIN));
}

#endif
