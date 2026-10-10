// Entry points of one firmware instance (build/node.so), for the simulated site (world.cpp): the boot and the loop
// as GateLink.ino runs them, and a few shortcuts for tests. Everything else in the .so is the firmware itself; its
// hardware calls resolve to the test program.
#include <ArduinoJson.h>
#include "app.h"
#include "config.h"
#include "log.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT void node_relays_begin() {
  appRelaysBegin();
}

EXPORT void node_setup(uint8_t resetCause, const uint32_t *serial) {
  appSetup(resetCause, serial);
}

EXPORT void node_loop() {
  appLoop();
}

// paramSet: range-checked, unsaved, no radio restart.
EXPORT bool node_param_set(const char *name, int32_t value) {
  const ParamDef *p = paramByName(name);
  return p && paramSet(p, value);
}

EXPORT bool node_param_get(const char *name, int32_t *value) {
  const ParamDef *p = paramByName(name);
  if (!p) return false;
  *value = cfg.*(p->field);
  return true;
}

EXPORT bool node_save() {
  return configSave();
}

EXPORT bool node_set_key(const uint8_t *key) {
  memcpy(cfg.key, key, sizeof(cfg.key));
  cfg.key_set = 1;
  return configSaveKey();
}

EXPORT size_t node_status(char *buf, size_t n) {
  JsonDocument d;
  appFillStatus(d.to<JsonObject>());
  return serializeJson(d, buf, n);
}

EXPORT int32_t node_role() {
  return activeRole;
}

EXPORT void node_log(uint8_t code, int32_t a, int32_t b) {
  logEvent((LogCode)code, a, b);
}
