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
#include "supply.h"
#include "roles.h"
#include "console.h"
#include "app.h"
#include "history.h"

Input in1, in2, in3, in4;
static uint8_t resetCause = 0;  // PM->RCAUSE at boot
static bool cfgLoaded = false;
static uint32_t bootCount = 0;
static uint32_t identifyUntil = 0;  // LED strobes until then (0 = off)

static const char *resetCauseName(uint8_t rc) {
  if (rc & PM_RCAUSE_WDT) return "watchdog";
  if (rc & (PM_RCAUSE_BOD12 | PM_RCAUSE_BOD33)) return "brownout";
  if (rc & PM_RCAUSE_POR) return "power_on";
  if (rc & PM_RCAUSE_EXT) return "reset_pin";
  if (rc & PM_RCAUSE_SYST) return "software";
  return "unknown";
}

Relay k1, k2;

static uint16_t pingId = 0;
static uint32_t pingAt = 0;

const char *gateStateName(uint8_t s) {
  static const char *const n[] = { "unknown", "closed", "open", "between", "fault", "no_power" };
  static_assert(sizeof(n) / sizeof(n[0]) == GS_COUNT, "a name per GateState");
  return s < GS_COUNT ? n[s] : "?";
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

uint32_t appBootCount() {
  return bootCount;
}

void appRestartRadio() {
  radioBegin();  // logs radio_fail itself
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

bool appRelaysPulsing() {
  return k1.pulsing() || k2.pulsing();
}

static uint32_t loopMaxUs;  // longest loop pass since boot: how close the loop has come to the 8 s watchdog

// Gap between the heap's high-water mark and the stack: what's left for the deepest stack and a bigger reply.
extern "C" char *sbrk(int incr);
static uint32_t freeRam() {
  char top;
  return &top - sbrk(0);
}

bool appLinkUp(uint32_t now) {
  const LinkStats &st = linkStats();
  uint32_t timeout = activeRole == ROLE_HOUSE ? houseLinkTimeoutMs() : (uint32_t)cfg.link_timeout_s * 1000;
  return st.lastRxAt && !elapsed(now, st.lastRxAt, timeout);
}

void appFillStatus(JsonObject o) {
  uint32_t now = millis();
  o["fw"] = fwVersion();
  o["role"] = activeRole == ROLE_HOUSE ? "house" : activeRole == ROLE_GATE ? "gate" : "unset";
  o["reboot_pending"] = cfg.role != activeRole;
  o["uptime_ms"] = now;
  o["radio_ok"] = radioOk();
  o["radio_faults"] = radioFaults();
  o["reset_cause"] = resetCauseName(resetCause);
  if (supplyKnown()) o["supply"] = supplyGood();
  else o["supply"] = nullptr;
  o["cfg_loaded"] = cfgLoaded;
  o["cfg_store"] = configStoreName();
  o["key_set"] = (bool)cfg.key_set;
  o["free_ram"] = freeRam();
  o["usb_cut"] = consoleUsbCutLines();
  o["loop_max_us"] = loopMaxUs;
  JsonObject io = o["io"].to<JsonObject>();
  io["in1"] = in1.active();
  io["in2"] = in2.active();
  io["in3"] = in3.active();
  io["in4"] = in4.active();
  io["k1"] = k1.on();
  io["k2"] = k2.on();
  const LinkStats &st = linkStats();
  JsonObject l = o["link"].to<JsonObject>();
  l["verified"] = linkPeerVerified();
  l["age_ms"] = st.lastRxAt ? (int32_t)(now - st.lastRxAt) : -1;
  l["rssi"] = st.lastRssi;
  l["snr"] = st.lastSnr;
  l["fei"] = radioLastFei();
  l["tx"] = st.tx;
  l["rx"] = st.rx;
  l["retries"] = st.retries;
  l["giveups"] = st.giveups;
  l["mac_fail"] = st.macFail;
  l["replay"] = st.replay;
  l["sessions"] = st.sessions;
  l["lbt_defers"] = st.lbtDefers;
  l["lbt_forced"] = st.lbtForced;
  l["crc_err"] = radioCrcErrors();
  int16_t noise = histNoiseNow();
  if (noise == NOISE_NONE) l["noise"] = nullptr;
  else l["noise"] = noise;
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

bool updateSpareInputs(uint32_t now) {
  bool changed = false;
  if (in3.update(now, cfg.debounce_ms, cfg.in3_invert)) {
    logEvent(EV_INPUT, 3, in3.active());
    changed = true;
  }
  if (in4.update(now, cfg.debounce_ms, cfg.in4_invert)) {
    logEvent(EV_INPUT, 4, in4.active());
    changed = true;
  }
  return changed;
}

void appIdentify(uint32_t ms) {
  identifyUntil = (millis() + ms) | 1;
}

static void updateLed(uint32_t now) {
  // Identify: fast strobe. Unset role: solid. Link good: slow breathing. No link: lub-dub heartbeat.
  static int lastLevel = -1;
  uint8_t level;
  if (identifyUntil && (int32_t)(now - identifyUntil) >= 0) identifyUntil = 0;
  if (identifyUntil) {
    level = (now % 160) < 60 ? 255 : 0;
  } else if (activeRole == ROLE_UNSET) {
    level = 255;
  } else {
    bool up = appLinkUp(now);
    // Kept dim so neither reads as the solid "no role" light, and never fully dark between pulses.
    const uint8_t floorLevel = 2;
    if (up) {
      level = floorLevel + ledBump(now % 2500, 0, 2500, 40 - floorLevel);
    } else {
      uint32_t t = now % 850;
      level = floorLevel + ledBump(t, 0, 130, 15 - floorLevel) + ledBump(t, 170, 150, 7 - floorLevel);
    }
  }
  if (level != lastLevel) {
    analogWrite(LED_BUILTIN, level);
    lastLevel = level;
  }
}

void setup() {
  resetCause = PM->RCAUSE.reg;
  // Relays first so outputs are de-energized as early as possible.
  k1.begin(PIN_K1);
  k2.begin(PIN_K2);
  pinMode(LED_BUILTIN, OUTPUT);
  // Then the watchdog: the steps below touch the flash chip, the charger's I2C bus (the core's Wire waits without
  // a timeout) and the radio, and a hang in any of them must reset the board, not leave it stuck until power cycles.
  Watchdog.enable(8000);

  consoleBegin();
  cfgLoaded = configLoad();
  bootCount = configCountBoot();
  // Per-boot seed for session ids (link.cpp): a count that never repeats, this chip's serial number and timing. Without
  // the radio (init failed) these are all radioRandom32() has.
  uint32_t seed[] = { bootCount, *(volatile uint32_t *)0x0080A00C, *(volatile uint32_t *)0x0080A040,
                      *(volatile uint32_t *)0x0080A044, *(volatile uint32_t *)0x0080A048, resetCause, micros() };
  radioAddEntropy(seed, sizeof(seed));
  Watchdog.reset();
  consoleConfigure();  // before the boot event, so a UART console sees it
  activeRole = cfg.role;
  in1.begin(PIN_IN1, cfg.in1_invert);
  in2.begin(PIN_IN2, cfg.in2_invert);
  in3.begin(PIN_IN3, cfg.in3_invert);
  in4.begin(PIN_IN4, cfg.in4_invert);
  logEvent(EV_BOOT, resetCause, activeRole);
  logEvent(EV_CFG, configSource(), configDropped());
  histBegin();
  supplyBegin();
  Watchdog.reset();

  if (activeRole != ROLE_UNSET) {
    appRestartRadio();
    if (activeRole == ROLE_HOUSE) houseBegin();
    else gateBegin();
  }
}

void loop() {
  uint32_t start = micros();
  uint32_t now = millis();
  Watchdog.reset();
  k1.update(now);
  k2.update(now);
  consolePoll();
  supplyPoll(now);  // before the roles: the house's controller power sense reads it
  if (activeRole != ROLE_UNSET) {
    linkPoll(now);
    if (activeRole == ROLE_HOUSE) houseLoop(now);
    else gateLoop(now);
    histPoll(now, appLinkUp(now));
  }
  updateLed(now);
  uint32_t took = micros() - start;
  if (took > loopMaxUs) loopMaxUs = took;
}
