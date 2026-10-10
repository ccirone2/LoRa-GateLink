// Robustness of the whole site (CLAUDE.md "Behavioural invariants"), through the simulated site (world.h):
//  [wrap-safe]  timing across the millis() wrap (2^32 ms) and the signed-age flip (2^31 ms), and time stamps left alone
//               for weeks (quiet stretches are jumped over in hops, see hop());
//  [pull-down]  inputs pulled down: a dead opto or a cut wire reads inactive, never as a limit, and inN_invert stays 0;
//  [restarts]   warm resets, watchdog resets and power cuts of either board or both, anywhere, and a radio that fails
//               to come up;
//  [chaos]      seeded randomized runs of several hours with every fault mixed in and every monitor on.
// Each test checks pins, the opener, log events, console replies and status fields itself; the world's monitors catch
// whatever else breaks on the way.
//
// Timings as the site models them (world.cpp): a user action at X is the house's debounced IN1 edge at X + 51; the
// gate's limit change at L is its debounced input at L + 50 and, into `between`, its report 500 ms later; a K1 pulse
// closes the opener's OPEN input, which it takes 20 ms later.
#include "world.h"
#include <Arduino.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <exception>
#include <functional>
#include <map>
#include <set>

namespace {

enum { CAUSE_NONE_ = 0, CAUSE_LORA_ = 1, CAUSE_EXTERNAL_ = 2 };  // roles.h Cause
enum { ACT_OPEN_ = 1, ACT_CLOSE_ = 2 };                          // roles.h Action
enum : uint8_t { MSG_HELLO_ = 1, MSG_HELLO_ACK_ = 2, MSG_ACK_ = 3, MSG_CMD_ = 4, MSG_STATUS_ = 5 };  // link.h
const int RES_NO_POWER_ = 4;
const int ANY = INT_MIN;
const uint32_t F32 = 0;            // world time 2^32 (= 0): millis() wraps
const uint32_t F31 = 0x80000000u;  // 2^31: a stamp taken just before reads as in the future once this passes it
const uint32_t SYNC_MS = 3000, SETTLE_MS = 10000, CONFIRM_MS = 500, TRAVEL_MS = 60000, LINK_MS = 100000;
const uint32_t HB_MS = 30000, PULSE_MS = 500, MISMATCH_MS = 75000, RESYNC_MS = 1000, TTL_MS = 10000;
const uint32_t AGE_CAP = 0x40000000u;  // RX_AGE_CAP_MS (link.cpp) and STAMP_CAP_MS (role_house.cpp)
const uint32_t HOUR = 3600000u, DAY = 24 * HOUR;

std::string strf(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string strf(const char *f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

int32_t rel(uint32_t t, uint32_t f) {
  return (int32_t)(t - f);
}

// A CHECK failing while the monitors hold violations: print them next to it (and clear them, so the after-test hook
// doesn't throw a second time). Tests that get to their end call w.checkClean() themselves.
struct Guard {
  World &w;
  ~Guard() {
    if (!std::uncaught_exceptions() || w.violations.empty()) return;
    for (const std::string &v : w.violations) printf("      monitor: %s\n", v.c_str());
    w.violations.clear();
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

typedef std::pair<int, int> SC;  // gate_state (state, cause)
std::vector<SC> statesSince(const Board &b, size_t from) {
  std::vector<SC> v;
  for (size_t i = from; i < b.logs.size(); i++)
    if (b.logs[i].ev == "gate_state") v.push_back({ b.logs[i].a, b.logs[i].b });
  return v;
}

// --- recording the outputs ----------------------------------------------------------------------------------------
enum { GK1, GK2, HK1, HK2, SW, NSIG };  // gate K1/K2, house K1/K2 (the contact sensor), the Alarm.com switch
struct Edge {
  uint32_t at;
  bool on;
};
struct Pulse {
  uint32_t on, off;
  uint32_t len() const { return off - on; }
};

// Steps the world, recording every change of the gate's coils, the house's coils and the controller's relay with the
// world millisecond it happened in.
struct Rec {
  World &w;
  std::vector<Edge> e[NSIG];
  bool st[NSIG];
  std::function<void()> each;  // also run after every step

  explicit Rec(World &w) : w(w) {
    for (int i = 0; i < NSIG; i++) st[i] = sig(i);
  }
  bool sig(int i) const {
    switch (i) {
      case GK1: return w.gate.coil(1);
      case GK2: return w.gate.coil(2);
      case HK1: return w.house.coil(1);
      case HK2: return w.house.coil(2);
      default: return w.alarmSwitch();
    }
  }
  void sample() {
    for (int i = 0; i < NSIG; i++) {
      bool v = sig(i);
      if (v != st[i]) {
        e[i].push_back({ w.now, v });
        st[i] = v;
      }
    }
  }
  void step() {
    w.step();
    sample();
    if (each) each();
  }
  void run(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) step();
  }
  void runTo(uint32_t t) {
    if (rel(t, w.now) < 0) throw Failure(strf("runTo(%u): it's %u already", t, w.now));
    while (w.now != t) step();
  }
  bool until(const std::function<bool()> &cond, uint32_t maxMs) {
    for (uint32_t i = 0; i < maxMs; i++) {
      if (cond()) return true;
      step();
    }
    return cond();
  }
  // For conditions that read a board's status (too slow for every millisecond).
  bool poll(const std::function<bool()> &cond, uint32_t maxMs, uint32_t every = 50) {
    for (uint32_t t = 0; t < maxMs; t += every) {
      if (cond()) return true;
      run(every);
    }
    return cond();
  }
  size_t n(int s) const { return e[s].size(); }
  int ons(int s, size_t from) const {
    int k = 0;
    for (size_t i = from; i < e[s].size(); i++) k += e[s][i].on;
    return k;
  }
  // Complete closures of signal s since edge index `from`.
  std::vector<Pulse> pulses(int s, size_t from) const {
    std::vector<Pulse> v;
    for (size_t i = from; i + 1 < e[s].size(); i++)
      if (e[s][i].on && !e[s][i + 1].on) v.push_back({ e[s][i].at, e[s][i + 1].at });
    return v;
  }
};

// Runs until `ev` shows up on b (from log index `from`); the world stops on the step that logged it.
Found waitFor(Rec &r, const Board &b, const char *ev, size_t from, uint32_t maxMs, int a = ANY, int bv = ANY) {
  Found f;
  r.until([&] {
    f = find(b, ev, from, a, bv);
    return f.ok;
  }, maxMs);
  return f;
}

// A console request, stepping through the recorder until the board answers (relay tests aren't sent here).
JsonDocument req(Rec &r, Board &b, const std::string &cmd, const std::string &args = "") {
  int id = b.nextId++;
  std::string line = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"" + cmd + "\"" + (args.empty() ? "" : "," + args) + "}";
  r.w.trace.add(r.w.now, b.name + " <- " + line);
  b.send(line);
  if (!r.until([&] { return b.replies.count(id) > 0; }, 3000)) throw Failure(b.name + ": no reply to " + line);
  JsonDocument d = b.replies[id];
  b.replies.erase(id);
  return d;
}

const JsonDocument *eventSince(const Board &b, const char *name, size_t from) {
  for (size_t i = from; i < b.events.size(); i++)
    if (b.events[i]["event"] == name) return &b.events[i];
  return nullptr;
}

bool syncWindow(World &w) {
  return w.house.status()["sync_window"] == true;
}

// The world's monitors stamp a few things with 0 for "never" (world.cpp), which a clock past 2^31 reads as in the
// future: the no_power/fault display check would be off until the clock wraps. Start them at the world's time.
void freshMonitors(World &w) {
  w.resyncUntil = w.now;
  w.houseViewAt = w.now;
}

// The user moves the switch and the gate gets there; returns once the house shows it and the sync window K1's change
// opened is over (so the user's next switch is a command).
void userMove(Rec &r, bool open) {
  World &w = r.w;
  w.user(open);
  CHECK(r.until([&] { return open ? w.opener.atOpen() : w.opener.atClosed(); }, 20000));
  CHECK(r.until([&] { return w.houseSees() == (open ? GS_OPEN_ : GS_CLOSED_); }, 5000));
  CHECK(r.poll([&] { return !syncWindow(w); }, 6000, 20));
  CHECK_EQ(w.house.coil(1), open);
  CHECK_EQ(w.sensorClosed(), !open);
  CHECK_EQ(w.alarmSwitch(), open);
}

// The Alarm.com switch, K1 and the contact sensor agree with the real gate (closed: off, off, closed; anything else:
// on, on, open), and the house knows the state the gate is in.
void checkAgree(World &w) {
  bool closed = w.opener.atClosed();
  CHECK_EQ(w.houseSees(), w.gateTruth());
  CHECK_EQ(w.alarmSwitch(), !closed);
  CHECK_EQ(w.house.coil(1), !closed);
  CHECK_EQ(w.sensorClosed(), closed);
}

// Both boards up and settled: the house armed with no window open and the link up, the gate reporting.
void settled(Rec &r, uint32_t maxMs = 40000) {
  World &w = r.w;
  CHECK(r.poll([&] {
    if (!w.house.running() || !w.gate.running()) return false;
    JsonDocument h = w.house.status(), g = w.gate.status();
    return h["armed"] == true && h["sync_window"] == false && h["link_up"] == true && h["gate"] != "unknown"
           && h["resyncing"] == false && h["cmd_pending"] == false && g["settling"] == false
           && g["link"]["verified"] == true && h["link"]["verified"] == true;
  }, maxMs, 50));
}

// First transmission of each STATUS the gate built since `since` (its retries repeat the seq), in order.
std::vector<uint32_t> statusFirsts(const World &w, uint32_t since) {
  std::vector<uint32_t> t;
  std::set<uint32_t> seen;
  for (const AirFrame &f : w.air) {
    if (f.from != 1 || f.type() != MSG_STATUS_ || f.b.size() < 13 || rel(f.start, since) < 0) continue;
    uint32_t seq = f.b[9] | f.b[10] << 8 | f.b[11] << 16 | (uint32_t)f.b[12] << 24;
    if (seen.insert(seq).second) t.push_back(f.start);
  }
  return t;
}

int presses(const World &w) {
  return (int)(w.opener.presses[0].size() + w.opener.presses[1].size());
}

// Jumps the clock `ms` on at once: a quiet stretch the boards see as one long gap between two loop passes (the
// watchdog aside, it is what a stalled loop sees). Nothing may be moving. The site's and the monitors' own stamps
// more than a second old are pulled up to a second ago (or cleared), and delivered frames over a minute old dropped:
// world.cpp ages them signed, so left alone for 2^31 ms they would read as in the future (an external press held
// again weeks after it ended, an interlock breach from a release weeks ago, frames never pruned).
void hop(Rec &r, uint32_t ms) {
  World &w = r.w;
  CHECK(w.opener.dir == 0);
  CHECK(!w.gate.coil(1) && !w.gate.coil(2));
  w.now += ms;
  auto pull = [&](uint32_t &t) {
    if (rel(w.now, t) > 1000) t = w.now - 1000;
  };
  pull(w.resyncUntil);
  pull(w.houseViewAt);
  for (World::CoilTrack &c : w.gk)
    if (!c.on && c.offAt && rel(w.now, c.offAt) > 1000) c.offAt = 0;
  for (uint32_t *t : { &w.opener.extOpenUntil, &w.opener.extCloseUntil })
    if (*t && rel(w.now, *t) > 0) *t = 0;
  std::vector<AirFrame> keep;
  for (const AirFrame &f : w.air)
    if (!f.delivered || w.now - f.end < 60000) keep.push_back(f);
  w.air.swap(keep);
}

// `total` ms of quiet in hops of `hopMs`, each followed by `runMs` of loop passes, then `check`.
void quiet(Rec &r, uint64_t total, uint32_t hopMs, uint32_t runMs, const std::function<void()> &check = nullptr) {
  for (uint64_t t = 0; t < total; t += (uint64_t)hopMs + runMs) {
    hop(r, hopMs);
    r.run(runMs);
    if (check) check();
  }
}

// --- [wrap-safe] scenarios, each placed so that its timing straddles F (the wrap, or the signed flip) --------------

// Boot, link, first STATUS and the house's settle window across F.
void wrapBoot(uint32_t F) {
  World w(F - 12000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  const LogEv *boot = w.house.last("boot");
  CHECK(boot);
  CHECK(rel(boot->at, F) < 0);
  CHECK(rel(w.now, F) > 0);
  // The settle window houseBegin opens (ctrl_settle_ms + sync_window_ms, a few hundred ms into the boot, after the
  // radio's init) runs its full length across F: commission() returned the millisecond it closed.
  CHECK_IN(w.now - boot->at, SETTLE_MS + SYNC_MS, SETTLE_MS + SYNC_MS + 1000);
  JsonDocument h = w.house.status(), gs = w.gate.status();
  CHECK(h["armed"] == true);
  CHECK(h["link_up"] == true);
  CHECK(h["gate"] == "closed");
  CHECK(gs["gate"] == "closed");
  CHECK(gs["settling"] == false);
  checkAgree(w);
  // Commands work after it: a cycle each way, one pulse each, pulse_ms long
  size_t p1 = r.n(GK1), p2 = r.n(GK2);
  userMove(r, true);
  userMove(r, false);
  std::vector<Pulse> a = r.pulses(GK1, p1), b = r.pulses(GK2, p2);
  CHECK_EQ(a.size(), 1);
  CHECK_EQ(b.size(), 1);
  CHECK_IN(a[0].len(), PULSE_MS, PULSE_MS + 1);
  CHECK_IN(b[0].len(), PULSE_MS, PULSE_MS + 1);
  CHECK_EQ(w.gate.count("cmd_rx"), 2);
  CHECK_EQ(w.house.count("cmd_sent"), 2);
  w.checkClean();
}

// A command's pulse closes ~250 ms before F and releases ~250 ms after; the travel it starts ends after F.
void wrapPulse(uint32_t F) {
  World w(F - 70000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  // Before F: how long from the user's switch to the gate's K1 (and a close cycle back)
  size_t p0 = r.n(GK1);
  uint32_t u0 = w.now;
  userMove(r, true);
  CHECK(r.n(GK1) >= p0 + 2);
  uint32_t lat = r.e[GK1][p0].at - u0;
  CHECK_IN(lat, 51, 1000);
  userMove(r, false);
  uint32_t at = F - 250 - lat;
  CHECK(rel(w.now, at) < 0);
  r.runTo(at);
  size_t g0 = w.gate.logs.size(), h0 = w.house.logs.size();
  p0 = r.n(GK1);
  size_t q0 = r.n(GK2), k1 = r.n(HK1), k2 = r.n(HK2);
  userMove(r, true);
  std::vector<Pulse> p = r.pulses(GK1, p0);
  CHECK_EQ(p.size(), 1);
  CHECK(rel(p[0].on, F) < 0);
  CHECK(rel(p[0].off, F) > 0);
  CHECK_IN(p[0].len(), PULSE_MS, PULSE_MS + 1);
  CHECK_EQ(r.n(GK2), q0);
  CHECK_EQ(w.opener.presses[0].size(), 2);
  CHECK_EQ(w.opener.presses[1].size(), 1);
  CHECK_IN(w.opener.presses[0].back().second, PULSE_MS, PULSE_MS + 1);
  // The gate reports the move as ours, from its limits: between, then open; nothing timed out
  std::vector<SC> st = statesSince(w.gate, g0);
  CHECK_EQ(st.size(), 2);
  CHECK(st[0] == SC(GS_BETWEEN_, CAUSE_LORA_));
  CHECK(st[1] == SC(GS_OPEN_, CAUSE_LORA_));
  CHECK_EQ(countSince(w.gate, "travel_timeout", g0), 0);
  JsonDocument gs = w.gate.status();
  CHECK(gs["last_result"] == "reached");
  CHECK(gs["target"] == "");
  // The house: the sensor opens with `between`, K1 holds the closed level through the travel and opens with `open`
  Found hb = find(w.house, "gate_state", h0, GS_BETWEEN_), ho = find(w.house, "gate_state", h0, GS_OPEN_);
  CHECK(hb.ok && ho.ok);
  CHECK_EQ(hb.e.b, CAUSE_LORA_);
  CHECK_EQ(r.n(HK2), k2 + 1);
  CHECK(!r.e[HK2][k2].on);
  CHECK_EQ(r.e[HK2][k2].at, hb.e.at);
  CHECK_EQ(r.n(HK1), k1 + 1);
  CHECK(r.e[HK1][k1].on);
  CHECK_EQ(r.e[HK1][k1].at, ho.e.at);
  CHECK_IN(ho.e.at - p[0].on, 8000, 9000);
  // and back, after F
  q0 = r.n(GK2);
  userMove(r, false);
  std::vector<Pulse> c = r.pulses(GK2, q0);
  CHECK_EQ(c.size(), 1);
  CHECK_IN(c[0].len(), PULSE_MS, PULSE_MS + 1);
  CHECK_EQ(w.gate.count("cmd_rx"), 4);
  w.checkClean();
}

// The link lost 50 s before F: it stays up until link_timeout_s after the last frame (after F), and the gate's
// heartbeats keep their 30 s across F; it comes back with the next heartbeat.
void wrapLink(uint32_t F) {
  World w(F - 75000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  uint32_t t0 = w.now;
  r.runTo(F - 50000);
  size_t h0 = w.house.logs.size(), k2 = r.n(HK2);
  CHECK(w.sensorClosed());
  w.drop = [](const AirFrame &) { return true; };
  r.run(300);  // frames already on the air land
  JsonDocument s = w.house.status();
  CHECK(s["link_up"] == true);
  int32_t age = s["link"]["age_ms"] | -1;
  CHECK_IN(age, 300, HB_MS + 1000);
  uint32_t lastRx = w.now - (uint32_t)age;
  Found dn = waitFor(r, w.house, "link_down", h0, LINK_MS + 1000);
  CHECK(dn.ok);
  CHECK_IN(dn.e.t - lastRx, LINK_MS, LINK_MS + 2);
  CHECK(rel(dn.e.at, F) > 0);
  CHECK_EQ(countSince(w.house, "link_down", h0), 1);
  CHECK_EQ(countSince(w.house, "link_up", h0), 0);
  // The contact sensor stayed closed until then, and fails open with it
  CHECK_EQ(r.n(HK2), k2 + 1);
  CHECK(!r.e[HK2][k2].on);
  CHECK_EQ(r.e[HK2][k2].at, dn.e.at);
  CHECK(!w.house.coil(1));
  // The gate kept building a new STATUS every heartbeat_s (all lost on the way): 30 s apart, across F
  r.runTo(F + 60000);
  std::vector<uint32_t> hb = statusFirsts(w, t0);
  CHECK(hb.size() >= 4);
  bool across = false;
  for (size_t i = 1; i < hb.size(); i++) {
    CHECK_IN(hb[i] - hb[i - 1], HB_MS - 10, HB_MS + 300);
    across |= rel(hb[i - 1], F) < 0 && rel(hb[i], F) >= 0;
  }
  CHECK(across);
  // Back: the next heartbeat brings the link and the sensor back
  size_t h1 = w.house.logs.size(), k2b = r.n(HK2);
  w.drop = nullptr;
  Found up = waitFor(r, w.house, "link_up", h1, HB_MS + 2000);
  CHECK(up.ok);
  CHECK(r.until([&] { return w.sensorClosed(); }, 100));
  CHECK_EQ(r.n(HK2), k2b + 1);
  CHECK_EQ(r.e[HK2][k2b].at, up.e.at);
  checkAgree(w);
  CHECK(w.house.status()["link_up"] == true);
  w.checkClean();
}

// A gate jammed off its closed limit 30 s before F: the house holds K1 at the closed level for travel_timeout_s, which
// ends 30 s after F, then shows not-closed; the controller following K1 is a sync, not a command.
void wrapHold(uint32_t F) {
  World w(F - 50000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  r.runTo(F - 30500);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), k1 = r.n(HK1);
  w.opener.stuck = true;
  w.extPress(true, 300);
  Found hb = waitFor(r, w.house, "gate_state", h0, 3000, GS_BETWEEN_);
  CHECK(hb.ok);
  CHECK_EQ(hb.e.b, CAUSE_EXTERNAL_);
  CHECK(!w.sensorClosed());
  CHECK(rel(hb.e.at, F) < 0);
  CHECK(rel(hb.e.at + TRAVEL_MS, F) > 0);
  CHECK(r.until([&] { return r.n(HK1) > k1; }, TRAVEL_MS + 1000));
  CHECK(r.e[HK1][k1].on);
  CHECK_IN(r.e[HK1][k1].at - hb.e.at, TRAVEL_MS - 2, TRAVEL_MS + 5);
  CHECK(rel(r.e[HK1][k1].at, F) > 0);
  CHECK(r.until([&] { return w.alarmSwitch(); }, 500));
  r.run(SYNC_MS + 500);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(find(w.house, "sync", h0, 1).ok);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK(w.house.status()["gate"] == "between");
  // Freed, someone closes it
  size_t h1 = w.house.logs.size();
  w.opener.stuck = false;
  w.extPress(false, 300);
  CHECK(waitFor(r, w.house, "gate_state", h1, 3000, GS_CLOSED_).ok);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, 500));
  checkAgree(w);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  w.checkClean();
}

// K1's sync window opened ~1 s before F closes sync_window_ms later, after F (the controller is detached from SW for
// it, so no matching edge ends it early); the mismatch that leaves is resynced after mismatch_timeout_s.
void wrapWindow(uint32_t F) {
  World w(F - 90000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  w.shelly.mode = Shelly::DETACHED;
  r.runTo(F - 61600);
  size_t h0 = w.house.logs.size(), k1 = r.n(HK1);
  w.opener.stuck = true;
  w.extPress(true, 300);
  Found hb = waitFor(r, w.house, "gate_state", h0, 3000, GS_BETWEEN_);
  CHECK(hb.ok);
  CHECK(r.until([&] { return r.n(HK1) > k1; }, TRAVEL_MS + 1000));
  uint32_t K = r.e[HK1][k1].at;
  CHECK(r.e[HK1][k1].on);
  CHECK(rel(K, F) < 0);
  CHECK(rel(K + SYNC_MS, F) > 0);
  r.runTo(K + SYNC_MS - 10);
  CHECK(syncWindow(w));
  r.runTo(K + SYNC_MS + 10);
  CHECK(!syncWindow(w));
  CHECK(!w.alarmSwitch());
  // The controller follows SW again; K1 is on and it is off: put right after mismatch_timeout_s, counted from K
  w.shelly.mode = Shelly::FOLLOW;
  Found rs = waitFor(r, w.house, "resync", h0, MISMATCH_MS + 2000, 1);
  CHECK(rs.ok);
  CHECK_IN(rs.e.at - K, MISMATCH_MS - 2, MISMATCH_MS + 5);
  CHECK(r.until([&] { return w.alarmSwitch(); }, RESYNC_MS + 500));
  r.run(SYNC_MS + 1000);
  CHECK_EQ(countSince(w.house, "resync", h0), 1);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK(w.house.coil(1));
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  w.opener.stuck = false;
  w.extPress(false, 300);
  CHECK(r.until([&] { return w.houseSees() == GS_CLOSED_; }, 3000));
  CHECK(r.until([&] { return !w.alarmSwitch(); }, 500));
  checkAgree(w);
  w.checkClean();
}

// A controller power cut (house on its LiPo) that comes back ~3 s before F: its settle window runs ctrl_settle_ms +
// sync_window_ms, past F; a user edge inside it is a sync, and once it closes the controller is resynced at once.
void wrapPower(uint32_t F) {
  World w(F - 50000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  userMove(r, true);
  w.house.lipo = true;
  r.runTo(F - 6000);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.setRail12(false);
  Found dn = waitFor(r, w.house, "ctrl_power", h0, 1000, 0);
  CHECK(dn.ok);
  Found drop = waitFor(r, w.house, "ctrl", h0, 1000);
  CHECK(drop.ok);
  CHECK_EQ(drop.e.a, 0);
  CHECK_EQ(drop.e.b, 1);  // the relay dropping with the supply: ignored
  r.runTo(F - 3000);
  w.setRail12(true);
  Found up = waitFor(r, w.house, "ctrl_power", h0, 1000, 1);
  CHECK(up.ok);
  uint32_t P = up.e.at;
  CHECK(rel(P, F) < 0);
  CHECK(rel(P + SETTLE_MS, F) > 0);
  // The controller boots at K1's level: a sync
  Found boot = waitFor(r, w.house, "sync", h0, 3000, 1);
  CHECK(boot.ok);
  CHECK(w.alarmSwitch());
  // The user switches off inside the window: a sync too, never a command
  r.runTo(P + SETTLE_MS + 2000 - 51);
  size_t h1 = w.house.logs.size();
  w.user(false);
  Found s2 = waitFor(r, w.house, "sync", h1, 200, 0);
  CHECK(s2.ok);
  CHECK(rel(s2.e.at, F) > 0);
  r.runTo(P + SETTLE_MS + SYNC_MS - 10);
  CHECK(syncWindow(w));
  // The window closes on time; the controller, still out of step, is resynced at once (checkSoon)
  Found rs = waitFor(r, w.house, "resync", h0, 1000, 1);
  CHECK(rs.ok);
  CHECK_IN(rs.e.at - P, SETTLE_MS + SYNC_MS, SETTLE_MS + SYNC_MS + 3);
  CHECK(r.until([&] { return w.alarmSwitch(); }, RESYNC_MS + 500));
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl", h0, ANY, 0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK(w.opener.atOpen());
  // Then a switch is a command again
  CHECK(r.poll([&] { return !syncWindow(w); }, 6000, 20));
  userMove(r, false);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  w.checkClean();
}

// A CLOSE edge 249 ms before F waits ctrl_confirm_ms, to 251 ms after it.
void wrapConfirm(uint32_t F) {
  World w(F - 50000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  userMove(r, true);
  r.runTo(F - 300);
  size_t h0 = w.house.logs.size(), q0 = r.n(GK2);
  w.user(false);
  Found c = waitFor(r, w.house, "ctrl", h0, 200, 0, 0);
  CHECK(c.ok);
  CHECK(rel(c.e.t, F) < 0);
  Found s = waitFor(r, w.house, "cmd_sent", h0, CONFIRM_MS + 200, ACT_CLOSE_);
  CHECK(s.ok);
  CHECK_IN(s.e.t - c.e.t, CONFIRM_MS, CONFIRM_MS + 2);
  CHECK(rel(s.e.t, F) > 0);
  CHECK(r.until([&] { return w.opener.atClosed(); }, 15000));
  CHECK(r.until([&] { return w.houseSees() == GS_CLOSED_; }, 3000));
  std::vector<Pulse> p = r.pulses(GK2, q0);
  CHECK_EQ(p.size(), 1);
  CHECK_IN(p[0].len(), PULSE_MS, PULSE_MS + 1);
  checkAgree(w);
  w.checkClean();
}

// Glitches shorter than debounce_ms on every input that can have one, all starting in the last 50 ms before F, where
// the edge's stamp + debounce_ms lands past F (past the wrap at 2^32, so an unsigned `now >= since + debounce` would
// take the glitch at once): the controller's relay (house IN1) bouncing on for 20 ms, the IN2 opto
// dipping for 40 ms, and at the gate the closed limit dropping for 20 ms, IN3 (AC) for 35 ms and IN4 on for 40 ms.
// None of them is an edge: no command, no controller power change, no input or gate_state event, nothing moves.
void wrapGlitch(uint32_t F) {
  World w(F - 40000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  CHECK(rel(w.now, F - 1000) < 0);
  r.runTo(F - 50);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  size_t n0[NSIG];
  for (int i = 0; i < NSIG; i++) n0[i] = r.n(i);
  int pr = presses(w);
  for (int32_t t = -50; t < 30; t++) {  // world time F + t, set before the step that reaches F + t + 1
    bool relay = t >= -45 && t < -25, optoOff = t >= -30 && t < 10;
    bool limitOff = t >= -35 && t < -15, acOff = t >= -40 && t < -5, in4 = t >= -20 && t < 20;
    w.shelly.relay = relay;  // straight on the contact: not the user (the provenance monitor would flag a command)
    w.shelly.opto = !optoOff;
    w.shelly.optoAt = 0;
    w.opener.force[1] = limitOff ? 0 : -1;
    w.opener.force[2] = acOff ? 0 : -1;
    w.opener.in4 = in4;
    r.step();
  }
  r.run(5000);
  for (int i = 0; i < NSIG; i++) CHECK_EQ(r.n(i), n0[i] + (i == SW ? 2 : 0));  // the controller's bounce itself
  for (const char *ev : { "ctrl", "sync", "ctrl_power", "cmd_sent", "resync", "gate_state", "input" })
    if (countSince(w.house, ev, h0)) throw Failure(strf("house logged %s for a glitch shorter than debounce_ms", ev));
  for (const char *ev : { "input", "gate_state", "cmd_rx", "pulse" })
    if (countSince(w.gate, ev, g0)) throw Failure(strf("gate logged %s for a glitch shorter than debounce_ms", ev));
  CHECK_EQ(presses(w), pr);
  JsonDocument h = w.house.status(), gs = w.gate.status();
  CHECK(h["ctrl"] == false);
  CHECK(h["ctrl_power"] == true);
  CHECK(h["sync_window"] == false);
  CHECK(h["gate"] == "closed");
  CHECK(gs["gate"] == "closed");
  CHECK(gs["ac_power"] == true);
  CHECK(gs["io"]["in2"] == true);
  CHECK(gs["io"]["in3"] == true);
  CHECK(gs["io"]["in4"] == false);
  checkAgree(w);
  // The inputs still work after F: a cycle each way, one pulse each
  size_t p1 = r.n(GK1), p2 = r.n(GK2);
  userMove(r, true);
  userMove(r, false);
  CHECK_EQ(r.pulses(GK1, p1).size(), 1);
  CHECK_EQ(r.pulses(GK2, p2).size(), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 2);
  w.checkClean();
}

// The 24 V that wets the gate's inputs failing: the closed limit's opto drops 200 ms before IN3's (bench,
// 2026-10-06). The move into between that reads is held BETWEEN_HOLD_MS, and no_power comes within it: reported as
// no_power with no cause, never as a move. Coming back, IN3 returns a second before the limit: out of no_power the
// hold is BOOT_SETTLE_MS, so it reads closed again, never between. With `returning` the return (else the failure)
// falls just before F, where the hold's end (its start + the hold) would wrap.
void wrapInputSupply(uint32_t F, bool returning) {
  World w(F - 40000);
  Guard g{ w };
  freshMonitors(w);
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), k2 = r.n(HK2);
  int pr = presses(w);
  uint32_t failAt = returning ? w.now + 1000 : F - 300;
  CHECK(rel(w.now, failAt) < 0);
  r.runTo(failAt);
  w.opener.force[1] = 0;  // the closed limit's opto first
  r.runTo(failAt + 200);
  w.opener.force[2] = 0;  // then IN3's
  Found np = waitFor(r, w.gate, "gate_state", g0, 2000, GS_NO_POWER_);
  CHECK(np.ok);
  CHECK_IN(np.e.at - (failAt + 200), 50, 52);
  if (!returning) {
    CHECK(rel(failAt + 50 + 500, F) > 0);  // the hold, from the limit's debounced drop, ends after F
    CHECK(rel(np.e.at, F) < 0);
  }
  std::vector<SC> st = statesSince(w.gate, g0);
  CHECK_EQ(st.size(), 1);
  CHECK(st[0] == SC(GS_NO_POWER_, CAUSE_NONE_));
  Found hn = waitFor(r, w.house, "gate_state", h0, 1000, GS_NO_POWER_);
  CHECK(hn.ok);
  CHECK_EQ(hn.e.b, CAUSE_NONE_);
  CHECK_EQ(countSince(w.house, "gate_state", h0), 1);
  CHECK(r.until([&] { return w.house.coil(1) && !w.sensorClosed(); }, 100));  // no_power: shown not-closed
  CHECK_EQ(r.n(HK2), k2 + 1);
  r.run(5000);
  // Back: IN3 first, the limit a second later
  uint32_t backAt = returning ? F - 2000 : w.now + 1000;
  CHECK(rel(w.now, backAt) < 0);
  r.runTo(backAt);
  size_t h1 = w.house.logs.size(), g1 = w.gate.logs.size();
  w.opener.force[2] = -1;
  r.runTo(backAt + 1000);
  w.opener.force[1] = -1;
  Found cl = waitFor(r, w.gate, "gate_state", g1, 2000, GS_CLOSED_);
  CHECK(cl.ok);
  CHECK_IN(cl.e.at - (backAt + 1000), 50, 52);
  if (returning) {
    CHECK(rel(backAt + 50 + 3000, F) > 0);  // the hold out of no_power, from IN3's debounced return, ends after F
    CHECK(rel(cl.e.at, F) < 0);
  }
  st = statesSince(w.gate, g1);
  CHECK_EQ(st.size(), 1);
  CHECK(st[0] == SC(GS_CLOSED_, CAUSE_NONE_));
  CHECK(waitFor(r, w.house, "gate_state", h1, 1000, GS_CLOSED_).ok);
  CHECK_EQ(countSince(w.house, "gate_state", h1), 1);
  CHECK_EQ(countSince(w.house, "gate_state", h0, GS_BETWEEN_), 0);
  CHECK(r.poll([&] { return !syncWindow(w); }, 6000, 20));
  checkAgree(w);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK_EQ(presses(w), pr);
  w.checkClean();
}

// --- chaos -----------------------------------------------------------------------------------------------------------

// Every fault the site can have, at random (seeded), with the monitors on all the time.
struct Chaos {
  World &w;
  Rec &r;
  uint64_t s;
  uint32_t railBackAt = 0, gateBackAt = 0, acBackAt = 0, sirenEndAt = 0, burstEndAt = 0, nextAt, lastUserAt;
  int burstDir = 0, corruptLeft = 0;
  uint32_t houseBoots;
  std::map<std::string, int> n;

  Chaos(World &w, Rec &r, uint64_t seed) : w(w), r(r), s(seed * 0x9E3779B97F4A7C15ULL + 1), nextAt(w.now + 1000),
                                           lastUserAt(w.now), houseBoots(w.house.boots) {
    w.drop = [this](const AirFrame &f) {
      if (!burstEndAt) return false;
      return burstDir == 0 || (burstDir == 1 && f.from == 0) || (burstDir == 2 && f.from == 1);
    };
    w.corrupt = [this](const AirFrame &) {
      if (corruptLeft <= 0) return false;
      corruptLeft--;
      n["corrupted"]++;
      return true;
    };
  }
  uint32_t rnd(uint32_t m) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return (uint32_t)((s >> 11) % m);
  }
  uint32_t range(uint32_t lo, uint32_t hi) { return lo + rnd(hi - lo + 1); }
  bool due(uint32_t t) const { return t && rel(w.now, t) >= 0; }
  void note(const std::string &what) {
    n[what]++;
    w.trace.add(w.now, "chaos: " + what);
  }

  void tick() {
    if (due(railBackAt)) {
      railBackAt = 0;
      w.setRail12(true);
    }
    if (due(gateBackAt)) {
      gateBackAt = 0;
      w.gate.cut = false;
      w.trace.add(w.now, "chaos: gate feed back");
    }
    if (due(acBackAt)) {
      acBackAt = 0;
      w.opener.ac = true;
      w.trace.add(w.now, "chaos: AC back");
    }
    if (due(sirenEndAt)) {
      sirenEndAt = 0;
      w.opener.extOpenHold = false;
      w.trace.add(w.now, "chaos: siren released");
    }
    if (due(burstEndAt)) {
      burstEndAt = 0;
      w.trace.add(w.now, "chaos: RF back");
    }
    // Harness: command ids start at random at every house boot, so two boots' ids can coincide; the per-id pulse
    // count must not add a later boot's command to an earlier one's (the gate forgets its last id on a new session).
    if (w.house.boots != houseBoots) {
      houseBoots = w.house.boots;
      w.pulsesPerCmd.clear();
    }
    if (!due(nextAt)) return;
    nextAt = w.now + range(1000, 40000);
    event();
  }

  void event() {
    uint32_t x = rnd(100);
    if (x < 28) {
      if (w.now - lastUserAt < 3000) return;  // nobody flips the Alarm.com switch faster than that
      lastUserAt = w.now;
      bool on = rnd(5) ? !w.alarmSwitch() : rnd(2) == 1;
      note(on ? "user on" : "user off");
      w.user(on);
    } else if (x < 40) {
      bool open = rnd(2);
      note(open ? "external OPEN" : "external CLOSE");
      w.extPress(open, range(100, 800));
    } else if (x < 45) {
      if (sirenEndAt) return;
      note("siren holds OPEN");
      w.opener.extOpenHold = true;
      sirenEndAt = (w.now + range(5000, 120000)) | 1;
    } else if (x < 52) {
      if (acBackAt) return;
      note("AC lost");
      w.opener.ac = false;
      acBackAt = (w.now + range(2000, 300000)) | 1;
    } else if (x < 62) {
      if (railBackAt) return;
      w.house.lipo = rnd(2);
      uint32_t ms = rnd(10) < 4 ? range(50, 700) : range(1000, 90000);
      note(strf("12 V %s, house LiPo %s", ms < 1000 ? "dip" : "cut", w.house.lipo ? "in" : "out"));
      w.setRail12(false);
      railBackAt = (w.now + ms) | 1;
    } else if (x < 68) {
      if (gateBackAt) return;
      w.gate.lipo = rnd(10) < 3;
      note(strf("gate feed cut, LiPo %s", w.gate.lipo ? "in" : "out"));
      w.gate.cut = true;
      gateBackAt = (w.now + range(200, 90000)) | 1;
    } else if (x < 78) {
      Board &b = rnd(2) ? w.gate : w.house;
      if (!b.running()) return;
      static const uint8_t rc[] = { PM_RCAUSE_SYST, PM_RCAUSE_WDT, PM_RCAUSE_EXT };
      note(b.name + " reset");
      b.reset(rc[rnd(3)]);
    } else if (x < 88) {
      if (burstEndAt) return;
      burstDir = rnd(3);
      note(burstDir == 0 ? "RF lost" : burstDir == 1 ? "RF lost house->gate" : "RF lost gate->house");
      burstEndAt = (w.now + (rnd(10) < 7 ? range(500, 20000) : range(20000, 150000))) | 1;
    } else if (x < 91) {
      note("corrupt frames");
      corruptLeft += range(1, 3);
    }
  }

  // Everything back to normal.
  void restore() {
    railBackAt = gateBackAt = acBackAt = sirenEndAt = burstEndAt = 0;
    corruptLeft = 0;
    w.drop = nullptr;
    w.corrupt = nullptr;
    w.setRail12(true);
    w.gate.cut = false;
    w.opener.ac = true;
    w.opener.extOpenHold = false;
  }
};

// `hours` of chaos, then everything restored and a few quiet minutes. With the controller set up as documented
// (FOLLOW: "contact closed = ON") the Alarm.com switch, K1 and the contact sensor must then agree with the real gate.
// A controller misconfigured to toggle on SW edges can't be put back by a resync (two K1 edges, two toggles), so then
// only the house's own outputs are required to agree; the monitors (no command without the user) hold either way.
void chaos(uint64_t seed, uint32_t hours, Shelly::Mode mode = Shelly::FOLLOW,
           World::GateFeed feed = World::FEED_FIXED) {
  World w;
  Guard g{ w };
  w.shelly.mode = mode;
  w.gateFeed = feed;
  w.commission();
  Rec r(w);
  Chaos c(w, r, seed);
  r.each = [&] { c.tick(); };
  r.run(hours * HOUR);
  r.each = nullptr;
  c.restore();
  // Quiet for a few minutes: everything that was left out of step is put right (mismatch_timeout_s, travel, link)
  r.run(6 * 60000);
  std::string sum;
  for (auto &kv : c.n) sum += strf("%s%s %d", sum.empty() ? "" : ", ", kv.first.c_str(), kv.second);
  printf("      seed %llu: %s; house boots %u, gate boots %u, commands %d, gate pulses %d, presses %d\n",
         (unsigned long long)seed, sum.c_str(), w.house.boots, w.gate.boots, w.house.count("cmd_sent"),
         w.gate.count("pulse"), presses(w));
  CHECK(w.opener.atOpen() || w.opener.atClosed());
  CHECK(w.house.running() && w.gate.running());
  CHECK(w.houseLinkUp());
  if (mode != Shelly::FOLLOW) CHECK(r.poll([&] { return w.house.status()["resyncing"] == false; }, 3000, 10));
  JsonDocument h = w.house.status(), gs = w.gate.status();
  bool closed = w.opener.atClosed();
  CHECK(gs["gate"] == (closed ? "closed" : "open"));
  CHECK(h["gate"] == (closed ? "closed" : "open"));
  CHECK(h["link_up"] == true);
  CHECK(h["armed"] == true);
  CHECK(h["cmd_pending"] == false);
  CHECK(h["resyncing"] == false);
  if (mode == Shelly::FOLLOW) {
    checkAgree(w);
  } else {
    CHECK_EQ(w.houseSees(), w.gateTruth());
    CHECK_EQ(w.house.coil(1), !closed);
    CHECK_EQ(w.sensorClosed(), closed);
  }
  w.checkClean();
}

}  // namespace

// =====================================================================================================================
// [wrap-safe]: the millis() wrap (2^32) and the signed flip (2^31)

// [wrap-safe]
TEST(robustness_wrap_2_32_boot_link_and_settle_window) {
  wrapBoot(F32);
}

// [wrap-safe]
TEST(robustness_wrap_2_31_boot_link_and_settle_window) {
  wrapBoot(F31);
}

// [wrap-safe] [pulse-only]
TEST(robustness_wrap_2_32_command_pulse_and_travel) {
  wrapPulse(F32);
}

// [wrap-safe] [pulse-only]
TEST(robustness_wrap_2_31_command_pulse_and_travel) {
  wrapPulse(F31);
}

// [wrap-safe] [sensor-closed-only-known]
TEST(robustness_wrap_2_32_link_loss_heartbeats_and_recovery) {
  wrapLink(F32);
}

// [wrap-safe] [sensor-closed-only-known]
TEST(robustness_wrap_2_31_link_loss_heartbeats_and_recovery) {
  wrapLink(F31);
}

// [wrap-safe] [travel-hold]
TEST(robustness_wrap_2_32_travel_hold_expiry) {
  wrapHold(F32);
}

// [wrap-safe] [travel-hold]
TEST(robustness_wrap_2_31_travel_hold_expiry) {
  wrapHold(F31);
}

// [wrap-safe] [sync-window]
TEST(robustness_wrap_2_32_k1_sync_window_and_mismatch) {
  wrapWindow(F32);
}

// [wrap-safe] [sync-window]
TEST(robustness_wrap_2_31_k1_sync_window_and_mismatch) {
  wrapWindow(F31);
}

// [wrap-safe] [ctrl-power] [settle-window]
TEST(robustness_wrap_2_32_controller_power_cut_and_settle_window) {
  wrapPower(F32);
}

// [wrap-safe] [ctrl-power] [settle-window]
TEST(robustness_wrap_2_31_controller_power_cut_and_settle_window) {
  wrapPower(F31);
}

// [wrap-safe] [ctrl-power]
TEST(robustness_wrap_2_32_close_confirm_wait) {
  wrapConfirm(F32);
}

// [wrap-safe] [ctrl-power]
TEST(robustness_wrap_2_31_close_confirm_wait) {
  wrapConfirm(F31);
}

// [wrap-safe] [pull-down] [ctrl-power]
TEST(robustness_wrap_2_32_input_glitches_shorter_than_debounce_ignored) {
  wrapGlitch(F32);
}

// [wrap-safe] [pull-down] [ctrl-power]
TEST(robustness_wrap_2_31_input_glitches_shorter_than_debounce_ignored) {
  wrapGlitch(F31);
}

// [wrap-safe] [ac-power]
TEST(robustness_wrap_2_32_input_supply_failing_reads_no_power_not_a_move) {
  wrapInputSupply(F32, false);
}

// [wrap-safe] [ac-power]
TEST(robustness_wrap_2_31_input_supply_failing_reads_no_power_not_a_move) {
  wrapInputSupply(F31, false);
}

// [wrap-safe] [ac-power]
TEST(robustness_wrap_2_32_input_supply_returning_reads_closed_not_a_move) {
  wrapInputSupply(F32, true);
}

// [wrap-safe] [ac-power]
TEST(robustness_wrap_2_31_input_supply_returning_reads_closed_not_a_move) {
  wrapInputSupply(F31, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// [wrap-safe]: long quiet stretches

// [wrap-safe]
TEST(robustness_quiet_2_hours_gate_open_nothing_moves) {
  // Real time, every millisecond: the link stays up on the heartbeats alone, and nothing else happens.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  userMove(r, true);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), e0 = w.house.events.size();
  size_t n0[NSIG];
  for (int i = 0; i < NSIG; i++) n0[i] = r.n(i);
  r.run(2 * HOUR);
  for (int i = 0; i < NSIG; i++) CHECK_EQ(r.n(i), n0[i]);
  for (const char *ev : { "link_down", "link_up", "gate_state", "resync", "ctrl", "sync", "cmd_sent", "tx_giveup",
                          "ctrl_power", "session" })
    if (countSince(w.house, ev, h0)) throw Failure(strf("house logged %s in the quiet", ev));
  for (const char *ev : { "gate_state", "pulse", "cmd_rx", "tx_giveup", "session", "input" })
    if (countSince(w.gate, ev, g0)) throw Failure(strf("gate logged %s in the quiet", ev));
  // One STATUS each heartbeat_s, all received
  std::vector<uint32_t> up;
  for (size_t i = e0; i < w.house.events.size(); i++)
    if (w.house.events[i]["event"] == "status") up.push_back(w.house.events[i]["status"]["uptime_ms"] | 0u);
  CHECK_IN(up.size(), 2 * HOUR / HB_MS - 1, 2 * HOUR / HB_MS + 1);
  for (size_t i = 1; i < up.size(); i++) CHECK_IN(up[i] - up[i - 1], HB_MS - 10, HB_MS + 100);
  JsonDocument h = w.house.status();
  CHECK(h["link_up"] == true);
  CHECK(h["gate"] == "open");
  CHECK_IN(h["link"]["age_ms"] | -1, 0, HB_MS + 1000);
  checkAgree(w);
  w.checkClean();
}

// [wrap-safe] [sensor-closed-only-known] [sync-window]
TEST(robustness_quiet_26_days_link_down_stays_down_and_stamps_stay_old) {
  // RF lost for 26 days (past 2^31 ms): the house's last-frame stamp and its settle stamp must not read as fresh
  // again. The link stays down (the sensor open) throughout; once RF returns, a controller edge matching K1 still ends
  // the sync window at once, as it does after any settle window long past.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), k2 = r.n(HK2);
  CHECK(w.sensorClosed());
  w.drop = [](const AirFrame &) { return true; };
  quiet(r, 26ull * DAY, HOUR, 200, [&] {
    CHECK(!w.houseLinkUp());
    CHECK(!w.sensorClosed());
    CHECK(!w.house.coil(1));
  });
  CHECK_EQ(countSince(w.house, "link_down", h0), 1);
  CHECK_EQ(countSince(w.house, "link_up", h0), 0);
  CHECK_EQ(r.ons(HK2, k2), 0);
  // Each hop closes a history bucket, and its write to the flash stops the loop ~0.5 s (a status read meanwhile would
  // see the board's clock past its last loop pass): let the last one end.
  r.run(1000);
  JsonDocument h = w.house.status(), gs = w.gate.status();
  CHECK(h["link_up"] == false);
  CHECK_IN(h["link"]["age_ms"] | -1, AGE_CAP, AGE_CAP + 5);  // held at the cap, not wrapped
  CHECK_IN(gs["link"]["age_ms"] | -1, AGE_CAP, AGE_CAP + 5);
  CHECK(h["sync_window"] == false);
  // RF back: the next heartbeat brings the link and the sensor back
  size_t h1 = w.house.logs.size();
  w.drop = nullptr;
  CHECK(waitFor(r, w.house, "link_up", h1, HB_MS + 2000).ok);
  CHECK(r.until([&] { return w.sensorClosed(); }, 100));
  checkAgree(w);
  // Someone opens it: K1 follows, the controller's matching edge ends the window, and the user's switch a second
  // later is a command
  size_t h2 = w.house.logs.size(), k1 = r.n(HK1);
  w.extPress(true, 300);
  CHECK(r.until([&] { return r.n(HK1) > k1; }, 12000));
  uint32_t K = r.e[HK1][k1].at;
  CHECK(r.until([&] { return w.alarmSwitch(); }, 200));
  Found s = waitFor(r, w.house, "sync", h2, 200, 1);
  CHECK(s.ok);
  r.runTo(K + 1000);
  CHECK(!syncWindow(w));
  w.user(false);
  Found c = waitFor(r, w.house, "cmd_sent", h2, 1000, ACT_CLOSE_);
  CHECK(c.ok);
  CHECK(find(w.house, "ctrl", h2, 0, 0).ok);
  CHECK(r.until([&] { return w.opener.atClosed(); }, 15000));
  CHECK(r.until([&] { return w.houseSees() == GS_CLOSED_; }, 3000));
  checkAgree(w);
  w.checkClean();
}

// [wrap-safe] [travel-hold]
TEST(robustness_quiet_26_days_jammed_between_keeps_showing_not_closed) {
  // A gate jammed off its closed limit for 26 days: once travel_timeout_s is over K1 shows not-closed, and the hold's
  // start must not read as recent again when it is 2^31 ms old (K1 would fall back to the closed level).
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size();
  w.opener.stuck = true;
  w.extPress(true, 300);
  Found hb = waitFor(r, w.house, "gate_state", h0, 3000, GS_BETWEEN_);
  CHECK(hb.ok);
  CHECK(r.until([&] { return w.house.coil(1); }, TRAVEL_MS + 1000));
  CHECK(r.until([&] { return w.alarmSwitch(); }, 500));
  r.run(SYNC_MS + 500);
  size_t k1 = r.n(HK1), sw = r.n(SW), k2 = r.n(HK2);
  quiet(r, 26ull * DAY, HOUR, 200, [&] {
    CHECK(w.house.coil(1));
    CHECK(w.alarmSwitch());
    CHECK(!w.sensorClosed());
  });
  CHECK_EQ(r.n(HK1), k1);
  CHECK_EQ(r.n(SW), sw);
  CHECK_EQ(r.n(HK2), k2);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK(w.house.status()["gate"] == "between");
  // Freed and closed by someone: the house follows
  w.opener.stuck = false;
  w.extPress(false, 300);
  CHECK(r.until([&] { return w.houseSees() == GS_CLOSED_; }, 3000));
  CHECK(r.until([&] { return !w.alarmSwitch(); }, 500));
  checkAgree(w);
  w.checkClean();
}

// [wrap-safe] [resync]
TEST(robustness_quiet_26_days_mismatch_resynced_as_soon_as_the_link_returns) {
  // The controller out of step with K1 when RF is lost (mismatch_timeout_s 600, longer than the link timeout). 26
  // days later RF returns: the mismatch has lasted far past the timeout, so the resync comes at once, not 2^31 ms
  // after its stamp turned negative.
  World w;
  Guard g{ w };
  w.commission([](Board &b) {
    if (b.idx == 0) CHECK(b.set("mismatch_timeout_s", 600));
  });
  Rec r(w);
  w.shelly.mode = Shelly::DETACHED;
  size_t h0 = w.house.logs.size();
  w.extPress(true, 300);
  CHECK(r.until([&] { return w.house.coil(1); }, 12000));
  CHECK(!w.alarmSwitch());
  r.run(10000);
  w.drop = [](const AirFrame &) { return true; };
  CHECK(waitFor(r, w.house, "link_down", h0, LINK_MS + 1000).ok);
  quiet(r, 26ull * DAY, HOUR, 200);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  w.shelly.mode = Shelly::FOLLOW;
  size_t h1 = w.house.logs.size();
  w.drop = nullptr;
  Found up = waitFor(r, w.house, "link_up", h1, HB_MS + 2000);
  CHECK(up.ok);
  Found rs = waitFor(r, w.house, "resync", h1, 2000, 1);
  CHECK(rs.ok);
  CHECK_IN(rs.e.at - up.e.at, 0, 3);
  CHECK(r.until([&] { return w.alarmSwitch(); }, RESYNC_MS + 500));
  r.run(SYNC_MS + 1000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  checkAgree(w);
  w.checkClean();
}

// Both boards up for `days` with the link working, then one reboots: the link must come back. It restarts once
// at the start too, so its peer has answered a HELLO from a session it hadn't verified (and stamped when: link.cpp's
// HELLO_ANSWER_GAP_MS check), as it would have after any restart in the field.
static void relinkAfterQuiet(bool houseReboots, uint32_t days) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  Board &b = houseReboots ? w.house : w.gate, &peer = houseReboots ? w.gate : w.house;
  size_t l0 = b.logs.size();
  b.reset(PM_RCAUSE_SYST);
  CHECK(waitFor(r, b, "boot", l0, 2000).ok);
  settled(r);
  checkAgree(w);
  quiet(r, (uint64_t)days * DAY, HOUR, 500);
  CHECK(r.until([&] { return w.houseLinkUp(); }, HB_MS + 2000));
  checkAgree(w);
  l0 = b.logs.size();
  uint32_t heard0 = peer.rxDone;
  b.reset(PM_RCAUSE_SYST);
  Found boot = waitFor(r, b, "boot", l0, 2000);
  CHECK(boot.ok);
  bool linked = r.poll([&] {
    return w.house.running() && w.gate.running() && w.house.status()["link"]["verified"] == true
           && w.gate.status()["link"]["verified"] == true;
  }, 60000, 100);
  if (!linked) {
    auto sentSince = [&](const Board &from, uint8_t type) {
      int k = 0;
      for (const AirFrame *f : w.sent(from, type)) k += rel(f->start, boot.e.at) > 0 && !f->dropped && !f->corrupt;
      return k;
    };
    throw Failure(strf("no link 60 s after the %s rebooted (%u days up): it sent %d HELLOs, all heard by the %s (%u "
                       "frames received), which sent %d HELLO_ACKs and %d HELLOs back",
                       b.name.c_str(), days, sentSince(b, MSG_HELLO_), peer.name.c_str(), peer.rxDone - heard0,
                       sentSince(peer, MSG_HELLO_ACK_), sentSince(peer, MSG_HELLO_)));
  }
  settled(r);
  checkAgree(w);
  w.checkClean();
}

// [wrap-safe] [restarts]
TEST(robustness_quiet_20_days_then_house_reboot_relinks) {
  // The control: 20 days (stamps under 2^31 ms old), the rebooted house's HELLO is answered as usual.
  relinkAfterQuiet(true, 20);
}

// [wrap-safe] [restarts]
TEST(robustness_quiet_20_days_then_gate_reboot_relinks) {
  relinkAfterQuiet(false, 20);
}

// [wrap-safe] [restarts]
TEST(robustness_quiet_26_days_then_house_reboot_relinks) {
  relinkAfterQuiet(true, 26);
}

// [wrap-safe] [restarts]
TEST(robustness_quiet_26_days_then_gate_reboot_relinks) {
  relinkAfterQuiet(false, 26);
}

// =====================================================================================================================
// [pull-down]

// [pull-down]
TEST(robustness_pulldown_every_input_pulled_down_and_never_inverted) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  static const char *const inv[] = { "in1_invert", "in2_invert", "in3_invert", "in4_invert" };
  for (Board *b : { &w.house, &w.gate }) {
    for (int pin = P_IN1; pin <= P_IN4; pin++)
      if (b->mode[pin] != INPUT_PULLDOWN) throw Failure(strf("%s: pin %d mode %d", b->name.c_str(), pin, b->mode[pin]));
    JsonDocument c = req(r, *b, "config.get");
    CHECK(c["ok"] == true);
    for (const char *p : inv) {
      CHECK_EQ(b->get(p), 0);
      CHECK_EQ(c["params"][p] | -1, 0);
      bool seen = false;
      for (JsonVariantConst m : c["meta"].as<JsonArrayConst>()) {
        if (m["name"] != p) continue;
        seen = true;
        CHECK(m["remote"] == false);  // never writable over the radio
      }
      CHECK(seen);
    }
  }
  // Unwired inputs read inactive (house IN3/IN4, gate IN4)
  JsonDocument hs = w.house.status(), gs = w.gate.status();
  CHECK(hs["io"]["in3"] == false);
  CHECK(hs["io"]["in4"] == false);
  CHECK(gs["io"]["in4"] == false);
  // The house refuses to send a remote write of them; the gate's DIAG doesn't list them
  for (const char *p : inv) {
    JsonDocument d = req(r, w.house, "remote.set", strf("\"name\":\"%s\",\"value\":1", p));
    CHECK(d["ok"] == false);
  }
  size_t e0 = w.house.events.size();
  CHECK(req(r, w.house, "remote.diag")["ok"] == true);
  CHECK(r.until([&] { return eventSince(w.house, "remote_diag", e0) != nullptr; }, 3000));
  const JsonDocument &d = *eventSince(w.house, "remote_diag", e0);
  CHECK_EQ(d["params"]["pulse_ms"] | -1, PULSE_MS);
  for (const char *p : inv) CHECK(d["params"][p].isNull());
  r.run(2000);
  CHECK_EQ(w.gate.count("cfg_remote"), 0);
  // After a reset and after a power cycle the boot sets them up the same way
  w.gate.reset(PM_RCAUSE_WDT);
  w.setRail12(false);
  r.run(2000);
  w.setRail12(true);
  settled(r);
  for (Board *b : { &w.house, &w.gate }) {
    for (int pin = P_IN1; pin <= P_IN4; pin++) CHECK_EQ(b->mode[pin], INPUT_PULLDOWN);
    for (const char *p : inv) CHECK_EQ(b->get(p), 0);
  }
  checkAgree(w);
  w.checkClean();
}

// [pull-down] [state-from-limits] [unknown-shows-open]
TEST(robustness_pulldown_dead_closed_limit_opto_never_reads_closed) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), k1 = r.n(HK1), k2 = r.n(HK2);
  // The closed limit's opto dies (or its wire is cut) with the gate closed: through the pull-down it reads inactive
  w.opener.force[1] = 0;
  Found gb = waitFor(r, w.gate, "gate_state", g0, 2000, GS_BETWEEN_);
  CHECK(gb.ok);
  CHECK_EQ(gb.e.b, CAUSE_EXTERNAL_);
  CHECK(w.opener.atClosed());  // it never moved
  Found hb = waitFor(r, w.house, "gate_state", h0, 2000, GS_BETWEEN_);
  CHECK(hb.ok);
  CHECK(!w.sensorClosed());
  CHECK_EQ(r.n(HK2), k2 + 1);
  CHECK_EQ(r.e[HK2][k2].at, hb.e.at);
  // K1 holds the closed level for the travel timeout (it may be a slow move), then shows not-closed
  CHECK(r.until([&] { return r.n(HK1) > k1; }, TRAVEL_MS + 1000));
  CHECK_IN(r.e[HK1][k1].at - hb.e.at, TRAVEL_MS - 2, TRAVEL_MS + 5);
  CHECK(r.until([&] { return w.alarmSwitch(); }, 500));
  JsonDocument gs = w.gate.status();
  CHECK(gs["gate"] == "between");
  CHECK(gs["io"]["in2"] == false);
  // Rebooted with the dead opto, the gate's first report is between, not closed
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK(r.poll([&] { return w.gate.running() && w.gate.status()["settling"] == false; }, 15000));
  CHECK(w.gate.status()["gate"] == "between");
  r.run(20000);
  // AC lost as well: nothing reads, no_power, still shown not-closed
  w.opener.ac = false;
  CHECK(waitFor(r, w.house, "gate_state", h0, 5000, GS_NO_POWER_).ok);
  r.run(1000);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(w.alarmSwitch());
  // Never closed all along, at the gate or at the house, and the sensor never closed
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_CLOSED_), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0, GS_CLOSED_), 0);
  CHECK_EQ(r.ons(HK2, k2), 0);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(presses(w), 0);
  // The opto (and AC) back: closed again
  size_t h1 = w.house.logs.size();
  w.opener.force[1] = -1;
  w.opener.ac = true;
  CHECK(waitFor(r, w.house, "gate_state", h1, 5000, GS_CLOSED_).ok);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, 500));
  checkAgree(w);
  w.checkClean();
}

// [pull-down] [state-from-limits]
TEST(robustness_pulldown_dead_open_limit_opto_never_reads_open) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  userMove(r, true);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), k1 = r.n(HK1);
  w.opener.force[0] = 0;
  Found gb = waitFor(r, w.gate, "gate_state", g0, 2000, GS_BETWEEN_);
  CHECK(gb.ok);
  CHECK_EQ(gb.e.b, CAUSE_EXTERNAL_);
  CHECK(w.opener.atOpen());
  CHECK(waitFor(r, w.house, "gate_state", h0, 2000, GS_BETWEEN_).ok);
  r.run(TRAVEL_MS + 5000);
  CHECK_EQ(r.n(HK1), k1);  // K1 stayed on: the open level, then not-closed
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  // The user closes it: the closed limit works
  userMove(r, false);
  // and opens it: the gate moves, but without its open limit it never reads open, and the travel times out
  size_t g1 = w.gate.logs.size(), p1 = r.n(GK1);
  w.user(true);
  CHECK(r.until([&] { return w.opener.atOpen(); }, 15000));
  Found tt = waitFor(r, w.gate, "travel_timeout", g1, TRAVEL_MS + 2000, GS_OPEN_);
  CHECK(tt.ok);
  CHECK(w.gate.status()["last_result"] == "timeout");
  CHECK(r.until([&] { return w.house.coil(1); }, 5000));
  CHECK(w.alarmSwitch());
  CHECK(!w.sensorClosed());
  CHECK_EQ(r.pulses(GK1, p1).size(), 1);
  CHECK_EQ(countSince(w.gate, "gate_state", g0, GS_OPEN_), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0, GS_OPEN_), 0);
  CHECK_EQ(countSince(w.house, "gate_state", h0, GS_BETWEEN_), 2);
  // The opto back: open
  size_t h1 = w.house.logs.size();
  w.opener.force[0] = -1;
  CHECK(waitFor(r, w.house, "gate_state", h1, 3000, GS_OPEN_).ok);
  r.run(5000);
  checkAgree(w);
  w.checkClean();
}

// [pull-down] [ac-power]
TEST(robustness_pulldown_dead_ac_sense_opto_refuses_commands) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  // IN3's opto dies: it reads AC lost (the safe side), and the closed limit is still trusted
  w.opener.force[2] = 0;
  Found in = waitFor(r, w.gate, "input", g0, 1000, 3);
  CHECK(in.ok);
  CHECK_EQ(in.e.b, 0);
  r.run(500);
  JsonDocument gs = w.gate.status(), hs = w.house.status();
  CHECK(gs["ac_power"] == false);
  CHECK(gs["gate"] == "closed");
  CHECK(hs["remote"]["ac_power"] == false);
  CHECK(hs["gate"] == "closed");
  CHECK(w.sensorClosed());
  // A command is refused without a pulse, and the controller is put back at once
  w.user(true);
  Found ref = waitFor(r, w.gate, "cmd_refused", g0, 2000, ACT_OPEN_);
  CHECK(ref.ok);
  Found rs = waitFor(r, w.house, "resync", h0, 3000, 0);
  CHECK(rs.ok);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, RESYNC_MS + 500));
  CHECK(w.house.status()["cmd_result"] == RES_NO_POWER_);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK_EQ(presses(w), 0);
  CHECK_EQ(r.n(GK1) + r.n(GK2), 0);
  // The whole opto board unpowered (every input dark): no_power, shown not-closed
  w.opener.force[0] = w.opener.force[1] = 0;
  CHECK(waitFor(r, w.house, "gate_state", h0, 5000, GS_NO_POWER_).ok);
  r.run(1000);
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  for (int i = 0; i < 3; i++) w.opener.force[i] = -1;
  CHECK(r.until([&] { return w.houseSees() == GS_CLOSED_; }, 5000));
  r.run(5000);
  checkAgree(w);
  w.checkClean();
}

// [pull-down] [ctrl-power]
TEST(robustness_pulldown_dead_controller_power_opto_blocks_commands) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  // The IN2 opto dies with the controller powered: it reads unpowered
  uint32_t t0 = w.now;
  w.shelly.opto = false;
  w.shelly.optoAt = (w.now + 1000000000u) | 1;
  Found off = waitFor(r, w.house, "ctrl_power", h0, 500, 0);
  CHECK(off.ok);
  CHECK_IN(off.e.at - t0, 50, 53);
  // The user's switch is ignored, and nothing is resynced while it reads so
  w.user(true);
  Found c = waitFor(r, w.house, "ctrl", h0, 500);
  CHECK(c.ok);
  CHECK_EQ(c.e.a, 1);
  CHECK_EQ(c.e.b, 1);
  r.run(MISMATCH_MS + 10000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "resync", h0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(presses(w), 0);
  CHECK(w.alarmSwitch());
  // The opto back: after the settle window the controller is put back at once
  w.shelly.optoAt = (w.now + 1) | 1;
  Found on = waitFor(r, w.house, "ctrl_power", h0, 500, 1);
  CHECK(on.ok);
  Found rs = waitFor(r, w.house, "resync", h0, SETTLE_MS + SYNC_MS + 1000, 0);
  CHECK(rs.ok);
  CHECK_IN(rs.e.at - on.e.at, SETTLE_MS + SYNC_MS, SETTLE_MS + SYNC_MS + 3);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, RESYNC_MS + 500));
  r.run(SYNC_MS + 1000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  checkAgree(w);
  w.checkClean();
}

// =====================================================================================================================
// [restarts]

// [restarts] [pulse-only]
TEST(robustness_restart_gate_warm_reset_after_pulse_before_its_ack_never_pulses_again) {
  // debug.reboot_after_cmd: the gate pulses for the command, then resets before ACKing it (as a power cut would).
  // The house's retries must not run it again on the restarted gate: its HELLO holds the command, and the new
  // session verifying drops it.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  for (int dir = 0; dir < 2; dir++) {
    bool open = dir == 0;
    int k = open ? GK1 : GK2, other = open ? GK2 : GK1;
    if (!open) CHECK(r.poll([&] { return !syncWindow(w); }, 6000, 20));
    CHECK(req(r, w.gate, "debug.reboot_after_cmd")["ok"] == true);
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), p0 = r.n(k), q0 = r.n(other);
    size_t pr = w.opener.presses[open ? 0 : 1].size(), po = w.opener.presses[open ? 1 : 0].size();
    uint32_t boots = w.gate.boots;
    w.user(open);
    Found sent = waitFor(r, w.house, "cmd_sent", h0, 2000, open ? ACT_OPEN_ : ACT_CLOSE_);
    CHECK(sent.ok);
    Found rx = waitFor(r, w.gate, "cmd_rx", g0, 2000);
    CHECK(rx.ok);
    CHECK_EQ(rx.e.b, sent.e.b);
    Found boot = waitFor(r, w.gate, "boot", g0, 3000);
    CHECK(boot.ok);
    CHECK_EQ(boot.e.a, PM_RCAUSE_SYST);
    CHECK_IN(boot.e.at - rx.e.at, PULSE_MS + 100 + 500, PULSE_MS + 100 + 520);
    CHECK_EQ(w.gate.boots, boots + 1);
    // The house never got the ACK: its command is held on the restarted gate's HELLO, then dropped
    Found hold = waitFor(r, w.house, "cmd_hold", h0, 10000, 1);
    CHECK(hold.ok);
    Found drop = waitFor(r, w.house, "cmd_hold", h0, 10000, 2);
    CHECK(drop.ok);
    Found dropped = waitFor(r, w.house, "cmd_dropped", h0, 1000);
    CHECK(dropped.ok);
    CHECK_EQ(dropped.e.b, sent.e.b);
    // The gate gets there on its own and reports it; nothing pulses again
    CHECK(r.until([&] { return open ? w.opener.atOpen() : w.opener.atClosed(); }, 15000));
    CHECK(r.until([&] { return w.houseSees() == (open ? GS_OPEN_ : GS_CLOSED_); }, 15000));
    r.run(TTL_MS + 10000);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
    CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 1);
    std::vector<Pulse> p = r.pulses(k, p0);
    CHECK_EQ(p.size(), 1);
    CHECK_IN(p[0].len(), PULSE_MS, PULSE_MS + 1);
    CHECK_EQ(r.n(other), q0);
    CHECK_EQ(w.opener.presses[open ? 0 : 1].size(), pr + 1);
    CHECK_EQ(w.opener.presses[open ? 1 : 0].size(), po);
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
    JsonDocument h = w.house.status();
    CHECK(h["cmd_pending"] == false);
    CHECK(h["cmd_result"] == -2);
    checkAgree(w);
  }
  w.checkClean();
}

// [restarts] [pulse-only]
TEST(robustness_restart_gate_power_cut_after_pulse_ack_lost_never_pulses_again) {
  // The gate's ACK is lost on the air (its re-ACKs too), and right after the pulse its supply is cut (no LiPo: it
  // dies 0.6 s later). Back 2 s later, it must not run the command again.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  for (int dir = 0; dir < 2; dir++) {
    bool open = dir == 0;
    int k = open ? GK1 : GK2;
    if (!open) CHECK(r.poll([&] { return !syncWindow(w); }, 6000, 20));
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), p0 = r.n(k);
    int pr = presses(w), rx0 = w.gate.count("cmd_rx");
    uint32_t boots = w.gate.boots;
    int lost = 0;
    w.drop = [&](const AirFrame &f) {
      bool d = f.from == 1 && f.type() == MSG_ACK_ && w.gate.count("cmd_rx") > rx0 && w.gate.boots == boots;
      lost += d;
      return d;
    };
    w.user(open);
    Found sent = waitFor(r, w.house, "cmd_sent", h0, 2000);
    CHECK(sent.ok);
    CHECK(r.until([&] { return r.pulses(k, p0).size() == 1; }, 2000));
    w.gate.lipo = false;
    w.gate.cut = true;
    CHECK(r.until([&] { return !w.gate.running(); }, 1000));
    r.run(2000);
    w.gate.cut = false;
    Found boot = waitFor(r, w.gate, "boot", g0, 2000);
    CHECK(boot.ok);
    CHECK_EQ(boot.e.a, PM_RCAUSE_POR);
    CHECK(lost >= 1);
    Found dropped = waitFor(r, w.house, "cmd_dropped", h0, TTL_MS + 2000);
    CHECK(dropped.ok);
    CHECK_EQ(dropped.e.b, sent.e.b);
    CHECK(r.until([&] { return w.houseSees() == (open ? GS_OPEN_ : GS_CLOSED_); }, 15000));
    r.run(TTL_MS + 10000);
    w.drop = nullptr;
    CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
    CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 1);
    CHECK_EQ(r.pulses(k, p0).size(), 1);
    CHECK_EQ(presses(w), pr + 1);
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
    checkAgree(w);
  }
  w.checkClean();
}

// [restarts] [armed-after-status]
TEST(robustness_restart_house_resets_and_power_cycles_keep_the_gate_where_it_is) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  for (int pos = 0; pos < 2; pos++) {
    bool open = pos == 1;
    if (open) userMove(r, true);
    static const uint8_t kinds[] = { PM_RCAUSE_SYST, PM_RCAUSE_WDT, PM_RCAUSE_EXT, 0 };  // 0: a 12 V power cycle
    for (uint8_t rc : kinds) {
      size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), p1 = r.n(GK1), p2 = r.n(GK2);
      int pr = presses(w);
      if (rc) {
        w.house.reset(rc);
      } else {
        w.house.lipo = false;  // a 12 V cut takes the house board and the controller down
        w.setRail12(false);
        r.run(3000);
        w.setRail12(true);
      }
      Found b = waitFor(r, w.house, "boot", h0, 3000);
      CHECK(b.ok);
      CHECK_EQ(b.e.a, rc ? rc : PM_RCAUSE_POR);
      settled(r);
      CHECK(open ? w.opener.atOpen() : w.opener.atClosed());
      CHECK_EQ(w.houseSees(), open ? GS_OPEN_ : GS_CLOSED_);
      checkAgree(w);
      CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
      CHECK_EQ(countSince(w.house, "ctrl", h0, ANY, 0), 0);  // nothing the controller did was taken as the user
      CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
      CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
      CHECK_EQ(r.n(GK1), p1);
      CHECK_EQ(r.n(GK2), p2);
      CHECK_EQ(presses(w), pr);
    }
  }
  w.checkClean();
}

// [restarts] [armed-after-status]
TEST(robustness_restart_house_reset_before_the_users_edge_never_commands_from_the_boot_level) {
  // The user switches on and the house resets before it has taken the edge: it boots to a controller showing on and
  // a closed gate. That level is never a command; once the settle window is over the controller is put back.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.user(true);
  r.run(30);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  w.house.reset(PM_RCAUSE_WDT);
  Found b = waitFor(r, w.house, "boot", h0, 2000);
  CHECK(b.ok);
  CHECK(w.alarmSwitch());
  Found rs = waitFor(r, w.house, "resync", h0, SETTLE_MS + SYNC_MS + 5000, 0);
  CHECK(rs.ok);
  CHECK_IN(rs.e.at - b.e.at, SETTLE_MS + SYNC_MS, SETTLE_MS + SYNC_MS + 1000);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, RESYNC_MS + 500));
  r.run(SYNC_MS + 5000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl", h0, ANY, 0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(presses(w), 0);
  checkAgree(w);
  w.checkClean();
}

// [restarts] [pulse-only]
TEST(robustness_restart_house_reset_while_its_command_is_on_the_air_runs_it_once) {
  // The house resets with its command on the air: the gate runs it and ACKs to a house in its bootloader. The
  // rebooted house has forgotten it (nothing is resent) and follows the gate there.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), c0 = w.sent(w.house, MSG_CMD_).size(), p0 = r.n(GK1);
  w.user(true);
  CHECK(r.until([&] { return w.sent(w.house, MSG_CMD_).size() > c0; }, 2000));
  w.house.reset(PM_RCAUSE_SYST);
  CHECK(waitFor(r, w.gate, "cmd_rx", g0, 1000).ok);
  CHECK(r.until([&] { return w.opener.atOpen(); }, 15000));
  settled(r);
  r.run(TTL_MS + 10000);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 1);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK_EQ(r.pulses(GK1, p0).size(), 1);
  CHECK_EQ(presses(w), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(w.sent(w.house, MSG_CMD_).size(), c0 + 1);  // and never again
  checkAgree(w);
  CHECK(w.opener.atOpen());
  w.checkClean();
}

// [restarts] [pulse-only] [armed-after-status]
TEST(robustness_restart_both_boards_reboot_together) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  userMove(r, true);
  auto quietSince = [&](size_t h0, size_t g0, int pr) {
    CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
    CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
    CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
    CHECK_EQ(presses(w), pr);
  };
  // (1) Both warm-reset in the same millisecond
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  int pr = presses(w);
  w.house.reset(PM_RCAUSE_SYST);
  w.gate.reset(PM_RCAUSE_WDT);
  r.run(1000);
  settled(r);
  CHECK(w.opener.atOpen());
  checkAgree(w);
  quietSince(h0, g0, pr);
  // (2) Both power-cycled for 5 s (house 12 V and the gate's feed, no LiPos)
  h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.house.lipo = w.gate.lipo = false;
  w.setRail12(false);
  w.gate.cut = true;
  r.run(5000);
  CHECK(!w.house.running() && !w.gate.running());
  w.setRail12(true);
  w.gate.cut = false;
  r.run(1000);
  settled(r);
  CHECK_EQ(find(w.house, "boot", h0).e.a, PM_RCAUSE_POR);
  CHECK_EQ(find(w.gate, "boot", g0).e.a, PM_RCAUSE_POR);
  CHECK(w.opener.atOpen());
  checkAgree(w);
  quietSince(h0, g0, pr);
  // (3) Both reset while the gate travels after a command (ACKed): it gets there, nothing pulses again
  h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  pr = presses(w);
  w.user(false);
  CHECK(waitFor(r, w.gate, "pulse", g0, 2000).ok);
  r.run(PULSE_MS + 1000);
  CHECK(w.opener.dir < 0);
  w.house.reset(PM_RCAUSE_EXT);
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK(r.until([&] { return w.opener.atClosed(); }, 15000));
  r.run(1000);
  settled(r);
  checkAgree(w);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(presses(w), pr + 1);
  // (4) Both reset the millisecond the gate takes a command (the pulse cut to nothing, the ACK lost): the opener never
  // saw it; the rebooted house never commands from the controller's level and puts it back
  h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  pr = presses(w);
  w.user(true);
  CHECK(waitFor(r, w.gate, "cmd_rx", g0, 2000).ok);
  w.house.reset(PM_RCAUSE_SYST);
  w.gate.reset(PM_RCAUSE_SYST);
  r.run(1000);
  settled(r);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, 5000));
  CHECK(w.opener.atClosed());
  checkAgree(w);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 1);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 1);
  CHECK_EQ(presses(w), pr);
  CHECK(find(w.house, "resync", h0, 0).ok);
  // (5) Both reset 200 ms into a pulse (the opener took it): the gate opens, once
  CHECK(r.poll([&] { return !syncWindow(w); }, 6000, 20));
  h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  pr = presses(w);
  w.user(true);
  CHECK(waitFor(r, w.gate, "pulse", g0, 2000).ok);
  r.run(200);
  w.house.reset(PM_RCAUSE_WDT);
  w.gate.reset(PM_RCAUSE_WDT);
  CHECK(r.until([&] { return w.opener.atOpen(); }, 15000));
  r.run(1000);
  settled(r);
  r.run(TTL_MS + 5000);
  checkAgree(w);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 1);
  CHECK_EQ(presses(w), pr + 1);
  w.checkClean();
}

// [restarts] [sensor-closed-only-known]
TEST(robustness_restart_gate_radio_fails_to_initialise_then_recovers) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.gate.radioPresent = false;
  w.gate.reset(PM_RCAUSE_SYST);
  Found b = waitFor(r, w.gate, "boot", g0, 2000);
  CHECK(b.ok);
  r.run(10);
  CHECK(find(w.gate, "radio_fail", g0, 0).ok);
  CHECK(w.gate.status()["radio_ok"] == false);
  // Silent: the house loses the link and fails the sensor open
  Found dn = waitFor(r, w.house, "link_down", h0, LINK_MS + 5000);
  CHECK(dn.ok);
  CHECK(!w.sensorClosed());
  CHECK(!w.house.coil(1));
  r.run(20000);
  for (const AirFrame &f : w.air) CHECK(!(f.from == 1 && rel(f.start, b.e.at) > 0));
  CHECK_EQ(countSince(w.gate, "radio_fail", g0, 3), 0);
  // The module answers again: the next retry (every 5 s) brings it up, and the link follows
  size_t h1 = w.house.logs.size();
  w.gate.radioPresent = true;
  Found ok = waitFor(r, w.gate, "radio_fail", g0, 6000, 3);
  CHECK(ok.ok);
  CHECK(waitFor(r, w.house, "link_up", h1, 30000).ok);
  CHECK(r.until([&] { return w.sensorClosed(); }, 15000));
  settled(r);
  CHECK(w.gate.status()["radio_ok"] == true);
  checkAgree(w);
  userMove(r, true);
  userMove(r, false);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 2);
  w.checkClean();
}

// [restarts] [pulse-only]
TEST(robustness_restart_house_radio_fails_on_restart_command_given_up_then_recovers) {
  // The house's radio doesn't come back after a restart (a radio setting changed). A command meanwhile can't go out
  // and gives up at cmd_ttl_s; it never runs late once the radio is back, and the controller is resynced.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  w.house.radioPresent = false;
  CHECK(req(r, w.house, "config.set", "\"params\":{\"tx_power\":5}")["ok"] == true);
  CHECK(find(w.house, "radio_fail", h0, 0).ok);
  CHECK(w.house.status()["radio_ok"] == false);
  r.run(1000);
  w.user(true);
  Found sent = waitFor(r, w.house, "cmd_sent", h0, 1000, ACT_OPEN_);
  CHECK(sent.ok);
  Found drop = waitFor(r, w.house, "cmd_dropped", h0, TTL_MS + 2000);
  CHECK(drop.ok);
  // At the TTL, or after the radio retry (every 5 s) that holds the loop pass the TTL ends in (LoRa.begin(): 470 ms)
  CHECK_IN(drop.e.at - sent.e.at, TTL_MS - 10, TTL_MS + 500);
  r.run(5000);
  size_t h1 = w.house.logs.size();
  w.house.radioPresent = true;
  CHECK(waitFor(r, w.house, "radio_fail", h0, 6000, 3).ok);
  CHECK(waitFor(r, w.house, "link_up", h1, 30000).ok);
  Found rs = waitFor(r, w.house, "resync", h1, MISMATCH_MS + 10000, 0);
  CHECK(rs.ok);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, RESYNC_MS + 500));
  r.run(SYNC_MS + 1000);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK_EQ(presses(w), 0);
  CHECK(w.sent(w.house, MSG_CMD_).empty());
  checkAgree(w);
  w.checkClean();
}

// [restarts] [gate-boot-settle]
TEST(robustness_restart_gate_board_restarting_with_the_opener_first_report_waits_for_the_limits) {
  // The opener restarting with the gate board, its limit relays dark until its own controller has booted, 2.5 s
  // after the board went: (1) a power blip at the gate, the board's brown-out detector resetting it; (2) both
  // powered down (the board's feed first, then the opener, its battery flat) and back together. The gate's first
  // STATUS waits until its inputs have held still BOOT_SETTLE_MS, so the house never hears the closed gate as
  // anything else: the contact sensor never flashes open, K1 and the controller never move, the link stays up.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t g00 = w.gate.logs.size();
  for (int v = 0; v < 2; v++) {
    size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
    size_t n0[NSIG];
    for (int i = 0; i < NSIG; i++) n0[i] = r.n(i);
    CHECK(w.sensorClosed());
    uint32_t P;
    if (v == 0) {
      w.opener.force[0] = w.opener.force[1] = 0;
      w.gate.reset(PM_RCAUSE_BOD33);
      P = w.now;
    } else {
      w.gate.lipo = false;
      w.setGateRail(false);
      CHECK(r.until([&] { return !w.gate.running(); }, 1000));
      w.opener.ac = w.opener.battery = false;
      r.run(5000);
      w.opener.force[0] = w.opener.force[1] = 0;
      w.opener.ac = w.opener.battery = true;
      w.setGateRail(true);
      P = w.now;
    }
    Found boot = waitFor(r, w.gate, "boot", g0, 1000);
    CHECK(boot.ok);
    CHECK_EQ(boot.e.a, v == 0 ? PM_RCAUSE_BOD33 : PM_RCAUSE_POR);
    // The link comes back while the limits are still dark: nothing reported yet
    CHECK(r.poll([&] { return w.gate.status()["link"]["verified"] == true; }, 1900, 20));
    r.runTo(P + 2500);
    JsonDocument gs = w.gate.status();
    CHECK(gs["settling"] == true);
    CHECK(gs["io"]["in2"] == false);
    CHECK(statusFirsts(w, boot.e.at).empty());
    w.opener.force[0] = w.opener.force[1] = -1;
    uint32_t L = w.now;
    // The first STATUS once they have held still for BOOT_SETTLE_MS (from the closed limit's debounced edge)
    CHECK(r.until([&] { return !statusFirsts(w, boot.e.at).empty(); }, 5000));
    CHECK_IN(statusFirsts(w, boot.e.at)[0] - L, 50 + 3000, 50 + 3000 + 20);
    r.run(1000);
    gs = w.gate.status();
    CHECK(gs["settling"] == false);
    CHECK(gs["gate"] == "closed");
    // The house heard closed all along: no gate_state, K1, K2 and the controller never moved, the link never dropped
    for (int i = 0; i < NSIG; i++) CHECK_EQ(r.n(i), n0[i]);
    CHECK_EQ(countSince(w.house, "gate_state", h0), 0);
    CHECK_EQ(countSince(w.gate, "gate_state", g0), 0);
    CHECK_EQ(countSince(w.house, "link_down", h0), 0);
    CHECK(w.house.status()["gate"] == "closed");
    checkAgree(w);
  }
  userMove(r, true);
  userMove(r, false);
  CHECK_EQ(countSince(w.gate, "pulse", g00), 2);
  w.checkClean();
}

// [restarts] [armed-after-status]
TEST(robustness_restart_house_reboot_with_rf_down_arms_only_a_sync_window_after_the_first_status) {
  // The house reboots while RF is lost and hears the gate only long after its settle window has closed. The gate is
  // closed, so that first STATUS moves no relay and opens no sync window; the house still takes no IN1 edge as a
  // command until sync_window_ms after it. The controller's relay coming on by itself 1 s after it (the hub
  // re-sending a stale state as the house reappears, or chatter: not the user) is no command; once the house arms,
  // the controller is put back at once.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size();
  int pr = presses(w);
  w.drop = [](const AirFrame &) { return true; };
  w.house.reset(PM_RCAUSE_WDT);
  Found boot = waitFor(r, w.house, "boot", h0, 2000);
  CHECK(boot.ok);
  r.run(SETTLE_MS + SYNC_MS + 5000);
  JsonDocument h = w.house.status();
  CHECK(h["armed"] == false);
  CHECK(h["sync_window"] == false);
  CHECK(h["gate"] == "unknown");
  w.drop = nullptr;
  Found st = waitFor(r, w.house, "gate_state", h0, 30000, GS_CLOSED_);
  CHECK(st.ok);
  uint32_t T = st.e.at;
  CHECK(rel(T, boot.e.at + SETTLE_MS + SYNC_MS + 1000) > 0);
  CHECK(!w.house.coil(1));
  CHECK(r.until([&] { return w.sensorClosed(); }, 100));
  h = w.house.status();
  CHECK(h["armed"] == false);
  CHECK(h["sync_window"] == false);
  r.runTo(T + 1000);
  CHECK(w.house.status()["armed"] == false);
  size_t h1 = w.house.logs.size();
  w.shelly.relay = true;
  w.trace.add(w.now, "controller: relay on by itself");
  Found c = waitFor(r, w.house, "ctrl", h1, 200, 1);
  CHECK(c.ok);
  CHECK_IN(c.e.at - (T + 1000), 50, 52);
  r.run(CONFIRM_MS + 200);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK(w.house.status()["armed"] == false);
  // Armed sync_window_ms after the first STATUS, and the controller, out of step, resynced at once
  Found rs = waitFor(r, w.house, "resync", h1, SYNC_MS + 1000, 0);
  CHECK(rs.ok);
  CHECK_IN(rs.e.at - T, SYNC_MS, SYNC_MS + 3);
  CHECK(w.house.status()["armed"] == true);
  CHECK(r.until([&] { return !w.alarmSwitch(); }, RESYNC_MS + 500));
  r.run(SYNC_MS + 1000);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK_EQ(presses(w), pr);
  CHECK(w.opener.atClosed());
  checkAgree(w);
  w.checkClean();
}

// [chaos] [ctrl-power] [settle-window]
TEST(robustness_controller_slow_boot_after_a_k1_change_inside_its_settle_window_is_a_sync) {
  // A 12 V cut (the house on its LiPo) while the AES opens the gate; power returns before the gate gets there, and K1
  // follows it to open ~3 s later, early in the settle window (ctrl_settle_ms). K1's change opens a sync window of
  // its own, shorter: it must not cut the settle window short. The AES closes the gate again, and the controller,
  // slow to boot (9 s), comes up at K1's level (on) while the gate is between. That edge is a sync, never a command:
  // an OPEN would reverse the gate.
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  w.house.lipo = true;
  w.shelly.bootMs = 9000;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), k1 = r.n(HK1);
  w.setRail12(false);
  CHECK(waitFor(r, w.house, "ctrl_power", h0, 1000, 0).ok);
  r.run(50);
  w.extPress(true, 300);
  r.run(5000);
  CHECK(w.opener.dir > 0);
  w.setRail12(true);
  uint32_t P = w.now;
  Found on = waitFor(r, w.house, "ctrl_power", h0, 1000, 1);
  CHECK(on.ok);
  CHECK(r.until([&] { return r.n(HK1) > k1; }, 8000));
  uint32_t X = r.e[HK1][k1].at;
  CHECK(r.e[HK1][k1].on);
  CHECK(w.opener.atOpen());
  CHECK(!w.shelly.booted);
  CHECK(syncWindow(w));
  r.runTo(X + 500);
  size_t hx = w.house.logs.size();
  w.extPress(false, 300);
  CHECK(waitFor(r, w.house, "gate_state", hx, 2000, GS_BETWEEN_).ok);
  CHECK(w.house.coil(1));  // holding open's level while it travels
  // The controller boots past K1's window but inside the settle window
  CHECK(r.until([&] { return w.shelly.booted; }, 10000));
  uint32_t S = w.now;
  CHECK_IN(S - P, 9000, 9002);
  CHECK(rel(S, X + SYNC_MS + 100) > 0);
  CHECK(rel(S + 100, on.e.at + SETTLE_MS) < 0);
  CHECK(w.alarmSwitch());
  Found sy = waitFor(r, w.house, "sync", hx, 200, 1);
  CHECK(sy.ok);
  CHECK_IN(sy.e.at - S, 49, 52);
  // The gate closes; K1 and the controller follow; nothing was ever sent
  CHECK(r.until([&] { return w.opener.atClosed(); }, 12000));
  CHECK(r.until([&] { return w.houseSees() == GS_CLOSED_; }, 3000));
  settled(r);
  r.run(SYNC_MS + 1000);
  checkAgree(w);
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl", h0, ANY, 0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK_EQ(countSince(w.gate, "pulse", g0), 0);
  CHECK_EQ(presses(w), 2);
  w.checkClean();
}

// =====================================================================================================================
// [chaos]: several simulated hours each, every fault mixed in, every monitor on; at the end, with everything restored
// and a few quiet minutes, the Alarm.com switch, K1 and the contact sensor agree with the real gate.

// [chaos]
TEST(robustness_chaos_seed_1) {
  chaos(1, 3);
}

// [chaos]
TEST(robustness_chaos_seed_2_gate_board_on_the_mains_psu) {
  // The gate board fed from the opener's mains PSU: every AC loss also takes it down (unless its LiPo is in).
  chaos(2, 3, Shelly::FOLLOW, World::FEED_PSU);
}

// [chaos]
TEST(robustness_chaos_seed_3) {
  chaos(3, 3);
}

// [chaos] [sync-window]
TEST(robustness_chaos_toggling_controller_seed_4) {
  chaos(4, 2, Shelly::TOGGLE);
}

// [chaos] [sync-window] [resync]
TEST(robustness_chaos_seed_8) {
  // This seed found, at t=7447834, a mismatch resync started 32 ms after the user switched on (the edge still in its
  // 50 ms debounce): that edge, at the resync's target level, ended the resync's sync window early, and the controller
  // following K1's resync pulse (off) was taken as the user, a CLOSE sent for an open gate. See
  // robustness_resync_window_outlasts_a_matching_edge_so_the_resync_never_commands_the_gate.
  chaos(8, 3);
}

// [sync-window] [resync]
TEST(robustness_resync_window_outlasts_a_matching_edge_so_the_resync_never_commands_the_gate) {
  // Chaos seed 8, made deterministic. The controller is out of step (detached while someone opens the gate): K1 on,
  // the Alarm.com switch off. mismatch_timeout_s after K1 opened, the house resyncs: K1 off for resync_ms, then on.
  // The user switches on 32 ms before that (towards the open gate); the house takes the edge 19 ms into the resync.
  // That edge is at the level the window expects, but it must not end the window before K1 is back: the controller
  // following K1 off, and back on, are syncs too, never a command (a CLOSE, away from where the gate is).
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  w.shelly.mode = Shelly::DETACHED;
  size_t h0 = w.house.logs.size(), g0 = w.gate.logs.size(), k1 = r.n(HK1);
  w.extPress(true, 300);
  CHECK(r.until([&] { return r.n(HK1) > k1; }, 12000));
  uint32_t K = r.e[HK1][k1].at;
  CHECK(w.opener.atOpen());
  r.run(SYNC_MS + 500);  // K1's edge has reached the (detached) controller and its window is over
  CHECK(!w.alarmSwitch());
  CHECK(!syncWindow(w));
  w.shelly.mode = Shelly::FOLLOW;
  uint32_t R = (K | 1) + MISMATCH_MS;  // the resync is due (mismatchSince = K | 1)
  r.runTo(R - 32);
  w.user(true);
  Found rs = waitFor(r, w.house, "resync", h0, 200, 1);
  CHECK(rs.ok);
  CHECK_EQ(rs.e.at, R);
  size_t h1 = w.house.logs.size();
  Found ue = waitFor(r, w.house, "sync", h1, 100, 1);  // the user's edge, inside the resync's window
  CHECK(ue.ok);
  CHECK_EQ(ue.e.at, R + 19);
  r.run(RESYNC_MS + SYNC_MS + 15000);
  // Every controller edge here came from K1, or was the user switching to where the gate already is: none may become
  // a command, and the gate stays open
  CHECK_EQ(countSince(w.house, "cmd_sent", h0), 0);
  CHECK_EQ(countSince(w.house, "ctrl", h0), 0);
  CHECK_EQ(countSince(w.gate, "cmd_rx", g0), 0);
  CHECK(w.opener.atOpen());
  checkAgree(w);
  w.checkClean();
}
