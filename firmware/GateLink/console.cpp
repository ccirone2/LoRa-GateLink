#include "console.h"
#include "app.h"
#include "config.h"
#include "link.h"
#include "roles.h"

#define LINE_MAX 1024  // a config.set with every param fits (a full import after a firmware upload)

static char line[LINE_MAX];
static size_t lineLen = 0;
static bool overflow = false;

// One USB write per line: serialized straight to Serial, every character was its own USB transfer, and with the
// radio transmitting asynchronously single bytes went missing on the bench (lines like `{"event":"lo",...`).
static void send(JsonDocument &doc) {
  if (!Serial.dtr()) return;  // not Serial's bool operator: it delays 10 ms
  static char out[4096];
  size_t n = measureJson(doc);
  if (n + 1 > sizeof(out)) {  // doesn't fit: stream it
    serializeJson(doc, Serial);
    Serial.write('\n');
    return;
  }
  serializeJson(doc, out, sizeof(out));
  out[n++] = '\n';
  Serial.write((const uint8_t *)out, n);
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static const char *roleName(int32_t r) {
  return r == ROLE_HOUSE ? "house" : r == ROLE_GATE ? "gate" : "unset";
}

static void fillParams(JsonDocument &res) {
  JsonObject params = res["params"].to<JsonObject>();
  JsonArray meta = res["meta"].to<JsonArray>();
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    const ParamDef &p = PARAMS[i];
    params[p.name] = cfg.*(p.field);
    JsonObject m = meta.add<JsonObject>();
    m["name"] = p.name;
    m["id"] = p.id;
    m["min"] = p.minV;
    m["max"] = p.maxV;
    m["radio"] = (bool)(p.flags & P_RADIO);
    m["remote"] = (bool)(p.flags & P_REMOTE);
    m["reboot"] = (bool)(p.flags & P_REBOOT);
  }
  res["key_set"] = (bool)cfg.key_set;
}

static void handle(JsonDocument &req) {
  JsonDocument res;
  res["id"] = req["id"];
  res["ok"] = true;
  const char *cmd = req["cmd"] | "";

  if (!strcmp(cmd, "info")) {
    res["fw"] = FW_VERSION;
    res["board"] = "MKR WAN 1310";
    res["role"] = roleName(activeRole);
    res["saved_role"] = roleName(cfg.role);
    res["key_set"] = (bool)cfg.key_set;
  } else if (!strcmp(cmd, "status")) {
    appFillStatus(res["status"].to<JsonObject>());
  } else if (!strcmp(cmd, "config.get")) {
    fillParams(res);
  } else if (!strcmp(cmd, "config.set")) {
    JsonObject in = req["params"];
    JsonArray applied = res["applied"].to<JsonArray>();
    JsonArray errors = res["errors"].to<JsonArray>();
    bool radio = false, reboot = false;
    for (JsonPair kv : in) {
      const ParamDef *p = paramByName(kv.key().c_str());
      if (p && kv.value().is<int32_t>() && cfg.*(p->field) == kv.value().as<int32_t>()) continue;
      if (!p || !kv.value().is<int32_t>() || !paramSet(p, kv.value().as<int32_t>())) {
        errors.add(kv.key().c_str());
        continue;
      }
      applied.add(p->name);
      radio |= (p->flags & P_RADIO) != 0;
      reboot |= (p->flags & P_REBOOT) != 0;
    }
    if (radio) appRestartRadio();
    res["reboot_required"] = reboot;
    res["ok"] = errors.size() == 0;
  } else if (!strcmp(cmd, "config.save")) {
    configSave();
  } else if (!strcmp(cmd, "config.reset")) {
    configDefaults(cfg);
    configSave();
    res["reboot_required"] = true;
  } else if (!strcmp(cmd, "key.set")) {
    const char *hex = req["key"] | "";
    uint8_t key[16];
    bool ok = strlen(hex) == 32;
    for (int i = 0; ok && i < 16; i++) {
      int hi = hexVal(hex[2 * i]), lo = hexVal(hex[2 * i + 1]);
      if (hi < 0 || lo < 0) ok = false;
      else key[i] = (hi << 4) | lo;
    }
    if (ok) {
      memcpy(cfg.key, key, 16);
      cfg.key_set = 1;
      configSaveKey();
      appRestartRadio();  // new key: re-establish sessions
    } else {
      res["ok"] = false;
      res["error"] = "key must be 32 hex chars";
    }
  } else if (!strcmp(cmd, "relay.test")) {
    uint8_t k = req["k"] | 0;
    uint32_t ms = req["ms"] | 500;
    if ((k != 1 && k != 2) || ms < 50 || ms > 5000 || activeRole == ROLE_UNSET) {
      res["ok"] = false;
      res["error"] = "k must be 1|2, ms 50..5000, role set";
    } else {
      appRelayTest(k, ms);
    }
  } else if (!strcmp(cmd, "radio.ping")) {
    if (!appPing()) {
      res["ok"] = false;
      res["error"] = "radio not ready, role unset, or no key set";
    }
  } else if (!strcmp(cmd, "remote.diag")) {
    if (activeRole != ROLE_HOUSE) {
      res["ok"] = false;
      res["error"] = "house node only";
    } else {
      houseRemoteDiag();
    }
  } else if (!strcmp(cmd, "remote.set")) {
    const ParamDef *p = paramByName(req["name"] | "");
    int32_t v = req["value"] | 0;
    if (!req["value"].is<int32_t>()) {
      res["ok"] = false;
      res["error"] = "value must be an integer";
    } else if (activeRole != ROLE_HOUSE || !p || !(p->flags & P_REMOTE) || v < p->minV || v > p->maxV) {
      res["ok"] = false;
      res["error"] = "house node only; param must be remote-writable and in range";
    } else if (linkPending(SLOT_CFG)) {
      // A new one would replace it in its slot, and the first would never get its remote_set event.
      res["ok"] = false;
      res["error"] = "busy";
    } else {
      houseRemoteSet(p->id, v);
    }
  } else if (!strcmp(cmd, "log.get")) {
    static LogEntry entries[LOG_SIZE];
    size_t n = logCopy(entries, LOG_SIZE);
    JsonArray arr = res["log"].to<JsonArray>();
    for (size_t i = 0; i < n; i++) {
      JsonObject e = arr.add<JsonObject>();
      e["t"] = entries[i].t;
      e["ev"] = logCodeName(entries[i].code);
      e["a"] = entries[i].a;
      e["b"] = entries[i].b;
    }
    res["now"] = millis();
  } else if (!strcmp(cmd, "reboot")) {
    send(res);
    Serial.flush();
    delay(100);
    NVIC_SystemReset();
  } else if (!strcmp(cmd, "identify")) {
    uint32_t ms = req["ms"] | 6000;
    appIdentify(ms > 60000 ? 60000 : ms);
  } else if (!strcmp(cmd, "debug.replay")) {
    linkDebugReplay();
  } else {
    res["ok"] = false;
    res["error"] = "unknown cmd";
  }
  send(res);
}

void consoleBegin() {
  Serial.begin(115200);
}

// Reply to a request we couldn't parse, with its id if one can be found in the raw text, so the caller gets
// the error instead of waiting for a timeout.
static void sendError(const char *raw, const char *error) {
  JsonDocument res;
  const char *id = strstr(raw, "\"id\"");
  if (id) {
    id = strchr(id + 4, ':');
    char *end;
    long v = id ? strtol(id + 1, &end, 10) : 0;
    if (id && end != id + 1) res["id"] = v;
  }
  res["ok"] = false;
  res["error"] = error;
  send(res);
}

void consolePoll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (lineLen < LINE_MAX - 1) line[lineLen++] = c;
      else overflow = true;
      continue;
    }
    line[lineLen] = 0;
    if (overflow) {
      sendError(line, "line too long");
    } else if (lineLen) {
      JsonDocument req;
      if (deserializeJson(req, line) == DeserializationError::Ok) handle(req);
      else sendError(line, "bad json");
    }
    lineLen = 0;
    overflow = false;
  }
}

void consoleEmitLog(const LogEntry &e) {
  JsonDocument doc;
  doc["event"] = "log";
  doc["t"] = e.t;
  doc["ev"] = logCodeName(e.code);
  doc["a"] = e.a;
  doc["b"] = e.b;
  send(doc);
}

void consoleEventStatus() {
  JsonDocument doc;
  doc["event"] = "status";
  appFillStatus(doc["status"].to<JsonObject>());
  send(doc);
}

void consoleEventPong(uint16_t id, uint32_t rttMs, int16_t rssi, float snr, int16_t peerRssi, int8_t peerSnr) {
  JsonDocument doc;
  doc["event"] = "pong";
  doc["ping_id"] = id;
  doc["rtt_ms"] = rttMs;
  doc["rssi"] = rssi;
  doc["snr"] = snr;
  doc["peer_rssi"] = peerRssi;
  doc["peer_snr"] = peerSnr;
  send(doc);
}

void consoleEventDiag(const uint8_t *p, uint8_t len) {
  if (len < 19) return;
  JsonDocument doc;
  doc["event"] = "remote_diag";
  char fw[16];
  snprintf(fw, sizeof(fw), "%u.%u.%u", p[0], p[1], p[2]);
  doc["fw"] = fw;
  doc["uptime_s"] = getU32(p + 3);
  static const char *const names[6] = { "tx", "rx", "mac_fail", "replay", "retries", "giveups" };
  JsonObject c = doc["counters"].to<JsonObject>();
  for (int i = 0; i < 6; i++) c[names[i]] = getU16(p + 7 + 2 * i);
  JsonObject params = doc["params"].to<JsonObject>();
  for (uint8_t n = 19; n + 5 <= len; n += 5) {
    const ParamDef *def = paramById(p[n]);
    if (def) params[def->name] = (int32_t)getU32(p + n + 1);
  }
  send(doc);
}

void consoleEventRemoteSet(bool acked, uint8_t result) {
  JsonDocument doc;
  doc["event"] = "remote_set";
  doc["acked"] = acked;
  doc["ok"] = acked && result == RES_OK;
  send(doc);
}
