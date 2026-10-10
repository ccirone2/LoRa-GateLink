// House controller sync (CLAUDE.md "Behavioural invariants"): K1 mirrors the real gate into the Shelly (the Alarm.com
// switch), holds the limit the gate left while it travels, opens a sync window on every change so the Shelly's own
// edges never become commands, resyncs a Shelly left out of step, and never commands the gate from the Shelly's level
// at boot. Each test drives the simulated site (world.h): both boards' firmware, the opener, the Shelly and the radio.
//
// Timings below are exact where the firmware makes them so. The house's loop runs every simulated millisecond, and a
// K1 change happens on the very pass the house handles the STATUS that causes it (the gate_state log of that pass).
// The Shelly acts on its SW input 30 ms after K1 changes (from the next world step), and the house debounces IN1 for
// debounce_ms (50) from the pass that first sees it: a sync edge follows K1 by 81 ms. A user action at world time X is
// seen at X + 1 and debounced at X + 51.
#include <Arduino.h>
#include <limits.h>
#include <functional>
#include "world.h"

namespace {

enum { CAUSE_NONE_ = 0, CAUSE_LORA_ = 1, CAUSE_EXTERNAL_ = 2 };  // roles.h Cause
enum { ACT_OPEN_ = 1, ACT_CLOSE_ = 2 };                          // roles.h Action
enum : uint8_t { MSG_ACK_ = 3, MSG_CMD_ = 4, MSG_STATUS_ = 5 };  // link.h MsgType
enum { RES_NO_POWER_ = 4 };                                      // link.h AckResult
const int ANY = INT_MIN;
const uint32_t SYNC_MS = 3000;  // sync_window_ms default
const uint32_t SYNC_EDGE = 81;  // K1 change -> the Shelly's relay -> the house's debounced IN1 edge

// The site (a monitor breach fails the test through world.cpp's after-test hook).
typedef World Site;

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

// The world time the house's K1 first reads `on` (0 = not within maxMs).
uint32_t k1At(World &w, bool on, uint32_t maxMs) {
  uint32_t at = 0;
  w.runUntil([&] {
    if (w.house.coil(1) != on) return false;
    at = w.now;
    return true;
  }, maxMs);
  return at;
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

// When the first frame of `type` that b put on the air at or after world time `at` ends (0 = none yet). A copy, not a
// pointer: w.air reallocates as frames are added.
uint32_t sentEnd(const World &w, const Board &b, uint8_t type, uint32_t at) {
  for (const AirFrame &f : w.air)
    if (f.from == b.idx && f.type() == type && (int32_t)(f.start - at) >= 0) return f.end;
  return 0;
}

// The world time the house handled a STATUS reporting last_result `result` (its {"event":"status"}, emitted by
// handleStatus), scanning its events from index *from on; 0 = not yet.
uint32_t statusSeen(World &w, size_t *from, const char *result) {
  for (; *from < w.house.events.size(); (*from)++) {
    JsonDocument &d = w.house.events[*from];
    if (d["event"] == "status" && d["status"]["last_result"] == result) {
      (*from)++;
      return w.now;
    }
  }
  return 0;
}

// The user opens the gate from Alarm.com and it gets there; returns the world time the house heard open (and turned
// K1 on, the same pass).
uint32_t openByUser(World &w) {
  size_t h0 = w.house.logs.size();
  w.user(true);
  Found op = waitFor(w, w.house, "gate_state", h0, 15000, GS_OPEN_);
  CHECK(op.ok);
  CHECK(w.house.coil(1));
  return op.e.at;
}

// Someone else (AES, local button) opens the gate; returns the world time the house heard open.
uint32_t openByOther(World &w) {
  size_t h0 = w.house.logs.size();
  w.extPress(true, 300);
  Found op = waitFor(w, w.house, "gate_state", h0, 15000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_EXTERNAL_);
  CHECK(w.house.coil(1));
  return op.e.at;
}

void configureHouse(Board &b, const char *param, int32_t v) {
  if (b.idx == 0) CHECK(b.set(param, v));
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// [k1-mirror]

// [k1-mirror] [sync-window]
TEST(house_sync_k1_mirrors_moves_by_other_controllers) {
  // The AES, a local button and the siren sensor move the gate behind our back: K1 follows the limits, the Shelly
  // follows K1 (its SW input) and that edge is logged as a sync. Nothing is commanded, nothing is pulsed.
  Site w;
  w.commission();
  CHECK(!w.house.coil(1));
  CHECK(!w.alarmSwitch());
  CHECK(w.sensorClosed());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();

  // AES opens it.
  uint32_t t = openByOther(w);
  Found sy = waitFor(w, w.house, "sync", h0, 500);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 1);
  CHECK_IN(sy.e.at - t, SYNC_EDGE - 1, SYNC_EDGE + 1);
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  w.run(4000);

  // Local button closes it: K1 releases on the pass the house hears closed.
  size_t h1 = w.house.logs.size();
  w.extPress(false, 300);
  Found cl = waitFor(w, w.house, "gate_state", h1, 15000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_EQ(cl.e.b, CAUSE_EXTERNAL_);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  Found sy2 = waitFor(w, w.house, "sync", h1, 500);
  CHECK(sy2.ok);
  CHECK_EQ(sy2.e.a, 0);
  CHECK_IN(sy2.e.at - cl.e.at, SYNC_EDGE - 1, SYNC_EDGE + 1);
  CHECK(!w.alarmSwitch());
  w.run(4000);

  // The siren sensor holds OPEN: open, and it stays open after the siren lets go.
  size_t h2 = w.house.logs.size();
  w.opener.extOpenHold = true;
  Found op3 = waitFor(w, w.house, "gate_state", h2, 15000, GS_OPEN_);
  CHECK(op3.ok);
  CHECK(w.house.coil(1));
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 200));
  w.opener.extOpenHold = false;
  w.run(5000);
  CHECK(w.house.coil(1));
  CHECK(w.alarmSwitch());

  CHECK_EQ(countSince(w.house, "sync", h0), 3);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_suppressed", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
}

// [k1-mirror]
TEST(house_sync_k1_energized_whenever_the_gate_is_not_closed) {
  // Energized = not closed: a fault (both limits) and no_power (the opener dead, no limit reading) show not closed too,
  // and the Shelly follows; back at the closed limit both go off. Each change happens on the pass the house hears it.
  Site w;
  w.commission();
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();

  w.opener.force[0] = 1;  // the open limit reads as well: fault
  Found f = waitFor(w, w.house, "gate_state", h0, 3000);
  CHECK(f.ok);
  CHECK_EQ(f.e.a, GS_FAULT_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, SYNC_EDGE));
  w.run(4000);
  size_t h1 = w.house.logs.size();
  w.opener.force[0] = -1;
  Found c = waitFor(w, w.house, "gate_state", h1, 3000);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, GS_CLOSED_);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, SYNC_EDGE));
  w.run(4000);

  size_t h2 = w.house.logs.size();
  w.opener.ac = false;
  w.opener.battery = false;  // dead: its limit relays drop with IN3
  Found np = waitFor(w, w.house, "gate_state", h2, 3000);
  CHECK(np.ok);
  CHECK_EQ(np.e.a, GS_NO_POWER_);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, SYNC_EDGE));
  w.run(4000);
  size_t h3 = w.house.logs.size();
  w.opener.ac = true;
  w.opener.battery = true;
  Found c2 = waitFor(w, w.house, "gate_state", h3, 3000);
  CHECK(c2.ok);
  CHECK_EQ(c2.e.a, GS_CLOSED_);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, SYNC_EDGE));
  w.run(2000);

  CHECK_EQ(countSince(w.house, "sync", h0), 4);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
}

// [k1-mirror] [travel-hold]
TEST(house_sync_between_after_no_power_or_fault_holds_no_limit) {
  // The gate is closed, then its position can't be known: no_power (the opener dead) or a fault (both limits read),
  // and meanwhile it is moved off the closed limit (by hand, or by another controller). When it then reads between, the
  // closed limit it was at before is not a limit it was seen to leave: there is nothing to hold, so K1 stays energized
  // (not closed) from the pass the house hears between, and the Shelly stays ON, for the whole of what would otherwise
  // be a travel hold. Back at the closed limit both go off.
  for (int fault = 0; fault < 2; fault++) {
    Site w;
    w.commission();
    CHECK(!w.house.coil(1));
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    if (fault) {
      w.opener.force[0] = w.opener.force[1] = 1;  // both limits read, wherever the gate is
    } else {
      w.opener.ac = false;
      w.opener.battery = false;  // dead: no limit reads
    }
    Found lost = waitFor(w, w.house, "gate_state", h0, 5000);
    CHECK(lost.ok);
    CHECK_EQ(lost.e.a, fault ? GS_FAULT_ : GS_NO_POWER_);
    CHECK(w.house.coil(1));
    CHECK(w.runUntil([&] { return w.alarmSwitch(); }, SYNC_EDGE));
    w.run(4000);

    size_t h1 = w.house.logs.size();
    if (fault) {
      w.opener.stuck = true;  // another controller opens it; it jams just off the closed limit
      w.extPress(true, 300);
      w.run(2000);
      CHECK_EQ(w.opener.pos, 1);
      CHECK_EQ(countSince(w.house, "gate_state", h1), 0);  // still a fault
      w.opener.force[0] = w.opener.force[1] = -1;  // the limits read true again: neither
    } else {
      w.opener.pos = 3000;  // pushed partly open by hand while dead
      w.opener.ac = true;
      w.opener.battery = true;
    }
    Found btw = waitFor(w, w.house, "gate_state", h1, 10000);
    CHECK(btw.ok);
    CHECK_EQ(btw.e.a, GS_BETWEEN_);
    CHECK_EQ(btw.e.b, fault ? CAUSE_EXTERNAL_ : CAUSE_NONE_);
    CHECK(!w.opener.atOpen() && !w.opener.atClosed());
    CHECK(w.house.coil(1));  // on the pass it hears between: no hold of the closed limit
    // Past the 20 s check: the default travel_timeout_s (60) would end any hold only well after it.
    CHECK_EQ(watch(w, 20000, [&] { return w.house.coil(1) && w.alarmSwitch() && !w.sensorClosed(); }), 0);
    JsonDocument s = w.house.status();
    CHECK_STR(str(s, "gate"), "between");
    CHECK_EQ(s["remote"]["travel_timeout_s"].as<int>(), 60);
    CHECK_EQ(countSince(w.house, "gate_state", h1), 1);
    CHECK_EQ(countSince(w.house, "sync", h1), 0);  // the Shelly never moved

    // Closed again by another controller.
    size_t h2 = w.house.logs.size();
    w.opener.stuck = false;
    w.extPress(false, 300);
    Found cl = waitFor(w, w.house, "gate_state", h2, 10000, GS_CLOSED_);
    CHECK(cl.ok);
    CHECK(!w.house.coil(1));
    CHECK(w.sensorClosed());
    CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, SYNC_EDGE));
    w.run(3000);
    CHECK(w.opener.atClosed());
    CHECK_EQ(countSince(w.house, "sync", h0), 2);
    CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.house, "resync", h0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// [travel-hold]

// [travel-hold] [k1-mirror]
TEST(house_sync_travel_hold_keeps_the_limit_it_left_until_the_far_one) {
  // While the gate travels K1 (and so the Shelly) keeps showing the limit it left, though the contact sensor opens at
  // once; it changes on the pass the house hears the far limit. A travel that turns back to the limit it left never
  // moves the Shelly at all.
  Site w;
  w.commission();
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();

  // Opening: K1 off (closed) the whole way.
  w.extPress(true, 300);
  Found btw = waitFor(w, w.house, "gate_state", h0, 3000);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.a, GS_BETWEEN_);
  CHECK(!w.house.coil(1));
  CHECK(!w.sensorClosed());
  uint32_t bad = 0;
  CHECK(w.runUntil([&] {
    if (w.houseSees() == GS_OPEN_) return true;
    if (!bad && (w.house.coil(1) || w.alarmSwitch() || w.sensorClosed())) bad = w.now;
    return false;
  }, 15000));
  CHECK_EQ(bad, 0);
  Found op = find(w.house, "gate_state", h0, GS_OPEN_);
  CHECK_EQ(op.e.at, w.now);
  CHECK_IN(op.e.at - btw.e.at, 7000, 8000);  // the hold lasted the travel (8 s, less the between hold)
  CHECK(w.house.coil(1));
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, SYNC_EDGE));
  w.run(4000);

  // Closing: K1 on (open) the whole way, the contact sensor open until closed.
  size_t h1 = w.house.logs.size();
  w.extPress(false, 300);
  Found btw2 = waitFor(w, w.house, "gate_state", h1, 3000);
  CHECK(btw2.ok);
  CHECK_EQ(btw2.e.a, GS_BETWEEN_);
  bad = 0;
  CHECK(w.runUntil([&] {
    if (w.houseSees() == GS_CLOSED_) return true;
    if (!bad && (!w.house.coil(1) || !w.alarmSwitch() || w.sensorClosed())) bad = w.now;
    return false;
  }, 15000));
  CHECK_EQ(bad, 0);
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, SYNC_EDGE));
  w.run(4000);

  // Opening, then closed again mid-travel: K1 and the Shelly never move.
  size_t h2 = w.house.logs.size();
  w.extPress(true, 300);
  CHECK(waitFor(w, w.house, "gate_state", h2, 3000, GS_BETWEEN_).ok);
  w.run(2000);
  w.extPress(false, 300);
  bad = 0;
  CHECK(w.runUntil([&] {
    if (w.houseSees() == GS_CLOSED_) return true;
    if (!bad && (w.house.coil(1) || w.alarmSwitch())) bad = w.now;
    return false;
  }, 15000));
  CHECK_EQ(bad, 0);
  w.run(3000);
  CHECK(!w.house.coil(1));
  CHECK(!w.alarmSwitch());
  CHECK(w.sensorClosed());
  CHECK_EQ(countSince(w.house, "sync", h2), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h2), 2);

  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
}

// [travel-hold]
TEST(house_sync_travel_hold_lasts_the_gates_travel_timeout_not_the_houses) {
  // The gate jams just off its closed limit (someone else's move). K1 holds closed for exactly the gate's
  // travel_timeout_s (from its STATUS) after the house heard between: not the house's own, longer or shorter.
  struct Cfg {
    int32_t gate, house;
  } cfgs[] = { { 20, 5 }, { 10, 120 } };
  for (const Cfg &c : cfgs) {
    Site w;
    w.commission([&](Board &b) { CHECK(b.set("travel_timeout_s", b.idx == 1 ? c.gate : c.house)); });
    CHECK_EQ(w.house.get("travel_timeout_s"), c.house);
    CHECK_EQ(w.house.status()["remote"]["travel_timeout_s"].as<int>(), c.gate);
    w.opener.stuck = true;
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    w.extPress(true, 300);
    Found btw = waitFor(w, w.house, "gate_state", h0, 3000);
    CHECK(btw.ok);
    CHECK_EQ(btw.e.a, GS_BETWEEN_);
    CHECK(w.opener.pos == 1 && w.opener.dir == 0);
    uint32_t hold = (uint32_t)c.gate * 1000;
    CHECK_EQ(watch(w, btw.e.at + hold - 1 - w.now, [&] { return !w.house.coil(1) && !w.alarmSwitch(); }), 0);
    CHECK(!w.sensorClosed());
    uint32_t on = k1At(w, true, 10);
    CHECK(on != 0);
    CHECK_EQ(on - btw.e.at, hold);
    Found sy = waitFor(w, w.house, "sync", h0, 200, 1);
    CHECK(sy.ok);
    CHECK_IN(sy.e.at - on, SYNC_EDGE - 1, SYNC_EDGE + 1);
    CHECK(w.alarmSwitch());
    w.run(5000);
    CHECK(w.house.coil(1));
    CHECK_STR(str(w.house.status(), "gate"), "between");
    CHECK_EQ(countSince(w.house, "gate_state", h0), 1);
    CHECK_EQ(countSince(w.house, "resync", h0), 0);
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  }
}

// [travel-hold] [resync]
TEST(house_sync_no_resync_while_holding_through_a_travel) {
  // The user opens and the gate jams just off its closed limit. The Shelly shows the user's ON, K1 holds closed (OFF):
  // out of step for the whole 30 s hold, three times mismatch_timeout_s, and no resync, since mid-travel the Shelly
  // may already show where the gate is going. After the hold K1 shows not closed, which agrees.
  Site w;
  w.commission([](Board &b) {
    if (b.idx == 0) CHECK(b.set("mismatch_timeout_s", 10));
    else CHECK(b.set("travel_timeout_s", 30));
  });
  w.opener.stuck = true;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.user(true);
  Found btw = waitFor(w, w.house, "gate_state", h0, 3000, GS_BETWEEN_);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  CHECK(w.alarmSwitch());
  CHECK_EQ(watch(w, btw.e.at + 30000 - 1 - w.now, [&] { return !w.house.coil(1) && w.alarmSwitch(); }), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK(find(w.gate, "travel_timeout", g0, GS_OPEN_).ok);  // the gate gave up on its target meanwhile
  uint32_t on = k1At(w, true, 10);
  CHECK(on != 0);
  CHECK_EQ(on - btw.e.at, 30000);
  w.run(15000);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK(w.alarmSwitch());
  CHECK(w.house.coil(1));
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
}

// [travel-hold] [resync]
TEST(house_sync_mismatch_counts_from_the_end_of_the_hold) {
  // From open the user closes and the gate jams just off its open limit. K1 holds open (ON) through the 30 s hold while
  // the Shelly shows the user's OFF: no resync. From the hold's end K1 shows not closed (between), the Shelly still
  // OFF: a lingering mismatch, resynced mismatch_timeout_s (10) later, not before.
  Site w;
  w.commission([](Board &b) {
    if (b.idx == 0) CHECK(b.set("mismatch_timeout_s", 10));
    else CHECK(b.set("travel_timeout_s", 30));
  });
  openByUser(w);
  w.run(4000);
  w.opener.stuck = true;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.user(false);
  Found btw = waitFor(w, w.house, "gate_state", h0, 3000, GS_BETWEEN_);
  CHECK(btw.ok);
  CHECK_EQ(btw.e.b, CAUSE_LORA_);
  uint32_t H = btw.e.at + 30000;  // the hold's end
  CHECK_EQ(watch(w, H - 1 - w.now, [&] { return w.house.coil(1) && !w.alarmSwitch(); }), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(watch(w, H + 9999 - w.now, [&] { return w.house.coil(1) && !w.alarmSwitch(); }), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  Found rs = waitFor(w, w.house, "resync", h0, 10);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK_IN(rs.e.at - H, 10000, 10001);
  // The cycle: K1 to the Shelly's level (off) for resync_ms, then back on; the Shelly follows the second edge.
  CHECK(!w.house.coil(1));
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 1100));
  CHECK_IN(w.now - rs.e.at, 1030, 1032);
  Found sy = waitFor(w, w.house, "sync", h0, 200, 1);
  CHECK(sy.ok);
  CHECK_IN(sy.e.at - rs.e.at, 1000 + SYNC_EDGE - 1, 1000 + SYNC_EDGE + 1);
  CHECK(w.house.coil(1));
  w.run(12000);
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 1);  // the user's
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
}

// [travel-hold] [armed-after-status]
TEST(house_sync_house_back_mid_travel_shows_not_closed_at_once) {
  // The house restarts while the gate travels: it doesn't know the limit the gate left, so there's nothing to hold and
  // K1 shows not closed on the pass the first STATUS (between) arrives; the Shelly's edge is a sync, not a command.
  Site w;
  w.commission();
  w.opener.travelMs = 40000;  // a slow gate: still travelling when the house is back
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.extPress(true, 300);
  CHECK(waitFor(w, w.house, "gate_state", h0, 3000, GS_BETWEEN_).ok);
  w.run(1000);
  CHECK(!w.house.coil(1));  // holding closed
  size_t h1 = w.house.logs.size();
  w.house.reset(PM_RCAUSE_WDT);
  Found st = waitFor(w, w.house, "gate_state", h1, 15000);
  CHECK(st.ok);
  CHECK_EQ(st.e.a, GS_BETWEEN_);
  CHECK(!w.opener.atOpen());
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  Found sy = waitFor(w, w.house, "sync", h1, 500, 1);
  CHECK(sy.ok);
  CHECK_IN(sy.e.at - st.e.at, SYNC_EDGE - 1, SYNC_EDGE + 1);
  CHECK(w.alarmSwitch());
  Found op = waitFor(w, w.house, "gate_state", h1, 45000, GS_OPEN_);
  CHECK(op.ok);
  CHECK(w.house.coil(1));
  CHECK(w.alarmSwitch());
  w.run(3000);
  CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
}

// ---------------------------------------------------------------------------------------------------------------
// [sync-window]

// [sync-window] [resync]
TEST(house_sync_toggle_controller_never_turns_sync_into_commands) {
  // A Shelly misconfigured to toggle its relay on every SW edge. The user opens; at the open limit K1 goes on and the
  // Shelly flips OFF though it already showed ON. That edge, and every one the resyncs cause after it, falls inside a
  // sync window: never a CLOSE. (Resync can't fix a toggling Shelly: it keeps trying, harmlessly.)
  Site w;
  w.shelly.mode = Shelly::TOGGLE;
  w.commission([](Board &b) { configureHouse(b, "mismatch_timeout_s", 10); });
  CHECK(!w.alarmSwitch());
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t t = openByUser(w);
  Found sy = waitFor(w, w.house, "sync", h0, 300);
  CHECK(sy.ok);
  CHECK_EQ(sy.e.a, 0);
  CHECK_IN(sy.e.at - t, SYNC_EDGE - 1, SYNC_EDGE + 1);
  CHECK(!w.alarmSwitch());
  // Out of step from that edge: resync mismatch_timeout_s later.
  Found rs = waitFor(w, w.house, "resync", h0, 11000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK_IN(rs.e.at - sy.e.at, 10000, 10001);
  w.run(35000);
  int resyncs = countSince(w.house, "resync", h0);
  CHECK(resyncs >= 3);
  CHECK(countSince(w.house, "sync", h0) >= 1 + 2 * (resyncs - 1));
  CHECK_EQ(countSince(w.house, "ctrl", h0), 1);  // the user's edge only
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0, ACT_CLOSE_), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK(w.opener.presses[1].empty());
  CHECK(w.opener.atOpen());
  CHECK(w.house.coil(1) || w.house.status()["resyncing"] == true);
  CHECK(!w.sensorClosed());
}

// [sync-window]
TEST(house_sync_window_boundary_inside_is_sync_outside_is_a_command) {
  // The user opens; K1 goes on at the open limit with the Shelly already ON, so no edge ends the window early: it lasts
  // sync_window_ms. The user's OFF debounced on its last ms is a sync (lost, by design: the resync puts the Shelly
  // back); one ms later it is a CLOSE, sent after ctrl_confirm_ms.
  for (int late = 0; late < 2; late++) {
    Site w;
    w.commission([](Board &b) { configureHouse(b, "mismatch_timeout_s", 10); });
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    uint32_t t = openByUser(w);
    runTo(w, t + 2900);
    CHECK(w.house.status()["sync_window"] == true);
    CHECK_EQ(countSince(w.house, "sync", h0), 0);
    size_t h1 = w.house.logs.size();
    runTo(w, t + SYNC_MS - 52 + late);
    w.user(false);
    uint32_t edge = t + SYNC_MS - 1 + late;
    if (!late) {
      Found sy = waitFor(w, w.house, "sync", h1, 100);
      CHECK(sy.ok);
      CHECK_EQ(sy.e.a, 0);
      CHECK_EQ(sy.e.at, edge);
      w.run(2);
      CHECK(w.house.status()["sync_window"] == false);
      w.run(2000);
      CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
      CHECK_EQ(countSince(w.house, "cmd_sent", h1), 0);
      CHECK(w.opener.atOpen());
      // Put back: the Shelly is out of step from that edge, resynced mismatch_timeout_s later.
      Found rs = waitFor(w, w.house, "resync", h1, 10000);
      CHECK(rs.ok);
      CHECK_EQ(rs.e.a, 1);
      CHECK_IN(rs.e.at - edge, 10000, 10001);
      CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 1200));
      w.run(2000);
      CHECK(w.opener.atOpen());
      CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
    } else {
      Found c = waitFor(w, w.house, "ctrl", h1, 100);
      CHECK(c.ok);
      CHECK_EQ(c.e.a, 0);
      CHECK_EQ(c.e.b, 0);
      CHECK_EQ(c.e.at, edge);
      CHECK_EQ(countSince(w.house, "sync", h1), 0);
      Found cs = waitFor(w, w.house, "cmd_sent", h1, 1000);
      CHECK(cs.ok);
      CHECK_EQ(cs.e.a, ACT_CLOSE_);
      CHECK_EQ(cs.e.at - edge, 500);  // ctrl_confirm_ms
      CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
      CHECK(waitFor(w, w.house, "gate_state", h1, 2000, GS_CLOSED_).ok);
      CHECK(!w.house.coil(1));
      CHECK_EQ(countSince(w.house, "resync", h0), 0);
    }
  }
}

// [sync-window]
TEST(house_sync_window_starts_on_the_pass_k1_changes) {
  // Another controller closes the gate from open. On the pass the house hears closed it releases K1 and opens the
  // window before it reads IN1, so the user's OFF debounced on that very pass is a sync, while one debounced on the pass
  // before is the user's (a CLOSE, which the house then drops: the gate is closed by then). The same site three times
  // (the simulation is deterministic): first to learn when the house hears closed.
  uint32_t t = 0;
  for (int run = 0; run < 3; run++) {  // 0: learn t; 1: edge on t (inside); 2: edge on t - 1 (outside)
    Site w;
    w.commission();
    openByOther(w);
    w.run(4000);
    CHECK(w.alarmSwitch());
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    w.extPress(false, 300);
    if (run) {
      runTo(w, t - (run == 1 ? 51 : 52));
      CHECK(w.house.coil(1));  // still holding open
      w.user(false);
    }
    Found cl = waitFor(w, w.house, "gate_state", h0, 15000, GS_CLOSED_);
    CHECK(cl.ok);
    CHECK_EQ(cl.e.b, CAUSE_EXTERNAL_);
    CHECK(!w.house.coil(1));
    if (!run) {
      t = cl.e.at;
      continue;
    }
    CHECK_EQ(cl.e.at, t);
    if (run == 1) {
      Found sy = find(w.house, "sync", h0, 0);
      CHECK(sy.ok);
      CHECK_EQ(sy.e.at, t);
      CHECK(w.house.status()["sync_window"] == false);  // it matched K1: ended there
      w.run(4000);
      CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
      CHECK_EQ(countSince(w.house, "cmd_suppressed", h0), 0);
    } else {
      Found c = find(w.house, "ctrl", h0, 0);
      CHECK(c.ok);
      CHECK_EQ(c.e.at, t - 1);
      CHECK_EQ(c.e.b, 0);
      Found su = waitFor(w, w.house, "cmd_suppressed", h0, 1000);
      CHECK(su.ok);
      CHECK_EQ(su.e.a, ACT_CLOSE_);
      CHECK_EQ(su.e.b, GS_CLOSED_);
      CHECK_EQ(su.e.at - c.e.at, 500);  // ctrl_confirm_ms
      w.run(4000);
      CHECK_EQ(countSince(w.house, "sync", h0), 0);
    }
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.house, "resync", h0), 0);
    CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
    CHECK(!w.alarmSwitch());
    CHECK(w.sensorClosed());
    CHECK(w.opener.atClosed());
  }
}

// [sync-window]
TEST(house_sync_window_ends_on_the_matching_edge_only) {
  // FOLLOW: another controller opens, K1 goes on and the Shelly's ON edge (the level K1 asked for) is a sync that ends
  // the window there and then, so the user's OFF 120 ms later is the user's CLOSE. TOGGLE: the Shelly flips the other
  // way, an edge that doesn't match, and the window stays open its full sync_window_ms: the user's ON inside it is a
  // sync (it matches, so it ends the window), never a command.
  {
    Site w;
    w.commission();
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    uint32_t t = openByOther(w);
    Found sy = waitFor(w, w.house, "sync", h0, 300, 1);
    CHECK(sy.ok);
    CHECK_EQ(sy.e.at - t, SYNC_EDGE);
    CHECK(w.house.status()["sync_window"] == false);
    runTo(w, sy.e.at + 120);
    size_t h1 = w.house.logs.size();
    w.user(false);
    Found c = waitFor(w, w.house, "ctrl", h1, 100, 0);
    CHECK(c.ok);
    CHECK_EQ(c.e.b, 0);
    CHECK_EQ(countSince(w.house, "sync", h1), 0);
    Found cs = waitFor(w, w.house, "cmd_sent", h1, 600);
    CHECK(cs.ok);
    CHECK_EQ(cs.e.a, ACT_CLOSE_);
    CHECK_EQ(cs.e.at - c.e.at, 500);
    CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
    CHECK(waitFor(w, w.house, "gate_state", h1, 2000, GS_CLOSED_).ok);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
    CHECK_EQ(countSince(w.gate, "pulse", g0, 2), 1);
  }
  {
    Site w;
    w.shelly.mode = Shelly::TOGGLE;
    w.commission();
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    uint32_t t = openByUser(w);  // the Shelly already ON, K1 goes on: it toggles OFF
    Found sy = waitFor(w, w.house, "sync", h0, 300, 0);
    CHECK(sy.ok);
    CHECK_EQ(sy.e.at - t, SYNC_EDGE);
    CHECK(!w.alarmSwitch());
    runTo(w, t + 2000);
    CHECK(w.house.status()["sync_window"] == true);
    size_t h1 = w.house.logs.size();
    w.user(true);  // inside the window
    Found sy2 = waitFor(w, w.house, "sync", h1, 100, 1);
    CHECK(sy2.ok);
    CHECK_EQ(sy2.e.at - t, 2051);
    CHECK(w.house.status()["sync_window"] == false);
    w.run(15000);
    CHECK_EQ(countSince(w.house, "ctrl", h1), 0);
    CHECK_EQ(countSince(w.house, "cmd_sent", h1), 0);
    CHECK_EQ(countSince(w.house, "cmd_suppressed", h1), 0);
    CHECK_EQ(countSince(w.house, "resync", h0), 0);  // in step again from that edge
    CHECK(w.alarmSwitch());
    CHECK(w.house.coil(1));
    CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
    CHECK(w.opener.atOpen());
  }
}

// [sync-window] [armed-after-status]
TEST(house_sync_k1_relay_test_never_becomes_a_command) {
  // A console test pulse of house K1 moves the Shelly on and off. The house disarms for the pulse plus sync_window_ms:
  // the ON edge is only noted, the OFF edge (after the pulse) falls in the window its end opens. Then it re-arms.
  Site w;
  w.commission();
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  JsonDocument r = w.house.request("relay.test", "\"k\":1,\"ms\":500");
  CHECK(r["ok"] == true);
  Found p = find(w.house, "pulse", h0, 1);
  CHECK(p.ok);
  CHECK_EQ(p.e.b, 500);
  CHECK(w.house.status()["armed"] == false);
  Found c = waitFor(w, w.house, "ctrl", h0, 300, 1);
  CHECK(c.ok);
  CHECK_EQ(c.e.b, 0);
  CHECK_IN(c.e.at - p.e.at, SYNC_EDGE - 1, SYNC_EDGE + 1);
  CHECK(w.alarmSwitch());
  Found sy = waitFor(w, w.house, "sync", h0, 1000, 0);
  CHECK(sy.ok);
  CHECK_IN(sy.e.at - p.e.at, 500 + SYNC_EDGE - 1, 500 + SYNC_EDGE + 1);
  CHECK(!w.alarmSwitch());
  CHECK(!w.house.coil(1));
  runTo(w, p.e.at + 500 + SYNC_MS - 1);
  CHECK(w.house.status()["armed"] == false);
  w.run(3);
  CHECK(w.house.status()["armed"] == true);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK(w.opener.atClosed());
  // Armed again: the user's switch is a command.
  w.run(1000);
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found cs = waitFor(w, w.house, "cmd_sent", h1, 200);
  CHECK(cs.ok);
  CHECK_EQ(cs.e.a, ACT_OPEN_);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
}

// [armed-after-status] [sync-window] [check-soon]
TEST(house_sync_k1_relay_test_before_the_first_status_does_not_arm) {
  // The house restarts and the gate's STATUS doesn't get through for a while. A console test pulse of K1 meanwhile
  // (someone checking the wiring) toggles the Shelly but must not arm the house: arming waits for the first STATUS
  // plus sync_window_ms whatever else happens, so the user's switch long after the pulse is only noted. Once the
  // STATUS (closed) gets through the house arms sync_window_ms later and puts the Shelly back. The gate never moves.
  Site w;
  w.commission([](Board &b) { configureHouse(b, "ctrl_settle_ms", 0); });
  w.run(2000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  bool block = true;
  w.drop = [&block](const AirFrame &f) { return block && f.from == 1 && f.type() == MSG_STATUS_; };
  w.house.reset(PM_RCAUSE_WDT);
  Found boot = waitFor(w, w.house, "boot", h0, 2000);
  CHECK(boot.ok);
  runTo(w, boot.e.at + 5000);
  JsonDocument s = w.house.status();
  CHECK(s["sync_window"] == false);
  CHECK(s["armed"] == false);
  CHECK(s["link_up"] == true);
  CHECK_STR(str(s, "gate"), "unknown");

  JsonDocument r = w.house.request("relay.test", "\"k\":1,\"ms\":500");
  CHECK(r["ok"] == true);
  Found p = find(w.house, "pulse", h0, 1);
  CHECK(p.ok);
  CHECK_EQ(p.e.b, 500);
  Found c = waitFor(w, w.house, "ctrl", h0, 300, 1);  // the Shelly's ON: noted (not armed), not a sync
  CHECK(c.ok);
  CHECK_EQ(c.e.b, 0);
  Found sy = waitFor(w, w.house, "sync", h0, 1000, 0);  // its OFF after the pulse: in the window the pulse's end opens
  CHECK(sy.ok);
  CHECK_IN(sy.e.at - p.e.at, 500 + SYNC_EDGE - 1, 500 + SYNC_EDGE + 1);
  CHECK(!w.alarmSwitch());
  CHECK(!w.house.coil(1));
  // Well past the pulse plus sync_window_ms (when a test after the first STATUS re-arms): still not armed.
  runTo(w, p.e.at + 500 + SYNC_MS + 1000);
  s = w.house.status();
  CHECK(s["armed"] == false);
  CHECK(s["sync_window"] == false);
  CHECK_STR(str(s, "gate"), "unknown");
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found c1 = waitFor(w, w.house, "ctrl", h1, 200, 1);
  CHECK(c1.ok);
  CHECK_EQ(c1.e.b, 0);
  w.run(2000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK(w.house.status()["armed"] == false);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 0);  // still no STATUS

  block = false;
  Found st = waitFor(w, w.house, "gate_state", h0, 40000);
  w.drop = nullptr;
  CHECK(st.ok);
  CHECK_EQ(st.e.a, GS_CLOSED_);
  uint32_t armAt = (st.e.at + SYNC_MS) | 1;
  runTo(w, armAt - 1);
  CHECK(w.house.status()["armed"] == false);
  w.run(2);
  CHECK(w.house.status()["armed"] == true);
  Found rs = waitFor(w, w.house, "resync", h1, 100);  // check-soon from the boot: the user's ON, the gate closed
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(rs.e.at - armAt, 0, 1);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1200));
  w.run(3000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK(w.opener.atClosed());
  CHECK(w.sensorClosed());
}

// ---------------------------------------------------------------------------------------------------------------
// [resync]

// [resync]
TEST(house_sync_lost_command_resynced_after_mismatch_timeout) {
  // Every CMD frame is lost: the house gives the command up after cmd_ttl_s, the Shelly still showing the user's ON.
  // No resync while the command is on its way; from the give-up, mismatch_timeout_s (10) to the ms. The cycle drives
  // K1 to the Shelly's level for resync_ms, then to the gate's; the Shelly's edge is a sync. The gate never moves.
  Site w;
  w.commission([](Board &b) { configureHouse(b, "mismatch_timeout_s", 10); });
  w.drop = [](const AirFrame &f) { return f.from == 0 && f.type() == MSG_CMD_; };
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.user(true);
  Found cs = waitFor(w, w.house, "cmd_sent", h0, 1000, ACT_OPEN_);
  CHECK(cs.ok);
  Found dr = waitFor(w, w.house, "cmd_dropped", h0, 15000);
  CHECK(dr.ok);
  CHECK_EQ(dr.e.a, ACT_OPEN_);
  CHECK_EQ(dr.e.b, cs.e.b);
  CHECK((int32_t)(dr.e.at - cs.e.at) >= 10000);  // longer than mismatch_timeout_s: a resync would have come by now
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK(w.alarmSwitch());
  CHECK(w.house.status()["link_up"] == true);  // only the command was lost
  w.drop = nullptr;
  CHECK_EQ(watch(w, dr.e.at + 9999 - w.now, [&] { return w.alarmSwitch() && !w.house.coil(1); }), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  Found rs = waitFor(w, w.house, "resync", h0, 10);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(rs.e.at - dr.e.at, 10000, 10001);
  CHECK(w.house.coil(1));
  CHECK(w.house.status()["resyncing"] == true);
  CHECK_EQ(watch(w, 999, [&] { return w.house.coil(1); }), 0);
  w.step();
  CHECK(!w.house.coil(1));
  CHECK_EQ(w.now - rs.e.at, 1000);  // resync_ms
  Found sy = waitFor(w, w.house, "sync", h0, 200, 0);
  CHECK(sy.ok);
  CHECK_IN(sy.e.at - rs.e.at, 1000 + SYNC_EDGE - 1, 1000 + SYNC_EDGE + 1);
  CHECK(!w.alarmSwitch());
  w.run(15000);
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK(w.opener.atClosed());
  CHECK(w.sensorClosed());
  CHECK(!w.alarmSwitch());
}

// [resync]
TEST(house_sync_command_lost_while_the_link_was_down_resynced_after_it_returns) {
  // Nothing gets through for a while: the house declares the link down (link_timeout_s 15, heartbeat_s 5), the user
  // switches ON and the command is lost. No resync while the link is down (the house can't know where the gate is),
  // however long; once the gate's STATUS gets through again (still closed) the Shelly is out of step from that pass,
  // and resynced mismatch_timeout_s (10) later. The gate never moves.
  Site w;
  w.commission([](Board &b) {
    if (b.idx == 0) {
      CHECK(b.set("link_timeout_s", 15));
      CHECK(b.set("mismatch_timeout_s", 10));
    } else {
      CHECK(b.set("heartbeat_s", 5));
    }
  });
  CHECK_EQ(w.houseLinkTimeoutMs(), 15000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.drop = [](const AirFrame &) { return true; };
  Found dn = waitFor(w, w.house, "link_down", h0, 25000);
  CHECK(dn.ok);
  CHECK(!w.sensorClosed());  // linkloss_open
  w.run(1000);
  w.user(true);
  Found cs = waitFor(w, w.house, "cmd_sent", h0, 200, ACT_OPEN_);
  CHECK(cs.ok);
  Found dr = waitFor(w, w.house, "cmd_dropped", h0, 12000);
  CHECK(dr.ok);
  CHECK_EQ(dr.e.b, cs.e.b);
  CHECK_EQ(watch(w, 30000, [&] { return w.alarmSwitch() && !w.house.coil(1); }), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.house, "link_up", h0), 0);
  w.drop = nullptr;
  Found up = waitFor(w, w.house, "link_up", h0, 30000);
  CHECK(up.ok);
  CHECK_STR(str(w.house.status(), "gate"), "closed");
  CHECK_EQ(watch(w, up.e.at + 9999 - w.now, [&] { return w.alarmSwitch() && !w.house.coil(1); }), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  Found rs = waitFor(w, w.house, "resync", h0, 10);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(rs.e.at - up.e.at, 10000, 10001);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1200));
  w.run(5000);
  CHECK(!w.alarmSwitch());
  CHECK(!w.house.coil(1));
  CHECK(w.sensorClosed());
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK(w.opener.atClosed());
}

// [resync]
TEST(house_sync_result_timeout_resyncs_at_once) {
  // The siren holds OPEN, so the opener ignores our CLOSE and the gate never leaves open; its target times out after
  // its travel_timeout_s (10). The house resyncs on the pass it hears that, not mismatch_timeout_s (75) after the
  // mismatch began.
  Site w;
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("travel_timeout_s", 10));
  });
  openByUser(w);
  w.run(4000);
  w.opener.extOpenHold = true;
  w.run(200);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  size_t ev = w.house.events.size();
  w.user(false);
  CHECK(waitFor(w, w.house, "cmd_sent", h0, 1000, ACT_CLOSE_).ok);
  Found p = waitFor(w, w.gate, "pulse", g0, 2000, 2);
  CHECK(p.ok);
  uint32_t heard = 0;
  CHECK(w.runUntil([&] {
    if (!heard) heard = statusSeen(w, &ev, "timeout");
    return find(w.house, "resync", h0).ok;
  }, 12000));
  CHECK(heard != 0);
  Found tt = find(w.gate, "travel_timeout", g0);
  CHECK(tt.ok);
  CHECK_EQ(tt.e.a, GS_CLOSED_);
  Found rs = find(w.house, "resync", h0);
  CHECK_EQ(rs.e.a, 1);
  CHECK_IN(rs.e.at - heard, 0, 2);
  CHECK(w.opener.atOpen());
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 1200));
  CHECK(w.house.coil(1));
  w.opener.extOpenHold = false;
  w.run(5000);
  CHECK(w.alarmSwitch());
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
}

// [resync]
XFAIL_TEST(house_sync_siren_override_mid_travel_resyncs_at_once,
           "role_house.cpp handleStatus: the TR_TIMEOUT fast-forward needs mismatchSince set, but holdingTravel() keeps "
           "it at 0 all through the travel, so a command overridden mid-travel waits mismatch_timeout_s (75 s)") {
  // The user closes; mid-travel the siren sensor holds OPEN and takes the gate back to open: our command was overridden
  // (result timeout). The Shelly shows the user's OFF with the gate open; it should be put back at once.
  Site w;
  w.commission();
  openByUser(w);
  w.run(4000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  size_t ev = w.house.events.size();
  w.user(false);
  CHECK(waitFor(w, w.house, "gate_state", h0, 3000, GS_BETWEEN_).ok);
  w.run(2000);
  w.opener.extOpenHold = true;
  Found op = waitFor(w, w.gate, "gate_state", g0, 15000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_EXTERNAL_);
  uint32_t heard = 0;
  CHECK(w.runUntil([&] { return (heard = statusSeen(w, &ev, "timeout")) != 0; }, 2000));
  CHECK(w.house.coil(1));
  CHECK(!w.alarmSwitch());  // the user's OFF, the gate open
  Found rs = waitFor(w, w.house, "resync", h0, 5000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 1);
  CHECK_IN(rs.e.at - heard, 0, 1000);
  CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 3000));
}

// [resync] [check-soon]
TEST(house_sync_no_power_refusal_resyncs_at_once) {
  // Without AC the gate refuses the user's OPEN (ACK no_power) and nothing moves. The house resyncs on the pass after
  // it hears the refusal, not mismatch_timeout_s later.
  Site w;
  w.commission();
  w.opener.ac = false;
  CHECK(poll(w, [&] { return w.house.status()["remote"]["ac_power"] == false; }, 3000));
  w.run(1000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.user(true);
  Found cs = waitFor(w, w.house, "cmd_sent", h0, 1000, ACT_OPEN_);
  CHECK(cs.ok);
  Found rf = waitFor(w, w.gate, "cmd_refused", g0, 2000);
  CHECK(rf.ok);
  CHECK_EQ(rf.e.b, cs.e.b);
  uint32_t ackEnd = 0;
  CHECK(w.runUntil([&] {
    ackEnd = sentEnd(w, w.gate, MSG_ACK_, rf.e.at);
    return ackEnd && (int32_t)(w.now - ackEnd) >= 0;
  }, 2000));
  Found rs = waitFor(w, w.house, "resync", h0, 3000);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(rs.e.at - ackEnd, 0, 2);
  CHECK_EQ(w.house.status()["cmd_result"].as<int>(), RES_NO_POWER_);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1200));
  CHECK(!w.house.coil(1));
  w.run(5000);
  CHECK(!w.alarmSwitch());
  CHECK(w.sensorClosed());
  CHECK(w.opener.atClosed());
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
}

// ---------------------------------------------------------------------------------------------------------------
// [armed-after-status]

// [armed-after-status] [check-soon] [k1-mirror]
TEST(house_sync_house_restart_never_moves_the_gate) {
  // The house board restarts (watchdog, or its own feed cut) with the gate open or closed and the Shelly ON or OFF as it
  // comes back. Whatever the Shelly shows, nothing is commanded: every IN1 edge is a sync, and a Shelly left out of
  // step is put back by a resync. The gate is never pulsed and never moves. One site, restarted again and again.
  struct Case {
    bool open, shellyOn, cut;
  } cases[] = { { false, false, false }, { false, true, false }, { false, true, true },
                { true, false, false },  { true, true, false },  { true, false, true } };
  Site w;
  w.commission();
  for (const Case &c : cases) {
    if (c.open && !w.opener.atOpen()) {
      openByOther(w);
      w.run(4000);
    }
    CHECK_EQ(w.house.coil(1), c.open);
    CHECK_EQ(w.alarmSwitch(), c.open);
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    size_t presses = w.opener.presses[0].size() + w.opener.presses[1].size();
    int32_t pos = w.opener.pos;
    if (c.cut) w.house.cut = true;
    else w.house.reset(PM_RCAUSE_WDT);
    w.run(300);
    CHECK(!w.house.coil(1));
    CHECK(!w.alarmSwitch());  // the Shelly followed K1 dropping with the board
    w.shelly.relay = c.shellyOn;  // what it shows when the house is back (not a user action)
    if (c.cut) {
      w.run(700);
      w.house.cut = false;
    }
    Found boot = waitFor(w, w.house, "boot", h0, 2000);
    CHECK(boot.ok);
    w.run(25000);
    JsonDocument s = w.house.status();
    CHECK(s["armed"] == true);
    CHECK_STR(str(s, "gate"), c.open ? "open" : "closed");
    CHECK_EQ(w.house.coil(1), c.open);
    CHECK_EQ(w.alarmSwitch(), c.open);
    CHECK_EQ(w.sensorClosed(), !c.open);
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.house, "cmd_suppressed", h0), 0);
    CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
    CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
    CHECK_EQ(w.opener.presses[0].size() + w.opener.presses[1].size(), presses);
    CHECK_EQ(w.opener.pos, pos);
    // K1 alone fixes a Shelly that shows OFF at an open gate (it goes on); one showing ON at a closed gate needs the
    // resync (K1 is already off).
    bool resync = c.shellyOn && !c.open;
    CHECK_EQ(countSince(w.house, "resync", h0), resync ? 1 : 0);
    if (resync) CHECK_EQ(find(w.house, "resync", h0).e.a, 0);
    CHECK_EQ(countSince(w.house, "sync", h0), c.open != c.shellyOn ? 1 : 0);  // K1 going on, or the resync's last edge
  }
}

namespace {

// ctrl_settle_ms 0, so the boot's sync window is only sync_window_ms. The house restarts and the gate's STATUS doesn't
// get through for a while: the window is over and the link verified, but nothing is armed, so the user's switch (ON,
// then OFF again) is noted, never sent. Then the first STATUS (closed); returns when the house arms (that STATUS +
// sync_window_ms, stamped `| 1`), with the world just after it.
uint32_t bootWithLateStatus(Site &w, size_t h0) {
  bool block = true;
  w.drop = [&block](const AirFrame &f) { return block && f.from == 1 && f.type() == MSG_STATUS_; };
  w.house.reset(PM_RCAUSE_WDT);
  Found boot = waitFor(w, w.house, "boot", h0, 2000);
  CHECK(boot.ok);
  runTo(w, boot.e.at + 5000);
  JsonDocument s = w.house.status();
  CHECK(s["sync_window"] == false);
  CHECK(s["armed"] == false);
  CHECK(s["link"]["verified"] == true);
  CHECK(s["link_up"] == true);
  CHECK_STR(str(s, "gate"), "unknown");
  w.user(true);
  Found c1 = waitFor(w, w.house, "ctrl", h0, 200, 1);
  CHECK(c1.ok);
  CHECK_EQ(c1.e.b, 0);  // taken as the user's (not ignored for power), but not armed
  w.run(1000);
  w.user(false);
  Found c2 = waitFor(w, w.house, "ctrl", h0, 200, 0);
  CHECK(c2.ok);
  w.run(1000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 0);  // still no STATUS
  block = false;
  Found st = waitFor(w, w.house, "gate_state", h0, 40000);
  w.drop = nullptr;
  CHECK(st.ok);
  CHECK_EQ(st.e.a, GS_CLOSED_);
  CHECK(!w.house.coil(1));  // already off: no K1 change, no sync window
  return (st.e.at + SYNC_MS) | 1;
}

}  // namespace

// [armed-after-status] [check-soon]
TEST(house_sync_edge_before_arming_is_not_a_command) {
  // The user's ON debounced on the ms before the house arms: noted, not sent. On the arming pass the check-soon left
  // from the boot finds the Shelly out of step and resyncs it at once. The gate never moves.
  Site w;
  w.commission([](Board &b) { configureHouse(b, "ctrl_settle_ms", 0); });
  w.run(2000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  uint32_t armAt = bootWithLateStatus(w, h0);
  runTo(w, armAt - 52);
  CHECK(w.house.status()["armed"] == false);
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found c = waitFor(w, w.house, "ctrl", h1, 100, 1);
  CHECK(c.ok);
  CHECK_EQ(c.e.at, armAt - 1);
  CHECK_EQ(c.e.b, 0);
  CHECK(w.house.status()["armed"] == false);
  w.run(2);
  CHECK(w.house.status()["armed"] == true);
  Found rs = waitFor(w, w.house, "resync", h1, 100);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.a, 0);
  CHECK_IN(rs.e.at - armAt, 0, 1);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1200));
  w.run(3000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK(w.opener.atClosed());
  CHECK(w.sensorClosed());
}

namespace {

// As above, but the user's ON is debounced on the arming pass itself: a command, sent on that pass.
void userOnTheArmingPass(Site &w, size_t h0) {
  uint32_t armAt = bootWithLateStatus(w, h0);
  runTo(w, armAt - 51);
  CHECK(w.house.status()["armed"] == false);
  size_t h1 = w.house.logs.size();
  w.user(true);
  Found c = waitFor(w, w.house, "ctrl", h1, 100, 1);
  CHECK(c.ok);
  CHECK_EQ(c.e.at, armAt);
  CHECK_EQ(c.e.b, 0);
  Found cs = find(w.house, "cmd_sent", h1);
  CHECK(cs.ok);
  CHECK_EQ(cs.e.a, ACT_OPEN_);
  CHECK_EQ(cs.e.at, armAt);
  CHECK(w.house.status()["armed"] == true);
}

}  // namespace

// [armed-after-status]
TEST(house_sync_edge_on_arming_is_a_command) {
  // The other side of the boundary: the user's ON debounced on the arming pass is the user's command. The gate gets it
  // once, pulses OPEN once and opens; the house ends showing open, the Shelly ON.
  Site w;
  w.commission([](Board &b) { configureHouse(b, "ctrl_settle_ms", 0); });
  w.run(2000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  userOnTheArmingPass(w, h0);
  Found rx = waitFor(w, w.gate, "cmd_rx", g0, 1000);
  CHECK(rx.ok);
  CHECK_EQ(rx.e.a, ACT_OPEN_);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  Found op = waitFor(w, w.house, "gate_state", h0, 2000, GS_OPEN_);
  CHECK(op.ok);
  CHECK_EQ(op.e.b, CAUSE_LORA_);
  w.run(4000);
  CHECK(w.house.coil(1));
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0, 1), 1);
  CHECK_EQ(w.opener.presses[0].size(), 1);
  CHECK(w.opener.presses[1].empty());
}

// [armed-after-status] [check-soon] [resync]
XFAIL_TEST(house_sync_edge_on_arming_is_not_reverted_by_the_boot_check_soon,
           "role_house.cpp: checkSoon from the boot outlives the user's command (sendCommand() clears mismatchSince "
           "but not checkSoon), so once its ACK lets the mismatch check run, the gate still reads closed and the house "
           "resyncs the Shelly OFF while the gate opens") {
  // The user's ON debounced on the arming pass: a command, sent at once. The Shelly was in step until the user moved
  // it; it must keep showing the user's ON while the gate opens (K1 holds closed, but mid-travel the Shelly may
  // already show the new command), not be cycled back OFF by a check-soon meant for the boot.
  Site w;
  w.commission([](Board &b) { configureHouse(b, "ctrl_settle_ms", 0); });
  w.run(2000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  userOnTheArmingPass(w, h0);
  uint32_t off = 0;
  CHECK(w.runUntil([&] {
    if (!off && !w.alarmSwitch()) off = w.now;
    return w.houseSees() == GS_OPEN_;
  }, 15000));
  CHECK_EQ(countSince(w.gate, "pulse", g0, 1), 1);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(off, 0);
}

// ---------------------------------------------------------------------------------------------------------------
// [check-soon]

// [check-soon] [armed-after-status]
TEST(house_sync_check_soon_after_boot_resyncs_when_the_settle_window_closes) {
  // The Shelly shows ON as the house comes back to a closed gate. Nothing until the boot's settle window
  // (ctrl_settle_ms + sync_window_ms from houseBegin) closes; the pass it closes finds the Shelly out of step and the
  // next one resyncs, not mismatch_timeout_s (75) later. Two settle lengths: the resync follows the window.
  const uint32_t settles[] = { 10000, 4000 };
  for (uint32_t settle : settles) {
    Site w;
    w.commission([&](Board &b) { configureHouse(b, "ctrl_settle_ms", (int32_t)settle); });
    w.run(2000);
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    w.house.reset(PM_RCAUSE_WDT);
    w.run(300);
    w.shelly.relay = true;
    Found boot = waitFor(w, w.house, "boot", h0, 2000);
    CHECK(boot.ok);
    // setup() logs the boot, starts the radio (blocking) and ends with houseBegin, which opens the window: the board's
    // clock when that call returned.
    uint32_t begun = w.house.busyUntil;
    CHECK_IN(begun - boot.e.at, 1, 2000);
    uint32_t close = begun + settle + SYNC_MS;
    runTo(w, close - 1);
    JsonDocument s = w.house.status();
    CHECK(s["armed"] == true);
    CHECK(s["sync_window"] == true);
    CHECK_STR(str(s, "gate"), "closed");
    CHECK_EQ(countSince(w.house, "resync", h0), 0);
    CHECK(w.alarmSwitch());
    w.step();
    CHECK(w.house.status()["sync_window"] == false);
    Found rs = waitFor(w, w.house, "resync", h0, 500);
    CHECK(rs.ok);
    CHECK_EQ(rs.e.a, 0);
    CHECK_EQ(rs.e.at - close, 1);
    CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 1200));
    w.run(3000);
    CHECK_EQ(countSince(w.house, "resync", h0), 1);
    CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
    CHECK(w.opener.atClosed());
  }
}

// [check-soon] [sync-window]
TEST(house_sync_check_soon_after_controller_power_returns) {
  // The house's 12 V (the Shelly, the IN2 opto, the board's VIN) is cut for 5 s with the gate open; the house rides
  // through on its LiPo. When power returns the settle window runs ctrl_settle_ms + sync_window_ms from the controller
  // power edge, and a matching edge (the Shelly restoring ON) doesn't end it early. A Shelly still out of step when it
  // closes (it booted OFF, or chattered OFF inside the window) is resynced on the next pass; one in step isn't.
  struct V {
    bool restore;
    uint32_t chatterAt;
  } vs[] = { { false, 0 }, { true, 0 }, { true, 6000 } };
  Site w;
  w.commission();
  w.house.lipo = true;
  openByOther(w);
  w.run(4000);
  for (const V &v : vs) {
    w.shelly.restoreSw = v.restore;
    CHECK(w.alarmSwitch());
    CHECK(w.house.status()["sync_window"] == false);
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    w.setRail12(false);
    Found off = waitFor(w, w.house, "ctrl_power", h0, 3000, 0);
    CHECK(off.ok);
    w.run(5000);
    CHECK(w.house.running());
    CHECK(!w.alarmSwitch());
    CHECK(w.house.coil(1));
    w.setRail12(true);
    Found on = waitFor(w, w.house, "ctrl_power", h0, 3000, 1);
    CHECK(on.ok);
    uint32_t P = on.e.at;
    if (v.chatterAt) {
      runTo(w, P + v.chatterAt);
      CHECK(w.alarmSwitch());
      w.shelly.relay = false;  // chatter, inside the window
    }
    runTo(w, P + 12950);
    JsonDocument s = w.house.status();
    CHECK(s["sync_window"] == true);
    CHECK_EQ(countSince(w.house, "resync", h0), 0);
    bool outOfStep = !v.restore || v.chatterAt;
    CHECK_EQ(w.alarmSwitch(), !outOfStep);
    if (v.restore) {
      Found sy = find(w.house, "sync", h0, 1);  // the Shelly booting back ON: a sync, matching, the window kept open
      CHECK(sy.ok);
      CHECK((int32_t)(sy.e.at - P) > 0);
    }
    if (v.chatterAt) {
      Found sy = find(w.house, "sync", h0, 0);
      CHECK(sy.ok);
      CHECK_EQ(sy.e.at - P, v.chatterAt + 51);
    }
    if (outOfStep) {
      Found rs = waitFor(w, w.house, "resync", h0, 200);
      CHECK(rs.ok);
      CHECK_EQ(rs.e.a, 1);
      CHECK_IN(rs.e.at - P, 13000, 13002);
      CHECK(w.runUntil([&] { return w.alarmSwitch(); }, 1200));
    } else {
      w.run(20000);
      CHECK_EQ(countSince(w.house, "resync", h0), 0);
    }
    w.run(3000);
    CHECK(w.alarmSwitch());
    CHECK(w.house.coil(1));
    CHECK_EQ(countSince(w.house, "resync", h0), outOfStep ? 1 : 0);
    CHECK_EQ(countSince(w.house, "ctrl", h0, ANY, 0), 0);  // the relay dropping in the cut: ignored (b=1)
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
    CHECK(w.opener.atOpen());
  }
}
