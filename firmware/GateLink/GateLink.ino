// GateLink: LoRa bridge between an Alarm.com/Shelly Wave 1 at the house and a
// LiftMaster CSW24UL gate opener. One firmware for both boards; the role (house or
// gate) is chosen in the stored config via the web UI.
//
// Board: Arduino MKR WAN 1310 + MKR Relay Proto Shield.
// Build: arduino-cli compile --fqbn arduino:samd:mkrwan1310 firmware/GateLink
#include <Adafruit_SleepyDog.h>
#include "pins.h"
#include "config.h"
#include "radio.h"
#include "link.h"
#include "io.h"
#include "log.h"
#include "roles.h"
#include "console.h"
#include "app.h"

Input in1, in2;
Relay k1, k2;

static uint16_t pingId = 0;
static uint32_t pingAt = 0;

const char *gateStateName(uint8_t s) {
  static const char *const n[] = { "unknown", "closed", "open", "between", "fault" };
  return s < 5 ? n[s] : "?";
}

const char *causeName(uint8_t c) {
  static const char *const n[] = { "none", "lora", "external" };
  return c < 3 ? n[c] : "?";
}

const char *resultName(uint8_t r) {
  static const char *const n[] = { "none", "reached", "timeout", "already" };
  return r < 4 ? n[r] : "?";
}

static void onRx(const RxMsg &m) {
  if (m.type == MSG_PING && m.len >= 2) {
    uint8_t p[5];
    memcpy(p, m.payload, 2);
    putU16(p + 2, (uint16_t)m.rssi);
    p[4] = (uint8_t)(int8_t)m.snr;
    linkSend(MSG_PONG, p, 5);
    return;
  }
  if (m.type == MSG_PONG && m.len >= 5) {
    uint16_t id = getU16(m.payload);
    if (id == pingId) {
      consoleEventPong(id, millis() - pingAt, m.rssi, m.snr, (int16_t)getU16(m.payload + 2), (int8_t)m.payload[4]);
    }
    return;
  }
  if (activeRole == ROLE_HOUSE) houseOnRx(m);
  else if (activeRole == ROLE_GATE) gateOnRx(m);
}

static void onAck(Slot slot, uint8_t type, bool acked, uint8_t result) {
  if (activeRole == ROLE_HOUSE) houseOnAck(slot, type, acked, result);
  else if (activeRole == ROLE_GATE) gateOnAck(slot, type, acked, result);
}

void appRestartRadio() {
  if (!radioBegin()) logEvent(EV_RADIO_FAIL);
  linkBegin(onRx, onAck);
}

bool appPing() {
  if (!radioOk() || activeRole == ROLE_UNSET || !cfg.key_set) return false;
  uint8_t p[2];
  putU16(p, ++pingId);
  pingAt = millis();
  linkSend(MSG_PING, p, 2);
  return true;
}

void appRelayTest(uint8_t k, uint32_t ms) {
  if (activeRole == ROLE_HOUSE) houseRelayTest(k, ms);
  else if (activeRole == ROLE_GATE) gateRelayTest(k, ms);
}

void appFillStatus(JsonObject o) {
  uint32_t now = millis();
  o["fw"] = FW_VERSION;
  o["role"] = activeRole == ROLE_HOUSE ? "house" : activeRole == ROLE_GATE ? "gate" : "unset";
  o["reboot_pending"] = cfg.role != activeRole;
  o["uptime_ms"] = now;
  o["radio_ok"] = radioOk();
  o["key_set"] = (bool)cfg.key_set;
  JsonObject io = o["io"].to<JsonObject>();
  io["in1"] = in1.active();
  io["in2"] = in2.active();
  io["k1"] = k1.on();
  io["k2"] = k2.on();
  const LinkStats &st = linkStats();
  JsonObject l = o["link"].to<JsonObject>();
  l["verified"] = linkPeerVerified();
  l["age_ms"] = st.lastRxAt ? (int32_t)(now - st.lastRxAt) : -1;
  l["rssi"] = st.lastRssi;
  l["snr"] = st.lastSnr;
  l["tx"] = st.tx;
  l["rx"] = st.rx;
  l["retries"] = st.retries;
  l["giveups"] = st.giveups;
  l["mac_fail"] = st.macFail;
  l["replay"] = st.replay;
  l["sessions"] = st.sessions;
  if (activeRole == ROLE_HOUSE) houseStatus(o);
  else if (activeRole == ROLE_GATE) gateStatus(o);
}

// Smooth 0..peak..0 bump over [start, start + len), squared for a perceptually even fade.
static uint8_t ledBump(uint32_t t, uint32_t start, uint32_t len, uint32_t peak) {
  if (t < start || t >= start + len) return 0;
  uint32_t x = (t - start) * 512 / len;  // 0..511
  uint32_t tri = x < 256 ? x : 511 - x;  // 0..255..0
  return tri * tri * peak / (255 * 255);
}

static void updateLed(uint32_t now) {
  // Unset role: solid. Link good: slow breathing. No link: lub-dub heartbeat.
  static int lastLevel = -1;
  uint8_t level;
  if (activeRole == ROLE_UNSET) {
    level = 255;
  } else {
    const LinkStats &st = linkStats();
    bool up = st.lastRxAt && now - st.lastRxAt < (uint32_t)cfg.link_timeout_s * 1000;
    if (up) {
      level = ledBump(now % 4000, 0, 4000, 255);
    } else {
      uint32_t t = now % 1400;
      level = ledBump(t, 0, 200, 255) + ledBump(t, 260, 240, 140);
    }
  }
  if (level != lastLevel) {
    analogWrite(LED_BUILTIN, level);
    lastLevel = level;
  }
}

void setup() {
  // Relays first so outputs are de-energized as early as possible.
  k1.begin(PIN_K1);
  k2.begin(PIN_K2);
  pinMode(LED_BUILTIN, OUTPUT);

  consoleBegin();
  bool loaded = configLoad();
  activeRole = cfg.role;
  in1.begin(PIN_IN1, cfg.in1_invert);
  in2.begin(PIN_IN2, cfg.in2_invert);
  logEvent(EV_BOOT, loaded, activeRole);

  if (activeRole != ROLE_UNSET) {
    appRestartRadio();
    if (activeRole == ROLE_HOUSE) houseBegin();
    else gateBegin();
  }
  Watchdog.enable(8000);
}

void loop() {
  uint32_t now = millis();
  Watchdog.reset();
  k1.update(now);
  k2.update(now);
  consolePoll();
  if (activeRole != ROLE_UNSET) {
    linkPoll(now);
    if (activeRole == ROLE_HOUSE) houseLoop(now);
    else gateLoop(now);
  }
  updateLed(now);
}
