#include "PsychicHandler.h"

PsychicHandler::PsychicHandler() :
  _filter(NULL),
  _server(NULL),
  _username(""),
  _password(""),
  _method(DIGEST_AUTH),
  _realm(""),
  _authFailMsg(""),
  _subprotocol(""),
  _clientsMutex(xSemaphoreCreateRecursiveMutex())
  {}

PsychicHandler::~PsychicHandler() {
  // actual PsychicClient deletion handled by PsychicServer
  // for (PsychicClient *client : _clients)
  //   delete(client);
  lockClients();
  _clients.clear();
  unlockClients();
  if (_clientsMutex) vSemaphoreDelete(_clientsMutex);
}

void PsychicHandler::lockClients() {
  if (_clientsMutex) xSemaphoreTakeRecursive(_clientsMutex, portMAX_DELAY);
}

void PsychicHandler::unlockClients() {
  if (_clientsMutex) xSemaphoreGiveRecursive(_clientsMutex);
}

PsychicHandler* PsychicHandler::setFilter(PsychicRequestFilterFunction fn) {
  _filter = fn;
  return this;
}

bool PsychicHandler::filter(PsychicRequest *request){
  return _filter == NULL || _filter(request);
}

void PsychicHandler::setSubprotocol(const String& subprotocol) {
    this->_subprotocol = subprotocol;
}
const char* PsychicHandler::getSubprotocol() const {
    return _subprotocol.c_str();
}

PsychicHandler* PsychicHandler::setAuthentication(const char *username, const char *password, HTTPAuthMethod method, const char *realm, const char *authFailMsg) {
  _username = String(username);
  _password = String(password);
  _method = method;
  _realm = String(realm);
  _authFailMsg = String(authFailMsg);
  return this;
};

bool PsychicHandler::needsAuthentication(PsychicRequest *request) {
  return (_username != "" && _password != "") && !request->authenticate(_username.c_str(), _password.c_str());
}

esp_err_t PsychicHandler::authenticate(PsychicRequest *request) {
  return request->requestAuthentication(_method, _realm.c_str(), _authFailMsg.c_str());
}

PsychicClient * PsychicHandler::checkForNewClient(PsychicClient *client)
{
  PsychicClient *c = PsychicHandler::getClient(client);
  if (c == NULL)
  {
    c = client;
    addClient(c);
    c->isNew = true;
  }
  else
    c->isNew = false;

  return c;
}

void PsychicHandler::checkForClosedClient(PsychicClient *client)
{
  if (hasClient(client))
  {
    closeCallback(client);
    removeClient(client);
  }
}

void PsychicHandler::addClient(PsychicClient *client) {
  lockClients();
  _clients.push_back(client);
  unlockClients();
}

void PsychicHandler::removeClient(PsychicClient *client) {
  lockClients();
  _clients.remove(client);
  unlockClients();
}

PsychicClient * PsychicHandler::getClient(int socket)
{
  lockClients();
  for (PsychicClient *client : _clients)
    if (client->socket() == socket) {
      unlockClients();
      return client;
    }

  //nothing found.
  unlockClients();
  return NULL;
}

PsychicClient * PsychicHandler::getClient(PsychicClient *client) {
  return PsychicHandler::getClient(client->socket());
}

bool PsychicHandler::hasClient(PsychicClient *socket) {
  return PsychicHandler::getClient(socket) != NULL;
}

int PsychicHandler::count() {
  lockClients();
  int clients = _clients.size();
  unlockClients();
  return clients;
}
