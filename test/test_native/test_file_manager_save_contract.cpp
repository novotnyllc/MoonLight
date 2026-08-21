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

TEST_CASE("FileManager inventory yields and has no backup scan route") {
  std::ifstream source("src/MoonBase/Modules/FileManager.cpp");
  REQUIRE(source.good());

  const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  CHECK(text.find("/rest/FileManagerBackup") == std::string::npos);

  const size_t close = text.find("file.close();");
  REQUIRE(close != std::string::npos);
  const size_t yield = text.find("vTaskDelay(1);", close);
  REQUIRE(yield != std::string::npos);
  CHECK(close < yield);
}

TEST_CASE("successful HTTP chunks yield before the next chunk") {
  std::ifstream source("lib/PsychicHttp/src/PsychicResponse.cpp");
  REQUIRE(source.good());

  const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
  const size_t send = text.find("esp_err_t err = httpd_resp_send_chunk");
  REQUIRE(send != std::string::npos);
  const size_t success = text.find("if (err == ESP_OK)", send);
  const size_t yield = text.find("vTaskDelay(1);", success);
  const size_t failure = text.find("if (err != ESP_OK)", send);
  REQUIRE(success != std::string::npos);
  REQUIRE(yield != std::string::npos);
  REQUIRE(failure != std::string::npos);
  CHECK(send < success);
  CHECK(success < yield);
  CHECK(yield < failure);
}
