// ESBMC harness: the house never commands the gate but for the owner's switch (bounded: PASSES loop passes from boot).
//
// The house board's real code (role_house.cpp, app.cpp's loop, io.cpp) in a model of the site around it:
//   - the Shelly: its relay is the Alarm.com switch and house IN1. It follows K1 on its SW input, or toggles on each
//     SW edge, or ignores SW (detached), within SW_LATENCY_MAX of the edge. The owner switches it at any pass.
//   - its 12 V rail (also the house board's supply, through its buck; the board keeps running on its LiPo): one
//     outage per run, of any length, starting at any time. Into it, the board's power good (BQ24195L, read by
//     supply.cpp) drops PG_LAG after the cut if the outage lasts that long, and comes back within PG_ON of the
//     return; IN2's opto the same with OPTO_LAG / OPTO_ON. An outage of BROWNOUT or longer reboots the Shelly: its
//     relay drops RELAY_DROP after the cut (even once power is back, as on the bench's 300 ms dips), and the Shelly
//     comes up BOOT later at any level (restoring its last state, following SW, or off).
//   - the gate: any STATUS (closed or open) at any pass; commands are ACKed or given up at any pass.
// Every timing is a range (-D NAME_MIN / NAME_MAX), every value in it tried. The Makefile runs the bench's measured
// ranges and the worst cases the sources allow (docs/architecture.md, "Verification"). With -DBROWNOUT_AFTER_PG the
// Shelly reboots only on an outage that also drops the board's power good (the bench's ordering, 30 of 30 runs).
//
//   owner-only  every command the house sends (OPEN or CLOSE) is one the owner switched to since the last command it
//               sent, and none goes before the house is armed (its first STATUS plus sync_window_ms)
//
// Loop passes: up to IDLE_MS before anything happens (K1 driven, the outage, the owner), PASS_MS after (the longest
// pass the bench has seen is ~0.5 s, a flash save; docs say up to ~1 s): a pass longer than the site's events would
// let the board miss them, as no real board does. The settings are the defaults
// unless -DANY_CONFIG (the ones this depends on, anywhere in their range).
#include "env.h"
#define private public
#include "io.h"
#undef private
#include "io.cpp"
#include "app.cpp"
#include "role_house.cpp"
#include "cfgmodel.h"

#ifndef PASSES
#define PASSES 8
#endif
#ifndef IDLE_MS
#define IDLE_MS 8000
#endif
#ifndef PASS_MS
#define PASS_MS 1000
#endif
// The site's timings, ms: the bench's measurements with a margin (docs/architecture.md, "Verification").
#ifndef SW_LATENCY_MAX
#define SW_LATENCY_MAX 200  // the Shelly's relay answering an SW edge (bench: K1 to IN1 within ~180 ms with debounce)
#endif
#ifndef PG_LAG_MIN
#define PG_LAG_MIN 150  // power good lost into a cut (bench ~250 ms: 196-222 ms before the relay's ~460)
#endif
#ifndef PG_LAG_MAX
#define PG_LAG_MAX 300
#endif
#ifndef PG_ON_MAX
#define PG_ON_MAX 500  // power good back after the return (not measured; the charger requalifies >= 100 ms)
#endif
#ifndef OPTO_LAG_MIN
#define OPTO_LAG_MIN 1500  // IN2's opto into a cut (bench 1.6-2.1 s; misses 300 ms dips)
#endif
#ifndef OPTO_LAG_MAX
#define OPTO_LAG_MAX 2500
#endif
#ifndef OPTO_ON_MAX
#define OPTO_ON_MAX 500  // IN2 back after the return (not measured)
#endif
#ifndef RELAY_DROP_MIN
#define RELAY_DROP_MIN 350  // the Shelly's relay dropping into a cut (bench ~460, with the 50 ms debounce)
#endif
#ifndef RELAY_DROP_MAX
#define RELAY_DROP_MAX 700
#endif
#ifndef BROWNOUT_MIN
#define BROWNOUT_MIN 0  // the shortest outage that reboots the Shelly (bench: a 300 ms dip does; shorter not measured)
#endif
#ifndef BROWNOUT_MAX
#define BROWNOUT_MAX 300
#endif
#ifndef BOOT_MIN
#define BOOT_MIN 2500  // the Shelly booting (bench ~3.1 s from power good back; its guide: LED blinks 4-5 s)
#endif
#ifndef BOOT_MAX
#define BOOT_MAX 5500
#endif

#define RANGE(v, lo, hi) do { v = nondet_u32(); ASSUME(v >= (lo) && v <= (hi)); } while (0)

// --- the site ------------------------------------------------------------------------------------------------------
enum Mode { FOLLOW, TOGGLE, DETACHED };
static uint8_t mode;
static bool relay;                // the Shelly's relay: the alarm switch, house IN1
static bool k1Level;              // K1 as the house drives it: the Shelly's SW input
static bool swPending;            // an SW edge the relay is still to answer ...
static uint64_t swAt;             // ... by then
static bool swLevel;
static bool outage;               // the run's outage has begun
static uint64_t cutT, backT;      // its start and end (backT = 0: not over)
static uint32_t pgLag, pgOn, optoLag, optoOn, relayDrop, brownout, shellyBootMs;
static bool shellyUp = true;      // the Shelly is running (not rebooting)
static bool rebooting, relayDropped;
static bool askedOpen, askedClose;  // ghosts: what the owner switched to since the house's last command
static bool armedEver;
static bool eventful;  // something has happened: passes are short from here on (see main)

static uint64_t outageSoFar() { return backT ? backT - cutT : T - cutT; }
// A signal that drops `lag` into the outage, if it lasts that long, and is back `on` after its end.
static bool lit(uint32_t lag, uint32_t on) {
  if (!outage || outageSoFar() < lag) return true;
  return backT && T >= backT + on;
}
bool supplyGood() { return lit(pgLag, pgOn); }
bool supplyKnown() { return true; }

int digitalRead(uint32_t pin) {
  if (pin == PIN_IN1) return relay ? HIGH : LOW;
  if (pin == PIN_IN2) return lit(optoLag, optoOn) ? HIGH : LOW;
  return LOW;
}
void pinMode(uint32_t, uint32_t) {}
void analogWrite(uint32_t, int) {}
void digitalWrite(uint32_t pin, uint32_t v) {
  if (pin != PIN_K1 || (v == HIGH) == k1Level) return;
  k1Level = v == HIGH;
  eventful = true;
  if (mode == DETACHED) return;
  uint32_t lat;
  RANGE(lat, 0, SW_LATENCY_MAX);
  swPending = true;  // a later edge supersedes one not yet answered
  swAt = T + lat;
  swLevel = k1Level;
}
static uint32_t keptRam[BOARD_KEEP_WORDS];
uint32_t *boardKeep() { return keptRam; }

// The site up to now, in time order: the Shelly answering SW, rebooting with the outage.
static void site() {
  bool powered = !outage || backT;
  if (outage && !rebooting && outageSoFar() >= brownout) {
#ifdef BROWNOUT_AFTER_PG
    if (outageSoFar() >= pgLag)
#endif
      rebooting = true;
  }
  if (rebooting && !relayDropped && T >= cutT + relayDrop) {
    relayDropped = true;
    shellyUp = false;
    relay = false;
    swPending = false;
  }
  if (relayDropped && !shellyUp && backT && T >= (backT > cutT + relayDrop ? backT : cutT + relayDrop) + shellyBootMs) {
    shellyUp = true;
    relay = nondet_bool();  // its last state, SW's level, or off
  }
  if (shellyUp && powered && swPending && T >= swAt) {
    swPending = false;
    relay = mode == FOLLOW ? swLevel : !relay;
  }
}

// --- the link: the gate's STATUS, and the commands the house sends ---------------------------------------------------
static LinkStats stats;
const LinkStats &linkStats() { return stats; }
bool linkPeerVerified() { return true; }
uint32_t linkPeerSession() { return 1; }
static bool cmdPending;
bool linkPending(Slot s) { return s == SLOT_CMD && cmdPending; }
void linkAck(uint32_t, uint8_t) {}
void linkSend(uint8_t, const uint8_t *, uint8_t) {}
void linkSendReliable(Slot slot, uint8_t type, const uint8_t *p, uint8_t len, uint32_t) {
  if (slot != SLOT_CMD || type != MSG_CMD) return;
  CHECK(len == CMD_LEN, "a command is CMD_LEN long");
  CHECK(armedEver || armed, "owner-only: a command before the house was armed");
  uint8_t a = p[2];
  CHECK(a == ACT_OPEN ? askedOpen : a == ACT_CLOSE && askedClose,
        "owner-only: a command the owner didn't switch to since the last one");
  if (a == ACT_OPEN) askedOpen = false;
  else askedClose = false;
  cmdPending = true;
}
static uint32_t statusSeqNext = 100;
void linkPoll(uint32_t) {
  if (cmdPending && nondet_bool()) {  // ACKed (any result) or given up
    cmdPending = false;
    houseOnAck(SLOT_CMD, MSG_CMD, nondet_bool(), nondet_u8() % 7);
  }
  if (!nondet_bool()) return;
  uint8_t p[ST_LEN] = {};
  p[ST_STATE] = nondet_bool() ? GS_CLOSED : GS_OPEN;
  putU16(p + ST_HEARTBEAT, 30);
  putU16(p + ST_TRAVEL, 60);
  putU48(p + ST_CLOCK, 1000000 + T);
  RxMsg m = { MSG_STATUS, statusSeqNext++, p, ST_LEN, 0, 0 };
  handleStatus(m, millis());
}
void consolePoll() {}

int main() {
  T = 1ull << 40;
  clk = nondet_u32();
  configDefaults(cfg);
  cfg.role = activeRole = ROLE_HOUSE;
  cfg.key_set = 1;
#define ANY(f) cfg.f = nondet_i32(); ASSUME(cfg.f >= PARAM_MIN_##f && cfg.f <= PARAM_MAX_##f);
#ifdef ANY_CONFIG
  ANY(debounce_ms) ANY(ctrl_confirm_ms) ANY(sync_window_ms) ANY(ctrl_settle_ms) ANY(resync_ms) ANY(mismatch_timeout_s)
  ANY(ctrl_sync)
#endif
#ifdef CONFIRM_MS
  cfg.ctrl_confirm_ms = CONFIRM_MS;
#endif
#ifdef SHELLY_MODE
  mode = SHELLY_MODE;  // 0 follow (the install's setting, docs/hardware.md), 1 toggle, 2 detached
#else
  mode = nondet_u8() % 3;
#endif
  RANGE(pgLag, PG_LAG_MIN, PG_LAG_MAX);
  RANGE(pgOn, 0, PG_ON_MAX);
  RANGE(optoLag, OPTO_LAG_MIN, OPTO_LAG_MAX);
  RANGE(optoOn, 0, OPTO_ON_MAX);
  RANGE(relayDrop, RELAY_DROP_MIN, RELAY_DROP_MAX);
  RANGE(brownout, BROWNOUT_MIN, BROWNOUT_MAX);
  RANGE(shellyBootMs, BOOT_MIN, BOOT_MAX);
  ASSUME(relayDrop >= brownout);  // the relay drops once the Shelly is browning out, not before
  relay = nondet_bool();
  appRelaysBegin();
  memoBegin(PM_RCAUSE_POR);
  in1.begin(PIN_IN1, cfg.in1_invert);
  in2.begin(PIN_IN2, cfg.in2_invert);
  in3.begin(PIN_IN3, cfg.in3_invert);
  in4.begin(PIN_IN4, cfg.in4_invert);
  houseBegin();
  for (int i = 0; i < PASSES; i++) {
    uint32_t d;
    RANGE(d, 1, eventful ? PASS_MS : IDLE_MS);
    // The outage may begin (or end) during the gap, at any time in it.
    if (!backT && nondet_bool()) {
      uint32_t at;
      RANGE(at, 0, d - 1);
      advance(at);
      site();
      if (!outage) {
        outage = true;
        cutT = T;
      } else {
        backT = T;
      }
      eventful = true;
      advance(d - at);
    } else {
      advance(d);
    }
    site();
    if (shellyUp && (!outage || backT) && nondet_bool()) {  // the owner, over Z-Wave
      relay = !relay;
      if (relay) askedOpen = true;
      else askedClose = true;
      eventful = true;
    }
    appLoop();
    if (armed) armedEver = true;
  }
#ifdef REACH
  CHECK(false, "reach: the end of the harness is reachable (this run must fail)");
#endif
  return 0;
}
