#include "doctest.h"

#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("EventSocket releases the subscription mutex before socket I/O") {
  std::ifstream source("lib/framework/EventSocket.cpp");
  REQUIRE(source.good());

  const std::string text((std::istreambuf_iterator<char>(source)),
                         std::istreambuf_iterator<char>());
  const size_t function = text.find(
      "void EventSocket::emitEvent(const String& event, const char *output");
  REQUIRE(function != std::string::npos);

  const size_t lock = text.find(
      "xSemaphoreTake(clientSubscriptionsMutex", function);
  const size_t unlock = text.find(
      "xSemaphoreGive(clientSubscriptionsMutex", lock);
  const size_t send = text.find("_socket.sendTo", lock);

  REQUIRE(lock != std::string::npos);
  REQUIRE(unlock != std::string::npos);
  REQUIRE(send != std::string::npos);
  CHECK(unlock < send);
  CHECK(text.find("std::array<int, CONFIG_LWIP_MAX_SOCKETS>", function) < send);
}
