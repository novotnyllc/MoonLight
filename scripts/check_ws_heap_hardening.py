from pathlib import Path


source = (Path(__file__).parents[1] / "src/MoonBase/SharedWebSocketServer.h").read_text()
request_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicRequest.cpp").read_text()
websocket_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicWebSocket.cpp").read_text()
server_source = (Path(__file__).parents[1] / "lib/PsychicHttp/src/PsychicHttpServer.cpp").read_text()
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
