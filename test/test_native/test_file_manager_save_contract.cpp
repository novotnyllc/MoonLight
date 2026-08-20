#include "doctest.h"

#include <fstream>
#include <iterator>
#include <string>

TEST_CASE("safe mode saves are rejected before persistence drains") {
  std::ifstream source("src/MoonBase/Modules/FileManager.cpp");
  REQUIRE(source.good());

  const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  const auto checkRoute = [&](const char* route) {
    const size_t handler = text.find(route);
    REQUIRE(handler != std::string::npos);
    const size_t nextHandler = text.find("_server->on(", handler + 1);
    const size_t guard = text.find("if (safeModeMB)", handler);
    const size_t conflict = text.find("request->reply(409", handler);
    const size_t drain = text.find("writeToFSDelayed('W')", handler);
    const size_t clearDirty = text.find("saveNeeded = false", handler);

    REQUIRE(guard < nextHandler);
    REQUIRE(conflict < nextHandler);
    REQUIRE(drain < nextHandler);
    REQUIRE(clearDirty < nextHandler);
    CHECK(guard < conflict);
    CHECK(conflict < drain);
    CHECK(conflict < clearDirty);
  };

  checkRoute("/rest/saveConfig");
  checkRoute("/rest/saveGolden");
}
