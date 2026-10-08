#include "console.h"
#include "app.h"
#include "config.h"
#include "extflash.h"
#include "link.h"
#include "roles.h"
#include "history.h"
#include "radio.h"

#define LINE_MAX 1024  // a config.set with every param fits (a full import after a firmware upload)
// Second console on Serial1 (uart_console), for bench power tests: a USB-to-UART adapter stays on the PC when the
// board loses power, so it sees the boot right away. Serial1 writes block once its 256-byte buffer is full (a ~5 KB
// config.get reply takes ~200 ms at 250 kbaud). Not faster: at 1 Mbaud ~4 % of requests arrived garbled on the
// bench (bad json), whatever the interrupt priority; at 250 kbaud none did with the board idle.
#define UART_BAUD 250000

// One console port, with its own request line so bytes from one can't garble a request on the other.
struct ConsolePort {
  Stream &io;
  bool usb;
  char line[LINE_MAX];
  size_t len;
  bool overflow;
  // Lines go out whole packets at a time (LineWriter): serialized straight to Serial, every character was its
  // own USB transfer, and single bytes went missing on the bench (lines like `{"event":"lo",...`). If the host
  // doesn't take a USB packet within 70 ms, the rest of the line is dropped. The cut line has no newline, so the
  // next line would run into it and be lost too: start the next one with a newline instead, so the host
  // discards only the cut line.
  bool lineCut;
};

static ConsolePort usbPort = { Serial, true, {}, 0, false, false };
static ConsolePort uartPort = { Serial1, false, {}, 0, false, false };
static bool uartOn = false;

static bool writable(const ConsolePort &p) {
  return p.usb ? Serial.dtr() : uartOn;  // not Serial's bool operator: it delays 10 ms
}

// How long a USB packet may wait for the host to take the previous one, as in the SAMD core.
#define USB_TX_TIMEOUT_MS 70

// The USB data IN endpoint (the bulk IN one). Its BK1RDY bit is set while a packet waits for the host.
static UsbDeviceEndpoint *usbInEndpoint() {
  for (int ep = 1; ep < 8; ep++)
    if (USB->DEVICE.DeviceEndpoint[ep].EPCFG.bit.EPTYPE1 == 3) return &USB->DEVICE.DeviceEndpoint[ep];
  return nullptr;
}

// Writes one line to a port in 64-byte pieces, and to USB each only once the host has taken the previous one.
// Handed a longer write, the core's USBDevice.send() waits between packets for the endpoint's transfer-complete
// flag, which its USB interrupt also clears (the CDC IN endpoint has no handler, so the interrupt acks all its
// flags; it runs at every 1 ms start of frame): when the interrupt got there first, send() waited out its 70 ms
// and dropped the rest of the line. A single packet onto an idle endpoint never waits. BK1RDY is cleared only by
// the hardware.
class LineWriter : public Print {
 public:
  explicit LineWriter(ConsolePort &p) : port(p) {}
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t n) override {
    for (size_t i = 0; i < n; i++) {
      buf[len++] = data[i];
      if (len == sizeof(buf)) push();
    }
    return n;
  }
  // Writes what's left; false if any of the line was lost.
  bool end() {
    push();
    return ok;
  }

 private:
  void push() {
    if (ok && len) ok = port.usb ? usbPacket() : port.io.write(buf, len) == len;
    len = 0;
  }
  bool usbPacket() {
    // Once the host has left a packet for USB_TX_TIMEOUT_MS, later ones don't wait until it takes that one (as
    // in the core), or every line would block the loop that long.
    static bool stalled = false;
    UsbDeviceEndpoint *ep = usbInEndpoint();
    if (!ep) return false;
    uint32_t t0 = millis();
    while (ep->EPSTATUS.bit.BK1RDY) {
      if (stalled || elapsed(millis(), t0, USB_TX_TIMEOUT_MS)) {
        stalled = true;
        return false;
      }
    }
    stalled = false;
    // On a failed send the core returns -1, which Serial.write passes on as a huge count.
    return Serial.write(buf, len) == len;
  }
  ConsolePort &port;
  uint8_t buf[64];
  size_t len = 0;
  bool ok = true;
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
    if ((to && p != to) || !writable(*p)) continue;
    LineWriter w(*p);
    if (p->lineCut) w.write('\n');
    if (!fits) {
      serializeJson(doc, w);
    } else {
      if (!serialized) serializeJson(doc, out, sizeof(out));  // once for both ports
      serialized = true;
      w.write((const uint8_t *)out, n);
    }
    w.write('\n');
    p->lineCut = !w.end();
    if (p->lineCut && p->usb) usbCutLines++;
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
    res["key_set"] = (bool)cfg.key_set;
    res["cfg_store"] = configStoreName();
    char id[7];
    snprintf(id, sizeof(id), "%06lx", (unsigned long)extFlashId());
    res["flash_id"] = id;
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
    if (ok) {
      memcpy(cfg.key, key, 16);
      cfg.key_set = 1;
      if (!configSaveKey()) saveFailed(res);
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
  } else if (!strcmp(cmd, "hist.get")) {
    histGet(res.as<JsonObject>(), req["from"] | -1, req["n"] | HIST_PAGE);
  } else if (!strcmp(cmd, "hist.clear")) {
    if (!histClear(req["period_s"] | histPeriod())) {
      res["ok"] = false;
      res["error"] = "period_s must be 60..3600";
    }
  } else if (!strcmp(cmd, "reboot")) {
    send(res, &from);
    from.io.flush();
    delay(100);
    NVIC_SystemReset();
  } else if (!strcmp(cmd, "identify")) {
    uint32_t ms = req["ms"] | 6000;
    appIdentify(ms > 60000 ? 60000 : ms);
  } else if (!strcmp(cmd, "debug.replay")) {
    linkDebugReplay();
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
  Serial.begin(115200);
}

void consoleConfigure() {
  bool on = cfg.uart_console;
  if (on == uartOn) return;
  uartOn = on;
  if (!on) {
    Serial1.end();
    return;
  }
  Serial1.begin(UART_BAUD);
  // While we were unpowered the adapter could pick up junk (its own TX leaking through our pins): start on a
  // fresh line, so it doesn't swallow the boot event.
  uartPort.lineCut = true;
  // Pull RX up, so an unplugged adapter reads as an idle line rather than noise.
  const PinDescription &rx = g_APinDescription[PIN_SERIAL1_RX];
  PORT->Group[rx.ulPort].PINCFG[rx.ulPin].bit.PULLEN = 1;
  PORT->Group[rx.ulPort].OUTSET.reg = 1ul << rx.ulPin;
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

// CRC-32 (IEEE 802.3, as zlib's crc32), four bits at a time.
static uint32_t crc32(const char *p, size_t n, uint32_t crc = 0) {
  static const uint32_t T[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint8_t)p[i];
    crc = (crc >> 4) ^ T[crc & 15];
    crc = (crc >> 4) ^ T[crc & 15];
  }
  return ~crc;
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

// At most one request per call: with the host sending requests back to back, handling them while bytes kept
// arriving never returned to loop(), and the watchdog reset the board (house, 4 status requests in flight). The
// rest waits in the port's buffer (USB holds it back from the host when that's full).
static void poll(ConsolePort &p) {
  while (p.io.available()) {
    char c = p.io.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (p.len < LINE_MAX - 1) p.line[p.len++] = c;
      else p.overflow = true;
      continue;
    }
    p.line[p.len] = 0;
    if (p.overflow) {
      sendError(p, p.line, "line too long");
    } else if (p.len) {
      int crc = checkCrc(p.line, p.len);
      JsonDocument req;
      if (crc < 0) sendError(p, p.line, "bad crc");
      else if (crc == 0 && !p.usb) sendError(p, p.line, "crc required");
      else if (deserializeJson(req, p.line) == DeserializationError::Ok) handle(req, p);
      else sendError(p, p.line, "bad json");
    }
    p.len = 0;
    p.overflow = false;
    return;
  }
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
  doc["applied"] = acked && (result == RES_OK || result == RES_NOT_SAVED);
  send(doc);
}
