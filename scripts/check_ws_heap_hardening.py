from pathlib import Path


source = (Path(__file__).parents[1] / "src/MoonBase/SharedWebSocketServer.h").read_text()
shared_event_source = (Path(__file__).parents[1] / "src/MoonBase/SharedEventEndpoint.h").read_text()
request_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicRequest.cpp").read_text()
json_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicJson.cpp").read_text()
response_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicResponse.cpp").read_text()
stream_response_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicStreamResponse.cpp").read_text()
websocket_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicWebSocket.cpp").read_text()
server_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicHttpServer.cpp").read_text()
wifi_source = (Path(__file__).parents[1] / "lib/framework/WiFiSettingsService.cpp").read_text()
pico_config = (Path(__file__).parents[1] / "firmware/esp32-d0.ini").read_text()
wifi_buffer_config = (Path(__file__).parents[1] / "lib/framework/WiFiStaticBuffers.h").read_text()
event_socket_header = (Path(__file__).parents[1] / "lib/framework/EventSocket.h").read_text()
event_socket_source = (Path(__file__).parents[1] / "lib/framework/EventSocket.cpp").read_text()
event_endpoint_source = (Path(__file__).parents[1] / "lib/framework/EventEndpoint.h").read_text()
websocket_server_source = (Path(__file__).parents[1] / "lib/framework/WebSocketServer.h").read_text()
http_endpoint_source = (Path(__file__).parents[1] / "lib/framework/HttpEndpoint.h").read_text()
file_manager_source = (Path(__file__).parents[1] / "src/MoonBase/Modules/FileManager.cpp").read_text()
sveltekit_source = (Path(__file__).parents[1] / "lib/framework/ESP32SvelteKit.cpp").read_text()
system_status_header = (Path(__file__).parents[1] / "lib/framework/SystemStatus.h").read_text()
system_status_source = (Path(__file__).parents[1] / "lib/framework/SystemStatus.cpp").read_text()
lights_control_source = (Path(__file__).parents[1] / "src/MoonLight/Modules/ModuleLightsControl.h").read_text()
allocator = "JsonDocument doc(PsychicJsonAllocator::instance());"
no_clients = "if (!client && _handler.count() == 0) return;"
event_guard = "if (!sync && !_socket->hasBroadcastRecipient(_event, originId)) return;"
websocket_guard = "if (!client && _webSocket.count() == 0) return;"

assert allocator in source, "WebSocket snapshots must allocate JSON in PSRAM"
assert "JsonDocument jsonBuffer(PsychicJsonAllocator::instance());" in json_source, "JSON request bodies must prefer PSRAM"
assert "JsonDocument doc(PsychicJsonAllocator::instance());" in event_socket_source, "Incoming event JSON must prefer PSRAM"
assert "PsychicJsonAllocator::instance()->allocate(outputSize)" in event_socket_source, "Event payloads must prefer PSRAM"
assert "static std::vector<uint8_t> outBuffer" not in event_socket_source, "Event payload capacity must not remain in internal RAM"
assert "heap_caps_malloc_prefer(size, 2," in response_source, "Response buffers must prefer PSRAM"
for response_user in (json_source, stream_response_source, sveltekit_source):
    assert "allocateResponseBuffer(" in response_user
assert allocator in shared_event_source, "Event snapshots must allocate JSON in PSRAM"
shared_event_guard = "if (!sync && !_socket->hasBroadcastRecipient(module->_moduleName, originId)) return;"
assert shared_event_guard in shared_event_source
assert shared_event_source.index(shared_event_guard) < shared_event_source.index("module->read")
assert no_clients in source, "WebSocket broadcasts must skip serialization with no clients"
assert source.index(no_clients) < source.index("String buffer;", source.index("void transmitData"))
assert "if (request->client()->isNew)" in source, "Initial state must follow the connection, not a reused fd"
assert "(*this->_session)[key] = value;" in request_source, "Session values must be replaceable on reconnect"
assert "PsychicHandler::getClient(request->client())" in websocket_source
assert "checkForNewClient(request->client())" not in websocket_source, "The first data frame must not clear isNew"
assert "isNew = client->isNew;" in websocket_source
assert "client->isNew = wsRequest.client()->isNew;" in websocket_source
assert "config.lru_purge_enable = true;" in server_source
assert ".handle_ws_control_frames" not in server_source, "ESP-IDF must own WebSocket control frames"
assert "WebSocket send failed for fd=%d" in websocket_source
assert "HTTPD_WS_TYPE_CLOSE" not in websocket_source, "ESP-IDF must own WebSocket close frames"
assert "HTTPD_WS_TYPE_PING" not in websocket_source, "ESP-IDF must own WebSocket ping frames"
assert "TCP_NODELAY" not in server_source, "HTTP and WebSocket sockets must use the supported TCP defaults"
assert "#ifdef WIFI_USE_STATIC_BUFFERS\n    WiFi.useStaticBuffers(true);\n#endif" in wifi_source
assert wifi_source.index("WiFi.useStaticBuffers(true);") < wifi_source.index("WiFi.mode(WIFI_MODE_STA)")
pico2_start = pico_config.index("[env:esp32-d0-pico2]")
pico2_config = pico_config[pico2_start:]
assert "WIFI_USE_STATIC_BUFFERS" not in pico2_config, "DigNext2 profile must not park WiFi buffers in internal RAM"
assert "CONFIG_RECOVERY_ENABLED" not in pico2_config, "DigNext2 profile must not auto-rollback user saves on panic"
assert "#define CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM 8" in wifi_buffer_config
assert "#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 0" in wifi_buffer_config
assert "#undef CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER" in wifi_buffer_config
assert "max_open_sockets = 6" in server_source, "HTTP server must reserve enough client slots for UI + WS"
assert "kPresetDmaMinBytes = 8192" in lights_control_source, "Preset folder scan must wait for healthy DMA headroom"
assert "refreshBuiltinPresetLabels" in lights_control_source, "Preset labels must refresh without wiping RAM cache"
assert "bool hasBroadcastRecipient(const String &event, const String &originId);" in event_socket_header
query_start = event_socket_source.index("bool EventSocket::hasBroadcastRecipient")
query_end = event_socket_source.index("// 🌙 Client info", query_start)
query_source = event_socket_source[query_start:query_end]
assert query_source.index("xSemaphoreTake(clientSubscriptionsMutex") < query_source.index("client_subscriptions.find(event)")
assert query_source.index("client_subscriptions.find(event)") < query_source.index("xSemaphoreGive(clientSubscriptionsMutex)")
assert event_guard in event_endpoint_source
event_sync = event_endpoint_source.index("void syncState")
assert event_endpoint_source.index(event_guard, event_sync) < event_endpoint_source.index("_statefulService->read", event_sync)
assert "syncState(originId, true)" in event_endpoint_source, "Subscriptions must still receive initial state"
assert "JsonDocument jsonDocument(PsychicJsonAllocator::instance());" in event_endpoint_source
assert 'jsonDocument["event"] = _event;' in event_endpoint_source
assert 'JsonObject root = jsonDocument["data"].to<JsonObject>();' in event_endpoint_source
assert "_socket->emitEvent(jsonDocument, originId.c_str(), sync);" in event_endpoint_source
assert websocket_guard in websocket_server_source
transmit_data = websocket_server_source.index("void transmitData")
assert websocket_server_source.index(websocket_guard, transmit_data) < websocket_server_source.index("_statefulService->read", transmit_data)
assert "transmitData(client, WEB_SOCKET_ORIGIN);" in websocket_server_source, "New clients must still receive initial state"
assert "JsonDocument jsonDocument(PsychicJsonAllocator::instance());" in websocket_server_source
assert 'if (!_readAfterUpdate)' in http_endpoint_source
assert 'return request->reply(200, "application/json", "{}");' in http_endpoint_source
assert 'AuthenticationPredicates::IS_AUTHENTICATED, false)' in file_manager_source
assert "uint32_t _sketchSize = 0;" in system_status_header
assert "esp_image_get_metadata(&position, &metadata)" in system_status_source
assert "_sketchSize = metadata.image_len;" in system_status_source
assert "ESP.getSketchSize()" not in system_status_source, "System status must not re-enter the hardware SHA engine"

on_update = lights_control_source.index("void onUpdate")
loop20ms = lights_control_source.index("void loop20ms() override")
assert 'copyFile(presetFile.c_str(), "/.config/effects.json")' not in lights_control_source[on_update:loop20ms], "Preset copies must not run on the httpd update path"
assert 'applyCachedPreset(' in lights_control_source[loop20ms:], 'Preset apply must stay in RAM on loop20ms'
assert 'writePresetSlotFromCache(' in lights_control_source[loop20ms:], 'Preset slot save must write via fd off the live path'
assert 'SharedFSPersistence::writeJsonPath' in lights_control_source, 'Preset slot writes must use atomic fd persistence'
assert 'copyFile(' not in lights_control_source[loop20ms:], 'Preset apply must not copy files on the live path'
assert 'lastDriversSnapshot' in (Path(__file__).parents[1] / "src/MoonLight/Nodes/Drivers/D_FastLEDAudio.h").read_text(), "Audio driver meters must use throttled snapshots"
status_handler = system_status_source.index("esp_err_t SystemStatus::systemStatus")
assert "ESP.getSketchSize()" not in system_status_source[status_handler:], "Live status requests must not re-enter the hardware SHA engine"
assert "/rest/saveGolden" in file_manager_source, "Golden snapshot must be exposed over REST"
assert "/rest/restoreGolden" in file_manager_source, "Golden restore must be exposed over REST"
assert "goldenSaveSnapshot()" in file_manager_source, "Golden save must use fd tree copy"
assert "goldenRestoreSnapshot()" in file_manager_source, "Golden restore must use fd tree copy"
assert "isProtectedGoldenPath" in file_manager_source, "Golden tree must be hidden from file manager"
assert "pollDigNext2Buttons" in lights_control_source, "Dig-Next-2 buttons must be polled from loop20ms"
assert "toggleDigNext2Power" in lights_control_source, "Dig-Next-2 both-hold must toggle relay via lightsOn"
assert "DIG_NEXT2_BOTH_POWER_MS" in (Path(__file__).parents[1] / "src/MoonLight/Modules/DigNext2ButtonPolicy.h").read_text(), "Dig-Next-2 power hold timing must be defined"
