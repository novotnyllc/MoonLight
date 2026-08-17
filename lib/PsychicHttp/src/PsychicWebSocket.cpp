#include "PsychicWebSocket.h"

namespace {
  const char* const PSYCHIC_WS_URI_SESSION_KEY = "psychic.ws.uri";
}

/*************************************/
/*  PsychicWebSocketRequest      */
/*************************************/

PsychicWebSocketRequest::PsychicWebSocketRequest(PsychicRequest *req) :
  PsychicRequest(req->server(), req->request()),
  _client(req->client())
{
  if (_uri.isEmpty() && hasSessionKey(PSYCHIC_WS_URI_SESSION_KEY))
    _uri = getSessionKey(PSYCHIC_WS_URI_SESSION_KEY);
}

PsychicWebSocketRequest::~PsychicWebSocketRequest()
{
}

PsychicWebSocketClient * PsychicWebSocketRequest::client() {
  return &_client;
}

esp_err_t PsychicWebSocketRequest::reply(httpd_ws_frame_t * ws_pkt)
{
  return httpd_ws_send_frame(this->_req, ws_pkt);
} 

esp_err_t PsychicWebSocketRequest::reply(httpd_ws_type_t op, const void *data, size_t len)
{
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = (uint8_t*)data;
  ws_pkt.len = len;
  ws_pkt.type = op;

  return this->reply(&ws_pkt);
}

esp_err_t PsychicWebSocketRequest::reply(const char *buf)
{
  return this->reply(HTTPD_WS_TYPE_TEXT, buf, strlen(buf));
}

/*************************************/
/*  PsychicWebSocketClient   */
/*************************************/

PsychicWebSocketClient::PsychicWebSocketClient(PsychicClient *client)
  : PsychicClient(client->server(), client->socket())
{
  isNew = client->isNew;
}

PsychicWebSocketClient::~PsychicWebSocketClient() {
}

esp_err_t PsychicWebSocketClient::sendMessage(httpd_ws_frame_t * ws_pkt)
{
  // 🌙
  // Guard: check socket is still a valid active WebSocket before sending.
  // Without this, httpd_ws_send_frame_async can propagate into lwIP and hard-assert
  // when the underlying netconn is in an invalid/closed state.
  httpd_ws_client_info_t info = httpd_ws_get_fd_info(this->server(), this->socket());
  if (info != HTTPD_WS_CLIENT_WEBSOCKET) {
    ESP_LOGD(PH_TAG, "underlying netconn is in an invalid/closed state.");
    return ESP_FAIL;
  }
  esp_err_t ret = httpd_ws_send_frame_async(this->server(), this->socket(), ws_pkt);
  if (ret != ESP_OK)
  {
    ESP_LOGW(PH_TAG, "WebSocket send failed for fd=%d (%s); closing session", this->socket(), esp_err_to_name(ret));
    close();
  }
  return ret;
} 

esp_err_t PsychicWebSocketClient::sendMessage(httpd_ws_type_t op, const void *data, size_t len)
{
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = (uint8_t*)data;
  ws_pkt.len = len;
  ws_pkt.type = op;

  return this->sendMessage(&ws_pkt);
}

esp_err_t PsychicWebSocketClient::sendMessage(const char *buf)
{
  return this->sendMessage(HTTPD_WS_TYPE_TEXT, buf, strlen(buf));
}

PsychicWebSocketHandler::PsychicWebSocketHandler() :
  PsychicHandler(),
  _onOpen(NULL),
  _onFrame(NULL),
  _onClose(NULL)
  {
  }

PsychicWebSocketHandler::~PsychicWebSocketHandler() {
}

PsychicWebSocketClient * PsychicWebSocketHandler::getClient(int socket)
{
  PsychicClient *client = PsychicHandler::getClient(socket);
  if (client == NULL)
    return NULL;

  if (client->_friend == NULL)
  {
    return NULL;
  }

  return (PsychicWebSocketClient *)client->_friend;
}

PsychicWebSocketClient * PsychicWebSocketHandler::getClient(PsychicClient *client) {
  return getClient(client->socket());
}

void PsychicWebSocketHandler::addClient(PsychicClient *client) {
  lockClients();
  client->_friend = new PsychicWebSocketClient(client);
  PsychicHandler::addClient(client);
  unlockClients();
}

void PsychicWebSocketHandler::removeClient(PsychicClient *client) {
  lockClients();
  PsychicHandler::removeClient(client);
  delete (PsychicWebSocketClient*)client->_friend;
  client->_friend = NULL;
  unlockClients();
}

void PsychicWebSocketHandler::openCallback(PsychicClient *client) {
  PsychicWebSocketClient *buddy = getClient(client);
  if (buddy == NULL)
  {
    return;
  }

  if (_onOpen != NULL)
    _onOpen(getClient(buddy));
}

void PsychicWebSocketHandler::closeCallback(PsychicClient *client) {
  PsychicWebSocketClient *buddy = getClient(client);
  if (buddy == NULL)
  {
    return;
  }

  if (_onClose != NULL)
    _onClose(getClient(buddy));
}

bool PsychicWebSocketHandler::isWebSocket() { return true; }

esp_err_t PsychicWebSocketHandler::handleRequest(PsychicRequest *request)
{
  // beginning of the ws URI handler and our onConnect hook
  if (request->method() == HTTP_GET)
  {
    // A handshake starts a new WebSocket lifetime even when ESP-IDF has already
    // reused the numeric socket descriptor. Replace any stale handler entry now
    // so later broadcasts always target this connection's buddy object.
    PsychicClient *client = PsychicHandler::getClient(request->client());
    if (client != NULL)
    {
      closeCallback(client);
      removeClient(client);
    }
    client = request->client();
    client->isNew = true;
    addClient(client);

    if (!request->url().isEmpty())
      request->setSessionKey(PSYCHIC_WS_URI_SESSION_KEY, request->url());

    openCallback(client);

    return ESP_OK;
  }

  // Keep the handshake's isNew marker until the first data frame consumes it.
  // checkForNewClient() would clear it before SharedWebSocketServer can send the
  // initial snapshot.
  PsychicClient *client = PsychicHandler::getClient(request->client());
  if (client == NULL)
  {
    client = request->client();
    addClient(client);
    client->isNew = true;
  }

  //prep our request
  PsychicWebSocketRequest wsRequest(request);

  //init our memory for storing the packet
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  uint8_t *buf = NULL;

  /* Set max_len = 0 to get the frame len */
  esp_err_t ret = httpd_ws_recv_frame(wsRequest.request(), &ws_pkt, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(PH_TAG, "httpd_ws_recv_frame failed to get frame len with %s", esp_err_to_name(ret));
    return ret;
  }

  //okay, now try to load the packet
  //ESP_LOGD(PH_TAG, "frame len is %d", ws_pkt.len);
  if (ws_pkt.len) {
    /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
    buf = (uint8_t*) calloc(1, ws_pkt.len + 1);
    if (buf == NULL) {
      ESP_LOGE(PH_TAG, "Failed to calloc memory for buf");
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    /* Set max_len = ws_pkt.len to get the frame payload */
    ret = httpd_ws_recv_frame(wsRequest.request(), &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      ESP_LOGE(PH_TAG, "httpd_ws_recv_frame failed with %s", esp_err_to_name(ret));
      free(buf);
      return ret;
    }
    //ESP_LOGD(PH_TAG, "Got packet with message: %s", ws_pkt.payload);
  }

  // Text messages are our payload.
  if (ws_pkt.type == HTTPD_WS_TYPE_TEXT || ws_pkt.type == HTTPD_WS_TYPE_BINARY)
  {
    if (this->_onFrame != NULL)
    {
      ret = this->_onFrame(&wsRequest, &ws_pkt);
      client->isNew = wsRequest.client()->isNew;
    }
  }

  //logging housekeeping
  if (ret != ESP_OK)
    ESP_LOGE(PH_TAG, "httpd_ws_send_frame failed with %s", esp_err_to_name(ret));
    // ESP_LOGD(PH_TAG, "ws_handler: httpd_handle_t=%p, sockfd=%d, client_info:%d", 
    //   request->server(),
    //   httpd_req_to_sockfd(request->request()),
    //   httpd_ws_get_fd_info(request->server()->server, httpd_req_to_sockfd(request->request())));

  //dont forget to release our buffer memory
  free(buf);

  return ret;
}

PsychicWebSocketHandler * PsychicWebSocketHandler::onOpen(PsychicWebSocketClientCallback fn) {
  _onOpen = fn;
  return this;
}

PsychicWebSocketHandler * PsychicWebSocketHandler::onFrame(PsychicWebSocketFrameCallback fn) {
  _onFrame = fn;
  return this;
}

PsychicWebSocketHandler * PsychicWebSocketHandler::onClose(PsychicWebSocketClientCallback fn) {
  _onClose = fn;
  return this;
}

void PsychicWebSocketHandler::sendAll(httpd_ws_frame_t * ws_pkt)
{
  lockClients();
  for (PsychicClient *client : _clients)
  {
    //ESP_LOGD(PH_TAG, "Active client (fd=%d) -> sending async message", client->socket());

    if (client->_friend == NULL)
    {
      continue;
    }

    if (((PsychicWebSocketClient*)client->_friend)->sendMessage(ws_pkt) != ESP_OK)
    {
      // 🌙 continue instead of break
      ESP_LOGD(PH_TAG, "sendAll: skip failed client fd=%d", client->socket());
      continue;
    }
  }
  unlockClients();
}

esp_err_t PsychicWebSocketHandler::sendTo(int socket, httpd_ws_type_t op, const void *data, size_t len)
{
  lockClients();
  PsychicWebSocketClient *client = getClient(socket);
  esp_err_t result = client ? client->sendMessage(op, data, len) : ESP_FAIL;
  unlockClients();
  return result;
}

void PsychicWebSocketHandler::sendAll(httpd_ws_type_t op, const void *data, size_t len)
{
  httpd_ws_frame_t ws_pkt;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

  ws_pkt.payload = (uint8_t*)data;
  ws_pkt.len = len;
  ws_pkt.type = op;

  this->sendAll(&ws_pkt);
}

void PsychicWebSocketHandler::sendAll(const char *buf)
{
  this->sendAll(HTTPD_WS_TYPE_TEXT, buf, strlen(buf));
}
