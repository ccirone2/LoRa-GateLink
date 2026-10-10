// House controller power, the settle window, the not-closed display and the contact sensor (CLAUDE.md "Behavioural
// invariants": [ctrl-power] [settle-window] [unknown-shows-open] [sensor-closed-only-known]), through the simulated site
// (world.h): both boards' whole firmware, the Shelly and the house board on the house 12 V rail, the opener, the radio.
//
// Timings as the site models them (world.cpp), counted from the world step before which the change is made:
//  - 12 V cut: the house board's power good drops 250 ms in (pgLagMs, plus up to 5 ms for the supply poll), the
//    Shelly's relay 460 ms in (the house's debounced IN1 edge ~511 ms in), the IN2 opto 2100 ms in (debounced ~2151).
//    Without its LiPo the house board dies 100 ms in (holdUpMs), before any of them.
//  - 12 V back: the opto 50 ms later (IN2 debounced ~101 ms), the power good at once, the Shelly boots 1500 ms later at
//    its SW input's (K1's) level (IN1 edge ~1551 ms).
//  - A user action at X is the house's debounced IN1 edge at X + 51; a K1 change at K moves the Shelly, whose relay
//    reaches the house as an IN1 edge at K + 81.
//  - A settle window (power return, boot) opened at P lasts until P + ctrl_settle_ms + sync_window_ms
//    (openSyncWindow(now, .., ctrl_settle_ms)); an IN1 edge at K1's level ends it early only after P + ctrl_settle_ms.
// Window boundaries use the house's own clock (log `t`, or busyUntil right after its setup), which runs ahead of the
// world's while the board blocks (setup touches the flash and the radio).
#include <Arduino.h>
#include <limits.h>
#include <stdio.h>
#include <functional>
#include "world.h"

namespace {

enum { CAUSE_NONE_ = 0, CAUSE_LORA_ = 1, CAUSE_EXTERNAL_ = 2 };  // roles.h Cause
enum { ACT_OPEN_ = 1, ACT_CLOSE_ = 2 };                          // roles.h Action
const int ANY = INT_MIN;
const uint32_t SETTLE_MS = 10000;                // ctrl_settle_ms default
const uint32_t SYNC_MS = 3000;                   // sync_window_ms default
const uint32_t WINDOW_MS = SETTLE_MS + SYNC_MS;  // a settle window's full length
const uint32_t CONFIRM_MS = 500;                 // ctrl_confirm_ms default
const uint32_t MISMATCH_MS = 75000;              // mismatch_timeout_s default

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

// Runs until `ev` shows up on b (from log index `from`); the world stops on the step that logged it.
Found waitFor(World &w, const Board &b, const char *ev, size_t from, uint32_t maxMs, int a = ANY, int bv = ANY) {
  Found f;
  w.runUntil([&] {
    f = find(b, ev, from, a, bv);
    return f.ok;
  }, maxMs);
  return f;
}

// Checks `cond` every `every` ms (for conditions that read a board's status, which is expensive).
bool poll(World &w, const std::function<bool()> &cond, uint32_t maxMs, uint32_t every = 50) {
  uint32_t start = w.now;
  for (;;) {
    if (cond()) return true;
    if ((int32_t)(w.now - start) >= (int32_t)maxMs) return false;
    w.run(every);
  }
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

bool syncWindow(World &w) {
  return w.house.status()["sync_window"] == true;
}

std::string str(const JsonVariantConst &v) {
  return v.isNull() ? "(null)" : v.as<std::string>();
}

#define CHECK_STR(expr, want) \
  do { \
    std::string got_ = (expr), want_ = (want); \
    if (got_ != want_) \
      throw Failure(where(__FILE__, __LINE__, "CHECK_STR(" #expr "): \"" + got_ + "\" != \"" + want_ + "\"")); \
  } while (0)

// The gate's relay pulses since gate log index g0.
int pulses(const World &w, size_t g0) {
  return countSince(w.gate, "pulse", g0);
}

int presses(const World &w) {
  return (int)(w.opener.presses[0].size() + w.opener.presses[1].size());
}

// The user opens the gate from Alarm.com; returns once it's open, the house shows it (K1 on, sensor open, the
// controller on) and the sync window K1's change opened is over, so the user's next switch is a command.
void openByUser(World &w) {
  size_t h0 = w.house.logs.size();
  w.user(true);
  CHECK(waitFor(w, w.house, "gate_state", h0, 20000, GS_OPEN_).ok);
  CHECK(w.opener.atOpen());
  CHECK(poll(w, [&] { return !syncWindow(w); }, 5000));
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(w.alarmSwitch());
}

// Waits out the sync window (a K1 change, a power return) so that a user action is a command again.
void syncWindowOver(World &w, uint32_t maxMs = 20000) {
  CHECK(poll(w, [&] { return !syncWindow(w); }, maxMs, 20));
}

void houseParam(Board &b, const char *param, int32_t v) {
  if (b.idx == 0) CHECK(b.set(param, v));
}

// Cuts the house 12 V rail for `ms` (the house board on its LiPo or not, as set) and brings it back; returns the
// house's ctrl_power ON event (with the house up throughout: nothing else may have logged one since h0).
Found cutRail(World &w, uint32_t ms) {
  size_t h0 = w.house.logs.size();
  w.setRail12(false);
  w.run(ms);
  size_t h1 = w.house.logs.size();
  w.setRail12(true);
  Found up = waitFor(w, w.house, "ctrl_power", h1, 3000, 1);
  CHECK(up.ok);
  CHECK(find(w.house, "ctrl_power", h0, 0).ok);
  return up;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// [ctrl-power]: 12 V cuts with the house LiPo in and out, short dips, opto-only sensing, the confirm wait

// [ctrl-power] [settle-window]
TEST(house_power_cut_lipo_in_gate_open) {
  // The house board rides through on its LiPo. Its power good drops ~0.2 s before the controller's relay, so the
  // relay's OFF edge finds the controller already unpowered: ignored, never a CLOSE. The Shelly boots at K1's level
  // when power returns, inside the settle window: a sync, never an OPEN.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent"), press0 = presses(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);

  Found sup = waitFor(w, w.house, "supply", h0, 1000, 0);
  CHECK(sup.ok);
  CHECK_IN(since(sup.e.at, t0), 250, 258);
  Found dn = find(w.house, "ctrl_power", h0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.a, 0);
  CHECK_EQ(dn.e.b, 0);  // nothing pending to discard
  CHECK_EQ(dn.e.at, sup.e.at);
  CHECK(w.alarmSwitch());  // the relay hasn't dropped yet
  CHECK(w.shelly.opto);    // nor the opto

  Found rel = waitFor(w, w.house, "ctrl", h0, 1000);
  CHECK(rel.ok);
  CHECK_EQ(rel.e.a, 0);
  CHECK_EQ(rel.e.b, 1);  // ignored: controller unpowered
  CHECK_IN(since(rel.e.at, t0), 508, 515);
  CHECK(since(rel.e.at, dn.e.at) >= 200);  // the board's supply saw it ~0.2 s before the relay dropped
  CHECK(!w.alarmSwitch());

  // On its LiPo the house keeps running and showing the gate open; the opto going dark changes nothing more.
  uint32_t bad = watch(w, 4000, [&] { return w.house.running() && w.house.coil(1) && !w.house.coil(2); });
  CHECK_EQ(bad, 0);
  CHECK(!w.shelly.opto);
  JsonDocument s = w.house.status();
  CHECK(s["ctrl_power"] == false);
  CHECK(s["ctrl"] == false);
  CHECK(s["supply"] == false);
  CHECK(s["io"]["in2"] == false);
  CHECK_STR(str(s["gate"]), "open");
  CHECK(s["link_up"] == true);
  CHECK_EQ(countSince(w.house, "ctrl_power", h0), 1);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 1);

  size_t h1 = w.house.logs.size();
  uint32_t t1 = w.now;
  w.setRail12(true);
  Found up = waitFor(w, w.house, "ctrl_power", h1, 1000, 1);
  CHECK(up.ok);
  CHECK_EQ(up.e.b, 0);
  CHECK_IN(since(up.e.at, t1), 99, 106);  // once the opto is back (50 ms) and debounced; the power good came first
  Found sy = waitFor(w, w.house, "sync", h1, 3000);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 1);
  CHECK_IN(since(sy.e.at, t1), 1548, 1556);  // the Shelly booting at K1's level
  CHECK(w.alarmSwitch());
  CHECK(sy.e.t < up.e.t + SETTLE_MS);

  // That edge matched K1 but came before the settle time: the window runs its full length.
  runTo(w, up.e.t + WINDOW_MS - 5);
  CHECK(syncWindow(w));
  runTo(w, up.e.t + WINDOW_MS + 5);
  CHECK(!syncWindow(w));
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK_EQ(presses(w), press0);
  CHECK(w.opener.atOpen());

  // Back to normal: the user's next switch is a command.
  size_t h2 = w.house.logs.size();
  w.user(false);
  Found cmd = waitFor(w, w.house, "cmd_sent", h2, 1000, ACT_CLOSE_);
  CHECK(cmd.ok);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
}

// [ctrl-power] [settle-window] [sensor-closed-only-known]
TEST(house_power_cut_lipo_in_gate_closed) {
  // Gate closed: the relay is already off, so the cut makes no IN1 edge at all, and the Shelly boots off. The house
  // keeps the contact sensor closed throughout (it still knows the gate closed: link up, on its LiPo).
  World w;
  w.commission();
  w.house.lipo = true;
  CHECK(w.sensorClosed());
  CHECK(!w.house.coil(1));
  CHECK(!w.alarmSwitch());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  Found dn = waitFor(w, w.house, "ctrl_power", h0, 1000, 0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.b, 0);
  CHECK_IN(since(dn.e.at, t0), 250, 258);
  // The user tries the switch while the controller is down: it can't reach us, now or when it boots.
  runTo(w, t0 + 1000);
  CHECK(!w.shelly.booted);
  w.user(true);
  uint32_t bad = watch(w, 4000, [&] { return w.sensorClosed() && !w.house.coil(1) && w.house.running(); });
  CHECK_EQ(bad, 0);

  size_t h1 = w.house.logs.size();
  uint32_t t1 = w.now;
  w.setRail12(true);
  Found up = waitFor(w, w.house, "ctrl_power", h1, 1000, 1);
  CHECK(up.ok);
  CHECK_IN(since(up.e.at, t1), 99, 106);
  CHECK(w.runUntil([&] { return w.shelly.booted; }, 2000));
  CHECK(!w.alarmSwitch());  // booted at K1's level: off
  // No IN1 edge, so the window runs its full length.
  runTo(w, up.e.t + WINDOW_MS - 5);
  CHECK(syncWindow(w));
  runTo(w, up.e.t + WINDOW_MS + 5);
  CHECK(!syncWindow(w));
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "sync", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.sensorClosed());
  CHECK(w.opener.atClosed());
}

// [ctrl-power] [settle-window]
TEST(house_power_cut_lipo_out_gate_open) {
  // Without its LiPo the house board dies with the rail, 0.1 s in, before its power good or the relay drop; it boots
  // when power returns, ahead of the Shelly. Nothing it sees on IN1 then is a command: the Shelly's restoring edge
  // falls inside the boot / power-return settle window.
  World w;
  w.commission();
  openByUser(w);
  int cmds = w.house.count("cmd_sent"), press0 = presses(w);
  uint32_t boots = w.house.boots;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  CHECK(w.runUntil([&] { return !w.house.running(); }, 500));
  CHECK_IN(since(w.now, t0), 99, 103);
  CHECK(!w.house.coil(1));
  CHECK(!w.sensorClosed());  // both relays released with the board: the sensor reads open
  CHECK_EQ(countSince(w.house, "ctrl_power", h0), 0);
  CHECK_EQ(countSince(w.house, "supply", h0), 0);
  runTo(w, t0 + 5000);
  CHECK(!w.shelly.booted);

  size_t h1 = w.house.logs.size();
  w.setRail12(true);
  Found boot = waitFor(w, w.house, "boot", h1, 1000);
  CHECK(boot.ok);
  CHECK_EQ(boot.e.a, PM_RCAUSE_POR);
  CHECK_EQ(boot.e.b, 1);  // house
  CHECK_EQ(w.house.boots, boots + 1);
  // It booted before the opto came back: unpowered, then powered (a settle window either way).
  Found up = waitFor(w, w.house, "ctrl_power", h1, 1000, 1);
  CHECK(up.ok);
  Found sy = waitFor(w, w.house, "sync", h1, 5000, 1);
  CHECK(sy.ok);
  CHECK(poll(w, [&] {
    JsonDocument s = w.house.status();
    return s["armed"] == true && s["sync_window"] == false;
  }, 20000));
  CHECK(w.house.coil(1));
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK_EQ(presses(w), press0);
  CHECK(w.opener.atOpen());
}

// [ctrl-power] [sensor-closed-only-known]
TEST(house_power_cut_lipo_out_gate_closed) {
  // The board dies: the sensor reads open while it's off (K2 released), and after it boots it stays open until the
  // gate's first STATUS reports closed, closing on that very pass. The Shelly boots off: no edge, no command.
  World w;
  w.commission();
  CHECK(w.sensorClosed());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  CHECK(w.runUntil([&] { return !w.house.running(); }, 500));
  CHECK(!w.sensorClosed());
  runTo(w, t0 + 4000);
  size_t h1 = w.house.logs.size();
  w.setRail12(true);
  CHECK(w.runUntil([&] { return w.house.running(); }, 1000));
  Found gs;
  uint32_t early = 0;
  CHECK(w.runUntil([&] {
    gs = find(w.house, "gate_state", h1);
    if (!gs.ok && w.sensorClosed() && !early) early = w.now;
    return gs.ok;
  }, 20000));
  CHECK_EQ(early, 0);
  CHECK_EQ(gs.e.a, GS_CLOSED_);
  CHECK(w.sensorClosed());  // the same pass
  CHECK(!w.house.coil(1));
  w.run(15000);
  CHECK(!w.alarmSwitch());
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "sync", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.sensorClosed());
}

// [ctrl-power] [settle-window]
TEST(house_power_dips_lipo_in_gate_open) {
  // 200 ms: shorter than the charger takes to report VIN lost: nothing at all. 300 ms: the power good drops, the
  // relay (460 ms) and the opto (2.1 s) ride through: the controller counts as unpowered for the dip and gets a settle
  // window after it, and no IN1 edge happens.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.setRail12(false);
  w.run(200);
  w.setRail12(true);
  w.run(3000);
  CHECK_EQ(countSince(w.house, "supply", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl_power", h0), 0);
  CHECK(!syncWindow(w));
  CHECK(w.alarmSwitch());

  size_t h1 = w.house.logs.size();
  uint32_t t1 = w.now;
  w.setRail12(false);
  uint32_t bad = watch(w, 300, [&] { return w.alarmSwitch() && w.shelly.opto; });
  w.setRail12(true);
  bad = bad ? bad : watch(w, 3000, [&] { return w.alarmSwitch() && w.shelly.opto; });
  CHECK_EQ(bad, 0);
  Found dn = find(w.house, "ctrl_power", h1, 0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.b, 0);
  CHECK_IN(since(dn.e.at, t1), 250, 258);
  Found up = find(w.house, "ctrl_power", h1, 1);
  CHECK(up.ok);
  CHECK_IN(since(up.e.at, t1), 300, 307);
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(countSince(w.house, "sync", h1), 0);
  runTo(w, up.e.t + WINDOW_MS - 5);
  CHECK(syncWindow(w));
  runTo(w, up.e.t + WINDOW_MS + 5);
  CHECK(!syncWindow(w));
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
}

// [ctrl-power]
TEST(house_power_dip_600ms_lipo_in_relay_drops_opto_misses_it) {
  // A dip long enough to drop the controller's relay (and reboot it) but too short for the opto to see: only the
  // board's power good catches it. The relay's OFF edge is ignored, the Shelly's restoring ON edge is a sync.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  uint32_t bad = watch(w, 600, [&] { return w.shelly.opto; });
  w.setRail12(true);
  bad = bad ? bad : watch(w, 4000, [&] { return w.shelly.opto; });
  CHECK_EQ(bad, 0);  // the opto never went dark
  Found dn = find(w.house, "ctrl_power", h0, 0);
  CHECK(dn.ok);
  CHECK_IN(since(dn.e.at, t0), 250, 258);
  Found rel = find(w.house, "ctrl", h0);
  CHECK(rel.ok);
  CHECK_EQ(rel.e.a, 0);
  CHECK_EQ(rel.e.b, 1);
  CHECK_IN(since(rel.e.at, t0), 508, 515);
  Found up = find(w.house, "ctrl_power", h0, 1);
  CHECK(up.ok);
  CHECK_IN(since(up.e.at, t0), 600, 607);
  Found sy = find(w.house, "sync", h0, 1);
  CHECK(sy.ok);
  CHECK_IN(since(sy.e.at, t0), 2148, 2156);  // the Shelly rebooted 1.5 s after power returned
  CHECK_EQ(countSince(w.house, "ctrl", h0), 1);
  CHECK_EQ(countSince(w.house, "ctrl_power", h0), 2);
  CHECK(w.alarmSwitch());
  syncWindowOver(w);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(countSince(w.house, "cmd_suppressed", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
}

// [ctrl-power] [settle-window]
TEST(house_power_dip_300ms_lipo_out_board_reboots) {
  // Without its LiPo a 300 ms dip resets the house board (it holds up ~0.1 s) while the Shelly rides through. The
  // relay shield's K1 drops with the board, so the Shelly follows its SW input off; the house boots, hears the gate
  // open and turns K1 back on, and the Shelly's edges stay syncs. No command.
  World w;
  w.commission();
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  CHECK(w.runUntil([&] { return !w.house.running(); }, 300));
  CHECK_IN(since(w.now, t0), 99, 103);
  runTo(w, t0 + 300);
  w.setRail12(true);
  CHECK(w.shelly.booted);  // rode through
  Found boot = waitFor(w, w.house, "boot", h0, 1000);
  CHECK(boot.ok);
  CHECK_EQ(boot.e.a, PM_RCAUSE_POR);
  CHECK(!w.alarmSwitch());  // followed K1 off while the board was down
  Found sy = waitFor(w, w.house, "sync", h0, 10000, 1);
  CHECK(sy.ok);
  CHECK(w.house.coil(1));
  CHECK(w.alarmSwitch());
  // It came up with the controller powered (the opto never saw the dip): no ctrl_power event, still a settle window.
  CHECK_EQ(countSince(w.house, "ctrl_power", h0), 0);
  CHECK(sy.e.t < w.house.last("boot")->t + SETTLE_MS);
  syncWindowOver(w);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
}

// [ctrl-power]
TEST(house_power_opto_only_cut_gate_open_discards_close) {
  // ctrl_power_pmic 0: the opto alone, which lags the relay by ~1.65 s; ctrl_confirm_ms 3000 covers it. The relay's
  // OFF edge looks like the user's CLOSE and waits; the opto goes dark before the 3 s are up and the CLOSE is dropped
  // (ctrl_power b = the discarded action).
  World w;
  w.commission([](Board &b) {
    houseParam(b, "ctrl_power_pmic", 0);
    houseParam(b, "ctrl_confirm_ms", 3000);
  });
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  Found sup = waitFor(w, w.house, "supply", h0, 1000, 0);
  CHECK(sup.ok);  // seen, but not counted
  Found rel = waitFor(w, w.house, "ctrl", h0, 1000);
  CHECK(rel.ok);
  CHECK_EQ(rel.e.a, 0);
  CHECK_EQ(rel.e.b, 0);  // the house can't tell it from the user's
  CHECK_IN(since(rel.e.at, t0), 508, 515);
  CHECK(!find(w.house, "ctrl_power", h0).ok);
  Found dn = waitFor(w, w.house, "ctrl_power", h0, 3000, 0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.b, ACT_CLOSE_);
  CHECK_IN(since(dn.e.at, t0), 2148, 2156);
  CHECK(since(dn.e.at, rel.e.at) < 3000);
  runTo(w, t0 + 5000);  // well past when the CLOSE would have gone
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  size_t h1 = w.house.logs.size();
  w.setRail12(true);
  Found up = waitFor(w, w.house, "ctrl_power", h1, 1000, 1);
  CHECK(up.ok);
  Found sy = waitFor(w, w.house, "sync", h1, 3000, 1);
  CHECK(sy.ok);
  syncWindowOver(w);
  CHECK(w.alarmSwitch());
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
}

// [ctrl-power] [sensor-closed-only-known]
TEST(house_power_opto_only_cut_gate_closed) {
  // Opto-only, gate closed: no IN1 edge; the opto drop marks the controller unpowered with nothing pending.
  World w;
  w.commission([](Board &b) {
    houseParam(b, "ctrl_power_pmic", 0);
    houseParam(b, "ctrl_confirm_ms", 3000);
  });
  w.house.lipo = true;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  uint32_t bad = watch(w, 4000, [&] { return w.sensorClosed() && !w.house.coil(1); });
  CHECK_EQ(bad, 0);
  Found dn = find(w.house, "ctrl_power", h0, 0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.b, 0);
  CHECK_IN(since(dn.e.at, t0), 2148, 2156);
  size_t h1 = w.house.logs.size();
  w.setRail12(true);
  bad = watch(w, 16000, [&] { return w.sensorClosed() && !w.house.coil(1); });
  CHECK_EQ(bad, 0);
  CHECK(find(w.house, "ctrl_power", h1, 1).ok);
  CHECK(!w.alarmSwitch());
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
}

// [ctrl-power]
TEST(house_power_opto_only_dip_600ms_gate_open) {
  // Opto-only, a 600 ms dip: the relay drops (OFF edge, pending CLOSE) but the opto never sees the dip. The Shelly is
  // back at K1's level (ON) 1.5 s later, inside ctrl_confirm_ms 3000: that ON edge replaces the pending CLOSE, and the
  // OPEN it makes is suppressed (the gate is open). The CLOSE never goes.
  World w;
  w.commission([](Board &b) {
    houseParam(b, "ctrl_power_pmic", 0);
    houseParam(b, "ctrl_confirm_ms", 3000);
  });
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.setRail12(false);
  w.run(600);
  w.setRail12(true);
  runTo(w, t0 + 6000);
  CHECK_EQ(countSince(w.house, "ctrl_power", h0), 0);
  Found off = find(w.house, "ctrl", h0, 0);
  CHECK(off.ok);
  CHECK_EQ(off.e.b, 0);
  CHECK_IN(since(off.e.at, t0), 508, 515);
  Found on = find(w.house, "ctrl", h0, 1);
  CHECK(on.ok);
  CHECK_EQ(on.e.b, 0);
  CHECK(since(on.e.at, off.e.at) < 3000);
  Found sup = find(w.house, "cmd_suppressed", h0);
  CHECK(sup.ok);
  CHECK_EQ(sup.e.a, ACT_OPEN_);
  CHECK_EQ(sup.e.b, GS_OPEN_);
  CHECK_EQ(sup.e.at, on.e.at);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.alarmSwitch());
  CHECK(w.opener.atOpen());
}

// [ctrl-power]
TEST(house_power_close_waits_confirm_open_goes_at_once) {
  // A user OPEN (relay on), which no power loss can produce, is sent on the pass that debounces it; a user CLOSE is
  // sent exactly ctrl_confirm_ms later, not a millisecond before.
  World w;
  w.commission();
  size_t h0 = w.house.logs.size();
  uint32_t u = w.now;
  w.user(true);
  Found c = waitFor(w, w.house, "ctrl", h0, 200);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, 1);
  CHECK_EQ(c.e.b, 0);
  CHECK_EQ(since(c.e.at, u), 51);
  Found s = find(w.house, "cmd_sent", h0);
  CHECK(s.ok);
  CHECK_EQ(s.e.a, ACT_OPEN_);
  CHECK_EQ(s.e.at, c.e.at);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  CHECK(w.runUntil([&] { return w.house.coil(1); }, 3000));
  syncWindowOver(w);

  size_t h1 = w.house.logs.size();
  u = w.now;
  w.user(false);
  c = waitFor(w, w.house, "ctrl", h1, 200);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, 0);
  CHECK_EQ(c.e.b, 0);
  CHECK_EQ(since(c.e.at, u), 51);
  runTo(w, c.e.at + CONFIRM_MS - 1);
  CHECK(!find(w.house, "cmd_sent", h1).ok);
  w.run(1);
  s = find(w.house, "cmd_sent", h1);
  CHECK(s.ok);
  CHECK_EQ(s.e.a, ACT_CLOSE_);
  CHECK_EQ(since(s.e.at, c.e.at), CONFIRM_MS);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
}

// [ctrl-power]
TEST(house_power_close_discarded_by_power_loss_inside_confirm) {
  // The user's CLOSE waits ctrl_confirm_ms; the board's power good drops ~20 ms before it would go: the CLOSE is
  // discarded (ctrl_power b = CLOSE), as the relay dropping with a power cut looks just like it. The Shelly comes back
  // at K1's level (open): the controller ends up showing the real gate again.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.user(false);
  Found c = waitFor(w, w.house, "ctrl", h0, 200, 0);
  CHECK(c.ok);
  runTo(w, c.e.at + CONFIRM_MS - 250 - 25);
  w.setRail12(false);
  Found dn = waitFor(w, w.house, "ctrl_power", h0, 400, 0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.b, ACT_CLOSE_);
  CHECK_IN(since(dn.e.at, c.e.at), CONFIRM_MS - 25, CONFIRM_MS - 1);
  w.run(3000);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  size_t h1 = w.house.logs.size();
  w.setRail12(true);
  CHECK(waitFor(w, w.house, "sync", h1, 3000, 1).ok);
  CHECK(w.alarmSwitch());
  syncWindowOver(w);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
  CHECK(w.house.coil(1));
}

// [ctrl-power]
TEST(house_power_close_sent_when_power_lost_after_confirm) {
  // The same, with the power good dropping ~20 ms after ctrl_confirm_ms: the CLOSE has gone (the user's, confirmed),
  // the power loss finds nothing pending, and the gate closes.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  size_t h0 = w.house.logs.size();
  w.user(false);
  Found c = waitFor(w, w.house, "ctrl", h0, 200, 0);
  CHECK(c.ok);
  runTo(w, c.e.at + CONFIRM_MS - 250 + 15);
  w.setRail12(false);
  Found s = waitFor(w, w.house, "cmd_sent", h0, 400);
  CHECK(s.ok);
  CHECK_EQ(s.e.a, ACT_CLOSE_);
  CHECK_EQ(since(s.e.at, c.e.at), CONFIRM_MS);
  Found dn = waitFor(w, w.house, "ctrl_power", h0, 400, 0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.b, 0);
  CHECK(since(dn.e.at, s.e.at) > 0);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
  w.setRail12(true);
  CHECK(w.runUntil([&] { return w.shelly.booted; }, 2000));
  w.run(200);
  CHECK(!w.alarmSwitch());
  CHECK(!w.house.coil(1));
}

// [ctrl-power] [settle-window]
TEST(house_power_in2_off_alone_blocks_commands_and_resync) {
  // The board's supply is fine but IN2 reads the controller unpowered (the opto failed dark after a cut): the user's
  // switch is never a command, and the house doesn't resync the out-of-step controller, however long. Once IN2 is
  // back, the settle window runs and the house resyncs at once when it ends.
  World w;
  w.commission();
  w.house.lipo = true;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.setRail12(false);
  w.run(3000);
  CHECK(!w.shelly.opto);
  w.shelly.optoUpMs = 3600000;  // stays dark
  w.setRail12(true);
  CHECK(w.runUntil([&] { return w.shelly.booted; }, 2000));
  w.run(100);
  JsonDocument s = w.house.status();
  CHECK(s["ctrl_power"] == false);
  CHECK(s["supply"] == true);
  CHECK(s["io"]["in2"] == false);
  CHECK(!find(w.house, "ctrl_power", h0, 1).ok);

  size_t h1 = w.house.logs.size();
  uint32_t u = w.now;
  w.user(true);
  Found c = waitFor(w, w.house, "ctrl", h1, 200);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, 1);
  CHECK_EQ(c.e.b, 1);
  CHECK_EQ(since(c.e.at, u), 51);
  uint32_t bad = watch(w, MISMATCH_MS + 15000, [&] { return !w.house.coil(1) && w.alarmSwitch() && w.sensorClosed(); });
  CHECK_EQ(bad, 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);

  w.shelly.optoAt = (w.now + 1) | 1;  // the opto lights again
  Found up = waitFor(w, w.house, "ctrl_power", h1, 200, 1);
  CHECK(up.ok);
  Found rs = waitFor(w, w.house, "resync", h1, WINDOW_MS + 1000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);  // to the gate's level: closed
  CHECK_IN(since(rs.e.t, up.e.t), WINDOW_MS, WINDOW_MS + 5);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1500));
  CHECK(!w.house.coil(1));
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atClosed());
}

// [ctrl-power] [settle-window]
TEST(house_power_vin_off_alone_blocks_commands_and_resync) {
  // The house board's own feed is lost (on its LiPo) while the controller stays powered: the power good alone marks
  // the controller unpowered. Same outcome: no command, no resync until the feed is back and the window has run.
  World w;
  w.commission();
  w.house.lipo = true;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t0 = w.now;
  w.house.cut = true;
  Found dn = waitFor(w, w.house, "ctrl_power", h0, 1000, 0);
  CHECK(dn.ok);
  CHECK_IN(since(dn.e.at, t0), 250, 258);
  w.run(500);
  CHECK(w.shelly.booted);
  CHECK(w.shelly.opto);
  JsonDocument s = w.house.status();
  CHECK(s["ctrl_power"] == false);
  CHECK(s["supply"] == false);
  CHECK(s["io"]["in2"] == true);

  size_t h1 = w.house.logs.size();
  w.user(true);
  Found c = waitFor(w, w.house, "ctrl", h1, 200);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, 1);
  CHECK_EQ(c.e.b, 1);
  uint32_t bad = watch(w, MISMATCH_MS + 15000, [&] { return !w.house.coil(1) && w.alarmSwitch() && w.sensorClosed(); });
  CHECK_EQ(bad, 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);

  uint32_t t1 = w.now;
  w.house.cut = false;
  Found up = waitFor(w, w.house, "ctrl_power", h1, 200, 1);
  CHECK(up.ok);
  CHECK_IN(since(up.e.at, t1), 1, 7);
  Found rs = waitFor(w, w.house, "resync", h1, WINDOW_MS + 1000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(since(rs.e.t, up.e.t), WINDOW_MS, WINDOW_MS + 5);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1500));
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atClosed());
}

// [ctrl-power]
TEST(house_power_sensing_off_edges_are_commands) {
  // With both senses off (ctrl_power_sense 0, ctrl_power_pmic 0) the house can't know, so nothing gates the edges:
  // a lost board supply doesn't block them, and a CLOSE goes at once (no confirm wait without any power sense).
  World w;
  w.commission([](Board &b) {
    houseParam(b, "ctrl_power_sense", 0);
    houseParam(b, "ctrl_power_pmic", 0);
  });
  w.house.lipo = true;
  size_t h0 = w.house.logs.size();
  w.house.cut = true;
  w.run(1000);
  CHECK(find(w.house, "supply", h0, 0).ok);
  CHECK(!find(w.house, "ctrl_power", h0).ok);
  CHECK(w.house.status()["ctrl_power"] == true);
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found c = waitFor(w, w.house, "ctrl", h1, 200, 1);
  CHECK(c.ok);
  CHECK_EQ(c.e.b, 0);
  Found s = find(w.house, "cmd_sent", h1, ACT_OPEN_);
  CHECK(s.ok);
  CHECK_EQ(s.e.at, c.e.at);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  syncWindowOver(w);
  size_t h2 = w.house.logs.size();
  w.user(false);
  c = waitFor(w, w.house, "ctrl", h2, 200, 0);
  CHECK(c.ok);
  s = find(w.house, "cmd_sent", h2, ACT_CLOSE_);
  CHECK(s.ok);
  CHECK_EQ(s.e.at, c.e.at);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
}

// ---------------------------------------------------------------------------------------------------------------------
// [settle-window]

// [settle-window]
TEST(house_power_settle_user_edge_just_inside_window_ignored) {
  // An IN1 edge 20 ms before a power-return window ends is the Shelly's (a sync), even from the user; the house then
  // resyncs the controller to the gate as soon as the window is over.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t g0 = w.gate.logs.size();
  Found up = cutRail(w, 3000);
  runTo(w, up.e.t + WINDOW_MS - 51 - 20);
  size_t h1 = w.house.logs.size();
  w.user(false);
  Found sy = waitFor(w, w.house, "sync", h1, 200);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 0);
  CHECK_EQ(since(sy.e.t, up.e.t), (int32_t)WINDOW_MS - 20);
  CHECK(!find(w.house, "ctrl", h1).ok);
  Found rs = waitFor(w, w.house, "resync", h1, 500);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK_IN(since(rs.e.t, up.e.t), WINDOW_MS, WINDOW_MS + 5);
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 1500));
  w.run(3000);
  CHECK(w.house.coil(1));
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
}

// [settle-window]
TEST(house_power_settle_user_edge_just_after_window_commands) {
  // 20 ms after the window ends the same edge is the user's: a CLOSE, after ctrl_confirm_ms.
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  Found up = cutRail(w, 3000);
  runTo(w, up.e.t + WINDOW_MS - 51 + 20);
  size_t h1 = w.house.logs.size();
  w.user(false);
  Found c = waitFor(w, w.house, "ctrl", h1, 200);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, 0);
  CHECK_EQ(c.e.b, 0);
  CHECK_EQ(since(c.e.t, up.e.t), (int32_t)WINDOW_MS + 20);
  CHECK(!find(w.house, "sync", h1).ok);
  Found s = waitFor(w, w.house, "cmd_sent", h1, 1000);
  CHECK(s.ok);
  CHECK_EQ(s.e.a, ACT_CLOSE_);
  CHECK_EQ(since(s.e.at, c.e.at), CONFIRM_MS);
  CHECK(!find(w.house, "resync", h1).ok);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
}

// [settle-window]
TEST(house_power_settle_matching_edge_after_settle_until_ends_window) {
  // A slow controller boots 11.5 s after power returns: its edge to K1's level comes after ctrl_settle_ms, so it ends
  // the window at once (whereas the same edge before it, as in house_power_cut_lipo_in_gate_open, doesn't).
  World w;
  w.commission();
  w.house.lipo = true;
  openByUser(w);
  w.shelly.bootMs = 11500;
  Found up = cutRail(w, 3000);
  CHECK(w.shelly.bootAt != 0);
  uint32_t edgeAt = w.shelly.bootAt + 50;
  CHECK(since(edgeAt, up.e.t) > (int32_t)SETTLE_MS + 100);
  CHECK(since(edgeAt, up.e.t) < (int32_t)WINDOW_MS - 1000);
  size_t h1 = w.house.logs.size();
  runTo(w, edgeAt - 5);
  CHECK(syncWindow(w));
  Found sy = waitFor(w, w.house, "sync", h1, 100);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 1);
  CHECK_IN(since(sy.e.at, edgeAt), -1, 1);
  CHECK(!syncWindow(w));
  CHECK(w.alarmSwitch());
  // Over, and armed: the user's next switch is a command.
  w.run(100);
  size_t h2 = w.house.logs.size();
  w.user(false);
  Found s = waitFor(w, w.house, "cmd_sent", h2, 1000, ACT_CLOSE_);
  CHECK(s.ok);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
}

// A power return with a controller that boots off (not restoring K1's level) while the gate is open: K1 on, the
// Shelly off, no IN1 edge of its own. Returns the ctrl_power ON event; P = its `t` opened the settle window.
namespace {
Found returnWithControllerOff(World &w) {
  w.house.lipo = true;
  openByUser(w);
  w.shelly.restoreSw = false;
  Found up = cutRail(w, 3000);
  size_t h1 = w.house.logs.size();
  CHECK(w.runUntil([&] { return w.shelly.booted; }, 2000));
  w.run(100);
  CHECK(!w.alarmSwitch());
  CHECK(w.house.coil(1));
  CHECK(!find(w.house, "sync", h1).ok);
  CHECK(!find(w.house, "ctrl", h1).ok);
  CHECK(syncWindow(w));
  return up;
}
}  // namespace

// [settle-window]
TEST(house_power_settle_matching_edge_1ms_before_settle_until_holds_window) {
  // The controller is switched to K1's level 1 ms before settleUntil (ctrl_settle_ms after power return): a sync,
  // but the window still runs its full ctrl_settle_ms + sync_window_ms.
  World w;
  w.commission();
  Found up = returnWithControllerOff(w);
  int cmds = w.house.count("cmd_sent");
  size_t g0 = w.gate.logs.size();
  uint32_t P = up.e.t;
  runTo(w, P + SETTLE_MS - 1 - 51);
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found sy = waitFor(w, w.house, "sync", h1, 200);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 1);
  CHECK_EQ(since(sy.e.t, P), (int32_t)SETTLE_MS - 1);
  CHECK(!find(w.house, "ctrl", h1).ok);
  w.run(1);
  CHECK(syncWindow(w));
  runTo(w, P + WINDOW_MS - 5);
  CHECK(syncWindow(w));
  runTo(w, P + WINDOW_MS + 5);
  CHECK(!syncWindow(w));
  w.run(2000);
  CHECK_EQ(countSince(w.house, "resync", h1), 0);  // in step
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.alarmSwitch());
  CHECK(w.house.coil(1));
  CHECK(w.opener.atOpen());
}

// [settle-window]
TEST(house_power_settle_matching_edge_at_settle_until_ends_window) {
  // The same edge exactly at settleUntil ends the window on that pass: the user's next switch is a command.
  World w;
  w.commission();
  Found up = returnWithControllerOff(w);
  int cmds = w.house.count("cmd_sent");
  uint32_t P = up.e.t;
  runTo(w, P + SETTLE_MS - 51);
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found sy = waitFor(w, w.house, "sync", h1, 200);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 1);
  CHECK_EQ(since(sy.e.t, P), (int32_t)SETTLE_MS);
  w.run(1);
  CHECK(!syncWindow(w));
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  w.run(100);
  size_t h2 = w.house.logs.size();
  w.user(false);
  Found c = waitFor(w, w.house, "ctrl", h2, 200, 0);
  CHECK(c.ok);
  CHECK_EQ(c.e.b, 0);
  CHECK(since(c.e.t, P) < (int32_t)WINDOW_MS - 2000);  // long before the full window would have ended
  Found s = waitFor(w, w.house, "cmd_sent", h2, 1000, ACT_CLOSE_);
  CHECK(s.ok);
  CHECK_EQ(since(s.e.at, c.e.at), CONFIRM_MS);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
}

// [settle-window]
TEST(house_power_boot_settle_window_holds_through_k1_sync_and_chatter) {
  // A house reboot opens the settle window too. The first STATUS turns K1 on (its own 3 s window, which mustn't
  // shorten the settle window), the Shelly follows (a matching edge, too early to end it), and while the window is
  // still open the controller chattering is never a command, although the house is armed by then.
  World w;
  w.commission();
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.house.reset(PM_RCAUSE_EXT);
  Found boot = waitFor(w, w.house, "boot", h0, 2000);
  CHECK(boot.ok);
  CHECK_EQ(boot.e.a, PM_RCAUSE_EXT);
  uint32_t B = w.house.busyUntil;  // houseBegin(): the last thing setup does
  CHECK(!w.alarmSwitch());         // K1 dropped with the reset and the Shelly followed
  Found gs = waitFor(w, w.house, "gate_state", h0, 10000, GS_OPEN_);
  CHECK(gs.ok);
  CHECK(w.house.coil(1));
  Found sy = waitFor(w, w.house, "sync", h0, 500, 1);
  CHECK(sy.ok);
  CHECK_IN(since(sy.e.at, gs.e.at), 80, 82);
  CHECK(since(gs.e.t + SYNC_MS + 100, B) < (int32_t)WINDOW_MS);
  runTo(w, gs.e.at + SYNC_MS + 100);  // K1's own window would be over
  CHECK(syncWindow(w));
  CHECK(w.house.status()["armed"] == true);

  runTo(w, B + 8000);
  size_t h1 = w.house.logs.size();
  w.user(false);
  w.run(200);
  w.user(true);
  w.run(200);
  w.user(false);
  w.run(200);
  w.user(true);
  w.run(200);
  CHECK_EQ(countSince(w.house, "sync", h1), 4);
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  runTo(w, B + WINDOW_MS - 5);
  CHECK(syncWindow(w));
  runTo(w, B + WINDOW_MS + 5);
  CHECK(!syncWindow(w));
  w.run(2000);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);  // ended in step
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.alarmSwitch());
  CHECK(w.opener.atOpen());
}

// [settle-window]
TEST(house_power_settle_window_not_shortened_by_k1_change) {
  // ctrl_settle_ms 20000. The AES closes the gate during the window: K1 drops (a 3 s window of its own, ending long
  // before) and the Shelly follows (a matching edge before ctrl_settle_ms): the window still lasts 23 s.
  World w;
  w.commission([](Board &b) { houseParam(b, "ctrl_settle_ms", 20000); });
  w.house.lipo = true;
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t g0 = w.gate.logs.size();
  Found up = cutRail(w, 3000);
  uint32_t P = up.e.t;
  runTo(w, P + 100);
  size_t h1 = w.house.logs.size();
  w.extPress(false, 300);
  Found gs = waitFor(w, w.house, "gate_state", h1, 15000, GS_CLOSED_);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.b, CAUSE_EXTERNAL_);
  CHECK(!w.house.coil(1));
  Found sy = waitFor(w, w.house, "sync", h1 + 0, 500, 0);
  CHECK(sy.ok);
  CHECK(since(sy.e.t, P) < 20000);
  CHECK(since(gs.e.t + SYNC_MS, P) < (int32_t)(20000 + SYNC_MS - 1000));  // K1's own window ends well before
  runTo(w, gs.e.at + SYNC_MS + 100);
  CHECK(syncWindow(w));
  runTo(w, P + 20000 + SYNC_MS - 5);
  CHECK(syncWindow(w));
  runTo(w, P + 20000 + SYNC_MS + 5);
  CHECK(!syncWindow(w));
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(!w.alarmSwitch());
  CHECK(w.sensorClosed());
}

// [settle-window]
TEST(house_power_settle_window_extended_by_late_k1_change) {
  // With the Shelly's SW input detached (no edges of its own), a K1 change near the end of the settle window extends
  // it to that change's own sync_window_ms: the window lasts as long as the later of the two.
  World w;
  w.commission();
  w.house.lipo = true;
  w.shelly.mode = Shelly::DETACHED;
  size_t g0 = w.gate.logs.size();
  Found up = cutRail(w, 3000);
  uint32_t P = up.e.t;
  runTo(w, P + 3500);
  size_t h1 = w.house.logs.size();
  w.extPress(true, 300);
  Found gs = waitFor(w, w.house, "gate_state", h1, 15000, GS_OPEN_);
  CHECK(gs.ok);
  CHECK(w.house.coil(1));
  uint32_t K = gs.e.t;
  CHECK(since(K, P) > (int32_t)SETTLE_MS);
  CHECK(since(K + SYNC_MS, P) > (int32_t)WINDOW_MS + 300);
  runTo(w, P + WINDOW_MS + 100);
  CHECK(syncWindow(w));
  runTo(w, K + SYNC_MS - 1);
  CHECK(syncWindow(w));
  CHECK(!find(w.house, "resync", h1).ok);
  w.run(1);
  CHECK(!syncWindow(w));
  // The controller (detached) is out of step: the house resyncs it the moment the window is over, not before.
  Found rs = waitFor(w, w.house, "resync", h1, 100);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK_EQ(since(rs.e.t, K), (int32_t)SYNC_MS + 1);
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h1), 0);
  CHECK_EQ(pulses(w, g0), 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// [unknown-shows-open]

// [unknown-shows-open] [sensor-closed-only-known]
TEST(house_power_gate_no_power_shows_not_closed) {
  // The opener dead (no AC, battery flat): no limit reads and the gate reports no_power. The house shows not-closed
  // (K1 on, the controller following; sensor open) and commands nothing, however long. The user's CLOSE meanwhile is
  // refused at the gate, and the house puts the controller back to not-closed. Power back: closed again.
  World w;
  w.commission();
  CHECK(w.sensorClosed());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.ac = false;
  w.opener.battery = false;
  Found gs = waitFor(w, w.house, "gate_state", h0, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_NO_POWER_);
  CHECK_EQ(gs.e.b, CAUSE_NONE_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  Found sy = waitFor(w, w.house, "sync", h0, 500, 1);
  CHECK(sy.ok);
  CHECK(w.alarmSwitch());
  JsonDocument s = w.house.status();
  CHECK_STR(str(s["gate"]), "no_power");
  CHECK(s["remote"]["ac_power"] == false);
  CHECK(s["remote"]["open_limit"] == false);
  CHECK(s["remote"]["close_limit"] == false);
  uint32_t bad = watch(w, MISMATCH_MS + 15000, [&] { return w.house.coil(1) && !w.sensorClosed() && w.alarmSwitch(); });
  CHECK_EQ(bad, 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);

  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.user(false);
  Found cmd = waitFor(w, w.house, "cmd_sent", h1, 1000, ACT_CLOSE_);
  CHECK(cmd.ok);
  Found rf = waitFor(w, w.gate, "cmd_refused", g1, 3000, ACT_CLOSE_, cmd.e.b);
  CHECK(rf.ok);
  Found rs = waitFor(w, w.house, "resync", h1, 5000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK(w.runUntil([&] { return w.alarmSwitch() && w.house.coil(1); }, 2000));
  CHECK(!w.sensorClosed());
  CHECK_EQ(pulses(w, g0), 0);

  size_t h2 = w.house.logs.size();
  w.run(3000);
  w.opener.ac = true;
  w.opener.battery = true;
  gs = waitFor(w, w.house, "gate_state", h2, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_CLOSED_);
  CHECK_EQ(gs.e.b, CAUSE_NONE_);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 500));
  CHECK_EQ(countSince(w.house, "cmd_sent", h2), 0);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK_EQ(presses(w), 0);
}

// [unknown-shows-open]
TEST(house_power_gate_no_power_from_open_keeps_not_closed) {
  // The gate open, then the opener dead: no limit reads, no_power. K1 stays on (no K1 change, so no sync edge), the
  // sensor open, the controller on, and nothing is commanded or resynced however long. Power back: open again.
  World w;
  w.commission();
  openByUser(w);
  int cmds = w.house.count("cmd_sent");
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.ac = false;
  w.opener.battery = false;
  Found gs = waitFor(w, w.house, "gate_state", h0, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_NO_POWER_);
  CHECK_EQ(gs.e.b, CAUSE_NONE_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  uint32_t bad = watch(w, MISMATCH_MS + 15000, [&] { return w.house.coil(1) && !w.sensorClosed() && w.alarmSwitch(); });
  CHECK_EQ(bad, 0);
  CHECK_STR(str(w.house.status()["gate"]), "no_power");
  CHECK_EQ(countSince(w.house, "sync", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  size_t h1 = w.house.logs.size();
  w.opener.ac = true;
  w.opener.battery = true;
  gs = waitFor(w, w.house, "gate_state", h1, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_OPEN_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  w.run(3000);
  CHECK_EQ(w.house.count("cmd_sent"), cmds);
  CHECK_EQ(pulses(w, g0), 0);
  CHECK(w.opener.atOpen());
}

// [unknown-shows-open] [sensor-closed-only-known]
TEST(house_power_gate_fault_shows_not_closed) {
  // Both limits reading (a stuck limit relay, a wiring fault): fault. From closed the house turns K1 on and opens the
  // sensor; from open it keeps K1 on. Nothing is commanded; once one limit reads alone the house follows it again.
  World w;
  w.commission();
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.force[0] = 1;  // the open limit reads too
  Found gs = waitFor(w, w.house, "gate_state", h0, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_FAULT_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK_STR(str(w.house.status()["gate"]), "fault");
  uint32_t bad = watch(w, 30000, [&] { return w.house.coil(1) && !w.sensorClosed(); });
  CHECK_EQ(bad, 0);
  CHECK(w.alarmSwitch());
  size_t h1 = w.house.logs.size();
  w.opener.force[0] = -1;
  gs = waitFor(w, w.house, "gate_state", h1, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_CLOSED_);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  syncWindowOver(w);
  CHECK(!w.alarmSwitch());
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);

  openByUser(w);
  size_t h2 = w.house.logs.size(), g2 = w.gate.logs.size();
  w.opener.force[1] = 1;  // the closed limit reads too
  gs = waitFor(w, w.house, "gate_state", h2, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_FAULT_);
  bad = watch(w, 20000, [&] { return w.house.coil(1) && !w.sensorClosed() && w.alarmSwitch(); });
  CHECK_EQ(bad, 0);
  w.opener.force[1] = -1;
  gs = waitFor(w, w.house, "gate_state", h2 + 1, 5000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_OPEN_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK_EQ(countSince(w.house, "cmd_sent", h2), 0);
  CHECK_EQ(countSince(w.house, "sync", h2), 0);  // K1 never moved
  CHECK_EQ(pulses(w, g2), 0);
}

// [unknown-shows-open] [sensor-closed-only-known]
TEST(house_power_no_ac_with_limit_follows_limit) {
  // Mains off, the opener on its battery: a limit that reads is trusted, so the house shows closed (sensor closed)
  // and later open as usual. Between the limits nothing reads and that is no_power, shown not-closed at once (not held
  // at the limit the gate left, as a `between` would be).
  World w;
  w.commission();
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.ac = false;
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == false; }, 3000));
  JsonDocument s = w.house.status();
  CHECK_STR(str(s["gate"]), "closed");
  CHECK(s["remote"]["close_limit"] == true);
  uint32_t bad = watch(w, 10000, [&] { return w.sensorClosed() && !w.house.coil(1); });
  CHECK_EQ(bad, 0);
  CHECK(!find(w.house, "gate_state", h0).ok);

  w.extPress(true, 300);  // the local button, on battery
  Found np = waitFor(w, w.house, "gate_state", h0, 5000);
  CHECK(np.ok);
  CHECK_EQ(np.e.a, GS_NO_POWER_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(w.opener.pos > 0 && w.opener.pos < (int32_t)w.opener.travelMs - 1000);  // mid travel
  size_t h1 = w.house.logs.size();
  Found op = waitFor(w, w.house, "gate_state", h1, 15000);
  CHECK(op.ok);
  CHECK_EQ(op.e.a, GS_OPEN_);
  CHECK_EQ(op.e.b, CAUSE_NONE_);  // out of no_power: no cause
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());

  w.run(5000);
  size_t h2 = w.house.logs.size();
  w.extPress(false, 300);
  np = waitFor(w, w.house, "gate_state", h2, 5000);
  CHECK(np.ok);
  CHECK_EQ(np.e.a, GS_NO_POWER_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  Found cl = waitFor(w, w.house, "gate_state", h2 + 1, 15000);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.a, GS_CLOSED_);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
}

// [unknown-shows-open] [sensor-closed-only-known]
TEST(house_power_between_after_no_power_shows_not_closed) {
  // The gate closed, then the opener dead (no_power) and the gate pushed half open by hand meanwhile. Power back, it
  // reads between: the house has no limit to hold (the closed one it knew before no_power says nothing now), so it
  // keeps showing not-closed instead of the closed level a travel from the closed limit would hold.
  World w;
  w.commission();
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.opener.ac = false;
  w.opener.battery = false;
  CHECK(waitFor(w, w.house, "gate_state", h0, 5000, GS_NO_POWER_).ok);
  syncWindowOver(w);
  w.opener.pos = (int32_t)w.opener.travelMs / 2;  // pushed by hand
  size_t h1 = w.house.logs.size();
  w.opener.ac = true;
  w.opener.battery = true;
  Found gs = waitFor(w, w.house, "gate_state", h1, 8000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_BETWEEN_);
  CHECK_EQ(gs.e.b, CAUSE_NONE_);
  uint32_t bad = watch(w, 30000, [&] { return w.house.coil(1) && !w.sensorClosed() && w.alarmSwitch(); });
  CHECK_EQ(bad, 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(pulses(w, g0), 0);
  // The user closes it from there: a command, and the house follows the gate to closed.
  size_t h2 = w.house.logs.size();
  w.user(false);
  CHECK(waitFor(w, w.house, "cmd_sent", h2, 1000, ACT_CLOSE_).ok);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
  CHECK(!w.house.coil(1));
}

// ---------------------------------------------------------------------------------------------------------------------
// [sensor-closed-only-known]

// Gate closed, every frame lost from now on: the contact sensor stays closed until exactly `timeoutMs` after the last
// frame the house heard from the gate, opens on the link_down pass (K1 untouched), and closes again when a frame gets
// through. `stillClosedAt` (ms after that frame) is checked to still show closed.
namespace {
void linkLossFailsOpen(World &w, uint32_t timeoutMs, uint32_t stillClosedAt) {
  CHECK(w.sensorClosed());
  CHECK_EQ((uint32_t)(w.house.status()["link_timeout_eff_s"] | 0), timeoutMs / 1000);
  size_t h0 = w.house.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  w.run(1000);  // frames already on the air land
  int32_t age = w.house.status()["link"]["age_ms"] | -1;
  CHECK(age >= 0);
  uint32_t last = w.now - (uint32_t)age;
  runTo(w, last + stillClosedAt);
  CHECK(w.sensorClosed());
  runTo(w, last + timeoutMs - 2);
  CHECK(w.sensorClosed());
  CHECK(w.houseLinkUp());
  CHECK(w.runUntil([&] { return !w.sensorClosed(); }, 100));
  CHECK_IN(since(w.now, last), timeoutMs - 1, timeoutMs + 1);
  Found dn = find(w.house, "link_down", h0);
  CHECK(dn.ok);
  CHECK_EQ(dn.e.at, w.now);
  CHECK(!w.house.coil(1));  // only the sensor fails open
  CHECK_STR(str(w.house.status()["gate"]), "closed");
  uint32_t bad = watch(w, 10000, [&] { return !w.sensorClosed(); });
  CHECK_EQ(bad, 0);

  size_t h1 = w.house.logs.size();
  w.drop = nullptr;
  Found lu = waitFor(w, w.house, "link_up", h1, 90000);
  CHECK(lu.ok);
  CHECK(w.sensorClosed());  // the same pass
}
}  // namespace

// [sensor-closed-only-known]
TEST(house_power_sensor_fails_open_after_link_timeout) {
  // Defaults: link_timeout_s 100 beats 2.5 x the gate's 30 s heartbeat.
  World w;
  w.commission();
  linkLossFailsOpen(w, 100000, 75000 + 1000);
}

// [sensor-closed-only-known]
TEST(house_power_sensor_fails_open_after_2_5_heartbeats) {
  // The gate's heartbeat_s 60: 2.5 x 60 s beats link_timeout_s 100; the sensor still shows closed past 100 s.
  World w;
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("heartbeat_s", 60));
  });
  linkLossFailsOpen(w, 150000, 100000 + 1000);
}

// [sensor-closed-only-known]
TEST(house_power_sensor_linkloss_open_0_keeps_last_state) {
  // linkloss_open 0: the link goes down and the sensor keeps the last state the house knew (closed), even while the
  // gate is opened behind its back; once a frame gets through, it follows the gate again.
  World w;
  w.commission([](Board &b) { houseParam(b, "linkloss_open", 0); });
  CHECK(w.sensorClosed());
  size_t h0 = w.house.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  Found dn = waitFor(w, w.house, "link_down", h0, 110000);
  CHECK(dn.ok);
  CHECK(w.sensorClosed());
  CHECK(w.house.status()["link_up"] == false);
  uint32_t bad = watch(w, 20000, [&] { return w.sensorClosed(); });
  CHECK_EQ(bad, 0);

  w.limits.sensorTruth = false;  // its lag limit assumes linkloss_open 1: here lagging is the setting's point
  w.extPress(true, 300);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  bad = watch(w, 20000, [&] { return w.sensorClosed(); });
  CHECK_EQ(bad, 0);
  size_t h1 = w.house.logs.size();
  w.drop = nullptr;
  Found gs = waitFor(w, w.house, "gate_state", h1, 90000);
  CHECK(gs.ok);
  CHECK_EQ(gs.e.a, GS_OPEN_);
  CHECK(!w.sensorClosed());  // the same pass
  CHECK(find(w.house, "link_up", h1).ok);
  w.limits.sensorTruth = true;
  w.run(5000);
}

// [sensor-closed-only-known]
TEST(house_power_sensor_closed_exactly_while_reported_closed) {
  // Every millisecond of an open/close cycle: the sensor reads closed exactly when the house's last report says
  // closed and the link is up. It opens on the report of the gate leaving (before the gate is open), and closes only on
  // the report of it closed (not on the command, nor while it travels).
  World w;
  w.commission();
  auto consistent = [&] { return w.sensorClosed() == (w.houseSees() == GS_CLOSED_ && w.houseLinkUp()); };
  CHECK(consistent());
  CHECK(w.sensorClosed());
  uint32_t leftAt = 0, k2OffAt = 0, bad = 0;
  w.user(true);
  CHECK(w.runUntil([&] {
    if (!bad && !consistent()) bad = w.now;
    if (!leftAt && !w.opener.atClosed()) leftAt = w.now;
    if (!k2OffAt && !w.sensorClosed()) k2OffAt = w.now;
    return w.opener.atOpen() && w.houseSees() == GS_OPEN_;
  }, 20000));
  CHECK_EQ(bad, 0);
  CHECK(leftAt && k2OffAt);
  CHECK_IN(since(k2OffAt, leftAt), 50, 1500);  // the gate reports `between` after BETWEEN_HOLD_MS
  syncWindowOver(w);

  uint32_t k2OnAt = 0, closedAt = 0;
  w.user(false);
  CHECK(w.runUntil([&] {
    if (!bad && !consistent()) bad = w.now;
    if (!closedAt && w.opener.atClosed()) closedAt = w.now;
    if (!k2OnAt && w.sensorClosed()) k2OnAt = w.now;
    return k2OnAt != 0;
  }, 20000));
  CHECK_EQ(bad, 0);
  CHECK(closedAt);
  CHECK_IN(since(k2OnAt, closedAt), 50, 1500);
  w.run(2000);
  CHECK(consistent());
}

// [sensor-closed-only-known]
TEST(house_power_sensor_open_until_first_status_after_boot) {
  // After a house reboot nothing is known until the gate's first STATUS: with every frame lost the sensor stays open
  // (and K1 isn't driven), however long; the first STATUS that gets through closes it, on that pass.
  World w;
  w.commission();
  CHECK(w.sensorClosed());
  size_t h0 = w.house.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  w.house.reset(PM_RCAUSE_EXT);
  CHECK(w.runUntil([&] { return w.house.running(); }, 1000));
  uint32_t bad = watch(w, 60000, [&] { return !w.sensorClosed() && !w.house.coil(1); });
  CHECK_EQ(bad, 0);
  JsonDocument s = w.house.status();
  CHECK_STR(str(s["gate"]), "unknown");
  CHECK(s["link_up"] == false);
  CHECK_EQ(w.houseSees(), GS_UNKNOWN_);
  w.drop = nullptr;
  Found gs;
  uint32_t early = 0;
  CHECK(w.runUntil([&] {
    gs = find(w.house, "gate_state", h0);
    if (!gs.ok && w.sensorClosed() && !early) early = w.now;
    return gs.ok;
  }, 60000));
  CHECK_EQ(early, 0);
  CHECK_EQ(gs.e.a, GS_CLOSED_);
  CHECK(w.sensorClosed());
}

// [sensor-closed-only-known]
TEST(house_power_sensor_linkloss_open_0_unknown_after_boot_stays_open) {
  // linkloss_open 0 keeps the last state the house knew, but a rebooted house knows nothing: with every frame lost the
  // sensor stays open (the gate unknown, not closed), and closes on the first STATUS that gets through.
  World w;
  w.commission([](Board &b) { houseParam(b, "linkloss_open", 0); });
  CHECK(w.sensorClosed());
  size_t h0 = w.house.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  w.house.reset(PM_RCAUSE_WDT);
  CHECK(w.runUntil([&] { return w.house.running(); }, 1000));
  CHECK_EQ(w.house.get("linkloss_open"), 0);
  uint32_t bad = watch(w, 40000, [&] { return !w.sensorClosed(); });
  CHECK_EQ(bad, 0);
  CHECK_STR(str(w.house.status()["gate"]), "unknown");
  w.drop = nullptr;
  Found gs;
  uint32_t early = 0;
  CHECK(w.runUntil([&] {
    gs = find(w.house, "gate_state", h0);
    if (!gs.ok && w.sensorClosed() && !early) early = w.now;
    return gs.ok;
  }, 60000));
  CHECK_EQ(early, 0);
  CHECK_EQ(gs.e.a, GS_CLOSED_);
  CHECK(w.sensorClosed());
}
