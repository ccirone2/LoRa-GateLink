// The application around the roles: boot, the main loop, PING/PONG, dispatch by role, the status LED and the
// status report. Hardware-only bits (watchdog, chip registers) stay in GateLink.ino behind board.h, so this builds
// on the PC as well (tests/native).
#include "app.h"
#include "board.h"
#include "pins.h"
#include "config.h"
#include "radio.h"
#include "link.h"
#include "io.h"
#include "log.h"
#include "supply.h"
#include "roles.h"
#include "console.h"
#include "history.h"
#include "health.h"
#include "crc32.h"

Input in1, in2, in3, in4;
Relay k1, k2;
static uint8_t resetCause = 0;  // PM->RCAUSE at boot
static bool cfgLoaded = false;
static uint32_t bootCount = 0;
static uint32_t identifyUntil = 0;  // LED strobes until then (0 = off)
static uint32_t loopMaxUs;  // longest loop pass since boot: how close the loop has come to the 8 s watchdog

static const char *resetCauseName(uint8_t rc) {
  if (rc & PM_RCAUSE_WDT) return "watchdog";
  if (rc & (PM_RCAUSE_BOD12 | PM_RCAUSE_BOD33)) return "brownout";
  if (rc & PM_RCAUSE_POR) return "power_on";
  if (rc & PM_RCAUSE_EXT) return "reset_pin";
  if (rc & PM_RCAUSE_SYST) return "software";
  return "unknown";
}

// Where the loop is, for the watchdog's early warning (appLoop's steps; setup is everything before the first pass).
// STG_CORE: between passes (loop()'s watchdog kick, the core's USB and serial events).
enum Stage : uint8_t {
  STG_SETUP = 0, STG_RELAYS, STG_RADIO_RECOVER, STG_HIST_SAVE, STG_CONSOLE, STG_SUPPLY, STG_LINK, STG_ROLE,
  STG_HISTORY, STG_HEALTH, STG_LED, STG_CORE, STG_COUNT, STG_NONE = 0xFF
};
static volatile uint8_t stage = STG_SETUP;

static const char *stageName(uint8_t s) {
  static const char *const n[] = { "setup", "relays", "radio_recover", "hist_save", "console", "supply", "link",
                                   "role", "history", "health", "led", "core" };
  static_assert(sizeof(n) / sizeof(n[0]) == STG_COUNT, "a name per Stage");
  return s < STG_COUNT ? n[s] : "?";
}

// The reset memo, in RAM that resets other than power-on keep (boardKeep): what the next boot needs to know about
// this one. Checked by a CRC, so after a power-on (or a bootloader that touched it) it reads as no memo.
struct ResetMemo {
  uint32_t magic;
  uint8_t streak;   // watchdog or brown-out resets in a row, each within REBOOT_SETTLE_MS of the boot before
  uint8_t settled;  // this boot has run REBOOT_SETTLE_MS
  uint8_t stage;    // the early warning's: the loop's stage then (STG_NONE: no warning pending)
  uint8_t kept;     // the role's flags (appKeep), for the next boot after a crash
  uint32_t pc;      // the early warning's: the program counter it interrupted
  uint32_t check;   // CRC-32 of the above
};
static_assert(sizeof(ResetMemo) == 4 * BOARD_KEEP_WORDS, "the reset memo fills the kept RAM");
#define MEMO_MAGIC 0x6D656D6FUL
static ResetMemo *memo;
static uint8_t resetStreak;   // as the memo had it at boot (0 after a deliberate reset or power-on)
static uint8_t resetStage = STG_NONE;  // after a watchdog reset with an early warning: where it hung
static uint32_t resetPc;
static uint8_t keptFlags;  // the boot before's appKeep flags, after a crash reset (else 0)
static uint32_t bootMs;  // millis() at boot

static uint32_t memoCheck() {
  return crc32(memo, offsetof(ResetMemo, check));
}

static void memoSeal() {
  memo->check = memoCheck();
}

// At boot: read what the last boot left and start this one's.
static void memoBegin(uint8_t rc) {
  memo = (ResetMemo *)boardKeep();
  bootMs = millis();
  bool kept = memo->magic == MEMO_MAGIC && memo->check == memoCheck();
  bool crash = (rc & (PM_RCAUSE_WDT | PM_RCAUSE_BOD12 | PM_RCAUSE_BOD33)) && !(rc & PM_RCAUSE_POR);
  resetStreak = !crash ? 0 : !kept || memo->settled ? 1 : memo->streak < 255 ? memo->streak + 1 : 255;
  if (kept && (rc & PM_RCAUSE_WDT) && memo->stage != STG_NONE) {
    resetStage = memo->stage;
    resetPc = memo->pc;
  }
  keptFlags = kept && crash ? memo->kept : 0;
  *memo = ResetMemo{ MEMO_MAGIC, resetStreak, 0, STG_NONE, keptFlags, 0, 0 };
  memoSeal();
}

uint8_t appKept() {
  return keptFlags;
}

void appKeep(uint8_t flags) {
  if (!memo || memo->kept == flags) return;
  memo->kept = flags;
  memoSeal();
}

void appWatchdogWarning(uint32_t pc) {
  if (!memo) return;  // an interrupt: in case it ever came before appSetup
  memo->stage = stage;
  memo->pc = pc;
  memoSeal();
}

bool appRebootLoop() {
  return resetStreak >= REBOOT_LOOP_RESETS && !memo->settled;
}

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
  if (activeRole == ROLE_HOUSE) houseLinkRestarted();
}

bool appPing() {
  if (!radioOk() || activeRole == ROLE_UNSET || !cfg.key_set) return false;
  uint8_t p[2];
  putU16(p, ++pingId);
  pingAt = millis();
  linkSend(MSG_PING, p, 2);
  return true;
}

bool appRelayTest(uint8_t k, uint32_t ms) {
  if (activeRole == ROLE_GATE) return gateRelayTest(k, ms);
  if (activeRole == ROLE_HOUSE) houseRelayTest(k, ms);
  return true;
}

bool appRelaysPulsing() {
  return k1.pulsing() || k2.pulsing();
}

bool appLinkUp(uint32_t now) {
  // The house goes by the gate's STATUS reports alone, taken in order (houseStatusAt): any other frame, or a STATUS
  // held back and delivered late, says nothing about the gate now. The gate goes by anything heard from the house.
  // Either way the timeout covers 2.5 of the gate's heartbeats at least.
  uint32_t at = activeRole == ROLE_HOUSE ? houseStatusAt() : linkStats().lastRxAt;
  uint32_t timeout = activeRole == ROLE_HOUSE ? houseLinkTimeoutMs() : gateLinkTimeoutMs();
  return at && !elapsed(now, at, timeout);
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
  o["crash_resets"] = resetStreak;
  if (resetStage == STG_NONE) {
    o["hang_stage"] = nullptr;
    o["hang_pc"] = nullptr;
  } else {
    o["hang_stage"] = stageName(resetStage);
    o["hang_pc"] = resetPc;
  }
  if (supplyKnown()) o["supply"] = supplyGood();
  else o["supply"] = nullptr;
  o["cfg_loaded"] = cfgLoaded;
  o["cfg_store"] = configStoreName();
  o["key_set"] = (bool)cfg.key_set;
  o["free_ram"] = boardFreeRam();
  o["usb_cut"] = consoleUsbCutLines();
  o["loop_max_us"] = loopMaxUs;
  int8_t faultOut = healthFaultOut();
  if (faultOut < 0) o["fault_out"] = nullptr;
  else o["fault_out"] = faultOut > 0;
  JsonArray health = o["health"].to<JsonArray>();
  for (uint8_t i = 0; i < PROBLEM_COUNT; i++)
    if (healthProblems() & (1u << i)) health.add(healthName(i));
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

void appRelaysBegin() {
  // Relays first so outputs are de-energized as early as possible.
  k1.begin(PIN_K1);
  k2.begin(PIN_K2);
  pinMode(LED_BUILTIN, OUTPUT);
}

void appSetup(uint8_t rc, const uint32_t serial[4]) {
  // The watchdog runs from here on: the steps below touch the flash chip, the charger's I2C bus (the core's Wire
  // waits without a timeout) and the radio, and a hang in any of them must reset the board, not leave it stuck
  // until power cycles.
  resetCause = rc;
  memoBegin(rc);
  consoleBegin();
  cfgLoaded = configLoad();
  bootCount = configCountBoot();
  // Session ids are drawn from the count (link.cpp, drawSession), which never repeats. The per-boot seed for the random
  // part (and for everything else random): that count, this chip's serial number and timing. Without the radio (init
  // failed) these are all radioRandom32() has.
  linkSetBoot(bootCount, serial);
  uint32_t seed[] = { bootCount, serial[0], serial[1], serial[2], serial[3], resetCause, micros() };
  radioAddEntropy(seed, sizeof(seed));
  boardKick();
  consoleConfigure();  // before the boot event, so a UART console sees it
  activeRole = cfg.role;
  in1.begin(PIN_IN1, cfg.in1_invert);
  in2.begin(PIN_IN2, cfg.in2_invert);
  in3.begin(PIN_IN3, cfg.in3_invert);
  in4.begin(PIN_IN4, cfg.in4_invert);
  logEvent(EV_BOOT, resetCause, activeRole);
  if (resetStage != STG_NONE) logEvent(EV_HANG, resetPc, resetStage);
  logEvent(EV_CFG, configSource(), configDropped());
  healthBegin();  // fault_out: D5 LOW until the role has decided
  histBegin(bootCount);  // the history kept in the flash
  supplyBegin();
  boardKick();

  if (activeRole != ROLE_UNSET) {
    appRestartRadio();
    if (activeRole == ROLE_HOUSE) houseBegin();
    else gateBegin();
  }
}

void appLoop() {
  uint32_t start = micros();
  uint32_t now = millis();
  // A stall the early warning caught that ended without a reset: drop its note, so a later reset that the warning
  // didn't see (interrupts off) isn't blamed on it.
  if (*(volatile uint8_t *)&memo->stage != STG_NONE) {
    memo->stage = STG_NONE;
    memo->pc = 0;
    memoSeal();
  }
  if (!memo->settled && elapsed(now, bootMs, REBOOT_SETTLE_MS)) {
    memo->settled = 1;  // the next crash reset starts a streak over
    memoSeal();
  }
  stage = STG_RELAYS;
  k1.update(now);
  k2.update(now);
  // A radio that faulted or didn't start is re-initialised here, never while a relay pulses: LoRa.begin() stops the
  // loop ~0.5 s, and Relay::update couldn't end the pulse meanwhile (nor start one waiting out the interlock).
  stage = STG_RADIO_RECOVER;
  if (radioRecoverDue() && !appRelaysPulsing()) radioRecover();
  // Likewise a completed history bucket's write to the flash (~0.5 s, the radio restarted): it waits for the pulse.
  stage = STG_HIST_SAVE;
  if (histSaveDue() && !appRelaysPulsing()) histSave();
  stage = STG_CONSOLE;
  consolePoll();
  stage = STG_SUPPLY;
  supplyPoll(now);  // before the roles: the house's controller power sense reads it
  bool up = false;
  if (activeRole != ROLE_UNSET) {
    stage = STG_LINK;
    linkPoll(now);
    stage = STG_ROLE;
    if (activeRole == ROLE_HOUSE) houseLoop(now);
    else gateLoop(now);
    up = appLinkUp(now);
    stage = STG_HISTORY;
    histPoll(now, up);
  }
  stage = STG_HEALTH;
  healthPoll(now, up);  // after the roles, which it reads (and nothing there reads it)
  stage = STG_LED;
  updateLed(now);
  stage = STG_CORE;
  uint32_t took = micros() - start;
  if (took > loopMaxUs) loopMaxUs = took;
}
