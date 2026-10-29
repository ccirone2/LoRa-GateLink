// ESBMC harness: the gate board's relays and the commands that drive them, proved for runs of any length.
//
// An inductive proof. inv() below is an invariant over the relays, the coils as the opener sees them (ghosts, in
// physical time) and the gate's clock. The harness checks that it holds after boot (-DBOOT: appRelaysBegin, the reset
// memo, gateBegin) and that each segment of a loop pass (-DSEGMENT, see main) from ANY state satisfying it, with
// everything else in the firmware arbitrary, ends in a state satisfying it again. Together: it holds throughout every
// run, however long. The properties are checked inside each segment, so they hold throughout too:
//
//   interlock    a gate coil never energizes while the other is energized, nor within INTERLOCK_MS of its release
//   pulse-only   a coil releases within its pulse's length (pulse_ms when it was asked, or the relay test's ms) plus
//                one loop pass, and never energizes but for a command, a relay test or a pulse waiting to start
//   no-restart   a running or waiting pulse is never restarted: its start and end never move while it lasts
//   fresh-cmd    a command pulses only inside its deadline by the gate's clock (CMD_LATE_SLACK_MS plus two frames'
//                airtime late at most, CMD_AHEAD_MAX_MS early), stated in physical time since boot, not with the
//                firmware's clock
//   once         a command whose id is the last one taken (this house session) never pulses; one that pulses becomes
//                the last one taken
//   in-order     a command framed before the last one taken (a lower seq, this house session) never pulses
//   no-stall     no flash write, radio restart or log save runs while a coil pulses or waits to (each stops the loop)
//
// Everything outside the relays is arbitrary at the start of a step: the gate's state machine (state, target, the
// limit holds, the config queue...), the inputs, every setting in its range; in the pass, the inputs on every read,
// the frames the link hands over (any CMD or CFG_SET payload, a few per pass), the house session changing, console
// relay tests of either relay and any length. Time between passes: up to IDLE_MAX_MS while nothing pulses, PASS_MAX_MS
// while one does (env.h); millis() is anywhere, so passes cross its wrap.
#include "env.h"
#define private public  // the harness reads and sets Relay's and Input's state
#include "io.h"
#undef private
#include "io.cpp"
#include "app.cpp"
#include "role_gate.cpp"
#include "cfgmodel.h"

#define SEG_PASS 1
#define SEG_CMD 2
#define SEG_CFG 3
#define SEG_TEST 4
#define SEG_ROLE 5
#ifndef SEGMENT
#define SEGMENT SEG_PASS
#endif
// The longest loop pass while a relay pulses: nothing that blocks may run then (no-stall), so a pass is the radio
// poll, the role and the console, a few ms. A pulse can overrun its end by one pass.
#define PASS_MAX_MS 50

// --- the coils, as the opener sees them (physical time T) ----------------------------------------------------------
struct Coil {
  bool on;
  uint64_t onT;    // when it last energized
  bool released;   // it has released since boot
  uint64_t offT;   // when it last released
  uint32_t len;    // the length its current (or waiting) pulse was asked for
};
static Coil coil[3];
static uint32_t clkBase;  // clk - T, mod 2^32: millis() at T = 0
static uint32_t toClk(uint64_t t) { return clkBase + (uint32_t)t; }
static bool inHandler;    // a frame handler or the console is running (a pulse may start)
static bool waitingAtPass[3];  // a delayed pulse was waiting when the pass began (it may start in Relay::update)

int digitalRead(uint32_t) { return nondet_bool() ? HIGH : LOW; }
void pinMode(uint32_t, uint32_t) {}
void analogWrite(uint32_t, int) {}
void digitalWrite(uint32_t pin, uint32_t v) {
  if (pin != PIN_K1 && pin != PIN_K2) return;
  int k = pin == PIN_K1 ? 1 : 2;
  Coil &c = coil[k], &o = coil[3 - k];
  if (v == HIGH && !c.on) {
    CHECK(!o.on, "interlock: a gate coil energized while the other is");
    CHECK(!o.released || T - o.offT >= INTERLOCK_MS, "interlock: a gate coil energized within INTERLOCK_MS of the other's release");
    CHECK(inHandler || waitingAtPass[k], "pulse-only: a gate coil energized with no command, test or waiting pulse");
    c.on = true;
    c.onT = T;
  } else if (v == LOW && c.on) {
    CHECK(T - c.onT <= c.len + 1 + PASS_MAX_MS + SKEW_PASS, "pulse-only: a gate coil held past its pulse");
    c.on = false;
    c.offT = T;
    c.released = true;
  }
}

// --- what blocks the loop --------------------------------------------------------------------------------------
static bool pulsingNow() { return k1.pulsing() || k2.pulsing(); }
bool configSaveParam(const ParamDef *) {
  CHECK(!pulsingNow(), "no-stall: a remote config write saved while a relay pulses");
  return nondet_bool();
}
void radioRecover() { CHECK(!pulsingNow(), "no-stall: the radio restarted while a relay pulses"); }
void histSave() { CHECK(!pulsingNow(), "no-stall: the history saved while a relay pulses"); }
void logSave(uint32_t) { CHECK(!pulsingNow(), "no-stall: the event log saved while a relay pulses"); }
void boardReset() { ASSUME(false); }  // the bench's reboot-after-command: the run ends (the boot case covers after)
static uint32_t keptRam[BOARD_KEEP_WORDS];
uint32_t *boardKeep() { return keptRam; }

// --- the link --------------------------------------------------------------------------------------------------
static LinkStats stats;
static uint32_t houseSessionNow;  // the verified house session; changes when the house restarts
static bool verifiedNow;
const LinkStats &linkStats() { return stats; }
bool linkPeerVerified() { return verifiedNow; }
uint32_t linkPeerSession() { return houseSessionNow; }
void linkAck(uint32_t, uint8_t) {}
void linkAckLater(uint32_t) {}
void linkRefuse(uint32_t) {}
void linkSend(uint8_t, const uint8_t *, uint8_t) {}
void linkSendReliable(Slot, uint8_t, const uint8_t *, uint8_t, uint32_t) {}

static uint64_t bootT;  // T at gateBegin(): the gate's clock is clockOrigin + (T - bootT)
static bool inPass;     // between a pass's start (its `now`, passT) and its gateLoop
static uint64_t passT;
static uint64_t baseT;  // T when the firmware's clockBase was last read (uptimeMs)

static Relay &relay(int k) { return k == 1 ? k1 : k2; }

// After a request: the relay it newly set pulsing (running, or waiting out the interlock), with a pulse of `len`.
static int startedBy(bool before1, bool before2, uint32_t len) {
  bool s1 = !before1 && k1.pulsing(), s2 = !before2 && k2.pulsing();
  CHECK(!(s1 && s2), "interlock: one request started both relays");
  int k = s1 ? 1 : s2 ? 2 : 0;
  if (k) coil[k].len = len;
  return k;
}

// gateOnRx's cases, called directly (handleCmd, handleCfgSet): through its switch the checker also walks into sendDiag,
// which ESBMC can't follow (pointers to member). DIAG_REQ (sendDiag) only reads and sends; other types the gate ignores.
// Ghosts: the seq of the last command taken in this house session (role_gate.cpp keeps it as lastCmdSeq; a new house
// session clears haveCmd, and the ghost with it).
static bool seqHave;
static uint32_t seqLast;
static void deliverCmd() {
  uint8_t p[CMD_LEN];
  for (int i = 0; i < CMD_LEN; i++) p[i] = nondet_u8();
  RxMsg m = { MSG_CMD, nondet_u32(), p, (uint8_t)(nondet_bool() ? CMD_LEN : nondet_u8() % CMD_LEN), 0, 0 };
  bool before1 = k1.pulsing(), before2 = k2.pulsing();
  bool dup = haveCmd && lastCmdId == getU16(p);
  bool older = seqHave && m.seq < seqLast;
  uint32_t len = cfg.pulse_ms;
  uint64_t at = T;
  inHandler = true;
  handleCmd(m, millis());
  inHandler = false;
  if ((uint32_t)clockBase == toClk(T)) baseT = T;  // handleCmd read the clock (gateClock) at millis()
  if (!dup && haveCmd && lastCmdId == getU16(p)) {  // taken
    seqHave = true;
    seqLast = m.seq;
  }
  int k = startedBy(before1, before2, len);
  if (!k) return;
  CHECK(m.len >= CMD_LEN, "fresh-cmd: a short command pulsed");
  CHECK(k == 1 ? p[2] == ACT_OPEN : p[2] == ACT_CLOSE, "pulse-only: a command pulsed the other relay than it asked for");
  int64_t late = (int64_t)(clockOrigin + (at - bootT) - getU48(p + CMD_DEADLINE));
  int64_t slack = CMD_LATE_SLACK_MS + radioAirtimeMs(LINK_OVERHEAD + ST_LEN) + radioAirtimeMs(LINK_OVERHEAD + CMD_LEN);
  CHECK(late <= slack, "fresh-cmd: a command pulsed past its deadline");
  CHECK(late >= -(int64_t)CMD_AHEAD_MAX_MS, "fresh-cmd: a command pulsed with a deadline too far ahead");
  CHECK(!dup, "once: the last command taken pulsed again");
  CHECK(haveCmd && lastCmdId == getU16(p), "once: a command pulsed without becoming the last one taken");
  CHECK(!older, "in-order: a command framed before the last one taken pulsed");
}

static void deliverCfgSet() {
  uint8_t p[5];
  for (int i = 0; i < 5; i++) p[i] = nondet_u8();
  RxMsg m = { MSG_CFG_SET, nondet_u32(), p, (uint8_t)(nondet_bool() ? 5 : nondet_u8() % 5), 0, 0 };
  bool before1 = k1.pulsing(), before2 = k2.pulsing();
  inHandler = true;
  handleCfgSet(m);
  inHandler = false;
  CHECK(k1.pulsing() == before1 && k2.pulsing() == before2, "pulse-only: a config write started or ended a pulse");
}

static void segmentEnd();
// In the pass-start segment the pass is followed up to the link: the frames are the next segments.
void linkPoll(uint32_t) {
#if SEGMENT == SEG_PASS
  segmentEnd();
  ASSUME(false);  // the rest of the pass is the other segments
#endif
}

// The console: a relay test of either relay, any length it accepts (console.cpp: 50..5000 ms). Its own segment; in a
// pass, consolePoll runs before the link (appLoop).
static void relayTest() {
  uint8_t k = nondet_bool() ? 1 : 2;
  uint32_t ms = nondet_u32();
  ASSUME(ms >= 50 && ms <= 5000);
  bool before1 = k1.pulsing(), before2 = k2.pulsing();
  inHandler = true;
  bool ok = appRelayTest(k, ms);
  inHandler = false;
  CHECK(ok != (k == 1 ? before1 : before2), "no-restart: a relay test of a pulsing relay was taken, or of an idle one refused");
  if (ok) CHECK(startedBy(before1, before2, ms) == k, "pulse-only: a relay test started the other relay");
}
void consolePoll() {}

// --- the invariant ------------------------------------------------------------------------------------------------
static bool checking;  // false: inv() assumes (the start of a step); true: it checks
#define INV(c, msg) do { if (checking) CHECK(c, "invariant: " msg); else ASSUME(c); } while (0)
static int32_t ahead(uint32_t t) { return (int32_t)(t - clk); }  // how far a stamp lies ahead of millis()

static void invRelay(int k) {
  Relay &r = relay(k);
  Coil &c = coil[k], &o = coil[3 - k];
  INV(r._on == c.on, "Relay::_on is the coil");
  INV(!c.on || c.onT <= T, "a coil energized in the past");
  INV(!c.released || c.offT <= T, "a coil released in the past");
  INV(c.len >= 50 && c.len <= 5000, "a pulse's length is one that was asked for");
  if (r._on) {
    INV(r._startAt == 0 && r._pulseUntil != 0, "a coil on is a running pulse");
    INV((int32_t)(r._pulseUntil - toClk(c.onT)) >= 0, "a running pulse ends after it began");
    INV((int32_t)(r._pulseUntil - toClk(c.onT)) <= (int32_t)c.len + 1, "a running pulse ends within its length");
    INV(T - c.onT <= (uint64_t)c.len + 1 + PASS_MAX_MS + SKEW_PASS, "a running pulse has run within its length");
    // Relay::update ran at the pass's `now` and didn't end it: its end lies after that (as it does for one started since).
    INV((int32_t)(r._pulseUntil - toClk(passT)) > 0, "a running pulse ends after the pass's now");
  } else if (r._startAt) {
    INV(r._pulseUntil != 0, "a waiting pulse has an end");
    INV((int32_t)(r._startAt - toClk(passT)) > 0 && (int32_t)(r._startAt - toClk(passT)) <= INTERLOCK_MS + 1 + SKEW_PASS,
        "a waiting pulse starts after the pass's now, within the interlock");
    INV((int32_t)(r._pulseUntil - r._startAt) >= 0 && (int32_t)(r._pulseUntil - r._startAt) <= (int32_t)c.len + 1,
        "a waiting pulse ends within its length of its start");
    INV(!o.on, "a pulse waits only while the other coil is off");
    uint64_t startT = T + ahead(r._startAt);
    INV(!o.released || (int64_t)(startT - o.offT) >= INTERLOCK_MS,
        "a waiting pulse starts INTERLOCK_MS after the other's release");
  } else {
    INV(r._pulseUntil == 0, "an idle relay has no pulse");
  }
  if (r._offAt) {
    INV(c.released && r._offAt == (toClk(c.offT) | 1), "Relay::_offAt is the coil's last release");
    INV(c.offT >= passT || passT - c.offT <= RELEASE_MEMO_MS + 1, "Relay::_offAt is cleared once old (at the pass's now)");
  } else {
    INV(!c.released || T - c.offT >= RELEASE_MEMO_MS - 1, "a recent release is remembered");
    INV(!c.released || c.offT <= T, "a release in the past");
  }
}

static void inv() {
  invRelay(1);
  invRelay(2);
  INV(!(k1.pulsing() && k2.pulsing()), "one relay pulses at a time");
  // The gate's clock: clockBase is millis() extended, as read at baseT, this pass.
  // The pass: inside one, millis() is at most SKEW_PASS ahead of its `now`; the clock was read by the last gateLoop
  // (at most a pass's gap before this `now`) or by a handler since. Between passes, gateLoop has just read it.
  INV(baseT <= T, "the clock was read in the past");
  INV(passT <= T && T - passT <= SKEW_PASS, "a pass ends within SKEW_PASS of its now");
  if (inPass) {
    INV(baseT >= passT || passT - baseT <= IDLE_MAX_MS + SKEW_PASS, "the clock was read last pass");
  } else {
    INV(T - baseT <= SKEW_PASS, "the clock was read this pass");
  }
  INV(bootT <= baseT, "the clock was read since boot");
  INV(seqHave == haveCmd && (!haveCmd || seqLast == lastCmdSeq), "the ghost seq is the last command taken");
  INV((uint32_t)clockBase == toClk(baseT), "clockBase's low bits are millis() when read");
  INV(clockBase - bootAt == baseT - bootT, "clockBase - bootAt is the time since boot");
  INV(cfgQueued <= CFG_QUEUE, "the config queue holds at most CFG_QUEUE");
  INV(cfg.pulse_ms >= 100 && cfg.pulse_ms <= 5000, "pulse_ms in range");
}

// --- a step: any state satisfying the invariant -------------------------------------------------------------------
static void anyRelay(Relay &r, uint8_t pin) {
  r._pin = pin;
  r._on = nondet_bool();
  r._startAt = nondet_u32();
  r._pulseUntil = nondet_u32();
  r._offAt = nondet_u32();
}
static void anyInput(Input &in) {
  in._state = nondet_bool();
  in._raw = nondet_bool();
  in._candidate = nondet_bool();
  in._since = nondet_u32();
}
static void anyCoil(Coil &c) {
  c.on = nondet_bool();
  c.onT = nondet_u64();
  c.released = nondet_bool();
  c.offT = nondet_u64();
  c.len = nondet_u32();
}

static void anyState() {
  T = nondet_u64();
  ASSUME(T >= (1ull << 40) && T <= (1ull << 62));  // far from 0 either way: the ghosts' differences can't wrap
  clk = nondet_u32();
  clkBase = clk - (uint32_t)T;
  anyConfig(ROLE_GATE);
  anyRelay(k1, PIN_K1);
  anyRelay(k2, PIN_K2);
  anyCoil(coil[1]);
  anyCoil(coil[2]);
  anyInput(in1);
  anyInput(in2);
  anyInput(in3);
  anyInput(in4);
  memo = (ResetMemo *)keptRam;
  memo->stage = nondet_u8();
  memo->settled = nondet_u8();
  bootMs = nondet_u32();
  resetStreak = nondet_u8();
  // role_gate.cpp's state machine: anything.
  state = nondet_u8();
  cause = nondet_u8();
  lastResult = nondet_u8();
  target = nondet_u8();
  leaving = nondet_u8();
  targetSince = nondet_u32();
  haveCmd = nondet_bool();
  lastCmdSeq = nondet_u32();
  seqHave = nondet_bool();
  seqLast = nondet_u32();
  lastCmdId = nondet_u16();
  lastCmdAck = nondet_u8();
  leftClosed = nondet_u8();
  lastStatusAt = nondet_u32();
  reportedHeartbeat = nondet_i32();
  reportedRebootLoop = nondet_bool();
  houseSession = nondet_u32();
  houseVerified = nondet_bool();
  settling = nondet_bool();
  steadySince = nondet_u32();
  stuckSince = nondet_u32();
  heldAt = nondet_u32();
  heldTo = nondet_u8();
  closedUnproven = nondet_bool();
  bothSince = nondet_u32();
  fromOpen = nondet_bool();
  neitherSince = nondet_u32();
  disagreed = nondet_bool();
  disagreeSince = nondet_u32();
  rebootAfterCmd = false;  // the bench's fault injection: the run ends at its reset
  rebootAt = 0;
  clockOrigin = nondet_u64() & ((1ULL << 47) - 1);
  bootT = nondet_u64();
  baseT = nondet_u64();
  bootAt = nondet_u32();
  clockBase = nondet_u64();
  cfgQueued = nondet_u8();
  for (int i = 0; i < CFG_QUEUE; i++) {
    uint8_t j = nondet_u8();
    ASSUME(j < PARAM_COUNT && (PARAMS[j].flags & P_REMOTE));
    cfgQueue[i] = { nondet_u32(), &PARAMS[j], nondet_i32() };
    ASSUME(paramValid(cfgQueue[i].p, cfgQueue[i].v));
  }
  houseSessionNow = nondet_u32();
  verifiedNow = nondet_bool();
  inPass = nondet_bool();
  passT = nondet_u64();
}

// --- boot: from reset ---------------------------------------------------------------------------------------------
static void boot() {
  T = 1ull << 40;
  clk = nondet_u32();  // anywhere: the first passes may cross the wrap
  clkBase = clk - (uint32_t)T;
  anyConfig(ROLE_GATE);
  // A crash may have kept anything in the reset memo (the kept RAM's first 4 words; log.cpp's ring follows).
  for (int i = 0; i < 4; i++) keptRam[i] = nondet_u32();
  coil[1].len = coil[2].len = 50;
  appRelaysBegin();
  memoBegin(nondet_u8());
  in1.begin(PIN_IN1, cfg.in1_invert);
  in2.begin(PIN_IN2, cfg.in2_invert);
  in3.begin(PIN_IN3, cfg.in3_invert);
  in4.begin(PIN_IN4, cfg.in4_invert);
  bootT = baseT = passT = T;
  gateBegin();
  inPass = false;
  houseSessionNow = nondet_u32();
  verifiedNow = nondet_bool();
}

// The proof is split at the loop's cut points: the pass's start (time moves on, Relay::update, the radio and flash
// work appLoop guards, up to the link), one frame handled (a CMD or a CFG_SET, its millis() a little later), one
// console relay test, and the role's loop (gateLoop at the pass's `now`). Each segment, from any state satisfying the
// invariant, ends in one satisfying it, and a pass is a sequence of them in any number: so the invariant holds after
// every segment of every run, and the properties checked inside each hold throughout. The Makefile runs each segment
// (-DSEGMENT=...) as its own query.
static bool was[3];
static uint32_t untilAt[3], startAtAt[3], offsAt[3];
static uint32_t offCount[3];

static void snapshot() {
  for (int k = 1; k <= 2; k++) {
    Relay &r = relay(k);
    was[k] = r.pulsing();
    untilAt[k] = r._pulseUntil;
    startAtAt[k] = r._startAt;
    offCount[k] = coil[k].released ? (uint32_t)coil[k].offT : 0;
    waitingAtPass[k] = r._startAt != 0;
  }
}

static void segmentEnd() {
  if (!haveCmd) seqHave = false;  // a new house session (gateLoop): its seqs start over
  for (int k = 1; k <= 2; k++) {
    Relay &r = relay(k);
    bool cut = (coil[k].released ? (uint32_t)coil[k].offT : 0) != offCount[k];
    if (was[k] && r.pulsing() && !cut) {
      CHECK(r._pulseUntil == untilAt[k], "no-restart: a running pulse's end moved");
      CHECK(r._startAt == 0 || r._startAt == startAtAt[k], "no-restart: a waiting pulse's start moved");
    }
  }
  checking = true;
  inv();
#ifdef REACH
  CHECK(false, "reach: the end of the segment is reachable (this run must fail)");
#endif
}

int main() {
#ifdef BOOT
  boot();
  checking = true;
  inv();
#ifdef REACH
  CHECK(false, "reach: the end of the harness is reachable (this run must fail)");
#endif
  return 0;
#else
  anyState();
  checking = false;
  inv();
  snapshot();
#if SEGMENT == SEG_PASS
  // Time moves on, the house session may change; then appLoop up to the link (linkPoll ends the segment).
  ASSUME(!inPass);
  uint32_t d = nondet_u32();
  ASSUME(d >= 1 && d <= (pulsingNow() ? (uint32_t)PASS_MAX_MS : IDLE_MAX_MS));
  advance(d);
  inPass = true;
  passT = T;
  if (nondet_bool()) {
    verifiedNow = nondet_bool();
    houseSessionNow = nondet_u32();
  }
  appLoop();
  ASSUME(false);  // not reached: linkPoll ends it
#elif SEGMENT == SEG_CMD || SEGMENT == SEG_CFG || SEGMENT == SEG_TEST
  ASSUME(inPass);
  uint32_t skew = nondet_u32();
  ASSUME(T + skew - passT <= SKEW_PASS);
  advance(skew);  // a handler's millis() runs ahead of the pass's `now`
#if SEGMENT == SEG_CMD
  deliverCmd();
#elif SEGMENT == SEG_CFG
  deliverCfgSet();
#else
  relayTest();
#endif
  segmentEnd();
#elif SEGMENT == SEG_ROLE
  // gateLoop at the pass's `now`, a little behind millis() if handlers ran.
  ASSUME(inPass);
  gateLoop(toClk(passT));
  if (passT > baseT) baseT = passT;  // uptimeMs read it there, unless a handler had read it later
  inPass = false;
  segmentEnd();
#endif
  return 0;
#endif
}
