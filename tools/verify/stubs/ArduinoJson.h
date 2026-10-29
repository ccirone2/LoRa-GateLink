// Stand-in for ArduinoJson for the model checker: the roles' status reports (gateStatus, houseStatus) take a JsonObject,
// and nothing the proofs cover reads one. Assignments are accepted and dropped.
#pragma once
#include <stddef.h>
struct JsonVariant {
  template <typename T> JsonVariant &operator=(const T &) { return *this; }
  template <typename T> T to() { return T(); }
  JsonVariant operator[](const char *) { return JsonVariant(); }
};
struct JsonArray {
  template <typename T> void add(const T &) {}
};
struct JsonObject {
  JsonVariant operator[](const char *) { return JsonVariant(); }
};
