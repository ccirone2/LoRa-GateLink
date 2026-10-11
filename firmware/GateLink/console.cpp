#include "console.h"
#include "console_io.h"
#include "app.h"
#include "board.h"
#include "config.h"
#include "crc32.h"
#include "extflash.h"
#include "link.h"
#include "roles.h"
#include "history.h"
#include "radio.h"

#define LINE_MAX 1024  // a config.set with every param fits (a full import after a firmware upload)

// One console port, with its own request line so bytes from one can't garble a request on the other.
struct ConsolePort {
  uint8_t id;  // ConsolePortId
  char line[LINE_MAX];
  size_t len;
  bool overflow;
  // If the host doesn't take a USB packet within 70 ms, the rest of the line is dropped (console_io.cpp). The cut
  // line has no newline, so the next line would run into it and be lost too: start the next one with a newline
  // instead, so the host discards only the cut line.
  bool lineCut;
  bool held;  // line holds a request that waits for the relays (blocksLoop)
};

static ConsolePort usbPort = { CON_USB, {}, 0, false, false, false };
static ConsolePort uartPort = { CON_UART, {}, 0, false, false, false };
static bool uartOn = false;

// ArduinoJson writer: serializes straight onto one port's line.
struct LineWriter {
  uint8_t port;
  size_t write(uint8_t c) {
    conIoLineWrite(port, &c, 1);
    return 1;
  }
  size_t write(const uint8_t *data, size_t n) {
    conIoLineWrite(port, data, n);
    return n;
  }
};

static uint32_t usbCutLines = 0;

// A reply goes back to the port that asked (`to`), events to every open port.
static void send(JsonDocument &doc, ConsolePort *to = nullptr) {
  ConsolePort *const ports[2] = { &usbPort, &uartPort };
  static char out[4096];
  size_t n = measureJson(doc);
  bool fits = n < sizeof(out);  // else stream it
  bool serialized = false;
  for (ConsolePort *p : ports) {
    if ((to && p != to) || !conIoOpen(p->id)) continue;
    LineWriter w = { p->id };
    if (p->lineCut) w.write('\n');
    if (!fits) {
      serializeJson(doc, w);
    } else {
      if (!serialized) serializeJson(doc, out, sizeof(out));  // once for both ports
      serialized = true;
      w.write((const uint8_t *)out, n);
    }
    w.write('\n');
    p->lineCut = !conIoLineEnd(p->id);
    if (p->lineCut && p->id == CON_USB) usbCutLines++;
  }
}

uint32_t consoleUsbCutLines() {
  return usbCutLines;
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

// key_set, and the key's id (null without a key), which tells keys apart without revealing them.
static void fillKey(JsonDocument &res) {
  res["key_set"] = (bool)cfg.key_set;
  if (cfg.key_set) {
    char id[9];
    configKeyId(cfg.key, id);
    res["key_id"] = id;
  } else {
    res["key_id"] = nullptr;
  }
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
  fillKey(res);
}

static void saveFailed(JsonDocument &res) {
  res["ok"] = false;
  res["error"] = "flash write failed";
}

static void handle(JsonDocument &req, ConsolePort &from) {
  JsonDocument res;
  res["id"] = req["id"];
  res["ok"] = true;
  const char *cmd = req["cmd"] | "";

  if (!strcmp(cmd, "info")) {
    res["fw"] = fwVersion();
    res["board"] = "MKR WAN 1310";
    res["role"] = roleName(activeRole);
    res["saved_role"] = roleName(cfg.role);
    fillKey(res);
    res["cfg_store"] = configStoreName();
    char id[9];
    snprintf(id, sizeof(id), "%06lx", (unsigned long)extFlashId());
    res["flash_id"] = id;
    res["boot_count"] = appBootCount();
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
    if (!configSave()) saveFailed(res);
  } else if (!strcmp(cmd, "config.reset")) {
    if (!configFactoryReset()) saveFailed(res);
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
    if (!ok) {
      res["ok"] = false;
      res["error"] = "key must be 32 hex chars";
    } else if (configKeyWeak(key)) {
      res["ok"] = false;
      res["error"] = "weak key: all bytes equal, counting by one, or 8 or fewer distinct bytes";
    } else {
      memcpy(cfg.key, key, 16);
      cfg.key_set = 1;
      if (!configSaveKey()) saveFailed(res);
      appRestartRadio();  // new key: re-establish sessions
    }
  } else if (!strcmp(cmd, "relay.test")) {
    // Read wide: as a uint8_t, k 257 would be K1. And `| default` stands in for a value that doesn't fit, so
    // ms 2^32 + 500 would be the default 500: one given must be an int32.
    int32_t k = req["k"] | 0;
    int32_t ms = req["ms"].isNull() ? 500 : req["ms"].is<int32_t>() ? req["ms"].as<int32_t>() : -1;
    if ((k != 1 && k != 2) || ms < 50 || ms > 5000 || activeRole == ROLE_UNSET) {
      res["ok"] = false;
      res["error"] = "k must be 1|2, ms 50..5000, role set";
    } else if (!appRelayTest((uint8_t)k, (uint32_t)ms)) {
      res["ok"] = false;
      res["error"] = "busy";  // gate: that relay is pulsing already, and a running pulse is never restarted
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
    } else if (activeRole != ROLE_HOUSE || !p || !(p->flags & P_REMOTE) || !paramValid(p, v)) {
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
  } else if (!strcmp(cmd, "hist.get")) {
    histGet(res.as<JsonObject>(), req["from"] | -1, req["n"] | HIST_PAGE);
  } else if (!strcmp(cmd, "hist.clear")) {
    if (!histClear(req["period_s"] | histPeriod())) {
      res["ok"] = false;
      res["error"] = "period_s must be 60..3600";
    }
  } else if (!strcmp(cmd, "reboot")) {
    send(res, &from);
    conIoFlush(from.id);
    delay(100);
    boardReset();
    return;  // on the board it never does
  } else if (!strcmp(cmd, "identify")) {
    uint32_t ms = req["ms"] | 6000;
    appIdentify(ms > 60000 ? 60000 : ms);
  } else if (!strcmp(cmd, "debug.replay")) {
    res["sent"] = linkDebugReplay(req["hello"] | false);
  } else if (!strcmp(cmd, "debug.mute")) {
    uint32_t ms = req["ms"] | 0;
    linkDebugMute(ms > 60000 ? 60000 : ms);
  } else if (!strcmp(cmd, "debug.reboot_after_cmd")) {
    if (activeRole != ROLE_GATE) {
      res["ok"] = false;
      res["error"] = "gate node only";
    } else {
      gateDebugRebootAfterCmd();
    }
  } else {
    res["ok"] = false;
    res["error"] = "unknown cmd";
  }
  send(res, &from);
}

void consoleBegin() {
  conIoBegin();
}

void consoleConfigure() {
  bool on = cfg.uart_console;
  if (on == uartOn) return;
  uartOn = on;
  conIoUart(on);
  // While we were unpowered the adapter could pick up junk (its own TX leaking through our pins): start on a
  // fresh line, so it doesn't swallow the boot event.
  if (on) uartPort.lineCut = true;
}

// Reply to a request we couldn't parse, with its id if one can be found in the raw text, so the caller gets
// the error instead of waiting for a timeout.
static void sendError(ConsolePort &from, const char *raw, const char *error) {
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
  send(res, &from);
}

// A request may end with a CRC-32 of itself, `...,"crc":"89abcdef"}`, taken over the line as it reads without
// that member (up to its comma, then the closing brace). On the UART, where ~1 % of requests arrived garbled
// on the bench (some still valid JSON with a number changed), it is required, so a damaged request is refused
// rather than run. Returns 1 if the line has a matching CRC, 0 if it has none, -1 if it has one that doesn't match.
static int checkCrc(const char *line, size_t len) {
  static const char head[] = ",\"crc\":\"";
  const size_t suffix = sizeof(head) - 1 + 8 + 2;  // ,"crc":"xxxxxxxx"}
  if (len < suffix + 1) return 0;
  const char *s = line + len - suffix;
  if (strncmp(s, head, sizeof(head) - 1) || strcmp(s + suffix - 2, "\"}")) return 0;
  uint32_t want = 0;
  for (int i = 0; i < 8; i++) {
    int v = hexVal(s[sizeof(head) - 1 + i]);
    if (v < 0) return -1;
    want = (want << 4) | v;
  }
  return crc32("}", 1, crc32(line, s - line)) == want ? 1 : -1;
}

// Requests that save to flash or restart the radio, which blocks the loop for up to ~1 s: while a relay pulses they
// wait (appRelaysPulsing), or the pulse would be held that much longer. config.set restarts the radio for a radio
// param; it waits whatever it sets, which costs at most one pulse. reboot waits 100 ms after its reply before the
// reset drops the relays.
static bool blocksLoop(const char *cmd) {
  return !strcmp(cmd, "config.set") || !strcmp(cmd, "config.save") || !strcmp(cmd, "config.reset")
         || !strcmp(cmd, "key.set") || !strcmp(cmd, "reboot");
}

// At most one request per call: with the host sending requests back to back, handling them while bytes kept
// arriving never returned to loop(), and the watchdog reset the board (house, 4 status requests in flight). The
// rest waits in the port's buffer (USB holds it back from the host when that's full).
static void poll(ConsolePort &p) {
  if (p.held && appRelaysPulsing()) return;
  while (!p.held) {
    int r = conIoRead(p.id);
    if (r < 0) break;
    char c = (char)r;
    if (c == '\r') continue;
    if (c != '\n') {
      if (p.len < LINE_MAX - 1) p.line[p.len++] = c;
      else p.overflow = true;
      continue;
    }
    p.line[p.len] = 0;
    p.held = true;  // a complete line, handled below
  }
  if (!p.held) return;
  if (p.overflow) {
    sendError(p, p.line, "line too long");
  } else if (p.len) {
    int crc = checkCrc(p.line, p.len);
    JsonDocument req;
    if (crc < 0) {
      sendError(p, p.line, "bad crc");
    } else if (crc == 0 && p.id != CON_USB) {
      sendError(p, p.line, "crc required");
    } else if (deserializeJson(req, p.line) == DeserializationError::Ok) {
      if (blocksLoop(req["cmd"] | "") && appRelaysPulsing()) return;  // keep it until the pulse is over
      handle(req, p);
    } else {
      sendError(p, p.line, "bad json");
    }
  }
  p.len = 0;
  p.overflow = false;
  p.held = false;
}

void consolePoll() {
  consoleConfigure();  // uart_console may have just been set
  poll(usbPort);
  if (uartOn) poll(uartPort);
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
  doc["fei"] = radioLastFei();  // the pong was the last frame received
  send(doc);
}

void consoleEventDiag(const uint8_t *p, uint8_t len) {
  if (len < DIAG_HDR) return;
  JsonDocument doc;
  doc["event"] = "remote_diag";
  char fw[16];
  snprintf(fw, sizeof(fw), "%u.%u.%u", p[DIAG_FW], p[DIAG_FW + 1], p[DIAG_FW + 2]);
  doc["fw"] = fw;
  doc["uptime_s"] = getU32(p + DIAG_UPTIME);
  static const char *const names[] = { "tx", "rx", "mac_fail", "replay", "retries", "giveups" };  // DiagCounter order
  static_assert(sizeof(names) / sizeof(names[0]) == DC_COUNT, "a name per DIAG counter");
  JsonObject c = doc["counters"].to<JsonObject>();
  for (int i = 0; i < DC_COUNT; i++) c[names[i]] = getU16(p + DIAG_COUNTERS + 2 * i);
  JsonObject params = doc["params"].to<JsonObject>();
  for (uint8_t n = DIAG_HDR; n + 5 <= len; n += 5) {
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
  doc["applied"] = acked && (result == RES_OK || result == RES_NOT_SAVED);
  send(doc);
}
