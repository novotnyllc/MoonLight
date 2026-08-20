#pragma once

#include <cstddef>
#include <cstdio>
#include <ArduinoJson.h>

template <size_t N>
void copyJsonVariantText(JsonVariantConst value, char (&output)[N]) {
  static_assert(N > 0, "output buffer must not be empty");
  output[0] = '\0';
  if (value.isNull()) return;
  if (value.is<const char*>()) {
    const char* text = value.as<const char*>();
    std::snprintf(output, N, "%s", text ? text : "");
  } else {
    serializeJson(value, output, N);
    output[N - 1] = '\0';
  }
}

/// Remove one object row without invalidating the iterator used to enumerate
/// its members. Fixed-size stack copies keep the key/value valid after the live
/// member is removed, matching ModuleState's Char<20> dispatch contract.
template <size_t KeySize = 20, size_t ValueSize = 20, typename RemovedMemberCallback>
bool removeJsonObjectRow(JsonArray array, size_t index, RemovedMemberCallback&& onRemovedMember) {
  if (index >= array.size()) return false;

  JsonObject row = array[index];
  if (!row["valid"].isNull() && row["valid"].as<bool>()) return false;

  while (row.size() > 0) {
    auto memberIt = row.begin();
    for (auto it = row.begin(); it != row.end(); ++it) {
      if (strcmp(it->key().c_str(), "name") != 0) {
        memberIt = it;
        break;
      }
    }
    JsonPair member = *memberIt;
    JsonString liveKey = member.key();
    char key[KeySize];
    char oldValue[ValueSize];
    std::snprintf(key, KeySize, "%s", liveKey.c_str());
    copyJsonVariantText(member.value(), oldValue);
    row.remove(liveKey);
    onRemovedMember(key, oldValue);
  }
  array.remove(index);
  return true;
}
