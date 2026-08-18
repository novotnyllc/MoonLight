#include <EventSocket.h>

SemaphoreHandle_t clientSubscriptionsMutex = xSemaphoreCreateMutex();
SemaphoreHandle_t eventSerializationMutex = xSemaphoreCreateMutex();

EventSocket::EventSocket(PsychicHttpServer *server,
                         SecurityManager *securityManager,
                         AuthenticationPredicate authenticationPredicate) : _server(server),
                                                                            _securityManager(securityManager),
                                                                            _authenticationPredicate(authenticationPredicate)
{
}

void EventSocket::begin()
{
    _socket.setFilter(_securityManager->filterRequest(_authenticationPredicate));
    _socket.onOpen((std::bind(&EventSocket::onWSOpen, this, std::placeholders::_1)));
    _socket.onClose(std::bind(&EventSocket::onWSClose, this, std::placeholders::_1));
    _socket.onFrame(std::bind(&EventSocket::onFrame, this, std::placeholders::_1, std::placeholders::_2));
    _server->on(EVENT_SERVICE_PATH, &_socket);

    // 🌙 Set up handler for INCOMING client updates from clients
    registerEvent(EVENT_CLIENT_INFO);
    onEvent(EVENT_CLIENT_INFO, 
        [this](JsonObject &data, int originId) { 
            handleClientInfo(data, originId); 
        });

    ESP_LOGV(SVK_TAG, "Registered event socket endpoint: %s", EVENT_SERVICE_PATH);
}

void EventSocket::registerEvent(String event)
{
    if (!isEventValid(event))
    {
        ESP_LOGD(SVK_TAG, "Registering event: %s", event.c_str());
        events.push_back(event);
    }
    else
    {
        ESP_LOGW(SVK_TAG, "Event already registered: %s", event.c_str());
    }
}

void EventSocket::onWSOpen(PsychicWebSocketClient *client)
{
    ESP_LOGI(SVK_TAG, "ws[%s][%u] connect", client->remoteIP().toString().c_str(), client->socket());
}

void EventSocket::onWSClose(PsychicWebSocketClient *client)
{
    xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
    for (auto &event_subscriptions : client_subscriptions)
    {
        event_subscriptions.second.remove(client->socket());
    }
     _clientVisibility.erase((int)client->socket()); // 🌙
    xSemaphoreGive(clientSubscriptionsMutex);
    ESP_LOGI(SVK_TAG, "ws[%s][%u] disconnect", client->remoteIP().toString().c_str(), client->socket());
}

esp_err_t EventSocket::onFrame(PsychicWebSocketRequest *request, httpd_ws_frame *frame)
{
    ESP_LOGV(SVK_TAG, "ws[%s][%u] opcode[%d]", request->client()->remoteIP().toString().c_str(),
             request->client()->socket(), frame->type);

    JsonDocument doc(PsychicJsonAllocator::instance());
#if FT_ENABLED(EVENT_USE_JSON)
    if (frame->type == HTTPD_WS_TYPE_TEXT)
    {
        ESP_LOGV(SVK_TAG, "ws[%s][%u] request: %s", request->client()->remoteIP().toString().c_str(),
                 request->client()->socket(), (char *)frame->payload);

        DeserializationError error = deserializeJson(doc, (char *)frame->payload, frame->len);
#else
    if (frame->type == HTTPD_WS_TYPE_BINARY)
    {
        ESP_LOGV(SVK_TAG, "ws[%s][%u] request: %s", request->client()->remoteIP().toString().c_str(),
                 request->client()->socket(), (char *)frame->payload);

        DeserializationError error = deserializeMsgPack(doc, (char *)frame->payload, frame->len);
#endif

        if (!error && doc.is<JsonObject>())
        {
            String event = doc["event"];
            if (event == "subscribe")
            {
                // only subscribe to events that are registered
                if (isEventValid(doc["data"].as<String>()))
                {
                    xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
                    client_subscriptions[doc["data"]].push_back(request->client()->socket());
                    xSemaphoreGive(clientSubscriptionsMutex);
                    handleSubscribeCallbacks(doc["data"], String(request->client()->socket()));
                }
                else
                {
                    ESP_LOGW(SVK_TAG, "Client tried to subscribe to unregistered event: %s", doc["data"].as<String>().c_str());
                }
            }
            else if (event == "unsubscribe")
            {
                xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
                client_subscriptions[doc["data"]].remove(request->client()->socket());
                xSemaphoreGive(clientSubscriptionsMutex);
            }
            else
            {
                JsonObject jsonObject = doc["data"].as<JsonObject>();
                handleEventCallbacks(event, jsonObject, request->client()->socket());
            }
            return ESP_OK;
        }
        ESP_LOGW(SVK_TAG, "Error[%d] parsing JSON: %s", error, (char *)frame->payload);
    }
    return ESP_OK;
}

void EventSocket::emitEvent(const String& event, const JsonObject &jsonObject, const char *originId, bool onlyToSameOrigin)
{
    JsonDocument doc;
    doc["event"] = event;
    doc["data"] = jsonObject;

    emitEvent(doc, originId, onlyToSameOrigin);
}

// 🌙 extracted from above function so the caller can prepare the JsonDocument, which saves on heap usage
void EventSocket::emitEvent(const JsonDocument &doc, const char *originId, bool onlyToSameOrigin)
{
    xSemaphoreTake(eventSerializationMutex, portMAX_DELAY);
    #if FT_ENABLED(EVENT_USE_JSON)
        static String outBuffer;      // reused across calls to avoid repeated allocation
        outBuffer.clear();            // keep capacity, reset length
        outBuffer.reserve(measureJson(doc)); // pre-reserve exact size (optional, improves speed for large JSON)
        serializeJson(doc, outBuffer);

        emitEvent(doc["event"], outBuffer.c_str(), outBuffer.length(), originId, onlyToSameOrigin);
    #else
        // --- MsgPack path ---
        size_t outputSize = measureMsgPack(doc);
        uint8_t *outBuffer = static_cast<uint8_t *>(PsychicJsonAllocator::instance()->allocate(outputSize));
        if (!outBuffer) {
            ESP_LOGE(SVK_TAG, "Unable to allocate %zu-byte event buffer", outputSize);
            xSemaphoreGive(eventSerializationMutex);
            return;
        }

        size_t written = serializeMsgPack(doc, outBuffer, outputSize);
        if (written == outputSize) {
            emitEvent(doc["event"], (char *)outBuffer, written, originId, onlyToSameOrigin);
        } else {
            ESP_LOGE(SVK_TAG, "Event serialization wrote %zu of %zu bytes", written, outputSize);
        }
        PsychicJsonAllocator::instance()->deallocate(outBuffer);
    #endif
    xSemaphoreGive(eventSerializationMutex);
}

// 🌙 extracted from above function for FT_MONITOR, which uses char *output
void EventSocket::emitEvent(const String& event, const char *output, size_t len, const char *originId, bool onlyToSameOrigin)
{
    // Only process valid events
    if (!isEventValid(event))
    {
        ESP_LOGW(SVK_TAG, "Method tried to emit unregistered event: %s from %s (len %zu)", event.c_str(), originId, len);
        return;
    }

    int originSubscriptionId = originId[0] ? atoi(originId) : -1;
    xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
    auto &subscriptions = client_subscriptions[event];
    if (subscriptions.empty())
    {
        xSemaphoreGive(clientSubscriptionsMutex);
        return;
    }

    // if onlyToSameOrigin == true, send the message back to the origin
    if (onlyToSameOrigin && originSubscriptionId > 0)
    {
#if FT_ENABLED(EVENT_USE_JSON)
        esp_err_t result = _socket.sendTo(originSubscriptionId, HTTPD_WS_TYPE_TEXT, output, len);
#else
        esp_err_t result = _socket.sendTo(originSubscriptionId, HTTPD_WS_TYPE_BINARY, output, len);
#endif
        if (result != ESP_OK)
        {
            ESP_LOGW(SVK_TAG, "Failed to send event %s from %s to client %d: %s (len: %zu)", event.c_str(), originId, originSubscriptionId, esp_err_to_name(result), len);
        }
    }
    else
    { // else send the message to all other clients

        // 🌙 use iterator so remove / erase also removes from the iterator
        for (auto it = subscriptions.begin(); it != subscriptions.end();)
        {
            int subscription = *it;
            if (subscription == originSubscriptionId)
            {
                ++it;
                continue;
            }
#if FT_ENABLED(EVENT_USE_JSON)
            esp_err_t result = _socket.sendTo(subscription, HTTPD_WS_TYPE_TEXT, output, len);
#else
            esp_err_t result = _socket.sendTo(subscription, HTTPD_WS_TYPE_BINARY, output, len);
#endif
            // 🌙 error check
            if (result != ESP_OK)
            {
                ESP_LOGW(SVK_TAG, "Failed to send event %s from %s to client %u: %s (len: %zu)", event.c_str(), originId, subscription, esp_err_to_name(result), len);
                // it = subscriptions.erase(it);// do not erase as we hope for better times
                it = subscriptions.erase(it);  // remove dead client; don't keep retrying
                continue;
            }
            ++it;
        }
    }

    xSemaphoreGive(clientSubscriptionsMutex);
}

void EventSocket::handleEventCallbacks(String event, JsonObject &jsonObject, int originId)
{
    for (auto &callback : event_callbacks[event])
    {
        callback(jsonObject, originId);
    }
}

void EventSocket::handleSubscribeCallbacks(String event, const String &originId)
{
    for (auto &callback : subscribe_callbacks[event])
    {
        callback(originId);
    }
}

void EventSocket::onEvent(String event, EventCallback callback)
{
    if (!isEventValid(event))
    {
        ESP_LOGW(SVK_TAG, "Method tried to register unregistered event: %s", event.c_str());
        return;
    }
    event_callbacks[event].push_back(callback);
}

void EventSocket::onSubscribe(String event, SubscribeCallback callback)
{
    if (!isEventValid(event))
    {
        ESP_LOGW(SVK_TAG, "Method tried to subscribe to unregistered event: %s", event.c_str());
        return;
    }
    subscribe_callbacks[event].push_back(callback);
    ESP_LOGI(SVK_TAG, "onSubscribe for event: %s", event.c_str());
}

bool EventSocket::isEventValid(String event)
{
    return std::find(events.begin(), events.end(), event) != events.end();
}

unsigned int EventSocket::getConnectedClients()
{
    return (unsigned int)_socket.count();
}

bool EventSocket::hasBroadcastRecipient(const String &event, const String &originId)
{
    xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
    auto subscriptions = client_subscriptions.find(event);
    int originSubscriptionId = originId.toInt();
    bool subscribed = subscriptions != client_subscriptions.end() &&
                      std::any_of(subscriptions->second.begin(), subscriptions->second.end(),
                                  [originSubscriptionId](int subscription) { return subscription != originSubscriptionId; });
    xSemaphoreGive(clientSubscriptionsMutex);
    return subscribed;
}

// 🌙 Client info / visibility / active clients

void EventSocket::handleClientInfo(JsonObject &data, int originId)
{
    bool visible = data["visible"] | false;
    
    xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);
    _clientVisibility[originId] = visible;
    xSemaphoreGive(clientSubscriptionsMutex);

    ESP_LOGD(SVK_TAG, "Client %d visible: %s", originId, visible ? "Yes" : "No");
}

unsigned int EventSocket::getActiveClients() {
  unsigned int count = 0;
  xSemaphoreTake(clientSubscriptionsMutex, portMAX_DELAY);

  for (const auto& pair : _clientVisibility) {
    if (pair.second) count++;
  }

  xSemaphoreGive(clientSubscriptionsMutex);
  return count;
}
