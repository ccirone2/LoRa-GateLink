// Gate state, cause, boot settle, the between hold and AC power (CLAUDE.md "Behavioural invariants"), through the
// whole simulated site (world.h): both boards' firmware, the opener, the controller and the radio between them.
//
// Timings below are exact where the firmware makes them so: the gate's loop runs every simulated millisecond, inputs
// debounce for debounce_ms (50) from the pass that first sees them change, and the gate stamps the start of a
// between with `now | 1`.
#include <Arduino.h>
#include <limits.h>
#include <stdio.h>
#include <exception>
#include "world.h"

namespace {

enum { CAUSE_NONE_ = 0, CAUSE_LORA_ = 1, CAUSE_EXTERNAL_ = 2 };  // roles.h Cause
enum { ACT_OPEN_ = 1, ACT_CLOSE_ = 2 };                          // roles.h Action
enum { MSG_STATUS_ = 5 };                                        // link.h MsgType
enum { RES_NO_POWER_ = 4 };                                      // link.h AckResult
const uint32_t MISMATCH_MS = 75000;                              // mismatch_timeout_s default
const uint32_t SYNC_MS = 3000;                                   // sync_window_ms default
const int ANY = INT_MIN;

// The site, failing the test (not aborting the run) on a monitor breach. world.cpp hands the breaches of a World to an
// after-test hook that throws them outside the runner's try block, so a test that also failed a CHECK terminated the
// whole program; here they are reported in the test, and printed when it has already failed.
struct Site : World {
  ~Site() noexcept(false) {
    if (violations.empty()) return;
    std::string all = "invariant monitors:";
    for (const std::string &v : violations) all += "\n        " + v;
    violations.clear();
    if (std::uncaught_exceptions()) {
      printf("      %s\n", all.c_str());
      return;
    }
    throw Failure(all);
  }
};

struct Found {
  bool ok = false;
  LogEv e{};
};

// The first `ev` on b since log index `from` (with a, and b, if given).
Found find(const Board &b, const char *ev, size_t from, int a = ANY, int bv = ANY) {
  Found f;
  for (size_t i = from; i < b.logs.size(); i++) {
    const LogEv &e = b.logs[i];
    if (e.ev == ev && (a == ANY || e.a == a) && (bv == ANY || e.b == bv)) {
      f.ok = true;
      f.e = e;
      return f;
    }
  }
  return f;
}

int countSince(const Board &b, const char *ev, size_t from, int a = ANY, int bv = ANY) {
  int n = 0;
  for (size_t i = from; i < b.logs.size(); i++) {
    const LogEv &e = b.logs[i];
    n += e.ev == ev && (a == ANY || e.a == a) && (bv == ANY || e.b == bv);
  }
  return n;
}

Found waitFor(World &w, Board &b, const char *ev, size_t from, uint32_t maxMs, int a = ANY, int bv = ANY) {
  Found f;
  w.runUntil([&] {
    f = find(b, ev, from, a, bv);
    return f.ok;
  }, maxMs);
  return f;
}

// Runs `ms`, checking `ok` every millisecond; returns the world time it first failed (0 = never).
uint32_t watch(World &w, uint32_t ms, const std::function<bool()> &ok) {
  uint32_t bad = 0;
  for (uint32_t i = 0; i < ms; i++) {
    w.step();
    if (!bad && !ok()) bad = w.now;
  }
  return bad;
}

std::string str(const JsonDocument &d, const char *k) {
  return d[k].isNull() ? "(null)" : d[k].as<std::string>();
}

#define CHECK_STR(expr, want) \
  do { \
    std::string got_ = (expr), want_ = (want); \
    if (got_ != want_) \
      throw Failure(where(__FILE__, __LINE__, "CHECK_STR(" #expr "): \"" + got_ + "\" != \"" + want_ + "\"")); \
  } while (0)

// First frame of `type` the board put on the air at or after world time `at` (nullptr if none).
const AirFrame *firstSent(const World &w, const Board &b, uint8_t type, uint32_t at) {
  for (const AirFrame &f : w.air)
    if (f.from == b.idx && f.type() == type && (int32_t)(f.start - at) >= 0) return &f;
  return nullptr;
}

// The user opens the gate from Alarm.com and it gets there (our command, cause lora); then the sync window K1 opened
// runs out, so a later user action counts.
void openByUser(World &w) {
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found op = waitFor(w, w.gate, "gate_state", g0, 15000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  w.run(4000);
}

// Waits for the gate to report a state (its own log) and the house to have it too.
void waitBoth(World &w, int gs, uint32_t maxMs) {
  CHECK(w.runUntil([&] { return w.gateSees() == gs && w.houseSees() == gs; }, maxMs));
}

// Steps the world with the opener deaf to its OPEN input (the pulse arrives, nothing happens): while gate K1 is on, the
// opener's debounced OPEN input already reads pressed, so it never sees an edge.
uint32_t runDeafToOpen(World &w, uint32_t ms, const std::function<bool()> &ok) {
  uint32_t bad = 0;
  for (uint32_t i = 0; i < ms; i++) {
    if (w.gate.coil(1)) w.opener.open.raw = w.opener.open.stable = true;
    w.step();
    if (!bad && !ok()) bad = w.now;
  }
  return bad;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// [state-from-limits]

// [state-from-limits] [cause]
TEST(gate_state_command_reports_limits_not_the_command) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found p = waitFor(w, w.gate, "pulse", g0, 3000, 1);
  CHECK(p.ok);
  // Leaving the closed limit: between, ours.
  Found btw = waitFor(w, w.gate, "gate_state", g0, 3000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  // Mid-travel the command is known, but the gate is between and says so.
  w.run(3000);
  CHECK(!w.opener.atOpen() && !w.opener.atClosed());
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "between");
  CHECK_STR(str(s, "target"), "open");
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_OPEN_), 0);
  // Open only once the open limit has read for the debounce.
  uint32_t reachedAt = 0;
  CHECK(w.runUntil([&] {
    if (!reachedAt && w.opener.in(1)) reachedAt = w.now;
    return find(w.gate, "gate_state", g0, GS_OPEN_).ok;
  }, 10000));
  Found op = find(w.gate, "gate_state", g0, GS_OPEN_);
  CHECK(reachedAt != 0);
  CHECK_IN(op.e.at - reachedAt, 50, 52);  // debounce_ms 50
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  s = w.gate.status();
  CHECK_STR(str(s, "gate"), "open");
  CHECK_STR(str(s, "last_result"), "reached");
  CHECK_STR(str(s, "target"), "");
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 2);
}

// [state-from-limits] [cause]
TEST(gate_state_deaf_opener_stays_closed_until_travel_timeout) {
  // The opener ignores our OPEN pulse: the gate keeps reporting closed (its limit) and the contact sensor stays closed
  // the whole time; the command ends in a travel timeout, and the house puts the controller back off at once.
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  auto closedAll = [&] { return w.opener.atClosed() && w.sensorClosed() && !w.house.coil(1); };
  CHECK_EQ(runDeafToOpen(w, 3000, closedAll), 0);
  Found p = find(w.gate, "pulse", g0, 1);
  CHECK(p.ok);
  CHECK(w.opener.presses[0].size() == 1);  // the pulse reached the input
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "closed");
  CHECK_STR(str(s, "target"), "open");
  CHECK_EQ(runDeafToOpen(w, p.e.at + 59900 - w.now, closedAll), 0);
  CHECK_EQ(countSince(w.gate, "travel_timeout", g0), 0);
  // (K1 may cycle from here on: the resync)
  CHECK_EQ(runDeafToOpen(w, 300, [&] { return w.opener.atClosed() && w.sensorClosed(); }), 0);
  Found tt = find(w.gate, "travel_timeout", g0);
  CHECK(tt.ok);
  CHECK_EQ(tt.e.a, GS_OPEN_);
  CHECK_IN(tt.e.at - p.e.at, 60000, 60002);  // travel_timeout_s 60
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 0);
  s = w.gate.status();
  CHECK_STR(str(s, "gate"), "closed");
  CHECK_STR(str(s, "last_result"), "timeout");
  CHECK_STR(str(s, "target"), "");
  CHECK_STR(str(w.house.status(), "last_result"), "timeout");
  // Resync at once (mismatch_timeout_s is 75 s): the controller goes back off.
  Found rs = waitFor(w, w.house, "resync", h0, 2000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(rs.e.at - tt.e.at, 0, 1000);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 3000));
  CHECK(w.sensorClosed());
}

// [state-from-limits] [cause]
TEST(gate_state_ignored_pulse_keeps_reporting_the_limit) {
  // The siren holds OPEN, so the opener ignores our CLOSE: the gate keeps reporting open (its limit), never closed or
  // between, and the command ends in a travel timeout, which makes the house put the controller back at once.
  Site w;
  w.commission();
  openByUser(w);
  w.opener.extOpenHold = true;
  w.run(200);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(false);
  Found p = waitFor(w, w.gate, "pulse", g0, 3000, 2);
  CHECK(p.ok);
  w.run(2000);
  CHECK(w.opener.atOpen());
  CHECK(w.opener.presses[1].size() == 1);  // the pulse reached the CLOSE input
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "open");
  CHECK_STR(str(s, "target"), "closed");
  // Up to the travel timeout: nothing moves, nothing is reported.
  CHECK_EQ(watch(w, p.e.at + 59900 - w.now, [&] { return w.gateSees() == GS_OPEN_ && w.opener.atOpen(); }), 0);
  Found tt = waitFor(w, w.gate, "travel_timeout", g0, 300);
  CHECK(tt.ok);
  CHECK_EQ(tt.e.a, GS_CLOSED_);
  CHECK_IN(tt.e.at - p.e.at, 60000, 60002);  // travel_timeout_s 60
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  s = w.gate.status();
  CHECK_STR(str(s, "gate"), "open");
  CHECK_STR(str(s, "last_result"), "timeout");
  CHECK_STR(str(s, "target"), "");
  // The house resyncs the controller to open right away (not after mismatch_timeout_s 75).
  Found rs = waitFor(w, w.house, "resync", h0, 3000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK_IN(rs.e.at - tt.e.at, 0, 1000);
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 3000));
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
}

// [state-from-limits] [cause]
TEST(gate_state_jammed_gate_reads_between_never_the_target) {
  Site w;
  w.commission();
  w.opener.stuck = true;
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  Found p = waitFor(w, w.gate, "pulse", g0, 3000, 1);
  CHECK(p.ok);
  Found btw = waitFor(w, w.gate, "gate_state", g0, 3000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  CHECK(w.opener.pos == 1 && w.opener.dir == 0);  // jammed just off the closed limit
  // K2 opens as soon as the house hears between; K1 holds the closed level through the travel.
  CHECK(w.runUntil([&] { return w.houseSees() == GS_BETWEEN_; }, 2000));
  w.run(100);
  CHECK(!w.sensorClosed());
  w.run(30000);
  CHECK(!w.house.coil(1));
  Found tt = waitFor(w, w.gate, "travel_timeout", g0, 35000);
  CHECK(tt.ok);
  CHECK_EQ(tt.e.a, GS_OPEN_);
  CHECK_IN(tt.e.at - p.e.at, 60000, 60002);
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 1);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "between");
  CHECK_STR(str(s, "last_result"), "timeout");
  CHECK_STR(str(s, "target"), "");
  // The house's own hold runs out: not closed.
  CHECK(w.runUntil([&] { return w.house.coil(1); }, 5000));
  CHECK_EQ(countSince(w.house, "resync", h0), 0);  // the controller (on) already agrees
}

// [state-from-limits]
TEST(gate_state_dead_open_limit_never_reports_open) {
  // The opener gets to open but its open limit never reads: the gate is between (where the command was going doesn't
  // matter) until the travel timeout; it reports open only once the limit reads, and by then it's not ours.
  Site w;
  w.commission();
  w.opener.force[0] = 0;
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found p = waitFor(w, w.gate, "pulse", g0, 3000, 1);
  CHECK(p.ok);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 10000));
  w.run(2000);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "between");
  CHECK_STR(str(s, "target"), "open");
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_OPEN_), 0);
  Found tt = waitFor(w, w.gate, "travel_timeout", g0, 60000);
  CHECK(tt.ok);
  CHECK_EQ(tt.e.a, GS_OPEN_);
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_OPEN_), 0);
  CHECK_STR(str(w.gate.status(), "gate"), "between");
  size_t g1 = w.gate.logs.size();
  uint32_t t = w.now + 1;
  w.opener.force[0] = -1;
  Found op = waitFor(w, w.gate, "gate_state", g1, 200);
  CHECK(op.ok);
  CHECK_EQ(op.e.a, GS_OPEN_);
  CHECK_EQ(op.e.b, CAUSE_EXTERNAL_);  // the target is over
  CHECK_IN(op.e.at - t, 50, 52);
}

// [state-from-limits]
TEST(gate_state_both_limits_read_fault) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  uint32_t t = w.now + 1;
  w.opener.force[0] = 1;  // the open limit reads too while the gate is closed
  Found f = waitFor(w, w.gate, "gate_state", g0, 1000);
  CHECK(f.ok);
  CHECK_EQ(f.e.a, GS_FAULT_);
  CHECK_EQ(f.e.b, CAUSE_EXTERNAL_);
  CHECK_IN(f.e.at - t, 50, 52);  // no between hold: it isn't between
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_; }, 2000));
  w.run(100);
  CHECK(w.house.coil(1));  // not closed
  CHECK(!w.sensorClosed());
  JsonDocument h = w.house.status();
  CHECK(h["remote"]["open_limit"] == true);
  CHECK(h["remote"]["close_limit"] == true);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  w.run(4000);
  size_t g1 = w.gate.logs.size();
  w.opener.force[0] = -1;
  // Which one lied can't be told: the closed limit alone isn't trusted until the open limit has read alone.
  w.run(5000);
  CHECK_EQ(countSince(w.gate, "gate_state", g1), 0);
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  // The gate opens (a relay test, a command runs as well: AC is on) and its open limit reads alone: trusted again.
  JsonDocument r = w.gate.request("relay.test", "\"k\":1,\"ms\":500");
  CHECK(r["ok"] == true);
  Found op = waitFor(w, w.gate, "gate_state", g1, 15000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  w.run(2000);
  size_t g2 = w.gate.logs.size();
  CHECK(w.gate.request("relay.test", "\"k\":2,\"ms\":500")["ok"] == true);
  Found c = waitFor(w, w.gate, "gate_state", g2, 15000, GS_CLOSED_);
  CHECK(c.ok);
  waitBoth(w, GS_CLOSED_, 2000);
  w.run(100);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
}

// Both limits for less than BOTH_LIMITS_HOLD_MS (500 ms; a glitch, the relays switching over): a moment's fault, and the
// closed limit is trusted again as soon as the open one releases, not held until the gate next opens.
// [state-from-limits]
TEST(gate_state_short_overlap_of_both_limits_is_not_held) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size();
  w.opener.force[0] = 1;
  w.run(300);
  w.opener.force[0] = -1;
  Found f = waitFor(w, w.gate, "gate_state", g0, 1000, GS_FAULT_);
  CHECK(f.ok);
  Found c = waitFor(w, w.gate, "gate_state", g0, 1000, GS_CLOSED_);
  CHECK(c.ok);
  CHECK_IN(c.e.at - f.e.at, 295, 305);
  waitBoth(w, GS_CLOSED_, 2000);
  w.run(100);
  CHECK(w.sensorClosed());
  CHECK(!w.house.coil(1));
}

// A closed limit stuck on (the AUX2 relay never energizing: its NC contact reads closed wherever the gate is) reads
// fault from the gate's first full opening on, and not closed again until the open limit reads alone, a restart
// excepted. Before that opening it can't be told from a closed gate. ROADMAP F14, threat closed-limit-nc-fail-unsafe.
// [state-from-limits]
TEST(gate_state_stuck_closed_limit_reads_fault_after_the_first_full_open) {
  Site w;
  w.commission();
  w.opener.force[1] = 1;
  size_t g0 = w.gate.logs.size();
  w.user(true);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_BETWEEN_), 0);  // the stuck limit hides the travel
  Found f = waitFor(w, w.gate, "gate_state", g0, 1000, GS_FAULT_);
  CHECK(f.ok);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_; }, 2000));
  w.run(SYNC_MS);  // K1 went on with the fault: a switch change inside its sync window is taken as its echo
  // Closed again, through the travel and at the limit: fault all along, the house never shows it closed.
  size_t g1 = w.gate.logs.size();
  w.user(false);
  CHECK_EQ(watch(w, 20000, [&] { return !w.sensorClosed() && w.house.coil(1); }), 0);
  CHECK(w.opener.atClosed());
  CHECK_EQ(countSince(w.gate, "gate_state", g1), 0);
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  // Repaired with the gate closed: still fault (nothing has shown the closed limit works) until the gate restarts.
  w.opener.force[1] = -1;
  w.run(5000);
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  w.gate.reset(PM_RCAUSE_SYST);
  w.run(1000);
  CHECK(poll(w, [&] { return w.gate.status()["gate"] == "closed"; }, 8000));
  CHECK(w.runUntil([&] { return w.houseSees() == GS_CLOSED_ && w.sensorClosed(); }, 5000));
}

// The both-limits hold survives a crash: a stuck closed limit, the gate part-open (the open limit released), then a
// watchdog or brown-out reset reads fault from the first STATUS on, never closed. A deliberate reset (software, the
// reset pin) clears it, as does a power-on: then the stuck limit reads closed until the next full opening, as before
// the hold. ROADMAP F14, threat closed-limit-nc-fail-unsafe. [state-from-limits]
TEST(gate_state_stuck_closed_limit_hold_survives_a_crash_reset) {
  const uint8_t causes[] = { PM_RCAUSE_WDT, PM_RCAUSE_BOD33, PM_RCAUSE_SYST, PM_RCAUSE_EXT, 0 };  // 0: power-on
  for (uint8_t rc : causes) {
    Site w;
    w.commission();
    w.opener.force[1] = 1;
    w.user(true);
    CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
    CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_; }, 3000));
    w.run(1000);  // both read past BOTH_LIMITS_HOLD_MS: held
    w.opener.force[0] = 0;  // part-open: the open limit released, the stuck closed one still reading
    w.run(2000);
    CHECK_STR(str(w.gate.status(), "gate"), "fault");
    size_t g0 = w.gate.logs.size();
    if (rc) w.gate.reset(rc);
    else w.gate.powerOn();
    bool kept = rc == PM_RCAUSE_WDT || rc == PM_RCAUSE_BOD33;
    if (kept) {
      // From the boot on, the house never sees it closed: K2 stays open and K1 shows not closed.
      CHECK_EQ(watch(w, 15000, [&] { return !w.sensorClosed() && w.house.coil(1); }), 0);
      CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_CLOSED_), 0);
      CHECK_STR(str(w.gate.status(), "gate"), "fault");
    } else {
      w.run(1000);  // through the bootloader
      CHECK(poll(w, [&] { return w.gate.status()["gate"] == "closed"; }, 8000));
    }
  }
}

// Once the open limit reads alone the hold is cleared for the next boot too: repaired, opened fully and closed, a crash
// afterwards reads closed again, not a stale fault. [state-from-limits]
TEST(gate_state_cleared_hold_stays_cleared_through_a_crash_reset) {
  Site w;
  w.commission();
  w.opener.force[1] = 1;
  w.user(true);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  w.run(1500);  // held
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  w.opener.force[1] = -1;  // repaired at the open limit: it reads alone
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  w.run(SYNC_MS);  // past the sync window K1 opened on going on with the fault
  w.user(false);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 20000));
  waitBoth(w, GS_CLOSED_, 3000);
  w.gate.reset(PM_RCAUSE_WDT);
  w.run(1000);  // through the bootloader
  CHECK(poll(w, [&] { return w.gate.status()["gate"] == "closed"; }, 8000));
  CHECK(w.runUntil([&] { return w.houseSees() == GS_CLOSED_ && w.sensorClosed(); }, 8000));
}

// The opener's controller restarting with the gate open: its AUX relays drop, so the open limit releases and the closed
// one's NC contact reads, for as long as the restart takes. No travel goes from open to closed without reading between:
// the gate holds that closed as fault until the open limit reads alone again. The relays change in either order (both
// or neither reading for a moment) or together. [state-from-limits]
TEST(gate_state_controller_restart_at_open_reads_fault_not_closed) {
  for (int order = 0; order < 3; order++) {
    Site w;
    w.commission();
    w.user(true);
    CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
    waitBoth(w, GS_OPEN_, 3000);
    w.run(3000);
    size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
    if (order == 1) {  // the closed limit first: both read for 20 ms
      w.opener.force[1] = 1;
      w.run(20);
    } else if (order == 2) {  // the open limit first: neither reads for 20 ms
      w.opener.force[0] = 0;
      w.run(20);
    }
    w.opener.force[0] = 0;
    w.opener.force[1] = 1;
    CHECK_EQ(watch(w, 20000, [&] { return !w.sensorClosed() && w.house.coil(1); }), 0);
    CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_CLOSED_), 0);
    CHECK_STR(str(w.gate.status(), "gate"), "fault");
    CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_; }, 1000));
    // Restarted: the open limit reads alone, the gate is open again, and a close then reads closed.
    w.opener.force[0] = w.opener.force[1] = -1;
    waitBoth(w, GS_OPEN_, 3000);
    w.run(3000);
    w.user(false);
    CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 20000));
    waitBoth(w, GS_CLOSED_, 3000);
    CHECK(w.runUntil([&] { return w.sensorClosed(); }, 2000));
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);  // the close; nothing while it read fault
  }
}

// Power back with the gate open and the opener restarting: its 24 V wets the closed limit's NC contact before it
// energizes the AUX2 relay, so for a moment the closed limit reads alone. Out of no_power a move into closed is held
// BOOT_SETTLE_MS, from when that reading starts: a relay energized within it never shows as closed, whether the limit
// reads with IN3, before it (IN3 settling late), or seconds after it. [state-from-limits]
TEST(gate_state_power_return_with_the_aux_relays_late_never_reads_closed) {
  for (int order = 0; order < 3; order++) {
    Site w;
    w.commission();
    w.user(true);
    CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
    waitBoth(w, GS_OPEN_, 3000);
    w.run(3000);
    size_t g0 = w.gate.logs.size();
    w.opener.ac = false;
    w.opener.battery = false;  // dead: its limits drop with IN3
    CHECK(waitFor(w, w.gate, "gate_state", g0, 2000, GS_NO_POWER_).ok);
    w.run(5000);
    size_t g1 = w.gate.logs.size(), h1 = w.house.logs.size();
    w.opener.force[0] = 0;  // the AUX relays not energized yet
    w.opener.force[1] = order == 2 ? 0 : 1;  // the closed limit's NC contact reads (order 2: not yet)
    if (order == 1) w.opener.force[2] = 0;  // IN3 settles 500 ms after the closed limit
    w.opener.ac = true;
    w.opener.battery = true;
    auto notClosed = [&] { return !w.sensorClosed() && w.house.coil(1); };
    if (order == 1) {
      CHECK_EQ(watch(w, 500, notClosed), 0);
      w.opener.force[2] = -1;
    } else if (order == 2) {  // IN3 back, nothing reads for 2.8 s, then the closed limit for a second
      CHECK_EQ(watch(w, 2800, notClosed), 0);
      w.opener.force[1] = 1;
    }
    CHECK_EQ(watch(w, order == 2 ? 1000 : 2500, notClosed), 0);
    w.opener.force[0] = w.opener.force[1] = -1;  // energized: the open limit reads
    Found op = waitFor(w, w.gate, "gate_state", g1, 2000, GS_OPEN_);
    CHECK(op.ok);
    CHECK_EQ(op.e.b, CAUSE_NONE_);
    CHECK_EQ(countSince(w.gate, "gate_state", g1, GS_CLOSED_), 0);
    CHECK_EQ(countSince(w.house, "gate_state", h1, GS_CLOSED_), 0);
    CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 2000));
  }
}

// ---------------------------------------------------------------------------------------------------------------
// [cause]

// [cause] [state-from-limits]
TEST(gate_state_external_moves_are_external) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.extPress(true, 300);  // AES / local button
  Found btw = waitFor(w, w.gate, "gate_state", g0, 2000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_EXTERNAL_);
  Found op = waitFor(w, w.gate, "gate_state", g0, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_EXTERNAL_);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 2000));
  CHECK_STR(str(w.house.status(), "cause"), "external");
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 2000));  // the controller follows K1
  w.run(4000);
  size_t g1 = w.gate.logs.size();
  w.extPress(false, 300);
  Found btw2 = waitFor(w, w.gate, "gate_state", g1, 2000);
  CHECK(btw2.ok);
  CHECK_EQ(btw2.e.a, GS_BETWEEN_);
  CHECK_EQ(btw2.e.b, CAUSE_EXTERNAL_);
  Found cl = waitFor(w, w.gate, "gate_state", g1, 10000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.b, CAUSE_EXTERNAL_);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 2000));
  w.run(4000);
  // The siren sensor holding OPEN.
  size_t g2 = w.gate.logs.size();
  w.opener.extOpenHold = true;
  Found btw3 = waitFor(w, w.gate, "gate_state", g2, 2000);
  CHECK(btw3.ok);
  CHECK_EQ(btw3.e.a, GS_BETWEEN_);
  CHECK_EQ(btw3.e.b, CAUSE_EXTERNAL_);
  Found op3 = waitFor(w, w.gate, "gate_state", g2, 10000, GS_OPEN_);
  CHECK(op3.ok);
  CHECK_EQ(op3.e.b, CAUSE_EXTERNAL_);
  w.opener.extOpenHold = false;
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 2000));
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
}

// [cause]
TEST(gate_state_external_right_after_our_command_is_external) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found op = waitFor(w, w.gate, "gate_state", g0, 15000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  // The moment our command is done, someone else closes the gate.
  size_t g1 = w.gate.logs.size();
  w.extPress(false, 300);
  Found btw = waitFor(w, w.gate, "gate_state", g1, 2000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_EXTERNAL_);
  Found cl = waitFor(w, w.gate, "gate_state", g1, 10000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.b, CAUSE_EXTERNAL_);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  // The house follows (K1 off, the controller off) without commanding anything.
  CHECK(w.runUntil([&] { return !w.alarmSwitch() && w.sensorClosed(); }, 3000));
  CHECK_EQ(w.house.count("cmd_sent"), 1);
}

// [cause]
TEST(gate_state_override_to_the_far_limit_ends_the_target_with_timeout) {
  // Our OPEN, then AES/local CLOSE mid-travel: back at closed is not ours, and the command is over (timeout).
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found btw = waitFor(w, w.gate, "gate_state", g0, 3000, GS_BETWEEN_);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  w.run(2000);
  w.extPress(false, 300);
  Found cl = waitFor(w, w.gate, "gate_state", g0, 10000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.b, CAUSE_EXTERNAL_);
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 2);  // between (ours), closed (external): nothing else
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "last_result"), "timeout");
  CHECK_STR(str(s, "target"), "");
  CHECK_EQ(countSince(w.gate, "travel_timeout", g0), 0);  // ended by the limit, not the timer
  CHECK(w.runUntil([&] { return w.houseSees() == GS_CLOSED_; }, 2000));
  JsonDocument h = w.house.status();
  CHECK_STR(str(h, "last_result"), "timeout");
  CHECK_STR(str(h, "cause"), "external");
  // A later external OPEN is external too: the target is gone.
  w.run(3000);
  size_t g1 = w.gate.logs.size();
  w.extPress(true, 300);
  Found b2 = waitFor(w, w.gate, "gate_state", g1, 2000);
  CHECK(b2.ok);
  CHECK_EQ(b2.e.b, CAUSE_EXTERNAL_);
}

// [cause]
TEST(gate_state_override_to_the_far_limit_resyncs_the_controller_at_once) {
  // The house held K1 at closed through the travel (holdingTravel()), so the mismatch never started; the timeout
  // result must still resync at once, not mismatch_timeout_s (75 s) later.
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  CHECK(waitFor(w, w.gate, "gate_state", g0, 3000, GS_BETWEEN_).ok);
  w.run(2000);
  w.extPress(false, 300);
  Found cl = waitFor(w, w.gate, "gate_state", g0, 10000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_CLOSED_; }, 2000));
  CHECK_STR(str(w.house.status(), "last_result"), "timeout");
  CHECK(w.alarmSwitch());  // the user's ON still showing, the gate closed
  // role_gate.cpp: "reporting it as a timeout makes the house resync its controller at once"
  Found rs = waitFor(w, w.house, "resync", h0, 5000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 3000));
}

// [cause]
TEST(gate_state_our_reversal_before_leaving_the_limit_is_ours) {
  // From open: CLOSE, then the user flips straight back to OPEN while the gate still reads open. The CLOSE pulse is
  // what moves it off the open limit (the reversal's target): that between is ours, and so is open again.
  Site w;
  w.commission();
  openByUser(w);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(false);
  Found cs = waitFor(w, w.house, "cmd_sent", h0, 2000, ACT_CLOSE_);
  CHECK(cs.ok);
  // Back on once the house knows the gate is heading for closed, while the gate still reads open.
  CHECK(poll(w, [&] { return str(w.house.status(), "target") == "closed"; }, 1000, 2));
  w.user(true);
  Found os = waitFor(w, w.house, "cmd_sent", h0, 2000, ACT_OPEN_);
  CHECK(os.ok);
  Found rx = waitFor(w, w.gate, "cmd_rx", g0, 2000, ACT_OPEN_);
  CHECK(rx.ok);
  Found btw = waitFor(w, w.gate, "gate_state", g0, 3000);
  CHECK(btw.ok);
  CHECK(btw.e.at > rx.e.at);               // the reversal reached the gate while it still read open
  CHECK(w.opener.presses[1].size() == 1);  // and the CLOSE pulse is what moved it off the open limit
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  Found op = waitFor(w, w.gate, "gate_state", g0, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  CHECK_STR(str(w.gate.status(), "last_result"), "reached");
  CHECK_EQ(countSince(w.gate, "pulse", g0, 2), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0, 1), 1);
}

// [cause]
TEST(gate_state_leaving_mark_does_not_outlive_our_pulse) {
  // The siren holds OPEN: our CLOSE is ignored and the user flips back to OPEN (a reversal while the gate reads open).
  // Neither pulse moves the gate. Seconds later the AES closes it: that is not ours. (`leaving`, the limit our pulse
  // is moving the gate off, stayed set as long as the target, so this move was attributed to lora.)
  Site w;
  w.commission();
  openByUser(w);
  w.opener.extOpenHold = true;
  w.run(200);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(false);
  CHECK(waitFor(w, w.gate, "pulse", g0, 3000, 2).ok);
  w.run(1500);
  w.user(true);
  CHECK(waitFor(w, w.house, "cmd_sent", h0, 2000, ACT_OPEN_).ok);
  CHECK(waitFor(w, w.gate, "pulse", g0, 2000, 1).ok);
  w.run(1000);
  CHECK(w.opener.atOpen());
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);  // the gate never moved
  CHECK_STR(str(w.gate.status(), "target"), "open");
  w.opener.extOpenHold = false;
  w.run(3000);
  size_t g1 = w.gate.logs.size();
  w.extPress(false, 300);
  Found btw = waitFor(w, w.gate, "gate_state", g1, 2000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_EXTERNAL_);
}

// [cause]
TEST(gate_state_relay_test_sets_a_target) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size();
  JsonDocument r = w.gate.request("relay.test", "\"k\":1,\"ms\":500");
  CHECK(r["ok"] == true);
  CHECK_STR(str(w.gate.status(), "target"), "open");
  Found btw = waitFor(w, w.gate, "gate_state", g0, 2000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  Found op = waitFor(w, w.gate, "gate_state", g0, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  CHECK_STR(str(w.gate.status(), "last_result"), "reached");
  w.run(4000);
  // K1 again at the open limit: nothing to attribute, no target; the next move off open is someone else's.
  size_t g1 = w.gate.logs.size();
  r = w.gate.request("relay.test", "\"k\":1,\"ms\":300");
  CHECK(r["ok"] == true);
  CHECK_STR(str(w.gate.status(), "target"), "");
  w.run(1000);
  w.extPress(false, 300);
  Found b2 = waitFor(w, w.gate, "gate_state", g1, 2000);
  CHECK(b2.ok);
  CHECK_EQ(b2.e.a, GS_BETWEEN_);
  CHECK_EQ(b2.e.b, CAUSE_EXTERNAL_);
  Found cl = waitFor(w, w.gate, "gate_state", g1, 10000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.b, CAUSE_EXTERNAL_);
  w.run(4000);
  // K2 from open: ours all the way to closed.
  r = w.gate.request("relay.test", "\"k\":1,\"ms\":500");
  CHECK(r["ok"] == true);
  CHECK(waitFor(w, w.gate, "gate_state", g1, 10000, GS_OPEN_).ok);
  w.run(4000);
  size_t g2 = w.gate.logs.size();
  r = w.gate.request("relay.test", "\"k\":2,\"ms\":500");
  CHECK(r["ok"] == true);
  CHECK_STR(str(w.gate.status(), "target"), "closed");
  Found b3 = waitFor(w, w.gate, "gate_state", g2, 2000);
  CHECK(b3.ok);
  CHECK_EQ(b3.e.a, GS_BETWEEN_);
  CHECK_EQ(b3.e.b, CAUSE_LORA_);
  Found c3 = waitFor(w, w.gate, "gate_state", g2, 10000, GS_CLOSED_);
  CHECK(c3.ok);
  CHECK_EQ(c3.e.b, CAUSE_LORA_);
  CHECK_STR(str(w.gate.status(), "last_result"), "reached");
}

// [cause] [ac-power]
TEST(gate_state_relay_test_without_power_sets_no_target) {
  // No AC and no limit reading (the opener may be dead): a relay test is only a wiring check. The limit read when
  // power returns isn't ours: no cause, neither reached nor a timeout, and the house doesn't resync.
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.opener.ac = false;
  w.opener.battery = false;
  Found np = waitFor(w, w.gate, "gate_state", g0, 1000);
  CHECK(np.ok);
  CHECK_EQ(np.e.a, GS_NO_POWER_);
  w.run(4000);
  JsonDocument r = w.gate.request("relay.test", "\"k\":1,\"ms\":500");
  CHECK(r["ok"] == true);
  CHECK(waitFor(w, w.gate, "pulse", g0, 100, 1).ok);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "target"), "");
  CHECK_STR(str(s, "last_result"), "none");
  w.run(2000);
  size_t g1 = w.gate.logs.size();
  w.opener.ac = true;
  w.opener.battery = true;
  Found c = waitFor(w, w.gate, "gate_state", g1, 4000);  // after the 3 s hold out of no_power
  CHECK(c.ok);
  CHECK_EQ(c.e.a, GS_CLOSED_);
  CHECK_EQ(c.e.b, CAUSE_NONE_);
  s = w.gate.status();
  CHECK_STR(str(s, "last_result"), "none");
  CHECK_STR(str(s, "target"), "");
  waitBoth(w, GS_CLOSED_, 2000);
  w.run(65000);  // past travel_timeout_s
  CHECK_EQ(countSince(w.gate, "travel_timeout", g0), 0);
  CHECK_STR(str(w.house.status(), "last_result"), "none");
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK(!w.alarmSwitch());
  CHECK(w.sensorClosed());
}

// ---------------------------------------------------------------------------------------------------------------
// [boot-settle]

// [boot-settle] [cause]
TEST(gate_state_boot_first_status_waits_for_settled_inputs) {
  Site w;
  w.commission();
  w.run(2000);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  uint32_t resetAt = w.now;
  w.gate.reset(PM_RCAUSE_WDT);
  Found boot = waitFor(w, w.gate, "boot", g0, 2000);
  CHECK(boot.ok);
  CHECK(w.gate.running());
  CHECK(w.gate.status()["settling"] == true);
  // The house keeps showing closed all along: K1 off, the sensor closed.
  auto houseShowsClosed = [&] { return !w.house.coil(1) && w.sensorClosed(); };
  CHECK_EQ(watch(w, boot.e.t + 2950 - w.now, houseShowsClosed), 0);
  CHECK(w.gate.status()["settling"] == true);
  CHECK(firstSent(w, w.gate, MSG_STATUS_, resetAt) == nullptr);
  CHECK(w.runUntil([&] { return firstSent(w, w.gate, MSG_STATUS_, resetAt) != nullptr; }, 2000));
  const AirFrame *st = firstSent(w, w.gate, MSG_STATUS_, resetAt);
  // BOOT_SETTLE_MS from gateBegin, which comes ~0.53 s after the boot event: LoRa.begin() (~470 ms) and the session draw.
  CHECK_IN(st->start - boot.e.t, 3400, 3700);
  CHECK(w.gate.status()["settling"] == false);
  // Nothing attributed to a boot.
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "closed");
  CHECK_STR(str(s, "cause"), "none");
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  CHECK_EQ(watch(w, 1000, houseShowsClosed), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 0);
  CHECK_STR(str(w.house.status(), "cause"), "none");
}

// [boot-settle]
TEST(gate_state_boot_waits_for_limits_that_come_back_late) {
  // A power blip at the gate restarts the opener with the board: its closed limit comes back 2 s after the board.
  // The gate reports nothing until the limit has held 3 s, then closed with no cause; the house never flickers.
  Site w;
  w.commission();
  w.run(2000);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.setGateRail(false);
  auto houseShowsClosed = [&] { return !w.house.coil(1) && w.sensorClosed(); };
  CHECK(w.runUntil([&] { return !w.gate.running(); }, 1000));  // its capacitors hold it up ~0.6 s
  w.opener.force[1] = 0;  // the opener restarting: its limit relays drop
  CHECK_EQ(watch(w, 1000, houseShowsClosed), 0);
  w.setGateRail(true);
  Found boot;
  CHECK(w.runUntil([&] {
    boot = find(w.gate, "boot", g0);
    return boot.ok;
  }, 1000));
  CHECK_EQ(watch(w, boot.e.at + 2000 - w.now, houseShowsClosed), 0);
  uint32_t limitAt = w.now + 1;
  w.opener.force[1] = -1;
  CHECK_EQ(watch(w, 2950, houseShowsClosed), 0);
  CHECK(w.gate.status()["settling"] == true);  // not 3 s since boot that counts: 3 s since the inputs changed
  CHECK(firstSent(w, w.gate, MSG_STATUS_, boot.e.at) == nullptr);
  CHECK(w.runUntil([&] { return firstSent(w, w.gate, MSG_STATUS_, boot.e.at) != nullptr; }, 2000));
  const AirFrame *st = firstSent(w, w.gate, MSG_STATUS_, boot.e.at);
  CHECK_IN(st->start - limitAt, 3050, 3100);  // debounce, then BOOT_SETTLE_MS steady
  CHECK_EQ(watch(w, 2000, houseShowsClosed), 0);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "closed");
  CHECK_STR(str(s, "cause"), "none");
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 0);
}

// [boot-settle]
TEST(gate_state_boot_settle_is_capped_at_10s) {
  // Inputs that never hold still for 3 s: the first STATUS goes anyway, 10 s after boot.
  Site w;
  w.commission();
  w.run(2000);
  size_t g0 = w.gate.logs.size();
  w.opener.force[1] = 0;
  w.gate.reset(PM_RCAUSE_WDT);
  Found boot = waitFor(w, w.gate, "boot", g0, 2000);
  CHECK(boot.ok);
  // Closed limit: off, on at +2 s, off at +4 s, on at +6 s, off at +8 s, on (for good) at +9 s.
  const uint32_t flips[] = { 2000, 4000, 6000, 8000, 9000 };
  for (uint32_t f : flips) {
    w.run(boot.e.t + f - w.now);
    w.opener.force[1] = w.opener.force[1] == 0 ? -1 : 0;
  }
  CHECK_EQ(w.opener.force[1], -1);
  w.run(boot.e.t + 9950 - w.now);
  CHECK(w.gate.status()["settling"] == true);
  CHECK(firstSent(w, w.gate, MSG_STATUS_, boot.e.at) == nullptr);
  CHECK(w.runUntil([&] { return firstSent(w, w.gate, MSG_STATUS_, boot.e.at) != nullptr; }, 1000));
  const AirFrame *st = firstSent(w, w.gate, MSG_STATUS_, boot.e.at);
  CHECK_IN(st->start - boot.e.t, 10400, 10700);  // BOOT_SETTLE_MAX_MS from gateBegin (~0.53 s after boot, as above)
  JsonDocument s = w.gate.status();
  CHECK(s["settling"] == false);
  CHECK_STR(str(s, "gate"), "closed");
  CHECK_STR(str(s, "cause"), "none");
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
}

// [boot-settle] [cause]
TEST(gate_state_boot_mid_travel_reports_between_with_no_cause) {
  // The gate resets while our OPEN is under way. It comes back to a moving gate: after settling it reports between
  // with no cause (the boot isn't a movement, and our target died with the reset); open is someone else's.
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  Found b = waitFor(w, w.gate, "gate_state", g0, 3000);
  CHECK(b.ok);
  CHECK_EQ(b.e.a, GS_BETWEEN_);
  CHECK_EQ(b.e.b, CAUSE_LORA_);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_BETWEEN_; }, 2000));
  w.run(1500);
  size_t g1 = w.gate.logs.size(), h1 = w.house.logs.size();
  w.gate.reset(PM_RCAUSE_WDT);
  Found boot = waitFor(w, w.gate, "boot", g1, 2000);
  CHECK(boot.ok);
  CHECK(w.runUntil([&] { return firstSent(w, w.gate, MSG_STATUS_, boot.e.at) != nullptr; }, 4000));
  CHECK(!w.opener.atOpen());  // still travelling
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "between");
  CHECK_STR(str(s, "cause"), "none");
  CHECK_STR(str(s, "target"), "");
  CHECK_EQ(countSince(w.gate, "gate_state", g1), 0);
  CHECK(poll(w, [&] { return str(w.house.status(), "cause") == "none"; }, 1000, 10));
  CHECK_EQ(countSince(w.house, "gate_state", h1), 0);  // between all along
  Found op = waitFor(w, w.gate, "gate_state", g1, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_EXTERNAL_);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 2000));
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);  // the house didn't repeat it
}

// [boot-settle] [cause]
TEST(gate_state_boot_reports_a_move_made_while_down_with_no_cause) {
  // Opened by our command (lora); the gate board loses power and someone closes the gate meanwhile. Back up, it
  // reports closed after settling, with no cause: neither ours nor a movement it saw.
  Site w;
  w.commission();
  openByUser(w);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.setGateRail(false);
  w.run(1000);
  CHECK(!w.gate.running());
  w.extPress(false, 300);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 10000));
  w.setGateRail(true);
  Found boot;
  CHECK(w.runUntil([&] {
    boot = find(w.gate, "boot", g0);
    return boot.ok;
  }, 1000));
  Found hs = waitFor(w, w.house, "gate_state", h0, 6000);
  CHECK(hs.ok);
  CHECK_EQ(hs.e.a, GS_CLOSED_);
  CHECK_EQ(hs.e.b, CAUSE_NONE_);
  CHECK((int32_t)(hs.e.at - boot.e.t) >= 3000);
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "closed");
  CHECK_STR(str(s, "cause"), "none");
  CHECK_STR(str(s, "target"), "");
  CHECK_STR(str(w.house.status(), "cause"), "none");
  CHECK(w.runUntil([&] { return w.sensorClosed() && !w.alarmSwitch(); }, 3000));
}

// ---------------------------------------------------------------------------------------------------------------
// [between-hold]

// [between-hold] [ac-power]
TEST(gate_state_failing_input_supply_reads_no_power_not_a_move) {
  // The 24 V that wets the inputs fails: the closed limit's opto drops before IN3. Within the hold, that is no_power
  // with no cause; no between or external is ever logged, and the house never hears of a move.
  Site w;
  w.commission();
  const uint32_t gaps[] = { 0, 300, 440 };
  for (uint32_t d : gaps) {
    size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
    uint32_t t = w.now + 1;
    w.opener.force[1] = 0;
    w.run(d);
    w.opener.ac = false;
    Found f = waitFor(w, w.gate, "gate_state", g0, 2000);
    CHECK(f.ok);
    CHECK_EQ(f.e.a, GS_NO_POWER_);
    CHECK_EQ(f.e.b, CAUSE_NONE_);
    CHECK_IN(f.e.at - t, d + 50, d + 51);  // IN3's debounce
    w.run(1000);
    CHECK_EQ(countSince(w.gate, "gate_state", g0), 1);
    CHECK_EQ(countSince(w.gate, "gate_state", g0, ANY, CAUSE_EXTERNAL_), 0);
    JsonDocument s = w.gate.status();
    CHECK_STR(str(s, "gate"), "no_power");
    CHECK(s["ac_power"] == false);
    CHECK(w.runUntil([&] { return w.houseSees() == GS_NO_POWER_; }, 2000));
    w.run(100);
    CHECK(w.house.coil(1));  // not closed
    CHECK(!w.sensorClosed());
    CHECK_EQ(countSince(w.house, "gate_state", h0), 1);
    // Supply back: closed, no cause.
    size_t g1 = w.gate.logs.size();
    w.opener.ac = true;
    w.opener.force[1] = -1;
    Found c = waitFor(w, w.gate, "gate_state", g1, 4000);
    CHECK(c.ok);
    CHECK_EQ(c.e.a, GS_CLOSED_);
    CHECK_EQ(c.e.b, CAUSE_NONE_);
    waitBoth(w, GS_CLOSED_, 2000);
    w.run(5000);
  }
}

// [between-hold]
TEST(gate_state_between_hold_is_500ms) {
  Site w;
  w.commission();
  // A limit glitch shorter than the hold is never reported.
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.opener.force[1] = 0;
  w.run(400);
  w.opener.force[1] = -1;
  w.run(1500);
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  CHECK_STR(str(w.gate.status(), "gate"), "closed");
  // A limit dropping with AC staying is a movement: between (external), once the 500 ms hold is over.
  uint32_t t = w.now + 1;
  w.opener.force[1] = 0;
  w.run(545);
  CHECK_STR(str(w.gate.status(), "gate"), "closed");  // debounce 50, then 495 of the hold
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  Found b = waitFor(w, w.gate, "gate_state", g0, 1000);
  CHECK(b.ok);
  CHECK_EQ(b.e.a, GS_BETWEEN_);
  CHECK_EQ(b.e.b, CAUSE_EXTERNAL_);
  CHECK_EQ(b.e.at - t, ((t + 50) | 1) + 500 - t);  // debounce_ms 50 + BETWEEN_HOLD_MS 500, from an odd stamp
  CHECK(w.runUntil([&] { return w.houseSees() == GS_BETWEEN_; }, 2000));
  CHECK_EQ(countSince(w.house, "gate_state", h0), 1);
  size_t g1 = w.gate.logs.size();
  w.opener.force[1] = -1;
  CHECK(waitFor(w, w.gate, "gate_state", g1, 4000, GS_CLOSED_).ok);
  w.run(5000);
  // Just outside the hold: the limit drops, AC 600 ms later. The move is reported first, then no_power.
  g1 = w.gate.logs.size();
  t = w.now + 1;
  w.opener.force[1] = 0;
  w.run(600);
  w.opener.ac = false;
  Found np = waitFor(w, w.gate, "gate_state", g1, 2000, GS_NO_POWER_);
  CHECK(np.ok);
  CHECK_EQ(np.e.b, CAUSE_NONE_);
  Found b2 = find(w.gate, "gate_state", g1, GS_BETWEEN_);
  CHECK(b2.ok);
  CHECK_EQ(b2.e.b, CAUSE_EXTERNAL_);
  CHECK_EQ(b2.e.at - t, ((t + 50) | 1) + 500 - t);
  CHECK(b2.e.at < np.e.at);
  w.opener.ac = true;
  w.opener.force[1] = -1;
  CHECK(waitFor(w, w.gate, "gate_state", g1, 4000, GS_CLOSED_).ok);
}

// [between-hold] [ac-power]
TEST(gate_state_power_return_holds_between_for_boot_settle) {
  // Out of no_power the hold is BOOT_SETTLE_MS: IN3 can come back before the limits (the opener restarting).
  Site w;
  w.commission();
  for (int late = 0; late < 2; late++) {
    size_t g0 = w.gate.logs.size();
    w.opener.ac = false;
    w.opener.battery = false;  // the opener is dead: no limit reads
    Found np = waitFor(w, w.gate, "gate_state", g0, 2000);
    CHECK(np.ok);
    CHECK_EQ(np.e.a, GS_NO_POWER_);
    CHECK_EQ(np.e.b, CAUSE_NONE_);
    w.run(2000);
    size_t g1 = w.gate.logs.size();
    w.opener.force[1] = 0;  // the limit relays come back after IN3
    w.opener.ac = true;
    w.opener.battery = true;
    uint32_t r = w.now + 1;
    uint32_t limitMs = late ? 4000 : 2900;
    w.run(limitMs);
    w.opener.force[1] = -1;
    Found c = waitFor(w, w.gate, "gate_state", g1, 4000, GS_CLOSED_);
    CHECK(c.ok);
    if (!late) {
      // Within the hold: closed 3 s after the limit reads (it may be a restarting opener's AUX2 relay not energized
      // yet), no cause, never between.
      CHECK_EQ(countSince(w.gate, "gate_state", g1), 1);
      CHECK_EQ(c.e.b, CAUSE_NONE_);
      CHECK_IN(c.e.at - r, limitMs + 3050, limitMs + 3052);
    } else {
      // Past it: between (power return, no cause) after IN3's debounce + 3 s, then closed.
      Found b = find(w.gate, "gate_state", g1, GS_BETWEEN_);
      CHECK(b.ok);
      CHECK_EQ(b.e.b, CAUSE_NONE_);
      CHECK_EQ(b.e.at - r, ((r + 50) | 1) + 3000 - r);
      CHECK_EQ(countSince(w.gate, "gate_state", g1), 2);
    }
    waitBoth(w, GS_CLOSED_, 2000);
    w.run(5000);
  }
}

// [between-hold] [ac-power]
TEST(gate_state_power_sense_off_ignores_in3_and_does_not_hold) {
  Site w;
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("power_sense", 0));
  });
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.opener.ac = false;
  Found in = waitFor(w, w.gate, "input", g0, 200, 3);
  CHECK(in.ok);
  CHECK_EQ(in.e.b, 0);  // IN3 itself is still read and logged
  CHECK(poll(w, [&] { return w.house.status()["remote"]["in3"] == false; }, 3000));
  JsonDocument s = w.gate.status();
  CHECK(s["power_sense"] == false);
  CHECK(s["ac_power"] == true);
  CHECK_STR(str(s, "gate"), "closed");
  CHECK(w.house.status()["remote"]["ac_power"] == true);  // STATUS bit 6 clear
  // A command is pulsed, and the move reported between at once (no hold), not no_power.
  w.user(true);
  Found p = waitFor(w, w.gate, "pulse", g0, 3000, 1);
  CHECK(p.ok);
  CHECK_EQ(countSince(w.gate, "cmd_refused", g0), 0);
  Found b = waitFor(w, w.gate, "gate_state", g0, 2000);
  CHECK(b.ok);
  CHECK_EQ(b.e.a, GS_BETWEEN_);
  CHECK_EQ(b.e.b, CAUSE_LORA_);
  CHECK_IN(b.e.at - p.e.at, 70, 72);  // the opener's 20 ms + debounce 50
  Found op = waitFor(w, w.gate, "gate_state", g0, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  w.run(4000);
  // The opener dies too (no AC, no battery): the limit drops, and that's a move, reported at once.
  size_t g1 = w.gate.logs.size();
  uint32_t t = w.now + 1;
  w.opener.battery = false;
  Found b2 = waitFor(w, w.gate, "gate_state", g1, 1000);
  CHECK(b2.ok);
  CHECK_EQ(b2.e.a, GS_BETWEEN_);
  CHECK_EQ(b2.e.b, CAUSE_EXTERNAL_);
  CHECK_IN(b2.e.at - t, 50, 51);
  w.opener.battery = true;
  CHECK(waitFor(w, w.gate, "gate_state", g1, 1000, GS_OPEN_).ok);
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_NO_POWER_), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
}

// ---------------------------------------------------------------------------------------------------------------
// [ac-power]

// [ac-power]
TEST(gate_state_ac_lost_limit_still_trusted) {
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.opener.ac = false;  // on battery
  Found in = waitFor(w, w.gate, "input", g0, 200, 3);
  CHECK(in.ok);
  CHECK_EQ(in.e.b, 0);
  w.run(100);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "closed");
  CHECK(s["ac_power"] == false);
  // The house hears of it at once (not at the next heartbeat): STATUS bit 6.
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == false; }, 1000, 10));
  JsonDocument h = w.house.status();
  CHECK_STR(str(h, "gate"), "closed");
  CHECK(h["remote"]["in3"] == false);
  CHECK(h["remote"]["close_limit"] == true);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 0);
  // On battery someone opens it: no limit reading without AC is no_power (no cause), then open (no cause).
  w.extPress(true, 300);
  Found np = waitFor(w, w.gate, "gate_state", g0, 2000);
  CHECK(np.ok);
  CHECK_EQ(np.e.a, GS_NO_POWER_);
  CHECK_EQ(np.e.b, CAUSE_NONE_);
  Found op = waitFor(w, w.gate, "gate_state", g0, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_NONE_);
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_BETWEEN_), 0);
  waitBoth(w, GS_OPEN_, 2000);
  w.run(4000);
  // AC back: the state doesn't change, the house hears of it.
  size_t g1 = w.gate.logs.size();
  w.opener.ac = true;
  Found in2 = waitFor(w, w.gate, "input", g1, 200, 3);
  CHECK(in2.ok);
  CHECK_EQ(in2.e.b, 1);
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == true; }, 1000, 10));
  CHECK(w.house.status()["remote"]["in3"] == true);
  CHECK_EQ(countSince(w.gate, "gate_state", g1), 0);
  CHECK(w.gate.status()["ac_power"] == true);
}

// [ac-power]
TEST(gate_state_without_ac_commands_run_while_a_limit_reads) {
  Site w;
  w.commission();
  w.opener.ac = false;
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == false; }, 3000));
  w.run(1000);
  // OPEN from closed runs on the opener's battery. On the way no limit reads, so the gate reports no_power (the house
  // shows not-closed); open once it gets there, no cause either way (into and out of no_power).
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  Found cs = waitFor(w, w.house, "cmd_sent", h0, 1000, ACT_OPEN_);
  CHECK(cs.ok);
  CHECK(waitFor(w, w.gate, "pulse", g0, 2000, 1).ok);
  CHECK(poll(w, [&] { return w.house.status()["cmd_result"].as<int>() == 0; }, 2000, 10));
  Found np0 = waitFor(w, w.gate, "gate_state", g0, 3000, GS_NO_POWER_);
  CHECK(np0.ok);
  CHECK_EQ(np0.e.b, CAUSE_NONE_);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  Found op = waitFor(w, w.gate, "gate_state", g0, 2000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_NONE_);
  waitBoth(w, GS_OPEN_, 2000);
  CHECK_EQ(countSince(w.gate, "cmd_refused", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  CHECK_STR(str(w.gate.status(), "last_result"), "reached");
  w.run(5000);
  // CLOSE from open runs too: cutting the AC mustn't keep the gate open.
  size_t g1 = w.gate.logs.size(), h1 = w.house.logs.size();
  w.user(false);
  Found cs2 = waitFor(w, w.house, "cmd_sent", h1, 2000, ACT_CLOSE_);
  CHECK(cs2.ok);
  Found p = waitFor(w, w.gate, "pulse", g1, 2000, 2);
  CHECK(p.ok);
  CHECK(poll(w, [&] { return w.house.status()["cmd_result"].as<int>() == 0; }, 2000, 10));
  Found np = waitFor(w, w.gate, "gate_state", g1, 3000, GS_NO_POWER_);
  CHECK(np.ok);
  CHECK_EQ(np.e.b, CAUSE_NONE_);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  Found cl = waitFor(w, w.gate, "gate_state", g1, 4000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.b, CAUSE_NONE_);
  waitBoth(w, GS_CLOSED_, 2000);
  CHECK(w.runUntil([&] { return !w.alarmSwitch() && w.sensorClosed(); }, MISMATCH_MS + 5000));
  w.run(5000);
  CHECK(!w.alarmSwitch());
  CHECK_EQ(countSince(w.gate, "cmd_refused", g1), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g1), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h1), 1);
  CHECK_STR(str(w.gate.status(), "last_result"), "reached");
  // Dead opener (no limit reads): no_power, and CLOSE is refused too.
  w.opener.battery = false;
  CHECK(poll(w, [&] { return w.house.status()["gate"] == "no_power"; }, 5000));
  w.run(MISMATCH_MS + 5000);
  CHECK(w.alarmSwitch());  // following the house's not-closed
  size_t g2 = w.gate.logs.size(), h2 = w.house.logs.size();
  w.user(false);
  Found cs3 = waitFor(w, w.house, "cmd_sent", h2, 2000, ACT_CLOSE_);
  CHECK(cs3.ok);
  Found rf3 = waitFor(w, w.gate, "cmd_refused", g2, 2000, ACT_CLOSE_, cs3.e.b);
  CHECK(rf3.ok);
  CHECK(poll(w, [&] { return w.house.status()["cmd_result"].as<int>() == RES_NO_POWER_; }, 2000, 10));
  CHECK_EQ(countSince(w.gate, "pulse", g2), 0);
  // Both limits reading (fault: the position can't be verified): CLOSE refused as well.
  w.opener.force[0] = w.opener.force[1] = 1;
  CHECK(poll(w, [&] { return w.gate.status()["gate"] == "fault"; }, 3000));
  w.run(MISMATCH_MS + 5000);
  size_t g4 = w.gate.logs.size(), h4 = w.house.logs.size();
  w.user(true);
  w.run(3000);
  w.user(false);
  Found cs4 = waitFor(w, w.house, "cmd_sent", h4, 5000, ACT_CLOSE_);
  CHECK(cs4.ok);
  CHECK(waitFor(w, w.gate, "cmd_refused", g4, 2000, ACT_CLOSE_, cs4.e.b).ok);
  CHECK_EQ(countSince(w.gate, "pulse", g4), 0);
  w.opener.force[0] = w.opener.force[1] = -1;
  // AC back: commands go through again. The closed limit alone isn't trusted yet after both read (still fault), so
  // the CLOSE pulses, and the OPEN that follows clears it.
  w.opener.ac = w.opener.battery = true;
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == true; }, 3000));
  w.run(5000);
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  CHECK(w.alarmSwitch());
  size_t g5 = w.gate.logs.size();
  w.user(false);
  CHECK(waitFor(w, w.gate, "pulse", g5, 3000, 2).ok);
  CHECK_EQ(countSince(w.gate, "cmd_refused", g5), 0);
  w.run(5000);
  size_t g3 = w.gate.logs.size();
  w.user(true);
  Found p3 = waitFor(w, w.gate, "pulse", g3, 3000, 1);
  CHECK(p3.ok);
  CHECK_EQ(countSince(w.gate, "cmd_refused", g3), 0);
  CHECK(waitFor(w, w.gate, "gate_state", g3, 15000, GS_OPEN_).ok);
}

// [ac-power]
TEST(gate_state_without_ac_reversal_mid_travel_runs) {
  // On battery no limit reads mid-travel (no_power), but while our OPEN is still under way the opener has just run:
  // the user's CLOSE reverses it, rather than being refused and the gate left open.
  Site w;
  w.commission();
  w.opener.ac = false;
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == false; }, 3000));
  w.run(1000);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  CHECK(waitFor(w, w.gate, "pulse", g0, 2000, 1).ok);
  CHECK(waitFor(w, w.gate, "gate_state", g0, 3000, GS_NO_POWER_).ok);
  w.run(4000);  // past the sync window K1's change to not-closed opened (travel takes 8 s)
  CHECK(!w.opener.atOpen() && !w.opener.atClosed());
  size_t g1 = w.gate.logs.size();
  w.user(false);
  Found cs = waitFor(w, w.house, "cmd_sent", h0, 2000, ACT_CLOSE_);
  CHECK(cs.ok);
  CHECK(waitFor(w, w.gate, "pulse", g1, 2000, 2).ok);
  CHECK(poll(w, [&] { return w.house.status()["cmd_result"].as<int>() == 0; }, 2000, 10));
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(waitFor(w, w.gate, "gate_state", g1, 4000, GS_CLOSED_).ok);
  waitBoth(w, GS_CLOSED_, 2000);
  CHECK(w.runUntil([&] { return !w.alarmSwitch() && w.sensorClosed(); }, 5000));
  CHECK_EQ(countSince(w.gate, "cmd_refused", g0), 0);
  CHECK(!w.opener.atOpen());
  CHECK_STR(str(w.gate.status(), "last_result"), "reached");
}

// [ac-power] [cause]
TEST(gate_state_no_power_transitions_have_no_cause) {
  // Our OPEN is under way when AC goes: no_power (no cause) while no limit reads; the opener finishes on its battery
  // and open is reported with no cause either, though it was our target.
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found b = waitFor(w, w.gate, "gate_state", g0, 3000);
  CHECK(b.ok);
  CHECK_EQ(b.e.a, GS_BETWEEN_);
  CHECK_EQ(b.e.b, CAUSE_LORA_);
  w.run(2000);
  w.opener.ac = false;
  Found np = waitFor(w, w.gate, "gate_state", g0, 1000, GS_NO_POWER_);
  CHECK(np.ok);
  CHECK_EQ(np.e.b, CAUSE_NONE_);
  CHECK_STR(str(w.gate.status(), "target"), "open");  // still heading there
  CHECK(w.runUntil([&] { return w.houseSees() == GS_NO_POWER_; }, 2000));
  w.run(100);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  Found op = waitFor(w, w.gate, "gate_state", g0, 10000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_NONE_);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "last_result"), "reached");
  CHECK_STR(str(s, "target"), "");
  CHECK(s["ac_power"] == false);
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 3);
}

// [ac-power] [between-hold] [cause]
TEST(gate_state_power_return_mid_travel_is_not_a_move) {
  // The opener dies mid-travel (AC and battery) and stops; when power returns, between is reported after the
  // BOOT_SETTLE_MS hold with no cause, though our target is still open; then the travel timeout ends it.
  Site w;
  w.commission();
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  w.user(true);
  Found p = waitFor(w, w.gate, "pulse", g0, 3000, 1);
  CHECK(p.ok);
  CHECK(waitFor(w, w.gate, "gate_state", g0, 3000, GS_BETWEEN_).ok);
  w.run(2000);
  w.opener.ac = false;
  w.opener.battery = false;
  Found np = waitFor(w, w.gate, "gate_state", g0, 1000, GS_NO_POWER_);
  CHECK(np.ok);
  CHECK_EQ(np.e.b, CAUSE_NONE_);
  w.run(3000);
  size_t g1 = w.gate.logs.size();
  uint32_t r = w.now + 1;
  w.opener.ac = true;
  w.opener.battery = true;
  Found b = waitFor(w, w.gate, "gate_state", g1, 5000);
  CHECK(b.ok);
  CHECK_EQ(b.e.a, GS_BETWEEN_);
  CHECK_EQ(b.e.b, CAUSE_NONE_);
  CHECK_EQ(b.e.at - r, ((r + 50) | 1) + 3000 - r);
  CHECK_STR(str(w.gate.status(), "target"), "open");
  Found tt = waitFor(w, w.gate, "travel_timeout", g0, 60000);
  CHECK(tt.ok);
  CHECK_EQ(tt.e.a, GS_OPEN_);
  CHECK_IN(tt.e.at - p.e.at, 60000, 60002);
  JsonDocument s = w.gate.status();
  CHECK_STR(str(s, "gate"), "between");
  CHECK_STR(str(s, "last_result"), "timeout");
  w.run(3000);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);  // the controller shows open, as K1 now does
}

// ---------------------------------------------------------------------------------------------------------------
// [spare-before-state]

// [spare-before-state] [ac-power]
TEST(gate_state_power_and_limit_lost_together_read_no_power) {
  Site w;
  w.commission();
  for (int fromOpen = 0; fromOpen < 2; fromOpen++) {
    if (fromOpen) openByUser(w);
    size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
    uint32_t t = w.now + 1;
    w.opener.ac = false;
    w.opener.battery = false;  // the opener dies: its limit relay and IN3 drop in the same instant
    Found f = waitFor(w, w.gate, "gate_state", g0, 2000);
    CHECK(f.ok);
    CHECK_EQ(f.e.a, GS_NO_POWER_);
    CHECK_EQ(f.e.b, CAUSE_NONE_);
    CHECK_IN(f.e.at - t, 50, 51);
    w.run(1000);
    CHECK_EQ(countSince(w.gate, "gate_state", g0), 1);
    CHECK_EQ(countSince(w.house, "gate_state", h0), 1);
    w.opener.ac = true;
    w.opener.battery = true;
    CHECK(w.runUntil([&] { return w.gateSees() == (fromOpen ? GS_OPEN_ : GS_CLOSED_); }, 4000));
    CHECK_EQ(countSince(w.gate, "gate_state", g0, ANY, CAUSE_EXTERNAL_), 0);
    w.run(5000);
  }
}

// [spare-before-state] [between-hold]
TEST(gate_state_ac_edge_on_the_hold_expiry_reads_no_power) {
  // The closed limit drops, and IN3's debounced edge lands on the very pass the 500 ms between hold expires. IN3 is
  // read before the state, so that pass sees no_power; read after, it would report between/external first. One ms
  // later and the hold is over first: between, then no_power (the boundary). heldAt is stamped `now | 1`, so the
  // expiry pass depends on the parity: both are tried.
  Site w;
  w.commission();
  for (int parity = 0; parity < 2; parity++) {
    for (int late = 0; late < 2; late++) {
      if (((w.now + 1) & 1) != (uint32_t)parity) w.run(1);
      size_t g0 = w.gate.logs.size();
      uint32_t t = w.now + 1;                     // the gate sees the limit drop at t, debounced at t + 50
      uint32_t expiry = ((t + 50) | 1) + 500;     // the hold's last pass
      uint32_t d = expiry - 50 - t + late;        // AC drop seen at t + d, debounced at t + d + 50
      w.opener.force[1] = 0;
      w.run(d);
      w.opener.ac = false;
      Found f = waitFor(w, w.gate, "gate_state", g0, 2000);
      CHECK(f.ok);
      w.run(500);
      Found np = find(w.gate, "gate_state", g0, GS_NO_POWER_);
      CHECK(np.ok);
      CHECK_EQ(np.e.b, CAUSE_NONE_);
      CHECK_EQ(np.e.t, t + d + 50);  // the premise: IN3 debounced on that pass
      if (!late) {
        CHECK_EQ(f.e.a, GS_NO_POWER_);
        CHECK_EQ(countSince(w.gate, "gate_state", g0), 1);
      } else {
        CHECK_EQ(f.e.a, GS_BETWEEN_);
        CHECK_EQ(f.e.b, CAUSE_EXTERNAL_);
        CHECK_EQ(f.e.t, expiry);
      }
      w.opener.ac = true;
      w.opener.force[1] = -1;
      CHECK(w.runUntil([&] { return w.gateSees() == GS_CLOSED_; }, 4000));
      w.run(5000);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// [state-from-limits] A second closed sensor on the leaf (closed_sense, IN4; F12, threat
// closed-limit-nc-fail-unsafe): closed reads only while it and the closed limit agree, and a lasting disagreement reads
// fault. The monitor (world.cpp) fails any closed reported with the sensor off.

namespace {

// Commissions the site with a reed that reads within reedMs of travel from closed, and closed_sense as given.
void commissionReed(World &w, int32_t reedMs, int sense = 1) {
  w.opener.reedMs = reedMs;
  w.commission([&](Board &b) {
    if (b.idx == 1) CHECK(b.set("closed_sense", sense));
  });
}

}  // namespace

// The threat itself: the closed limit's relay failed (its NC contact reads closed wherever the gate is) and the gate is
// pushed part-open by hand. Without the sensor nothing tells it from closed; with it, between at once and fault once
// the two have disagreed for 5 s, and the house shows not closed throughout. [state-from-limits]
TEST(gate_state_closed_sense_stuck_closed_limit_part_open_never_reads_closed) {
  for (int sense = 0; sense < 2; sense++) {
    Site w;
    commissionReed(w, 200, sense);
    w.opener.force[1] = 1;
    w.run(2000);
    size_t g0 = w.gate.logs.size();
    uint32_t t0 = w.now;
    w.opener.pos = w.opener.travelMs / 2;
    if (!sense) {
      w.limits.sensorTruth = false;  // the gap F12 closes: the contact sensor stays closed with the gate part-open
      w.run(10000);
      CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
      CHECK_STR(str(w.gate.status(), "gate"), "closed");
      CHECK(w.sensorClosed());
      continue;
    }
    Found b = waitFor(w, w.gate, "gate_state", g0, 2000, GS_BETWEEN_);
    CHECK(b.ok);
    CHECK(w.runUntil([&] { return !w.sensorClosed(); }, 2000));
    Found f = waitFor(w, w.gate, "gate_state", g0, 6000, GS_FAULT_);
    CHECK(f.ok);
    CHECK(f.e.t - t0 >= 5050 && f.e.t - t0 <= 5060);  // debounced 50 ms after the move, then 5 s apart
    CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_; }, 2000));
    CHECK_EQ(watch(w, 10000, [&] { return !w.sensorClosed(); }), 0);
    CHECK_STR(str(w.gate.status(), "gate"), "fault");
  }
}

// The sensor failing (a magnet knocked off, a cut wire) with the gate closed: not closed, fault after 5 s, and closed
// again once it reads. [state-from-limits]
TEST(gate_state_closed_sense_failed_sensor_reads_fault_until_it_reads) {
  Site w;
  commissionReed(w, 200);
  size_t g0 = w.gate.logs.size();
  w.opener.force[3] = 0;
  Found f = waitFor(w, w.gate, "gate_state", g0, 7000, GS_FAULT_);
  CHECK(f.ok);
  CHECK(find(w.gate, "gate_state", g0, GS_BETWEEN_).ok);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_ && !w.sensorClosed(); }, 2000));
  size_t g1 = w.gate.logs.size();
  w.opener.force[3] = -1;
  Found c = waitFor(w, w.gate, "gate_state", g1, 2000, GS_CLOSED_);
  CHECK(c.ok);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_CLOSED_ && w.sensorClosed(); }, 5000));
}

// A sensor stuck reading closed is caught as soon as the gate leaves closed: the sensor without the closed limit is a
// disagreement too, fault 5 s on, mid-travel or at the open limit. Closing then reads closed again (both agree).
// [state-from-limits]
TEST(gate_state_closed_sense_stuck_sensor_reads_fault_once_the_gate_leaves) {
  Site w;
  commissionReed(w, 200);
  w.opener.force[3] = 1;
  size_t g0 = w.gate.logs.size();
  w.user(true);
  Found b = waitFor(w, w.gate, "gate_state", g0, 15000, GS_BETWEEN_);
  CHECK(b.ok);
  Found f = waitFor(w, w.gate, "gate_state", g0, 7000, GS_FAULT_);
  CHECK(f.ok);
  CHECK(f.e.t - b.e.t >= 4400 && f.e.t - b.e.t <= 4600);  // between was reported 500 ms into the disagreement
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 10000));
  w.run(1000);
  CHECK_STR(str(w.gate.status(), "gate"), "fault");
  CHECK(w.runUntil([&] { return w.houseSees() == GS_FAULT_; }, 2000));
  w.run(SYNC_MS);
  w.user(false);
  waitBoth(w, GS_CLOSED_, 20000);
  CHECK(w.sensorClosed());
}

// In normal travel the sensor and the limit part only between their switching points: a sensor that reads 3 s of
// travel before the limit (and goes off 3 s after it, opening) never reads fault, and the gate reads closed only at
// the limit. [state-from-limits]
TEST(gate_state_closed_sense_normal_travel_is_never_a_fault) {
  Site w;
  commissionReed(w, 3000);
  size_t g0 = w.gate.logs.size();
  for (int i = 0; i < 2; i++) {
    openByUser(w);
    w.user(false);
    waitBoth(w, GS_CLOSED_, 20000);
    CHECK(w.sensorClosed());
    w.run(4000);
  }
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_FAULT_), 0);
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_CLOSED_), 2);
}

// closed_sense off: IN4 is a spare again, whatever it reads. [state-from-limits]
TEST(gate_state_closed_sense_off_ignores_in4) {
  Site w;
  commissionReed(w, 200, 0);
  size_t g0 = w.gate.logs.size();
  w.opener.force[3] = 0;
  w.run(8000);
  CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
  CHECK_STR(str(w.gate.status(), "gate"), "closed");
}
