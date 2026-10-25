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
//   owner-only  every command the house sends comes from an IN1 edge (pendingAction, the one place it decides) to a
//               level the owner switched to since the last edge to that level the house acted on or set aside; a CLOSE
//               decided before the house saw the controller's power drop never goes; no command goes before the
//               house is armed (its first STATUS plus sync_window_ms)
//
// Owner edges inside a sync window are set aside by design (CLAUDE.md), and in follow mode the owner switching on just
// as K1 does is the same edge at IN1 as the Shelly following K1: a CLOSE decided before it still goes. That is the
// limit of what the house can tell apart, and the property states it at the decision, where it can (an earlier form
// checked at the send, against one flag per level, and flagged two CLOSEs for an owner's OFF, ON, OFF).
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
#include "supply.cpp"
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
#ifndef PG_ON_MIN
#define PG_ON_MIN 100  // power good back after the return: the charger's REGN turn-on delay, 100 ms min (datasheet)
#endif
#ifndef PG_ON_MAX
#define PG_ON_MAX 500  // (not measured)
#endif
// While power good is (or is about to be) low and the house hasn't yet seen it, its passes are at most POLL_MS apart:
// it reads the charger every 5 ms (supply.cpp) unless something stalls the loop. -DSTALLS_ANYWHERE lets a PASS_MS stall
// (a flash save, a radio restart: ~0.5 s on the bench) fall there too. A pass longer than POLL_MS is such a stall, and
// its waits call yield() (delay(), the flash's busy wait): the harness calls it once in the stall, at a time when power
// good is low if it is low for 5 ms or more of it, as the stall's reads every 5 ms would find it (supply.cpp's
// supplyWait, where the firmware has one; yield() has no body otherwise).
#ifndef POLL_MS
#define POLL_MS 50
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
// A signal that drops `lag` into the outage, if it lasts that long, and is back `on` after its end: at time t.
static bool litAt(uint32_t lag, uint32_t on, uint64_t t) {
  if (!outage || t < cutT) return true;
  uint64_t soFar = (backT && backT <= t ? backT : t) - cutT;
  if (soFar < lag) return true;
  return backT && t >= backT + on;
}
static bool lit(uint32_t lag, uint32_t on) { return litAt(lag, on, T); }
// The charger (supply.cpp reads it over I2C): REG08's power good bit, at T or inside a stall (sampleT).
static bool sampling;
static uint64_t sampleT;
uint8_t wireStatus() { return litAt(pgLag, pgOn, sampling ? sampleT : T) ? 0x04 : 0; }
extern "C" void yield(void);
// A stall from g0 to T: its waits read power good every 5 ms. One read stands for them: in the span where it is low,
// if that is 5 ms or more (past the first 5 ms, which the pass before read), else anywhere.
static void stallReads(uint64_t g0) {
  uint64_t lo = cutT + pgLag, hi = backT ? backT + pgOn : T;
  bool low = outage && (!backT || backT - cutT >= pgLag);
  if (lo < g0 + 5) lo = g0 + 5;
  if (hi > T) hi = T;
  uint32_t off;
  RANGE(off, 5, (uint32_t)(T - g0));
  sampleT = g0 + off;
  if (low && lo + 5 <= hi) ASSUME(sampleT >= lo && sampleT < hi);
  uint32_t c = clk;
  clk -= (uint32_t)(T - sampleT);
  sampling = true;
  yield();
  sampling = false;
  clk = c;
}

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
static uint32_t pendingAt0;   // pendingAt before this pass's appLoop: a change is a decision (role_house.cpp)
static bool decidedNow;       // one was made this pass
static bool staleClose;       // a decided CLOSE waits, and the house has seen the controller's power drop since
static bool pendingOk;        // the decision waiting in pendingAction came from the owner's edge
static void decided(uint8_t a) {
  pendingOk = a == ACT_OPEN ? askedOpen : a == ACT_CLOSE && askedClose;
  if (a == ACT_OPEN) askedOpen = false;
  else askedClose = false;
  pendingAt0 = pendingAt;
  decidedNow = true;
  staleClose = false;
}
static bool cmdPending;
bool linkPending(Slot s) { return s == SLOT_CMD && cmdPending; }
void linkAck(uint32_t, uint8_t) {}
void linkSend(uint8_t, const uint8_t *, uint8_t) {}
void linkSendReliable(Slot slot, uint8_t type, const uint8_t *p, uint8_t len, uint32_t) {
  if (slot != SLOT_CMD || type != MSG_CMD) return;
  CHECK(len == CMD_LEN, "a command is CMD_LEN long");
  CHECK(armedEver || armed, "owner-only: a command before the house was armed");
  uint8_t a = p[2];
  if (pendingAt != pendingAt0) decided(a);  // decided this pass and sent at once (an OPEN)
  CHECK(pendingOk, "owner-only: a command from an IN1 edge the owner didn't make");
  CHECK(a == ACT_OPEN || a == ACT_CLOSE, "owner-only: a command neither OPEN nor CLOSE");
  CHECK(!(a == ACT_CLOSE && staleClose), "owner-only: a CLOSE decided before the controller's power dropped went");
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
  RANGE(pgOn, PG_ON_MIN, PG_ON_MAX);
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
  supplyBegin();
  houseBegin();
  for (int i = 0; i < PASSES; i++) {
    uint32_t d;
    // Power good low, or due to drop, and not yet seen low by the house (ctrlPower): it polls.
    bool blind = outage && ctrlPower && (!backT || T < backT + pgOn);
#ifdef STALLS_ANYWHERE
    blind = false;
#endif
    RANGE(d, 1, blind ? POLL_MS : eventful ? PASS_MS : IDLE_MS);
    uint64_t g0 = T;
    // The outage may begin (or end) during the gap, at any time in it. The rest of the gap is bounded as a gap from
    // there would be: the house may be blind from the cut on.
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
      bool blindNow = ctrlPower && (!backT || T < backT + pgOn);
#ifdef STALLS_ANYWHERE
      blindNow = false;
#endif
      uint32_t rest;
      RANGE(rest, 1, blindNow ? POLL_MS : PASS_MS);
      advance(rest);
    } else {
      advance(d);
    }
    if (T - g0 > POLL_MS) stallReads(g0);
    site();
    if (shellyUp && (!outage || backT) && nondet_bool()) {  // the owner, over Z-Wave
      relay = !relay;
      if (relay) askedOpen = true;
      else askedClose = true;
      eventful = true;
    }
    // pendingAt is read only while an action waits (role_house.cpp): with none, a time no decision this pass can stamp,
    // so a decision always changes it (one stamped at the old value, as at now = 0 after the wrap, went unseen).
    if (!pendingAction) pendingAt = clk + 1;
    pendingAt0 = pendingAt;
    decidedNow = false;
    bool level0 = shellyLevel;
    appLoop();
    if (pendingAction && pendingAt != pendingAt0) decided(pendingAction);
    // An edge the house set aside (a sync, the power off): the owner's switch to that level, if any, is spent.
    if (shellyLevel != level0 && !decidedNow) {
      if (shellyLevel) askedOpen = false;
      else askedClose = false;
    }
    if (pendingAction == ACT_CLOSE && !ctrlPower) staleClose = true;
    if (armed) armedEver = true;
  }
#ifdef REACH
  CHECK(false, "reach: the end of the harness is reachable (this run must fail)");
#endif
  return 0;
}
