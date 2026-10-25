// ESBMC harness: the house board's contact sensor (K2), proved for runs of any length.
//
// An inductive proof, as gate.cpp: inv() holds after boot (-DBOOT) and after one loop pass from any state satisfying it.
// The property, checked at the end of every pass, in physical time (T, whatever millis() does):
//
//   sensor-truth  K2 closes the alarm's contact (energized, sensor_invert 0) only while the last STATUS the house took
//                 said closed, and (linkloss_open) no longer than houseLinkTimeoutMs() after it took it (+1 ms: the
//                 stamps are `now | 1`), as of the loop pass: the one that reads the link down opens it
//   in-order      a STATUS is taken only if its seq is later than the last one taken in the same gate session
//
// K2 pulsed by a console relay test is the installer's own doing and excepted (as in the systests' monitor).
// Everything else in role_house.cpp is arbitrary at the start of a step (K1, the sync windows, the controller's power,
// the pending command...), the inputs on every read; in the pass, STATUS frames of any content, length and seq from any
// gate session, a few per pass. Time between passes as in gate.cpp.
#include "env.h"
#define private public
#include "io.h"
#undef private
#include "io.cpp"
#include "app.cpp"
#include "role_house.cpp"
#include "cfgmodel.h"

#define SEG_PASS 1
#define SEG_STATUS 2
#define SEG_ROLE 3
#ifndef SEGMENT
#define SEGMENT SEG_PASS
#endif
static bool inPass;     // between a pass's start (its `now`, passT) and its houseLoop
static uint64_t passT;

int digitalRead(uint32_t) { return nondet_bool() ? HIGH : LOW; }
void pinMode(uint32_t, uint32_t) {}
void analogWrite(uint32_t, int) {}
void digitalWrite(uint32_t, uint32_t) {}
static uint32_t keptRam[BOARD_KEEP_WORDS];
uint32_t *boardKeep() { return keptRam; }

static LinkStats stats;
static uint32_t gateSessionNow;  // the verified gate session (0 = none)
static bool verifiedNow;
const LinkStats &linkStats() { return stats; }
bool linkPeerVerified() { return verifiedNow; }
uint32_t linkPeerSession() { return gateSessionNow; }
bool linkPending(Slot) { return nondet_bool(); }
void linkAck(uint32_t, uint8_t) {}
void linkSend(uint8_t, const uint8_t *, uint8_t) {}
void linkSendReliable(Slot, uint8_t, const uint8_t *, uint8_t, uint32_t) {}

// Ghosts: the last STATUS taken, as it came.
static bool taken;
static uint64_t takenT;
static uint8_t takenState;
static uint32_t clkBase;
static uint32_t toClk(uint64_t t) { return clkBase + (uint32_t)t; }

// houseOnRx's MSG_STATUS case, called directly (as gate.cpp does).
static void deliverStatus() {
  uint8_t p[ST_LEN];
  for (int i = 0; i < ST_LEN; i++) p[i] = nondet_u8();
  uint8_t len = nondet_bool() ? ST_LEN : nondet_bool() ? ST_LEN_V4 : nondet_u8() % ST_LEN;
  RxMsg m = { MSG_STATUS, nondet_u32(), p, len, 0, 0 };
  if (nondet_bool()) gateSessionNow = nondet_u32();  // a new gate session verified meanwhile
  uint32_t sess0 = statusSession, seq0 = statusSeq, at0 = statusAt;
  handleStatus(m, millis());
  bool took = statusAt != at0 || statusSeq != seq0 || statusSession != sess0;
  if (!took) return;
  CHECK(len >= ST_LEN_V4, "in-order: a short STATUS was taken");
  CHECK(sess0 != gateSessionNow || m.seq > seq0, "in-order: a STATUS no later than the last one taken was taken");
  taken = true;
  takenT = T;
  takenState = p[ST_STATE];
}

static void segmentEnd();
// In the pass-start segment the pass is followed up to the link: the frames and houseLoop are the next segments.
void linkPoll(uint32_t) {
  segmentEnd();
  ASSUME(false);
}

// The console: relay tests of K2 are the exception the property names; anything else it does is outside (config
// writes change the link timeout: anyConfig draws it per step).
void consolePoll() {}

static bool checking;
#define INV(c, msg) do { if (checking) CHECK(c, "invariant: " msg); else ASSUME(c); } while (0)

static void inv() {
  INV(!statusAt || taken, "a STATUS time is a STATUS taken");
  INV(!taken || takenT <= T, "taken in the past");
  if (statusAt) {
    // The firmware's age of it (signed, as appLinkUp reads it) is never less than the real one, capped at
    // STAMP_CAP_MS, and never so large it would read as negative.
    int32_t age = (int32_t)(clk - statusAt);
    uint64_t real = T - takenT;
    // Capped at STAMP_CAP_MS by each houseLoop (capStamps), then a pass's gap at most.
    INV(age >= -2 && age <= (int32_t)(STAMP_CAP_MS + 2 * SKEW_PASS + (inPass ? IDLE_MAX_MS : 0)),
        "statusAt's age is in range (capped once a pass)");
    INV((int64_t)age + 1 >= (int64_t)(real < STAMP_CAP_MS ? real : STAMP_CAP_MS), "statusAt is no later than the take");
  }
  INV(taken ? gateState == takenState : gateState == GS_UNKNOWN, "gateState is the last STATUS taken, or unknown");
  INV(!inPass || (passT <= T && T - passT <= SKEW_PASS), "a pass's handlers run within SKEW_PASS of its now");
}

static void anyState() {
  T = nondet_u64();
  ASSUME(T >= (1ull << 40) && T <= (1ull << 62));
  clk = nondet_u32();
  clkBase = clk - (uint32_t)T;
  anyConfig(ROLE_HOUSE);
  k1._on = nondet_bool(); k1._startAt = nondet_u32(); k1._pulseUntil = nondet_u32(); k1._offAt = nondet_u32();
  k2._on = nondet_bool(); k2._startAt = nondet_u32(); k2._pulseUntil = nondet_u32(); k2._offAt = nondet_u32();
  Input *ins[] = { &in1, &in2, &in3, &in4 };
  for (Input *in : ins) {
    in->_state = nondet_bool(); in->_raw = nondet_bool(); in->_candidate = nondet_bool(); in->_since = nondet_u32();
  }
  memo = (ResetMemo *)keptRam;
  bootMs = nondet_u32();
  // role_house.cpp: anything.
  gateState = nondet_u8(); gateInputs = nondet_u8(); gateCause = nondet_u8(); gateResult = nondet_u8();
  gateTarget = nondet_u8(); gateHeartbeat = nondet_u16(); gateTravel = nondet_u16(); haveStatus = nondet_bool();
  statusSession = nondet_u32(); statusSeq = nondet_u32(); statusAt = nondet_u32();
  clockSession = nondet_u32(); clockAt = nondet_u32(); clockGate = nondet_u64(); clockWait = nondet_bool();
  gateLeft = nondet_u8(); leftWhenClosed = nondet_u8(); sensorClosed = nondet_bool(); missedUntil = nondet_u32();
  lastEnd = nondet_u8(); betweenSince = nondet_u32(); stuckSince = nondet_u32(); gateCmdId = nondet_u16();
  linkUp = nondet_bool(); armed = nondet_bool(); armAt = nondet_u32(); everArmed = nondet_bool();
  shellyLevel = nondet_bool(); syncActive = nondet_bool(); syncUntil = nondet_u32(); syncExpect = nondet_bool();
  settleUntil = nondet_u32(); resyncing = nondet_bool(); resyncUntil = nondet_u32(); mismatchSince = nondet_u32();
  checkSoon = nondet_bool(); k1WasPulsing = nondet_bool(); ctrlPower = nondet_bool(); senseDark = nondet_bool();
  senseLitAt = nondet_u32(); pendingAction = nondet_u8(); pendingAt = nondet_u32(); cmdId = nondet_u16();
  cmdAction = nondet_u8(); cmdResult = nondet_i32(); cmdUnreported = nondet_bool();
  taken = nondet_bool(); takenT = nondet_u64(); takenState = nondet_u8();
  inPass = nondet_bool(); passT = nondet_u64();
  gateSessionNow = nondet_u32();
  verifiedNow = nondet_bool();
}

static void boot() {
  T = 1ull << 40;
  clk = nondet_u32();
  clkBase = clk - (uint32_t)T;
  anyConfig(ROLE_HOUSE);
  for (int i = 0; i < 4; i++) keptRam[i] = nondet_u32();
  appRelaysBegin();
  memoBegin(nondet_u8());
  in1.begin(PIN_IN1, cfg.in1_invert);
  in2.begin(PIN_IN2, cfg.in2_invert);
  in3.begin(PIN_IN3, cfg.in3_invert);
  in4.begin(PIN_IN4, cfg.in4_invert);
  houseBegin();
  gateSessionNow = 0;
  inPass = false;
}

// As gate.cpp, the step is split at the loop's cut points: the pass's start (time moves on, Relay::update, up to the
// link), one STATUS handled (its millis() a little later), and houseLoop at the pass's `now`, where the sensor is
// judged (a pass's gap at most after the link times out: the watchdog bounds a pass at 8 s, the bench saw 0.5 s).
static void segmentEnd() {
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
#if SEGMENT == SEG_PASS
  ASSUME(!inPass);
  uint32_t d = nondet_u32();
  ASSUME(d >= 1 && d <= IDLE_MAX_MS);
  advance(d);
  inPass = true;
  passT = T;
  appLoop();  // linkPoll ends the segment
  ASSUME(false);
#elif SEGMENT == SEG_STATUS
  ASSUME(inPass);
  uint32_t skew = nondet_u32();
  ASSUME(T + skew - passT <= SKEW_PASS);
  advance(skew);
  deliverStatus();
  segmentEnd();
#elif SEGMENT == SEG_ROLE
  ASSUME(inPass);
  bool test = k2.pulsing();
  houseLoop(toClk(passT));
  inPass = false;
  // sensor-truth, sensor_invert 0: K2 energized is the contact closed.
  if (!cfg.sensor_invert && k2.on() && !test && !k2.pulsing()) {
    CHECK(taken && takenState == GS_CLOSED, "sensor-truth: K2 closed without a closed STATUS taken");
    // <= : statusAt is stamped `now | 1`, so the link reads up for the timeout's last millisecond too.
    if (cfg.linkloss_open)
      CHECK((takenT >= passT ? 0 : passT - takenT) <= houseLinkTimeoutMs(), "sensor-truth: K2 closed past the link timeout after the STATUS");
  }
  segmentEnd();
#endif
  return 0;
#endif
}
