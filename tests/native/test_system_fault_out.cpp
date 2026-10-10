// The fault output on D5 (`fault_out`, health.cpp; invariant tag [fault-out]): off by default (D5 left an input with
// its pull-down); with it on, D5 HIGH while the board is healthy and LOW otherwise; a problem must last fault_hold_s
// before D5 drops and recovery raises it at once; after a reset D5 reads as a fault until the role has decided (house:
// armed; gate: inputs settled, the first STATUS sent). Through the whole simulated site (world.h): both boards'
// firmware, the opener, the controller and the radio between them; World::monitorFaultOut watches every millisecond.
//
// Timings are exact where the firmware makes them so: each board's loop runs every simulated millisecond (its clock is
// the world's except while a call blocks), and the problems are evaluated after the roles, in the pass that saw them.
// A fault drops D5 fault_hold_s after the pass that first saw it: the house's `link_down` or `gate_state` log, the
// arrival of the gate's STATUS (the end of its frame on the air), the gate's debounced `input` / `gate_state` log.
#include <Arduino.h>
#include <limits.h>
#include <stdio.h>
#include <exception>
#include <functional>
#include "world.h"

namespace {

enum { MSG_STATUS_ = 5 };                // link.h MsgType
enum { F_PAYLOAD = 13 };                 // link.cpp HDR_LEN: the payload's offset in a frame
enum { ST_STATE_ = 0, ST_INPUTS_ = 1 };  // roles.h STATUS layout
enum { STI_AC_LOST_ = 0x40 };            // roles.h ST_INPUTS bit
// health.h Problem bits (log `health` b)
enum { P_STARTING = 0x01, P_RADIO = 0x02, P_LINK = 0x04, P_AC = 0x08, P_NO_POWER = 0x10, P_FAULT = 0x20 };
const int ANY = INT_MIN;
const uint32_t HOLD_MS = 10000;  // fault_hold_s default
const uint32_t SYNC_MS = 3000;   // sync_window_ms default: the house arms this long after the first STATUS

// The site, failing the test (not aborting the run) on a monitor breach (as in test_system_gate_state.cpp).
struct Site : World {
  using World::World;
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

int countSince(const Board &b, const char *ev, size_t from, int a = ANY) {
  int n = 0;
  for (size_t i = from; i < b.logs.size(); i++) n += b.logs[i].ev == ev && (a == ANY || b.logs[i].a == a);
  return n;
}

// Runs until `ev` shows up on b (from log index `from`); the world stops on the step that logged it.
Found waitFor(World &w, const Board &b, const char *ev, size_t from, uint32_t maxMs, int a = ANY, int bv = ANY) {
  Found f;
  w.runUntil([&] {
    f = find(b, ev, from, a, bv);
    return f.ok;
  }, maxMs);
  return f;
}

// Steps `ms`, checking `ok` after every step; returns the world time it first failed (0 = never).
uint32_t watch(World &w, uint32_t ms, const std::function<bool()> &ok) {
  uint32_t bad = 0;
  for (uint32_t i = 0; i < ms; i++) {
    w.step();
    if (!bad && !ok()) bad = w.now;
  }
  return bad;
}

// Runs the world up to world time t (which must not have passed).
void runTo(World &w, uint32_t t) {
  if ((int32_t)(t - w.now) < 0) throw Failure("runTo: " + std::to_string(t) + " has passed (now " + std::to_string(w.now) + ")");
  w.run(t - w.now);
}

int32_t since(uint32_t t, uint32_t t0) {
  return (int32_t)(t - t0);
}

// Status `health`, as "a,b" ("" when healthy).
std::string health(Board &b) {
  JsonDocument s = b.status();
  if (!s["health"].is<JsonArrayConst>()) return "(not a list)";
  std::string out;
  for (JsonVariantConst v : s["health"].as<JsonArrayConst>()) out += (out.empty() ? "" : ",") + v.as<std::string>();
  return out;
}

#define CHECK_STR(expr, want) \
  do { \
    std::string got_ = (expr), want_ = (want); \
    if (got_ != want_) \
      throw Failure(where(__FILE__, __LINE__, "CHECK_STR(" #expr "): \"" + got_ + "\" != \"" + want_ + "\"")); \
  } while (0)

// The end (= its arrival at the house) of the first STATUS the gate put on the air at or after world time `at` whose
// AC-lost bit reads `acLost`; 0 if none.
uint32_t statusArrival(const World &w, uint32_t at, bool acLost) {
  for (const AirFrame &f : w.air) {
    if (f.from != w.gate.idx || f.type() != MSG_STATUS_ || (int32_t)(f.start - at) < 0 || f.dropped) continue;
    if (f.b.size() > F_PAYLOAD + ST_INPUTS_ && ((f.b[F_PAYLOAD + ST_INPUTS_] & STI_AC_LOST_) != 0) == acLost) return f.end;
  }
  return 0;
}

// Both boards with fault_out on (and whatever `more` sets), saved by commission().
std::function<void(Board &)> faultOutOn(const std::function<void(Board &)> &more = nullptr) {
  return [more](Board &b) {
    CHECK(b.set("fault_out", 1));
    if (more) more(b);
  };
}

// A short link timeout: the house declares the link lost 15 s after the last STATUS (gate heartbeat 5 s, so 2.5
// heartbeats are 12.5 s), and the gate 15 s after it last heard the house.
void shortLinkTimeout(Board &b) {
  CHECK(b.set("link_timeout_s", 15));
  CHECK(b.set("heartbeat_s", 5));
}

void checkHealthy(Board &b) {
  CHECK_EQ(b.faultOut(), 1);
  CHECK_EQ(b.mode[P_D5], PM_OUTPUT);
  JsonDocument s = b.status();
  CHECK(s["fault_out"] == true);
  CHECK_STR(health(b), "");
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------

// [fault-out] Off by default: D5 stays an input with its pull-down, never driven, through a fault and a reboot. The
// status shows fault_out null; the health list works regardless, and there is no `health` event.
TEST(fault_out_off_by_default_leaves_d5_an_input) {
  Site w;
  w.commission();
  for (Board *b : { &w.house, &w.gate }) {
    CHECK_EQ(b->get("fault_out"), 0);
    CHECK_EQ(b->get("fault_hold_s"), 10);
    CHECK_EQ(b->mode[P_D5], PM_INPUT_PULLDOWN);
    CHECK_EQ(b->faultOut(), -1);
    JsonDocument s = b->status();
    CHECK(s["fault_out"].isNull());
    CHECK_STR(health(*b), "");
  }
  // AC lost at the gate, past the hold: the health lists name it, D5 stays untouched.
  w.opener.ac = false;
  auto untouched = [&] {
    for (Board *b : { &w.house, &w.gate })
      if (b->running() && (b->mode[P_D5] == PM_OUTPUT || b->out[P_D5])) return false;
    return true;
  };
  CHECK_EQ(watch(w, HOLD_MS + 3000, untouched), 0);
  CHECK_STR(health(w.gate), "ac");
  CHECK_STR(health(w.house), "ac");
  CHECK(w.gate.status()["fault_out"].isNull());
  w.opener.ac = true;
  // A reboot leaves it an input too.
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK_EQ(watch(w, 6000, untouched), 0);
  CHECK(w.gate.running());
  CHECK_EQ(w.gate.mode[P_D5], PM_INPUT_PULLDOWN);
  CHECK_EQ(w.house.count("health"), 0);
  CHECK_EQ(w.gate.count("health"), 0);
}

// [fault-out] Commissioned with fault_out on: both boards end up healthy, D5 HIGH, and stay so in normal operation
// (heartbeats, an open and a close). Each boot logged D5 LOW (starting) first, then HIGH once.
TEST(fault_out_healthy_after_commissioning) {
  Site w;
  w.commission(faultOutOn());
  for (Board *b : { &w.house, &w.gate }) {
    checkHealthy(*b);
    const LogEv *boot = b->last("boot");
    CHECK(boot);
    size_t i0 = boot - b->logs.data();
    Found low = find(*b, "health", i0, 0);
    CHECK(low.ok);
    CHECK_EQ(low.e.b, P_STARTING);  // at boot, before anything else is known
    Found high = find(*b, "health", i0, 1);
    CHECK(high.ok);
    CHECK_EQ(high.e.b, 0);
    CHECK_EQ(countSince(*b, "health", i0), 2);
  }
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  auto bothHigh = [&] { return w.house.faultOut() == 1 && w.gate.faultOut() == 1; };
  CHECK_EQ(watch(w, 65000, bothHigh), 0);  // two heartbeats and more
  w.user(true);
  CHECK_EQ(watch(w, 15000, bothHigh), 0);
  CHECK(w.opener.atOpen());
  w.user(false);
  CHECK_EQ(watch(w, 15000, bothHigh), 0);
  CHECK(w.opener.atClosed());
  CHECK_EQ(countSince(w.house, "health", h0), 0);
  CHECK_EQ(countSince(w.gate, "health", g0), 0);
}

// [fault-out] Link loss: the house drops D5 fault_hold_s after `link_down`, the gate fault_hold_s after its own link
// timeout (15 s since it last heard the house); both raise it again the moment the link is back.
TEST(fault_out_link_loss_drops_after_the_hold_and_recovers_at_once) {
  Site w;
  w.commission(faultOutOn(shortLinkTimeout));
  checkHealthy(w.house);
  checkHealthy(w.gate);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  int cmds = w.house.count("cmd_sent"), pulses = w.gate.count("pulse");
  w.drop = [](const AirFrame &) { return true; };
  Found down = waitFor(w, w.house, "link_down", h0, 20000);
  CHECK(down.ok);
  CHECK_EQ(w.house.faultOut(), 1);
  runTo(w, down.e.at + 1000);
  CHECK_STR(health(w.house), "link");
  Found hl = waitFor(w, w.house, "health", h0, HOLD_MS + 1000, 0);
  CHECK(hl.ok);
  CHECK_EQ(since(hl.e.at, down.e.at), (int32_t)HOLD_MS);
  CHECK_EQ(hl.e.b, P_LINK);
  CHECK_EQ(w.house.faultOut(), 0);
  // The gate last heard the house when the last frame from it that wasn't dropped arrived.
  uint32_t lastRx = 0;
  for (const AirFrame &f : w.air)
    if (f.from == w.house.idx && !f.dropped && f.delivered && (lastRx == 0 || since(f.end, lastRx) > 0)) lastRx = f.end;
  CHECK(lastRx);
  Found gl = waitFor(w, w.gate, "health", g0, 30000, 0);
  CHECK(gl.ok);
  CHECK_IN(since(gl.e.at, lastRx), 15000 + HOLD_MS, 15000 + HOLD_MS + 1);
  CHECK_EQ(gl.e.b, P_LINK);
  CHECK_STR(health(w.gate), "link");
  CHECK_EQ(w.gate.faultOut(), 0);

  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.drop = nullptr;
  Found up = waitFor(w, w.house, "link_up", h1, 15000);
  CHECK(up.ok);
  Found hh = find(w.house, "health", h1, 1);
  CHECK(hh.ok);
  CHECK_EQ(hh.e.at, up.e.at);  // at once
  CHECK_EQ(hh.e.b, 0);
  CHECK_EQ(w.house.faultOut(), 1);
  // The gate hears the house's ACK of that STATUS.
  Found gh = waitFor(w, w.gate, "health", g1, 2000, 1);
  CHECK(gh.ok);
  CHECK_IN(since(gh.e.at, up.e.at), 0, 500);
  checkHealthy(w.house);
  checkHealthy(w.gate);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);  // D5 commands nothing
  CHECK_EQ(w.gate.count("pulse"), pulses);
}

// [fault-out] The opener loses AC and its battery: the gate reads no_power (no AC, no limit). The gate drops D5
// fault_hold_s after it saw it, the house fault_hold_s after the STATUS that said so; both list ac and no_power. Power
// back: the gate at its closed limit again, both raise D5 at once.
TEST(fault_out_gate_no_power_drops_both_after_the_hold) {
  Site w;
  w.commission(faultOutOn());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  int cmds = w.house.count("cmd_sent"), pulses = w.gate.count("pulse");
  w.opener.ac = false;
  w.opener.battery = false;
  Found gs = waitFor(w, w.gate, "gate_state", g0, 1000, GS_NO_POWER_);
  CHECK(gs.ok);
  Found hs = waitFor(w, w.house, "gate_state", h0, 2000, GS_NO_POWER_);
  CHECK(hs.ok);
  runTo(w, hs.e.at + 1000);
  CHECK_STR(health(w.gate), "ac,no_power");
  CHECK_STR(health(w.house), "ac,no_power");
  CHECK_EQ(w.gate.faultOut(), 1);  // held
  CHECK_EQ(w.house.faultOut(), 1);
  Found gl = waitFor(w, w.gate, "health", g0, HOLD_MS + 1000, 0);
  CHECK(gl.ok);
  CHECK_EQ(since(gl.e.at, gs.e.at), (int32_t)HOLD_MS);
  CHECK_EQ(gl.e.b, P_AC | P_NO_POWER);
  Found hl = waitFor(w, w.house, "health", h0, 2000, 0);
  CHECK(hl.ok);
  CHECK_EQ(since(hl.e.at, hs.e.at), (int32_t)HOLD_MS);
  CHECK_EQ(hl.e.b, P_AC | P_NO_POWER);
  CHECK_EQ(w.gate.faultOut(), 0);
  CHECK_EQ(w.house.faultOut(), 0);

  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.opener.ac = true;
  w.opener.battery = true;
  Found gc = waitFor(w, w.gate, "gate_state", g1, 1000, GS_CLOSED_);
  CHECK(gc.ok);
  Found gh = find(w.gate, "health", g1, 1);
  CHECK(gh.ok);
  CHECK_EQ(gh.e.at, gc.e.at);
  Found hc = waitFor(w, w.house, "gate_state", h1, 2000, GS_CLOSED_);
  CHECK(hc.ok);
  Found hh = find(w.house, "health", h1, 1);
  CHECK(hh.ok);
  CHECK_EQ(hh.e.at, hc.e.at);
  checkHealthy(w.gate);
  checkHealthy(w.house);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(w.gate.count("pulse"), pulses);
}

// [fault-out] AC lost with the gate at its closed limit (the opener on its battery): the state stays closed, but both
// boards report ac and drop D5 after the hold (the house counting from the STATUS with the AC-lost bit); AC back raises
// both at once (the house with the next STATUS).
TEST(fault_out_ac_lost_drops_both_after_the_hold) {
  Site w;
  w.commission(faultOutOn());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.ac = false;
  Found in = waitFor(w, w.gate, "input", g0, 1000, 3, 0);
  CHECK(in.ok);
  uint32_t arrive = 0;
  CHECK(w.runUntil([&] { return (arrive = statusArrival(w, in.e.at, true)) != 0 && since(w.now, arrive) >= 0; }, 3000));
  runTo(w, arrive + 1000);
  CHECK_STR(health(w.gate), "ac");
  CHECK_STR(health(w.house), "ac");
  CHECK(w.house.status()["gate"] == "closed");
  Found gl = waitFor(w, w.gate, "health", g0, HOLD_MS + 1000, 0);
  CHECK(gl.ok);
  CHECK_EQ(since(gl.e.at, in.e.at), (int32_t)HOLD_MS);
  CHECK_EQ(gl.e.b, P_AC);
  Found hl = waitFor(w, w.house, "health", h0, 2000, 0);
  CHECK(hl.ok);
  CHECK_EQ(since(hl.e.at, arrive), (int32_t)HOLD_MS);
  CHECK_EQ(hl.e.b, P_AC);
  CHECK(w.sensorClosed());  // the contact sensor still shows the gate closed: D5 is a separate signal

  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.opener.ac = true;
  Found back = waitFor(w, w.gate, "input", g1, 1000, 3, 1);
  CHECK(back.ok);
  Found gh = find(w.gate, "health", g1, 1);
  CHECK(gh.ok);
  CHECK_EQ(gh.e.at, back.e.at);
  uint32_t arrive2 = 0;
  CHECK(w.runUntil([&] { return (arrive2 = statusArrival(w, back.e.at, false)) != 0 && since(w.now, arrive2) >= 0; }, 3000));
  Found hh = waitFor(w, w.house, "health", h1, 100, 1);
  CHECK(hh.ok);
  CHECK_EQ(hh.e.at, arrive2);
  checkHealthy(w.gate);
  checkHealthy(w.house);
}

// [fault-out] Both limits read at once: the gate reads fault, and both boards drop D5 after the hold from when each saw
// it; the limit released, both raise it at once.
TEST(fault_out_gate_fault_drops_both_after_the_hold) {
  Site w;
  w.commission(faultOutOn());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.force[0] = 1;  // the open limit reads too
  Found gs = waitFor(w, w.gate, "gate_state", g0, 1000, GS_FAULT_);
  CHECK(gs.ok);
  Found hs = waitFor(w, w.house, "gate_state", h0, 2000, GS_FAULT_);
  CHECK(hs.ok);
  runTo(w, hs.e.at + 500);
  CHECK_STR(health(w.gate), "fault");
  CHECK_STR(health(w.house), "fault");
  Found gl = waitFor(w, w.gate, "health", g0, HOLD_MS + 1000, 0);
  CHECK(gl.ok);
  CHECK_EQ(since(gl.e.at, gs.e.at), (int32_t)HOLD_MS);
  CHECK_EQ(gl.e.b, P_FAULT);
  Found hl = waitFor(w, w.house, "health", h0, 2000, 0);
  CHECK(hl.ok);
  CHECK_EQ(since(hl.e.at, hs.e.at), (int32_t)HOLD_MS);
  CHECK_EQ(hl.e.b, P_FAULT);

  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.opener.force[0] = -1;
  Found gc = waitFor(w, w.gate, "gate_state", g1, 1000, GS_CLOSED_);
  CHECK(gc.ok);
  Found gh = find(w.gate, "health", g1, 1);
  CHECK(gh.ok);
  CHECK_EQ(gh.e.at, gc.e.at);
  Found hc = waitFor(w, w.house, "gate_state", h1, 2000, GS_CLOSED_);
  CHECK(hc.ok);
  Found hh = find(w.house, "health", h1, 1);
  CHECK(hh.ok);
  CHECK_EQ(hh.e.at, hc.e.at);
  checkHealthy(w.gate);
  checkHealthy(w.house);
}

// [fault-out] A radio that resets and then doesn't start again (radio_fail 2, then 0 on every retry): that board drops
// D5 fault_hold_s later (or at the end of the retry blocking the loop then), the other board stays healthy; the radio
// back (radio_fail 3) raises it at once.
TEST(fault_out_radio_down_drops_after_the_hold_on_that_board) {
  for (int i = 0; i < 2; i++) {
    Site w;
    w.commission(faultOutOn());
    Board &b = w.board(i), &other = w.board(1 - i);
    size_t i0 = b.logs.size(), o0 = other.logs.size();
    b.radioPresent = false;
    b.radioFault = true;
    Found rf = waitFor(w, b, "radio_fail", i0, 100, 2);
    CHECK(rf.ok);
    runTo(w, rf.e.at + 1000);
    CHECK_STR(health(b), "radio");
    Found hl = waitFor(w, b, "health", i0, HOLD_MS + 1000, 0);
    CHECK(hl.ok);
    CHECK_IN(since(hl.e.at, rf.e.at), HOLD_MS, HOLD_MS + 500);  // a radio retry blocks the loop 0.47 s every 5 s
    CHECK_EQ(hl.e.b, P_RADIO);
    CHECK_EQ(b.faultOut(), 0);
    size_t i1 = b.logs.size();
    b.radioPresent = true;
    Found ok = waitFor(w, b, "radio_fail", i1, 6000, 3);
    CHECK(ok.ok);
    Found hh = find(b, "health", i1, 1);
    CHECK(hh.ok);
    CHECK_EQ(hh.e.at, ok.e.at);
    w.run(100);
    checkHealthy(b);
    // The other board heard less for a while, well inside its link timeout.
    checkHealthy(other);
    CHECK_EQ(countSince(other, "health", o0), 0);
  }
}

// [fault-out] A reset reads as a fault until the role has decided: nothing driven in the bootloader, then D5 LOW from
// setup (listing `starting`) until the house is armed (its first STATUS plus sync_window_ms) or the gate has settled
// and sent its first STATUS; the other board stays healthy meanwhile.
TEST(fault_out_reboot_reads_as_a_fault_until_decided) {
  Site w;
  w.commission(faultOutOn());

  // House
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.house.reset(PM_RCAUSE_SYST);
  CHECK_EQ(w.house.faultOut(), -1);
  CHECK_EQ(watch(w, 400, [&] { return w.house.faultOut() == -1; }), 0);  // the bootloader drives nothing
  Found boot = waitFor(w, w.house, "boot", h0, 2000);
  CHECK(boot.ok);
  Found low = find(w.house, "health", h0, 0);
  CHECK(low.ok);
  CHECK_EQ(low.e.b, P_STARTING);
  CHECK_EQ(w.house.faultOut(), 0);
  Found up = waitFor(w, w.house, "link_up", h0, 15000);
  CHECK(up.ok);
  CHECK_EQ(w.house.faultOut(), 0);
  runTo(w, up.e.at + 1000);
  CHECK_STR(health(w.house), "starting");  // link up, but not armed yet
  CHECK(w.house.status()["armed"] == false);
  Found high = waitFor(w, w.house, "health", h0, SYNC_MS, 1);
  CHECK(high.ok);
  CHECK_IN(since(high.e.t, up.e.t), SYNC_MS, SYNC_MS + 1);
  CHECK(w.house.status()["armed"] == true);
  for (size_t i = h0; i < w.house.logs.size(); i++)  // LOW all along until then
    if (w.house.logs[i].ev == "health") CHECK(w.house.logs[i].a == 0 || w.house.logs[i].at == high.e.at);
  CHECK_EQ(countSince(w.gate, "health", g0), 0);
  CHECK_EQ(w.gate.faultOut(), 1);
  checkHealthy(w.house);

  // Gate
  h0 = w.house.logs.size();
  g0 = w.gate.logs.size();
  w.gate.reset(PM_RCAUSE_WDT);
  CHECK_EQ(watch(w, 400, [&] { return w.gate.faultOut() == -1; }), 0);
  Found gboot = waitFor(w, w.gate, "boot", g0, 2000);
  CHECK(gboot.ok);
  Found glow = find(w.gate, "health", g0, 0);
  CHECK(glow.ok);
  CHECK_EQ(glow.e.b, P_STARTING);
  runTo(w, gboot.e.at + 2000);
  CHECK_EQ(w.gate.faultOut(), 0);
  JsonDocument s = w.gate.status();
  CHECK(s["settling"] == true);
  CHECK(s["fault_out"] == false);
  CHECK_STR(health(w.gate).substr(0, 8), "starting");
  Found ghigh = waitFor(w, w.gate, "health", g0, 10000, 1);
  CHECK(ghigh.ok);
  CHECK_IN(since(ghigh.e.t, gboot.e.t), 3000, 4500);  // BOOT_SETTLE_MS after gateBegin (after the radio's start)
  CHECK(w.gate.status()["settling"] == false);
  CHECK_EQ(countSince(w.gate, "health", g0), 2);
  CHECK_EQ(countSince(w.house, "health", h0), 0);  // the house kept the link (and D5) up through it
  checkHealthy(w.gate);
  checkHealthy(w.house);
}

// [fault-out] Faults shorter than fault_hold_s never drop D5: an AC blip, both limits for a moment, and a link loss
// that ends as soon as the house declares it. The health lists show them while they last.
TEST(fault_out_faults_shorter_than_the_hold_never_drop_it) {
  Site w;
  w.commission(faultOutOn(shortLinkTimeout));
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  auto bothHigh = [&] { return w.house.faultOut() == 1 && w.gate.faultOut() == 1; };

  w.opener.ac = false;
  CHECK_EQ(watch(w, 2000, bothHigh), 0);
  CHECK_STR(health(w.gate), "ac");
  CHECK_STR(health(w.house), "ac");
  CHECK_EQ(watch(w, 3000, bothHigh), 0);  // 5 s without AC
  w.opener.ac = true;
  CHECK_EQ(watch(w, 2000, bothHigh), 0);
  CHECK_STR(health(w.gate), "");
  CHECK_STR(health(w.house), "");

  w.opener.force[0] = 1;  // both limits for 3 s
  CHECK_EQ(watch(w, 1500, bothHigh), 0);
  CHECK_STR(health(w.gate), "fault");
  CHECK_EQ(watch(w, 1500, bothHigh), 0);
  w.opener.force[0] = -1;
  CHECK_EQ(watch(w, 3000, bothHigh), 0);
  CHECK_STR(health(w.house), "");

  size_t h1 = w.house.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  CHECK_EQ(watch(w, 1, bothHigh), 0);
  Found down;
  CHECK(w.runUntil([&] {
    down = find(w.house, "link_down", h1);
    return down.ok || !bothHigh();
  }, 20000));
  CHECK(down.ok);
  w.drop = nullptr;
  Found up = waitFor(w, w.house, "link_up", h1, HOLD_MS);
  CHECK(up.ok);
  CHECK(since(up.e.at, down.e.at) < (int32_t)HOLD_MS);
  CHECK_EQ(watch(w, 3000, bothHigh), 0);
  CHECK_EQ(countSince(w.house, "health", h0), 0);
  CHECK_EQ(countSince(w.gate, "health", g0), 0);
  checkHealthy(w.house);
  checkHealthy(w.gate);
}

// [fault-out] fault_hold_s 0: D5 drops in the very pass that sees the fault (the gate's debounced IN3, the STATUS's
// arrival at the house) and comes back as soon as it's over.
TEST(fault_out_hold_zero_drops_at_once) {
  Site w;
  w.commission(faultOutOn([](Board &b) { CHECK(b.set("fault_hold_s", 0)); }));
  checkHealthy(w.house);
  checkHealthy(w.gate);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.ac = false;
  Found in = waitFor(w, w.gate, "input", g0, 1000, 3, 0);
  CHECK(in.ok);
  Found gl = find(w.gate, "health", g0, 0);
  CHECK(gl.ok);
  CHECK_EQ(gl.e.at, in.e.at);
  CHECK_EQ(gl.e.b, P_AC);
  CHECK_EQ(w.gate.faultOut(), 0);
  uint32_t arrive = 0;
  CHECK(w.runUntil([&] { return (arrive = statusArrival(w, in.e.at, true)) != 0 && since(w.now, arrive) >= 0; }, 3000));
  Found hl = waitFor(w, w.house, "health", h0, 100, 0);
  CHECK(hl.ok);
  CHECK_EQ(hl.e.at, arrive);
  CHECK_EQ(w.house.faultOut(), 0);

  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.opener.ac = true;
  Found back = waitFor(w, w.gate, "input", g1, 1000, 3, 1);
  CHECK(back.ok);
  Found gh = find(w.gate, "health", g1, 1);
  CHECK(gh.ok);
  CHECK_EQ(gh.e.at, back.e.at);
  CHECK(waitFor(w, w.house, "health", h1, 3000, 1).ok);
  checkHealthy(w.house);
  checkHealthy(w.gate);
}

// [fault-out] fault_out takes effect at once from the console, without a save or reboot: on, D5 is driven HIGH (a
// healthy board) in the same pass; off, it's an input with its pull-down again (`health` a=-1). It isn't
// remote-writable; fault_hold_s is. A K1 relay test at the house (which disarms it for a while) isn't a new start: D5
// stays HIGH even with fault_hold_s 0.
TEST(fault_out_setting_applies_at_once_and_only_locally) {
  Site w;
  w.commission();
  CHECK_EQ(w.house.faultOut(), -1);
  size_t h0 = w.house.logs.size();
  JsonDocument r = w.house.request("config.set", "\"params\":{\"fault_out\":1,\"fault_hold_s\":0}");
  CHECK(r["ok"] == true);
  CHECK_EQ(w.house.faultOut(), 1);
  Found on = find(w.house, "health", h0, 1);
  CHECK(on.ok);
  CHECK_EQ(on.e.b, 0);
  CHECK(w.house.status()["fault_out"] == true);

  // The K1 test disarms the house until sync_window_ms after it: still decided, still healthy.
  size_t h1 = w.house.logs.size();
  CHECK(w.house.request("relay.test", "\"k\":1,\"ms\":500")["ok"] == true);
  CHECK_EQ(watch(w, 200, [&] { return w.house.faultOut() == 1; }), 0);
  CHECK(w.house.status()["armed"] == false);
  CHECK_STR(health(w.house), "");
  CHECK_EQ(watch(w, 4000, [&] { return w.house.faultOut() == 1; }), 0);
  CHECK_EQ(countSince(w.house, "health", h1), 0);

  // Not remote-writable; fault_hold_s is (the gate applies and saves it).
  CHECK(w.house.request("remote.set", "\"name\":\"fault_out\",\"value\":1")["ok"] == false);
  CHECK_EQ(w.gate.get("fault_out"), 0);
  size_t ev0 = w.house.events.size();
  CHECK(w.house.request("remote.set", "\"name\":\"fault_hold_s\",\"value\":30")["ok"] == true);
  const JsonDocument *set = nullptr;
  CHECK(w.runUntil([&] {
    for (size_t i = ev0; i < w.house.events.size() && !set; i++)
      if (w.house.events[i]["event"] == "remote_set") set = &w.house.events[i];
    return set != nullptr;
  }, 15000));
  CHECK((*set)["ok"] == true);
  CHECK_EQ(w.gate.get("fault_hold_s"), 30);
  CHECK_EQ(w.gate.faultOut(), -1);  // still off there

  size_t h2 = w.house.logs.size();
  CHECK(w.house.request("config.set", "\"params\":{\"fault_out\":0}")["ok"] == true);
  CHECK_EQ(w.house.faultOut(), -1);
  CHECK_EQ(w.house.mode[P_D5], PM_INPUT_PULLDOWN);
  Found off = find(w.house, "health", h2, -1);
  CHECK(off.ok);
  CHECK(w.house.status()["fault_out"].isNull());
}

// [fault-out] A gate heartbeat longer than link_timeout_s / 2.5 (allowed: the house then stretches its link timeout to
// 2.5 heartbeats, link_timeout_eff_s). The gate hears the house once a heartbeat (the ACK of its STATUS), so its own
// timeout stretches the same way: the link stays up on both boards between heartbeats and D5 stays HIGH. A real loss
// still drops the gate's D5, 2.5 heartbeats after it last heard the house plus the hold.
TEST(fault_out_gate_heartbeat_longer_than_link_timeout_stays_healthy) {
  Site w;
  w.commission(faultOutOn([](Board &b) {
    CHECK(b.set("link_timeout_s", 15));
    CHECK(b.set("heartbeat_s", 60));
  }));
  checkHealthy(w.house);
  checkHealthy(w.gate);
  CHECK(w.house.status()["link_timeout_eff_s"] == 150);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  auto bothHigh = [&] { return w.house.faultOut() == 1 && w.gate.faultOut() == 1; };
  CHECK_EQ(watch(w, 200000, bothHigh), 0);  // three heartbeats and more
  CHECK_STR(health(w.gate), "");
  CHECK_STR(health(w.house), "");
  CHECK_EQ(countSince(w.house, "health", h0), 0);
  CHECK_EQ(countSince(w.gate, "health", g0), 0);

  size_t g1 = w.gate.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  Found gl = waitFor(w, w.gate, "health", g1, 170000 + HOLD_MS, 0);
  CHECK(gl.ok);
  CHECK_EQ(gl.e.b, P_LINK);
  uint32_t lastRx = 0;
  for (const AirFrame &f : w.air)
    if (f.from == w.house.idx && !f.dropped && f.delivered && (lastRx == 0 || since(f.end, lastRx) > 0)) lastRx = f.end;
  CHECK(lastRx);
  CHECK_IN(since(gl.e.at, lastRx), 150000 + HOLD_MS, 150000 + HOLD_MS + 1);
  w.drop = nullptr;
}

// [fault-out] [wrap-safe] The hold straddling the millis() wraps (2^32, and 2^31 where signed differences flip): AC
// lost at the gate 5 s before F drops D5 exactly fault_hold_s after each board saw it, and comes back at once.
TEST(fault_out_hold_across_the_millis_wraps) {
  for (uint32_t F : { 0u, 0x80000000u }) {
    Site w(F - 40000);
    w.commission(faultOutOn());
    checkHealthy(w.house);
    checkHealthy(w.gate);
    runTo(w, F - 5000);
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    w.opener.ac = false;
    Found in = waitFor(w, w.gate, "input", g0, 1000, 3, 0);
    CHECK(in.ok);
    uint32_t arrive = 0;
    CHECK(w.runUntil([&] { return (arrive = statusArrival(w, in.e.at, true)) != 0 && since(w.now, arrive) >= 0; }, 3000));
    CHECK_EQ(watch(w, HOLD_MS - 1500, [&] { return w.house.faultOut() == 1 && w.gate.faultOut() == 1; }), 0);
    Found gl = waitFor(w, w.gate, "health", g0, 3000, 0);
    CHECK(gl.ok);
    CHECK_EQ(since(gl.e.at, in.e.at), (int32_t)HOLD_MS);
    CHECK(since(gl.e.at, F) > 0);  // the hold straddled F
    Found hl = waitFor(w, w.house, "health", h0, 3000, 0);
    CHECK(hl.ok);
    CHECK_EQ(since(hl.e.at, arrive), (int32_t)HOLD_MS);
    size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
    w.opener.ac = true;
    Found back = waitFor(w, w.gate, "input", g1, 1000, 3, 1);
    CHECK(back.ok);
    Found gh = find(w.gate, "health", g1, 1);
    CHECK(gh.ok);
    CHECK_EQ(gh.e.at, back.e.at);
    CHECK(waitFor(w, w.house, "health", h1, 3000, 1).ok);
    checkHealthy(w.house);
    checkHealthy(w.gate);
  }
}
