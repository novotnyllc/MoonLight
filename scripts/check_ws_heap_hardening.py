from pathlib import Path


source = (Path(__file__).parents[1] / "src/MoonBase/SharedWebSocketServer.h").read_text()
request_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicRequest.cpp").read_text()
websocket_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicWebSocket.cpp").read_text()
server_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicHttpServer.cpp").read_text()
wifi_source = (Path(__file__).parents[1] / "lib/framework/WiFiSettingsService.cpp").read_text()
pico_config = (Path(__file__).parents[1] / "firmware/esp32-d0.ini").read_text()
wifi_buffer_config = (Path(__file__).parents[1] / "lib/framework/WiFiStaticBuffers.h").read_text()
allocator = "JsonDocument doc(PsychicJsonAllocator::instance());"
no_clients = "if (!client && _handler.count() == 0) return;"

assert allocator in source, "WebSocket snapshots must allocate JSON in PSRAM"
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
assert "-D WIFI_USE_STATIC_BUFFERS=1" in pico_config
assert "-include lib/framework/WiFiStaticBuffers.h" in pico_config
assert "#define CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM 8" in wifi_buffer_config
assert "#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE 0" in wifi_buffer_config
assert "#undef CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER" in wifi_buffer_config
