// Gate relays (CLAUDE.md, "Behavioural invariants"): K1/K2 are only ever pulsed (pulse_ms, or a relay test's ms) and
// interlocked, a relay test sets a target only when the gate can move for it, nothing that stops the loop runs while
// a pulse does, and no loop pass or boot step comes near the 8 s watchdog. Each test drives the simulated site
// (world.h) and checks the coils, the opener, the logs, the flash and the console replies directly; the world's
// monitors catch whatever else breaks on the way.
#include "world.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <exception>
#include "crc32.h"

namespace {

enum { ACT_OPEN_ = 1, ACT_CLOSE_ = 2 };
enum { CAUSE_NONE_ = 0, CAUSE_LORA_ = 1, CAUSE_EXTERNAL_ = 2 };
enum : uint8_t { MSG_HELLO_ACK_ = 2, MSG_ACK_ = 3, MSG_CMD_ = 4, MSG_CFG_SET_ = 10 };
const int32_t INTERLOCK = 100;   // INTERLOCK_MS (role_gate.cpp)
const int32_t ID_PULSE_MS = 16;  // PARAMS id of pulse_ms (config.cpp)
// Frame fields (link.cpp: ver|type|net_id|src|dst|session u32|seq u32|payload|tag, little-endian)
const size_t F_SEQ = 9, F_PAYLOAD = 13;

uint32_t le32(const Bytes &b, size_t at) {
  return b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24;
}
uint32_t le16(const Bytes &b, size_t at) {
  return b[at] | (uint32_t)b[at + 1] << 8;
}

std::string strf(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string strf(const char *f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

// A CHECK failing while the monitors hold violations: print them next to it and clear them, so the failure doesn't
// depend on how the runner treats the after-test hook's throw (a runner calling it outside its try aborts the run).
// Tests that get to their end call w.checkClean() themselves.
struct Guard {
  World &w;
  ~Guard() {
    if (!std::uncaught_exceptions() || w.violations.empty()) return;
    for (const std::string &v : w.violations) printf("      monitor: %s\n", v.c_str());
    w.violations.clear();
  }
};

struct Pulse {
  int k;
  uint32_t on, off;  // world ms: first and last-plus-one ms the coil was closed
  bool open;         // still closed
  uint32_t len() const { return off - on; }
};

// Every gate coil closure, sampled each simulated millisecond (after both boards ran).
struct Rec {
  World &w;
  std::vector<Pulse> p;
  bool st[2] = { false, false };
  std::function<void()> each;  // also run after every step

  explicit Rec(World &w) : w(w) { sample(); }
  void sample() {
    for (int k = 1; k <= 2; k++) {
      bool on = w.gate.coil(k);
      if (on && !st[k - 1]) p.push_back({ k, w.now, 0, true });
      if (!on && st[k - 1]) {
        for (size_t i = p.size(); i-- > 0;) {
          if (p[i].k == k && p[i].open) {
            p[i].off = w.now;
            p[i].open = false;
            break;
          }
        }
      }
      st[k - 1] = on;
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
  bool until(const std::function<bool()> &cond, uint32_t maxMs) {
    for (uint32_t i = 0; i < maxMs; i++) {
      if (cond()) return true;
      step();
    }
    return cond();
  }
  // For conditions that call status() (too slow for every millisecond).
  bool poll(const std::function<bool()> &cond, uint32_t maxMs, uint32_t every = 50) {
    for (uint32_t t = 0; t < maxMs; t += every) {
      if (cond()) return true;
      run(every);
    }
    return cond();
  }
  std::vector<Pulse> of(int k) const {
    std::vector<Pulse> v;
    for (const Pulse &x : p)
      if (x.k == k) v.push_back(x);
    return v;
  }
  int count(int k) const { return (int)of(k).size(); }
  Pulse last(int k) const {
    for (size_t i = p.size(); i-- > 0;)
      if (p[i].k == k) return p[i];
    throw Failure(strf("K%d never closed", k));
  }
  // Never both closed, and neither closing within INTERLOCK ms of the other releasing.
  void checkInterlock() const {
    for (const Pulse &a : p) {
      for (const Pulse &b : p) {
        if (a.k != 1 || b.k != 2) continue;
        bool aFirst = (int32_t)(b.on - a.on) > 0;
        const Pulse &first = aFirst ? a : b, &second = aFirst ? b : a;
        if ((int32_t)(second.on - first.on) == 0 || first.open)
          throw Failure(strf("K1 and K2 closed together (t=%u)", second.on));
        int32_t gap = (int32_t)(second.on - first.off);
        if (gap < INTERLOCK)
          throw Failure(strf("K%d closed %d ms after K%d released (t=%u)", second.k, gap, first.k, second.on));
      }
    }
  }
};

// A console request, sent now without waiting (relay tests recorded for the monitors, as Board::request does). On
// the UART (port 1) it carries the CRC that port requires.
int sendReq(World &w, Board &b, const std::string &cmd, const std::string &args = "", int port = 0) {
  int id = b.nextId++;
  std::string body = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"" + cmd + "\"" + (args.empty() ? "" : "," + args);
  std::string line = body + "}";
  if (port == 1) line = body + strf(",\"crc\":\"%08x\"}", (unsigned)crc32(line.data(), line.size()));
  if (cmd == "relay.test") {
    JsonDocument a;
    deserializeJson(a, "{" + args + "}");
    w.relayTests[b.idx].push_back({ w.now, a["k"] | 0, a["ms"] | 500u });
  }
  w.trace.add(w.now, b.name + (port ? " <-uart " : " <- ") + line);
  b.send(line, port);
  return id;
}

// Steps (recording coils) until the board answers request `id`; `at` = the world ms of the loop pass that answered.
JsonDocument reply(Rec &r, Board &b, int id, uint32_t maxMs = 3000, uint32_t *at = nullptr) {
  if (!r.until([&] { return b.replies.count(id) > 0; }, maxMs))
    throw Failure(b.name + ": no reply to request " + std::to_string(id));
  if (at) *at = r.w.now;
  JsonDocument d = b.replies[id];
  b.replies.erase(id);
  return d;
}

JsonDocument req(Rec &r, Board &b, const std::string &cmd, const std::string &args = "", uint32_t *at = nullptr) {
  return reply(r, b, sendReq(r.w, b, cmd, args), 3000, at);
}

const JsonDocument *eventSince(const Board &b, const char *name, size_t from) {
  for (size_t i = b.events.size(); i-- > from;)
    if (b.events[i]["event"] == name) return &b.events[i];
  return nullptr;
}

typedef std::pair<int, int> SC;  // gate_state (state, cause)
std::vector<SC> statesSince(const Board &b, size_t from) {
  std::vector<SC> v;
  for (size_t i = from; i < b.logs.size(); i++)
    if (b.logs[i].ev == "gate_state") v.push_back({ b.logs[i].a, b.logs[i].b });
  return v;
}

// The user opens or closes from Alarm.com: wait for the gate to get there, the house to know, and its sync window.
void userMove(Rec &r, bool open) {
  World &w = r.w;
  w.user(open);
  CHECK(r.until([&] { return open ? w.opener.atOpen() : w.opener.atClosed(); }, 20000));
  CHECK(r.until([&] { return w.houseSees() == (open ? GS_OPEN_ : GS_CLOSED_); }, 5000));
  r.run(4000);  // past the sync window the house's K1 opened (sync_window_ms 3000)
}

std::string keyHex() {
  std::string s;
  for (uint8_t b : TEST_KEY) s += strf("%02x", b);
  return s;
}

// The world ms a Relay::pulse(now, ms) asked for at `on` ends (its end is stamped `| 1`).
uint32_t pulseEnd(uint32_t on, uint32_t ms) {
  return (on + ms) | 1;
}

}  // namespace

// ---- pulse-only ---------------------------------------------------------------------------------------------------

// [pulse-only]
TEST(gate_relays_cmd_open_pulses_k1_close_pulses_k2_once_each) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  userMove(r, true);
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 0);
  Pulse o = r.last(1);
  CHECK_IN(o.len(), 500, 501);
  const LogEv *rx = w.gate.last("cmd_rx"), *pl = w.gate.last("pulse");
  CHECK(rx && pl);
  CHECK_EQ(rx->a, ACT_OPEN_);
  CHECK_EQ(pl->a, 1);
  CHECK_EQ(pl->b, 500);
  CHECK_EQ(pl->at, rx->at);
  CHECK_EQ(o.on, pl->at);  // closed in the pass the command arrived
  CHECK_EQ(w.opener.presses[0].size(), 1);
  CHECK_IN(w.opener.presses[0][0].second, 500, 501);
  CHECK_EQ(w.opener.presses[1].size(), 0);

  userMove(r, false);
  r.run(35000);  // through the next heartbeat: nothing more closes
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 1);
  Pulse c = r.last(2);
  CHECK_IN(c.len(), 500, 501);
  CHECK_EQ(w.gate.last("cmd_rx")->a, ACT_CLOSE_);
  CHECK_EQ(w.gate.last("pulse")->a, 2);
  CHECK_EQ(w.gate.last("pulse")->b, 500);
  CHECK_EQ(c.on, w.gate.last("pulse")->at);
  CHECK_EQ(w.opener.presses[0].size(), 1);
  CHECK_EQ(w.opener.presses[1].size(), 1);
  CHECK_IN(w.opener.presses[1][0].second, 500, 501);
  CHECK_EQ(w.gate.count("pulse"), 2);
  JsonDocument s = w.gate.status();
  CHECK(s["io"]["k1"] == false);
  CHECK(s["io"]["k2"] == false);
  r.checkInterlock();
  w.checkClean();
}

// [pulse-only]
TEST(gate_relays_cmd_pulse_is_pulse_ms_at_both_bounds) {
  World w;
  Guard g{ w };
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("pulse_ms", 100));
  });
  Rec r(w);
  userMove(r, true);
  CHECK_EQ(r.count(1), 1);
  CHECK_IN(r.last(1).len(), 100, 101);
  CHECK_EQ(w.gate.last("pulse")->b, 100);
  CHECK_IN(w.opener.presses[0].back().second, 100, 101);

  // Out of range: refused by the house, and by the gate's own console
  CHECK(req(r, w.house, "remote.set", "\"name\":\"pulse_ms\",\"value\":5001")["ok"] == false);
  CHECK(req(r, w.house, "remote.set", "\"name\":\"pulse_ms\",\"value\":99")["ok"] == false);
  JsonDocument d = req(r, w.gate, "config.set", "\"params\":{\"pulse_ms\":99}");
  CHECK(d["ok"] == false);
  CHECK(d["errors"][0] == "pulse_ms");
  CHECK_EQ(w.gate.get("pulse_ms"), 100);

  // The longest, set from the house
  size_t ev0 = w.house.events.size();
  CHECK(req(r, w.house, "remote.set", "\"name\":\"pulse_ms\",\"value\":5000")["ok"] == true);
  CHECK(r.until([&] { return eventSince(w.house, "remote_set", ev0) != nullptr; }, 15000));
  CHECK((*eventSince(w.house, "remote_set", ev0))["ok"] == true);
  CHECK_EQ(w.gate.get("pulse_ms"), 5000);
  userMove(r, false);
  CHECK_EQ(r.count(2), 1);
  CHECK_IN(r.last(2).len(), 5000, 5001);
  CHECK_EQ(w.gate.last("pulse")->a, 2);
  CHECK_EQ(w.gate.last("pulse")->b, 5000);
  CHECK_IN(w.opener.presses[1].back().second, 5000, 5001);
  CHECK_EQ(r.count(1), 1);
  w.checkClean();
}

// [pulse-only]
TEST(gate_relays_relay_test_pulses_its_ms_and_refuses_out_of_range) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  const char *bad[] = { "\"k\":1,\"ms\":49",    "\"k\":2,\"ms\":5001",      "\"k\":0",
                        "\"k\":3,\"ms\":500",   "\"k\":257,\"ms\":500",     "\"k\":1,\"ms\":\"500\"",
                        "\"k\":1,\"ms\":-500",  "\"k\":1,\"ms\":4294967796", "\"ms\":500" };
  for (const char *a : bad) {
    JsonDocument d = req(r, w.gate, "relay.test", a);
    CHECK(d["ok"] == false);
    CHECK(d["error"] == "k must be 1|2, ms 50..5000, role set");
  }
  r.run(1000);
  CHECK(r.p.empty());
  CHECK_EQ(w.gate.count("pulse"), 0);

  struct {
    int k, ms;  // ms 0: left out (default 500)
  } good[] = { { 1, 50 }, { 2, 0 }, { 1, 5000 }, { 2, 5000 } };
  for (auto &t : good) {
    std::string a = "\"k\":" + std::to_string(t.k) + (t.ms ? ",\"ms\":" + std::to_string(t.ms) : "");
    uint32_t at, ms = t.ms ? t.ms : 500;
    CHECK(req(r, w.gate, "relay.test", a, &at)["ok"] == true);
    r.run(ms + 1500);
    Pulse p = r.last(t.k);
    CHECK_EQ(p.on, at);
    CHECK_IN(p.len(), ms, ms + 1);
    CHECK_EQ(w.gate.last("pulse")->a, t.k);
    CHECK_EQ(w.gate.last("pulse")->b, ms);
    CHECK_IN(w.opener.presses[t.k - 1].back().second, ms, ms + 1);
  }
  CHECK_EQ(r.count(1), 2);
  CHECK_EQ(r.count(2), 2);
  r.checkInterlock();
  w.checkClean();
}

// [pulse-only]
TEST(gate_relays_reset_mid_pulse_releases_and_nothing_closes_after_boot) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":3000")["ok"] == true);
  r.run(1000);
  CHECK(w.gate.coil(1));
  uint32_t boots = w.gate.boots;
  int bootLogs = w.gate.count("boot");
  w.gate.reset(PM_RCAUSE_WDT);
  r.run(1);
  CHECK(!w.gate.coil(1));
  r.run(15000);  // bootloader, boot, settling, the link back
  CHECK_EQ(w.gate.boots, boots + 1);
  CHECK_EQ(w.gate.count("boot"), bootLogs + 1);
  CHECK_EQ(w.gate.last("boot")->a, PM_RCAUSE_WDT);
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 0);
  CHECK_IN(r.last(1).len(), 1000, 1002);
  CHECK_EQ(w.gate.count("pulse"), 1);  // the one before the reset
  JsonDocument s = w.gate.status();
  CHECK(s["io"]["k1"] == false);
  CHECK(s["io"]["k2"] == false);
  CHECK(s["reset_cause"] == "watchdog");
  w.checkClean();
}

// [pulse-only]
TEST(gate_relays_lost_acks_retried_command_pulses_once) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  int dropped = 0;
  w.drop = [&](const AirFrame &f) {
    // The gate's ACKs of the command are lost twice: the house sends it three times.
    if (f.from == 1 && f.type() == MSG_ACK_ && w.gate.count("cmd_rx") > 0 && dropped < 2) return ++dropped, true;
    return false;
  };
  userMove(r, true);
  CHECK_EQ(dropped, 2);
  CHECK(w.sent(w.house, MSG_CMD_).size() >= 3);
  CHECK_EQ(w.gate.count("cmd_rx"), 1);
  CHECK_EQ(w.gate.count("pulse"), 1);
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 0);
  CHECK_IN(r.last(1).len(), 500, 501);
  CHECK_EQ(w.opener.presses[0].size(), 1);
  JsonDocument h = w.house.status();
  CHECK(h["cmd_pending"] == false);
  CHECK(h["cmd_result"] == 0);
  w.checkClean();
}

// [pulse-only]
TEST(gate_relays_renumbered_resend_of_a_run_command_pulses_once) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // The gate's ACKs of the OPEN are lost until it has seen the command again, so the house keeps resending it; its
  // resends with the same seq are re-ACKed from the link's ACK memo (lost too). Then the house answers a HELLO from
  // the gate's verified session (replayed here) and renumbers what it still has waiting (reframePending): that
  // resend escapes the gate's ACK memo, and only the gate's record of the last command id (lastCmdId) keeps the
  // OPEN from pulsing a second time.
  int dropped = 0;
  w.drop = [&](const AirFrame &f) {
    if (f.from == 1 && f.type() == MSG_ACK_ && w.gate.count("cmd_rx") > 0 && w.gate.count("cmd_dup") == 0)
      return ++dropped, true;
    return false;
  };
  size_t c0 = w.sent(w.house, MSG_CMD_).size();
  w.user(true);
  CHECK(r.until([&] { return w.gate.count("cmd_rx") == 1; }, 2000));
  const LogEv rx = *w.gate.last("cmd_rx");
  CHECK_EQ(rx.a, ACT_OPEN_);
  std::vector<const AirFrame *> cmds = w.sent(w.house, MSG_CMD_);
  CHECK_EQ(cmds.size(), c0 + 1);
  const uint32_t seq0 = le32(cmds[c0]->b, F_SEQ);
  CHECK_EQ(le16(cmds[c0]->b, F_PAYLOAD), rx.b);
  // A resend with the same seq (re-ACKed from the memo if heard, not run again), and the pulse over.
  CHECK(r.until([&] { return w.sent(w.house, MSG_CMD_).size() >= c0 + 2 && !w.gate.coil(1); }, 3000));
  cmds = w.sent(w.house, MSG_CMD_);
  for (size_t i = c0; i < cmds.size(); i++) CHECK_EQ(le32(cmds[i]->b, F_SEQ), seq0);
  CHECK_EQ(w.gate.count("cmd_dup"), 0);
  CHECK(w.house.status()["cmd_pending"] == true);

  // The gate's boot HELLO, sent again as-is (retried if it didn't go out, or collided with the house).
  size_t a0 = w.sent(w.house, MSG_HELLO_ACK_).size();
  bool answered = false;
  for (int i = 0; i < 10 && !answered; i++) {
    if (req(r, w.gate, "debug.replay", "\"hello\":true")["sent"] == true)
      answered = r.until([&] { return w.sent(w.house, MSG_HELLO_ACK_).size() > a0; }, 300);
    else
      r.run(10);
  }
  CHECK(answered);
  CHECK(w.gate.status()["link"]["verified"] == true);
  CHECK(r.until([&] { return w.gate.count("cmd_dup") > 0; }, 3000));
  const AirFrame *resend = w.sent(w.house, MSG_CMD_).back();
  CHECK(le32(resend->b, F_SEQ) != seq0);       // renumbered
  CHECK_EQ(le16(resend->b, F_PAYLOAD), rx.b);  // the same command
  CHECK_EQ(resend->b[F_PAYLOAD + 2], ACT_OPEN_);
  CHECK_EQ(w.gate.last("cmd_dup")->a, rx.b);
  CHECK(dropped >= 1);

  CHECK(r.until([&] { return w.opener.atOpen(); }, 15000));
  CHECK(r.until([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  r.run(4000);
  CHECK_EQ(w.gate.count("cmd_rx"), 1);
  CHECK_EQ(w.gate.count("cmd_dup"), 1);
  CHECK_EQ(w.gate.count("pulse"), 1);
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 0);
  CHECK_IN(r.last(1).len(), 500, 501);
  CHECK_EQ(w.opener.presses[0].size(), 1);
  CHECK_EQ(w.opener.presses[1].size(), 0);
  JsonDocument h = w.house.status();
  CHECK(h["cmd_pending"] == false);
  CHECK(h["cmd_result"] == 0);  // the duplicate was ACKed with the first one's result
  CHECK_EQ(w.house.count("tx_giveup"), 0);
  w.checkClean();
}

// [pulse-only]
XFAIL_TEST(gate_relays_second_open_cmd_mid_pulse_keeps_pulse_ms,
           "an OPEN arriving while K1 still pulses for the previous OPEN restarts K1's timer (Relay::pulse on a closed "
           "relay): the opener's OPEN input stays closed for the gap plus pulse_ms") {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // The switch goes on, off and on again inside ctrl_confirm_ms: the CLOSE never goes, two OPENs do.
  w.user(true);
  r.run(100);
  w.user(false);
  r.run(100);
  w.user(true);
  CHECK(r.until([&] { return w.gate.count("cmd_rx") == 2; }, 2000));
  CHECK_EQ(w.house.count("cmd_sent", ACT_OPEN_), 2);
  CHECK_EQ(w.house.count("cmd_sent", ACT_CLOSE_), 0);
  const LogEv rx2 = *w.gate.last("cmd_rx");
  CHECK_EQ(rx2.a, ACT_OPEN_);
  Pulse first = r.of(1).front();
  CHECK((int32_t)(rx2.at - first.on) > 0);
  CHECK(first.open || (int32_t)(first.off - rx2.at) > 0);  // K1 still closed for the first OPEN
  CHECK(r.until([&] { return w.opener.atOpen(); }, 20000));
  r.run(2000);
  CHECK_EQ(r.count(2), 0);
  for (const Pulse &p : r.of(1)) CHECK_IN(p.len(), 1, 501);
  for (auto &pr : w.opener.presses[0]) CHECK_IN(pr.second, 1, 501);
  w.checkClean();
}

// [pulse-only]
XFAIL_TEST(gate_relays_second_relay_test_mid_pulse_keeps_its_ms,
           "a relay.test of the relay already pulsing restarts its timer (Relay::pulse on a closed relay): one "
           "closure of the gap plus ms instead of pulses of ms") {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  uint32_t t0, t1;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500", &t0)["ok"] == true);
  r.run(199);
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500", &t1)["ok"] == true);
  CHECK_EQ(t1 - t0, 200);
  r.run(1500);
  CHECK_EQ(w.gate.count("pulse"), 2);
  CHECK_EQ(r.count(2), 0);
  for (const Pulse &p : r.of(1)) CHECK_IN(p.len(), 1, 501);
  for (auto &pr : w.opener.presses[0]) CHECK_IN(pr.second, 1, 501);
  w.checkClean();
}

// ---- interlock ----------------------------------------------------------------------------------------------------

// [interlock] [pulse-only]
TEST(gate_relays_interlock_reversal_test_cuts_the_first_pulse_short) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  uint32_t t0, t1;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":1000", &t0)["ok"] == true);
  r.run(299);
  CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":300", &t1)["ok"] == true);
  CHECK(!w.gate.coil(1));  // released at once
  CHECK(!w.gate.coil(2));  // waits for the interlock
  r.run(1000);
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 1);
  Pulse a = r.last(1), b = r.last(2);
  CHECK_EQ(a.on, t0);
  CHECK_EQ(a.off, t1);
  CHECK_EQ(a.len(), 300);
  CHECK_IN(b.on - a.off, INTERLOCK, INTERLOCK + 1);
  CHECK_IN(b.len(), 300, 301);  // the delay doesn't eat into the pulse
  CHECK_EQ(w.opener.presses[0].size(), 1);
  CHECK_EQ(w.opener.presses[1].size(), 1);
  auto &po = w.opener.presses[0][0], &pc = w.opener.presses[1][0];
  CHECK((int32_t)(pc.first - (po.first + po.second)) >= INTERLOCK);
  r.checkInterlock();
  w.checkClean();
}

// [interlock]
TEST(gate_relays_interlock_after_the_other_pulse_ended_by_itself) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // K2 ends by itself at T; K1 is asked for d ms later: inside the window it waits for T + 100, outside it closes
  // at once. (The release is stamped `| 1`, so the window's last ms can read as 1 ms longer.)
  for (uint32_t d : { 1u, 5u, 50u, 99u, 100u, 101u, 102u, 150u }) {
    CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":100")["ok"] == true);
    CHECK(r.until([&] { return !w.gate.coil(2); }, 200));
    uint32_t T = w.now;
    if (d > 1) r.run(d - 1);
    uint32_t at;
    CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":200", &at)["ok"] == true);
    CHECK_EQ(at - T, d);
    r.run(500);
    Pulse p = r.last(1);
    uint32_t start = p.on - T;
    if (d <= 100) CHECK_IN(start, 100, 101);
    else CHECK_EQ(start, d);
    CHECK_IN(p.len(), 200, 201);
    r.run(1500);  // past the relays' release memo (1 s)
  }
  CHECK_EQ(r.count(1), 8);
  CHECK_EQ(r.count(2), 8);
  r.checkInterlock();
  w.checkClean();
}

// [interlock]
TEST(gate_relays_interlock_after_a_release_in_a_late_loop_pass) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // A loop pass can start a few ms late (a USB write waiting on the host, radioRandom32's RSSI sampling): K2's pulse
  // then ends in a pass that starts after its end, at an odd or even ms. Its release is stamped millis() | 1, which
  // in an even ms is 1 ms ahead of the pass's `now`. A K1 test read in that same pass (the relays update first) must
  // still wait the whole interlock after the release.
  for (uint32_t late : { 1u, 2u, 3u, 4u }) {
    uint32_t t0;
    CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":200", &t0)["ok"] == true);
    const uint32_t E = pulseEnd(t0, 200);  // the pass K2 would release in
    CHECK(r.until([&] { return w.now == E - 1; }, 300));
    CHECK(w.gate.coil(2));
    int id = sendReq(w, w.gate, "relay.test", "\"k\":1,\"ms\":200");
    w.gate.busyUntil = E + late;  // the gate's last pass (E - 1) ran on until then
    uint32_t at;
    CHECK(reply(r, w.gate, id, 1000, &at)["ok"] == true);
    CHECK_EQ(at, E + late);
    Pulse c = r.last(2);
    CHECK(!c.open);
    CHECK_EQ(c.off, E + late);  // released in that pass
    CHECK(!w.gate.coil(1));     // and K1 not with it
    r.run(500);
    Pulse o = r.last(1);
    CHECK_IN(o.on - c.off, INTERLOCK, INTERLOCK + 1);
    CHECK_IN(o.len(), 200, 201);
    auto &pc = w.opener.presses[1].back(), &po = w.opener.presses[0].back();
    CHECK((int32_t)(po.first - (pc.first + pc.second)) >= INTERLOCK);
    r.run(1500);  // past the relays' release memo (1 s)
  }
  CHECK_EQ(r.count(1), 4);
  CHECK_EQ(r.count(2), 4);
  r.checkInterlock();
  w.checkClean();
}

// [interlock] [pulse-only]
TEST(gate_relays_interlock_close_cmd_cuts_open_pulse_short) {
  World w;
  Guard g{ w };
  w.commission([](Board &b) {
    if (b.idx == 0) CHECK(b.set("ctrl_confirm_ms", 0));  // the CLOSE goes as soon as the switch is off
    else CHECK(b.set("pulse_ms", 2000));
  });
  Rec r(w);
  w.user(true);
  CHECK(r.until([&] { return w.gate.coil(1); }, 2000));
  r.run(400);
  w.user(false);  // while K1 is still closed for the OPEN
  CHECK(r.until([&] { return w.gate.count("cmd_rx") == 2; }, 2000));
  const LogEv rx = *w.gate.last("cmd_rx");
  CHECK_EQ(rx.a, ACT_CLOSE_);
  CHECK(!w.gate.coil(1));
  CHECK(!w.gate.coil(2));
  r.run(3000);
  CHECK_EQ(r.count(1), 1);
  CHECK_EQ(r.count(2), 1);
  Pulse a = r.last(1), b = r.last(2);
  CHECK_EQ(a.off, rx.at);  // cut short when the CLOSE came
  CHECK_IN(a.len(), 350, 1000);
  CHECK_IN(b.on - a.off, INTERLOCK, INTERLOCK + 1);
  CHECK_IN(b.len(), 2000, 2001);
  CHECK_EQ(w.gate.count("pulse", 2), 1);
  auto &po = w.opener.presses[0].back(), &pc = w.opener.presses[1].back();
  CHECK((int32_t)(pc.first - (po.first + po.second)) >= INTERLOCK);
  CHECK(r.until([&] { return w.opener.atClosed(); }, 15000));
  r.checkInterlock();
  w.checkClean();
}

// [interlock]
TEST(gate_relays_interlock_open_cmd_5ms_after_close_pulse_ended) {
  World w;
  Guard g{ w };
  // A long pulse, so the house's OPEN (sent once the gate's ACK and STATUS for the CLOSE are through) is in hand
  // well before K2 ends.
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("pulse_ms", 2000));
  });
  Rec r(w);
  userMove(r, true);
  // The user closes, and while K2 runs switches back on. That OPEN is kept off the air and played to the gate so it
  // lands 5 ms after K2 released by itself.
  w.user(false);
  CHECK(r.until([&] { return w.gate.coil(2); }, 2000));
  uint32_t k2on = w.now;
  Bytes held;
  bool hold = true;
  w.drop = [&](const AirFrame &f) {
    if (!hold || f.from != 0 || f.type() != MSG_CMD_) return false;
    if (held.empty()) held = f.b;
    return true;
  };
  w.user(true);
  CHECK(r.until([&] { return !held.empty(); }, 1500));
  uint32_t T = pulseEnd(k2on, 2000), arrive = T + 5, air = w.airtimeMs(held.size());
  CHECK((int32_t)(arrive - air - w.now) > 0);
  CHECK(r.until([&] { return w.now == arrive - air; }, 2000));
  CHECK(w.gate.coil(2));
  w.airSend(held, 0);
  hold = false;
  CHECK(r.until([&] { return w.gate.count("cmd_rx") == 3; }, 100));
  const LogEv rx = *w.gate.last("cmd_rx");
  CHECK_EQ(rx.a, ACT_OPEN_);
  CHECK_EQ(rx.at, arrive);
  Pulse c = r.last(2);
  CHECK_EQ(c.off, T);
  CHECK_IN(c.len(), 2000, 2001);
  CHECK(!w.gate.coil(1));
  r.run(2500);
  Pulse o = r.last(1);
  CHECK_IN(o.on - T, INTERLOCK, INTERLOCK + 1);
  CHECK_IN(o.len(), 2000, 2001);
  auto &pc = w.opener.presses[1].back(), &po = w.opener.presses[0].back();
  CHECK((int32_t)(po.first - (pc.first + pc.second)) >= INTERLOCK);
  CHECK(r.until([&] { return w.opener.atOpen(); }, 15000));
  CHECK_EQ(r.count(1), 2);
  CHECK_EQ(r.count(2), 1);
  CHECK(w.house.status()["cmd_result"] == 0);
  r.checkInterlock();
  w.checkClean();
}

// [interlock]
TEST(gate_relays_interlock_cancelled_waiting_pulse_never_closes) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  uint32_t t0, t1, t2;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500", &t0)["ok"] == true);
  r.run(99);
  CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":300", &t1)["ok"] == true);  // K1 releases, K2 waits 100 ms
  r.run(49);
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":400", &t2)["ok"] == true);  // reversed before K2 closed
  r.run(1000);
  CHECK_EQ(r.count(2), 0);  // the CLOSE that was waiting never closed
  CHECK_EQ(w.opener.presses[1].size(), 0);
  std::vector<Pulse> k1s = r.of(1);
  CHECK_EQ(k1s.size(), 2);
  CHECK_EQ(k1s[0].off, t1);
  CHECK_IN(k1s[1].on - t2, 0, INTERLOCK + 1);
  CHECK_IN(k1s[1].len(), 400, 401);
  r.checkInterlock();
  w.checkClean();
}

// [interlock] [pulse-only]
TEST(gate_relays_interlock_holds_under_rapid_alternating_tests) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  uint64_t s = 0x9E3779B97F4A7C15ULL;
  auto rnd = [&](uint32_t n) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return (uint32_t)(s % n);
  };
  for (int i = 0, k = 1; i < 80; i++, k = 3 - k) {
    CHECK(req(r, w.gate, "relay.test", k == 1 ? "\"k\":1,\"ms\":120" : "\"k\":2,\"ms\":170")["ok"] == true);
    r.run(1 + rnd(280));
  }
  r.run(1000);
  r.checkInterlock();
  for (const Pulse &p : r.p) CHECK_IN(p.len(), 1, p.k == 1 ? 121 : 171);
  CHECK(r.count(1) >= 10);
  CHECK(r.count(2) >= 10);
  CHECK(!w.gate.coil(1));
  CHECK(!w.gate.coil(2));
  w.checkClean();
}

// [interlock] [pulse-only]
TEST(gate_relays_interlock_across_the_millis_wrap) {
  World w(0xFFFFFFFFu - 40000);
  Guard g{ w };
  // Harness: busyUntil starts at 0, which a clock past 2^31 reads as in the future (the boards would never run).
  w.house.busyUntil = w.gate.busyUntil = w.now;
  w.commission();
  Rec r(w);
  // K1 ends by itself ~40 ms before millis() wraps; K2 is asked for after the wrap, still inside the window.
  const uint32_t X = 0xFFFFFFFFu - 341;
  CHECK((int32_t)(w.now - X) < 0);
  CHECK(r.until([&] { return w.now == X; }, 60000));
  uint32_t t0;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":300", &t0)["ok"] == true);
  CHECK(r.until([&] { return !w.gate.coil(1); }, 400));
  uint32_t T = w.now;
  CHECK_IN(T - t0, 300, 301);
  CHECK((int32_t)T < 0);  // before the wrap
  r.run(69);
  uint32_t at;
  CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":300", &at)["ok"] == true);
  CHECK_EQ(at - T, 70);
  CHECK(at < 1000);  // after it
  r.run(1000);
  Pulse b = r.last(2);
  CHECK_IN(b.on - T, INTERLOCK, INTERLOCK + 1);
  CHECK_IN(b.len(), 300, 301);
  r.checkInterlock();
  w.checkClean();
}

// ---- relay-test-target --------------------------------------------------------------------------------------------

// [relay-test-target]
TEST(gate_relays_test_toward_the_other_limit_or_from_between_sets_a_target) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  JsonDocument s = w.gate.status();
  CHECK(s["gate"] == "closed");
  CHECK(s["target"] == "");
  size_t l0 = w.gate.logs.size();
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500")["ok"] == true);
  s = w.gate.status();
  CHECK(s["target"] == "open");
  CHECK(s["last_result"] == "none");
  CHECK(r.until([&] { return w.gateSees() == GS_OPEN_; }, 15000));
  std::vector<SC> st = statesSince(w.gate, l0);
  CHECK_EQ(st.size(), 2);
  CHECK(st[0] == SC(GS_BETWEEN_, CAUSE_LORA_));
  CHECK(st[1] == SC(GS_OPEN_, CAUSE_LORA_));
  s = w.gate.status();
  CHECK(s["target"] == "");
  CHECK(s["last_result"] == "reached");
  CHECK(r.until([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  CHECK(w.house.status()["cause"] == "lora");
  r.run(4000);

  // Between the limits (someone else is closing it): a test toward open is ours from there on
  l0 = w.gate.logs.size();
  w.extPress(false, 300);
  CHECK(r.until([&] { return w.gateSees() == GS_BETWEEN_; }, 3000));
  CHECK_EQ(w.gate.last("gate_state")->b, CAUSE_EXTERNAL_);
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500")["ok"] == true);
  s = w.gate.status();
  CHECK(s["gate"] == "between");
  CHECK(s["target"] == "open");
  CHECK(r.until([&] { return w.gateSees() == GS_OPEN_; }, 15000));
  st = statesSince(w.gate, l0);
  CHECK_EQ(st.size(), 2);
  CHECK(st[0] == SC(GS_BETWEEN_, CAUSE_EXTERNAL_));
  CHECK(st[1] == SC(GS_OPEN_, CAUSE_LORA_));
  s = w.gate.status();
  CHECK(s["last_result"] == "reached");
  CHECK(s["target"] == "");
  CHECK_EQ(w.gate.count("travel_timeout"), 0);
  w.checkClean();
}

// [relay-test-target]
TEST(gate_relays_test_at_the_limit_it_drives_to_sets_no_target) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // A command cycle first, so last_result reads "reached" (a new target would clear it to "none").
  userMove(r, true);
  userMove(r, false);
  JsonDocument s = w.gate.status();
  CHECK(s["last_result"] == "reached");

  // CLOSE test at the closed limit, then someone else opens it within the second
  CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":500")["ok"] == true);
  s = w.gate.status();
  CHECK(s["target"] == "");
  CHECK(s["last_result"] == "reached");
  r.run(600);
  CHECK_IN(r.last(2).len(), 500, 501);
  CHECK(w.opener.atClosed());
  size_t l0 = w.gate.logs.size();
  w.extPress(true, 500);
  CHECK(r.until([&] { return w.gateSees() == GS_OPEN_; }, 15000));
  std::vector<SC> st = statesSince(w.gate, l0);
  CHECK_EQ(st.size(), 2);
  CHECK(st[0] == SC(GS_BETWEEN_, CAUSE_EXTERNAL_));
  CHECK(st[1] == SC(GS_OPEN_, CAUSE_EXTERNAL_));
  s = w.gate.status();
  CHECK(s["last_result"] == "reached");  // not "timeout": nothing of ours was overridden
  CHECK(s["target"] == "");
  CHECK(r.until([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  JsonDocument h = w.house.status();
  CHECK(h["cause"] == "external");
  CHECK(h["last_result"] == "reached");
  r.run(4000);

  // OPEN test at the open limit, then someone else closes it
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500")["ok"] == true);
  s = w.gate.status();
  CHECK(s["target"] == "");
  CHECK(s["last_result"] == "reached");
  r.run(600);
  CHECK(w.opener.atOpen());
  l0 = w.gate.logs.size();
  w.extPress(false, 500);
  CHECK(r.until([&] { return w.gateSees() == GS_CLOSED_; }, 15000));
  st = statesSince(w.gate, l0);
  CHECK_EQ(st.size(), 2);
  CHECK(st[0] == SC(GS_BETWEEN_, CAUSE_EXTERNAL_));
  CHECK(st[1] == SC(GS_CLOSED_, CAUSE_EXTERNAL_));
  s = w.gate.status();
  CHECK(s["last_result"] == "reached");
  CHECK(s["target"] == "");
  CHECK_EQ(w.gate.count("travel_timeout"), 0);
  w.checkClean();
}

// [relay-test-target]
TEST(gate_relays_test_sets_no_target_while_the_opener_is_unpowered) {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // Dead opener (no AC, flat battery): no limit reads, no_power
  w.opener.ac = w.opener.battery = false;
  CHECK(r.until([&] { return w.gateSees() == GS_NO_POWER_; }, 3000));
  CHECK_EQ(w.gate.last("gate_state")->b, CAUSE_NONE_);
  uint32_t at;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500", &at)["ok"] == true);
  JsonDocument s = w.gate.status();
  CHECK(s["gate"] == "no_power");
  CHECK(s["target"] == "");
  CHECK(s["last_result"] == "none");
  r.run(65000);  // past travel_timeout_s (60): a target would have timed out by now
  Pulse p = r.last(1);
  CHECK_EQ(p.on, at);  // the wiring check still pulses
  CHECK_IN(p.len(), 500, 501);
  CHECK_EQ(w.opener.pos, 0);
  CHECK_EQ(w.gate.count("travel_timeout"), 0);
  CHECK(w.gate.status()["last_result"] == "none");
  // Power back at the closed limit: no cause, and not an overridden command
  w.opener.ac = w.opener.battery = true;
  CHECK(r.until([&] { return w.gateSees() == GS_CLOSED_; }, 5000));
  CHECK_EQ(w.gate.last("gate_state")->b, CAUSE_NONE_);
  s = w.gate.status();
  CHECK(s["last_result"] == "none");
  CHECK(s["target"] == "");
  r.run(5000);

  // On its battery and stopped between the limits: no_power too, though the opener does move for the test
  w.opener.ac = false;
  CHECK(r.until([&] { return w.gate.last("input") && w.gate.last("input")->a == 3 && w.gate.last("input")->b == 0; },
                1000));
  CHECK(w.gateSees() == GS_CLOSED_);  // its limit still reads
  w.extPress(true, 300);
  CHECK(r.until([&] { return w.opener.pos >= 3000; }, 5000));
  w.opener.dir = 0;  // stopped mid-travel
  CHECK(r.until([&] { return w.gateSees() == GS_NO_POWER_; }, 3000));
  CHECK_EQ(w.gate.last("gate_state")->b, CAUSE_NONE_);
  r.run(1000);
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":500")["ok"] == true);
  s = w.gate.status();
  CHECK(s["target"] == "");
  CHECK(s["last_result"] == "none");
  CHECK(r.until([&] { return w.gateSees() == GS_OPEN_; }, 15000));
  CHECK(w.opener.atOpen());
  CHECK_EQ(w.gate.last("gate_state")->b, CAUSE_NONE_);  // out of no_power: no cause
  s = w.gate.status();
  CHECK(s["last_result"] == "none");  // a target would read "reached"
  CHECK(s["target"] == "");
  CHECK_EQ(w.gate.count("travel_timeout"), 0);
  w.opener.ac = true;
  r.run(2000);
  w.checkClean();
}

// ---- no-stall-in-pulse --------------------------------------------------------------------------------------------

// [no-stall-in-pulse]
TEST(gate_relays_console_flash_and_radio_requests_wait_for_the_pulse) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;  // the datasheet's worst sector erase
  w.commission();
  Rec r(w);
  int lastErases = w.gate.flashErases, erasedDuringPulse = 0;
  r.each = [&] {
    if ((w.gate.coil(1) || w.gate.coil(2)) && w.gate.flashErases != lastErases) erasedDuringPulse++;
    lastErases = w.gate.flashErases;
  };
  struct Case {
    const char *cmd;
    std::string args;
    int erases;
  };
  const Case cases[] = {
    { "config.save", "", 1 },
    { "config.set", "\"params\":{\"debounce_ms\":60}", 0 },
    { "config.set", "\"params\":{\"tx_power\":5}", 0 },  // a radio param: restarts the radio
    { "key.set", "\"key\":\"" + keyHex() + "\"", 1 },    // saves and restarts the radio
    { "config.reset", "", 2 },                           // two sector erases
  };
  int k = 1;
  for (const Case &c : cases) {
    r.run(1500);
    int erases = w.gate.flashErases;
    uint32_t t0, sAt;
    CHECK(req(r, w.gate, "relay.test", k == 1 ? "\"k\":1,\"ms\":600" : "\"k\":2,\"ms\":600", &t0)["ok"] == true);
    r.run(99);
    CHECK(req(r, w.gate, "status", "", &sAt)["ok"] == true);  // one that doesn't stop the loop: answered at once
    CHECK_EQ(sAt, t0 + 100);
    int id = sendReq(w, w.gate, c.cmd, c.args);
    r.run(450);
    CHECK_EQ(w.gate.replies.count(id), 0);  // still held, the pulse still running
    CHECK_EQ(w.gate.flashErases, erases);
    if (c.args.find("debounce") != std::string::npos) CHECK_EQ(w.gate.get("debounce_ms"), 50);  // not applied yet
    if (c.args.find("tx_power") != std::string::npos) CHECK_EQ(w.gate.get("tx_power"), 17);
    uint32_t at;
    JsonDocument d = reply(r, w.gate, id, 2000, &at);
    Pulse p = r.last(k);
    CHECK(!p.open);
    CHECK_EQ(p.on, t0);
    CHECK_IN(p.len(), 600, 601);  // not a millisecond longer
    CHECK_EQ(at, p.off);          // handled in the pass the pulse ended, after the release
    CHECK(d["ok"] == true);
    CHECK_EQ(w.gate.flashErases - erases, c.erases);
    if (c.args.find("debounce") != std::string::npos) CHECK_EQ(w.gate.get("debounce_ms"), 60);
    if (c.args.find("tx_power") != std::string::npos) CHECK_EQ(w.gate.get("tx_power"), 5);
    if (!strcmp(c.cmd, "config.reset")) CHECK(d["reboot_required"] == true);
    k = 3 - k;
  }
  CHECK_EQ(erasedDuringPulse, 0);
  CHECK(w.gate.status()["key_set"] == false);  // config.reset did run
  w.checkClean();
}

// [no-stall-in-pulse] [interlock]
TEST(gate_relays_console_hold_ends_exactly_with_the_pulse) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission();
  Rec r(w);
  // A save asked for in a pulse's last millisecond is answered in the pass that releases it.
  uint32_t t0, at;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":200", &t0)["ok"] == true);
  uint32_t rel = pulseEnd(t0, 200);
  CHECK(r.until([&] { return w.now == rel - 2; }, 300));
  int id = sendReq(w, w.gate, "config.save");  // reaches the console at rel - 1
  CHECK(reply(r, w.gate, id, 2000, &at)["ok"] == true);
  CHECK_EQ(r.last(1).off, rel);
  CHECK_EQ(at, rel);
  CHECK_IN(r.last(1).len(), 200, 201);
  r.run(2000);

  // One reaching the console in the releasing pass itself is handled then (the relays update first).
  CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":200", &t0)["ok"] == true);
  rel = pulseEnd(t0, 200);
  CHECK(r.until([&] { return w.now == rel - 1; }, 300));
  id = sendReq(w, w.gate, "config.save");
  CHECK(reply(r, w.gate, id, 2000, &at)["ok"] == true);
  CHECK_EQ(r.last(2).off, rel);
  CHECK_EQ(at, rel);
  r.run(2000);

  // A pulse waiting for its interlock start counts as running: K1 300 ms, K2 asked for at +100 waits 100 ms.
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":300", &t0)["ok"] == true);
  r.run(99);
  CHECK(req(r, w.gate, "relay.test", "\"k\":2,\"ms\":300")["ok"] == true);
  CHECK(!w.gate.coil(1));
  CHECK(!w.gate.coil(2));
  int erases = w.gate.flashErases;
  id = sendReq(w, w.gate, "config.save");
  r.run(50);
  CHECK(!w.gate.coil(2));  // still in the interlock gap, and the save still held
  CHECK_EQ(w.gate.replies.count(id), 0);
  CHECK(reply(r, w.gate, id, 2000, &at)["ok"] == true);
  Pulse a = r.last(1), b = r.last(2);
  CHECK_EQ(a.off, t0 + 100);
  CHECK_IN(b.on - a.off, INTERLOCK, INTERLOCK + 1);
  CHECK_IN(b.len(), 300, 301);
  CHECK_EQ(at, b.off);
  CHECK_EQ(w.gate.flashErases, erases + 1);
  r.checkInterlock();
  w.checkClean();
}

// [no-stall-in-pulse]
TEST(gate_relays_remote_write_waits_for_the_pulse_then_saves) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("pulse_ms", 2000));
  });
  Rec r(w);
  // No pulse running: applied and saved the moment it arrives.
  int erases = w.gate.flashErases;
  size_t f0 = w.sent(w.house, MSG_CFG_SET_).size(), ev0 = w.house.events.size();
  CHECK(req(r, w.house, "remote.set", "\"name\":\"travel_timeout_s\",\"value\":70")["ok"] == true);
  CHECK(r.until([&] { return w.gate.count("cfg_remote") == 1; }, 5000));
  std::vector<const AirFrame *> frames = w.sent(w.house, MSG_CFG_SET_);
  CHECK_EQ(frames.size(), f0 + 1);
  CHECK_EQ(w.gate.last("cfg_remote")->at, frames.back()->end);
  CHECK_EQ(w.gate.flashErases, erases + 1);
  CHECK(r.until([&] { return eventSince(w.house, "remote_set", ev0) != nullptr; }, 5000));
  CHECK((*eventSince(w.house, "remote_set", ev0))["ok"] == true);
  r.run(2000);

  // K1 closed for the user's OPEN: a write arriving now waits for the release.
  w.user(true);
  CHECK(r.until([&] { return w.gate.coil(1); }, 2000));
  r.run(50);
  erases = w.gate.flashErases;
  f0 = w.sent(w.house, MSG_CFG_SET_).size();
  ev0 = w.house.events.size();
  int touched = 0;
  r.each = [&] {
    if (w.gate.coil(1)
        && (w.gate.flashErases != erases || w.gate.get("pulse_ms") != 2000 || w.gate.count("cfg_remote") != 1))
      touched++;
  };
  CHECK(req(r, w.house, "remote.set", "\"name\":\"pulse_ms\",\"value\":800")["ok"] == true);
  CHECK(r.until([&] { return !w.gate.coil(1); }, 3000));
  r.each = nullptr;
  Pulse p = r.last(1);
  CHECK_IN(p.len(), 2000, 2001);
  frames = w.sent(w.house, MSG_CFG_SET_);
  CHECK(frames.size() > f0);
  const AirFrame *first = frames[f0];
  CHECK(!first->dropped);
  CHECK((int32_t)(first->end - p.on) > 0);
  CHECK((int32_t)(p.off - first->end) > 500);  // it was there well before the release
  CHECK_EQ(touched, 0);
  CHECK_EQ(w.gate.count("cfg_remote"), 2);
  const LogEv cr = *w.gate.last("cfg_remote");
  CHECK_EQ(cr.at, p.off);  // applied in the pass that released K1
  CHECK_EQ(cr.a, ID_PULSE_MS);
  CHECK_EQ(cr.b, 800);
  CHECK_EQ(w.gate.flashErases, erases + 1);
  CHECK_EQ(w.gate.get("pulse_ms"), 800);
  CHECK(r.until([&] { return eventSince(w.house, "remote_set", ev0) != nullptr; }, 10000));
  const JsonDocument &e = *eventSince(w.house, "remote_set", ev0);
  CHECK(e["acked"] == true);
  CHECK(e["ok"] == true);
  CHECK(e["applied"] == true);
  CHECK(w.gate.status()["link"]["replay"] == 0);  // its resends meanwhile were held, not taken as replays

  // The next pulse is the new length, and it was saved.
  CHECK(r.until([&] { return w.opener.atOpen(); }, 15000));
  CHECK(r.until([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  r.run(4000);
  w.user(false);
  CHECK(r.until([&] { return w.opener.atClosed(); }, 15000));
  CHECK_EQ(r.count(2), 1);
  CHECK_IN(r.last(2).len(), 800, 801);
  CHECK_EQ(w.gate.last("pulse")->b, 800);
  r.run(3000);
  w.gate.reset(PM_RCAUSE_EXT);
  CHECK(r.until([&] { return w.gate.running(); }, 2000));
  CHECK_EQ(w.gate.get("pulse_ms"), 800);
  CHECK_EQ(w.gate.get("travel_timeout_s"), 70);
  r.run(5000);
  w.checkClean();
}

// [no-stall-in-pulse]
TEST(gate_relays_command_lost_to_a_save_still_gets_its_full_pulse) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission();
  Rec r(w);
  // The console saves just as the user's OPEN comes over the air: lost with the radio held, resent by the house,
  // and pulsed whole after the save.
  size_t c0 = w.sent(w.house, MSG_CMD_).size();
  w.user(true);
  r.run(55);
  uint32_t at;
  CHECK(req(r, w.gate, "config.save", "", &at)["ok"] == true);
  uint32_t saveEnd = w.gate.busyUntil;
  CHECK_IN(saveEnd - at, 400, 1500);  // an erase, plus the radio restart
  CHECK(r.until([&] { return w.opener.atOpen(); }, 20000));
  std::vector<const AirFrame *> cmds = w.sent(w.house, MSG_CMD_);
  CHECK(cmds.size() >= c0 + 2);
  CHECK((int32_t)(cmds[c0]->end - at) > 0);
  CHECK((int32_t)(saveEnd - cmds[c0]->end) > 0);  // it arrived during the save
  CHECK_EQ(w.gate.count("cmd_rx"), 1);
  CHECK_EQ(r.count(1), 1);
  Pulse p = r.last(1);
  CHECK((int32_t)(p.on - saveEnd) >= 0);
  CHECK_IN(p.len(), 500, 501);
  CHECK_IN(w.opener.presses[0].back().second, 500, 501);
  CHECK(r.until([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  r.run(4000);
  userMove(r, false);

  // Same with a remote write: the gate saves it as it arrives, and the OPEN sent right behind it is lost meanwhile.
  int n = w.gate.count("cfg_remote");
  uint32_t cfgAt = 0, cfgEnd = 0;
  r.each = [&] {
    if (!cfgAt && w.gate.count("cfg_remote") > n) {
      cfgAt = w.now;
      cfgEnd = w.gate.busyUntil;
    }
  };
  c0 = w.sent(w.house, MSG_CMD_).size();
  CHECK(req(r, w.house, "remote.set", "\"name\":\"debounce_ms\",\"value\":60")["ok"] == true);
  w.user(true);
  CHECK(r.until([&] { return w.opener.atOpen(); }, 20000));
  r.each = nullptr;
  CHECK(cfgAt != 0);
  CHECK_IN(cfgEnd - cfgAt, 400, 1500);
  cmds = w.sent(w.house, MSG_CMD_);
  CHECK(cmds.size() >= c0 + 2);
  CHECK((int32_t)(cmds[c0]->end - cfgAt) > 0);
  CHECK((int32_t)(cfgEnd - cmds[c0]->end) > 0);  // it arrived during the save
  CHECK_EQ(r.count(1), 2);
  p = r.last(1);
  CHECK((int32_t)(p.on - cfgEnd) >= 0);
  CHECK_IN(p.len(), 500, 501);
  CHECK_EQ(w.gate.get("debounce_ms"), 60);
  CHECK_EQ(w.gate.count("cmd_rx"), 3);
  w.checkClean();
}

// [no-stall-in-pulse]
XFAIL_TEST(gate_relays_radio_reinit_waits_for_the_pulse,
           "radio.cpp re-initialises the radio without waiting for appRelaysPulsing (the 5 s retry in radioReceive, "
           "and fault()): the pass is held for LoRa.begin() (25 ms in the sim, ~470 ms of module reset delays on the MKR "
           "WAN 1310) and a pulse ending meanwhile is released late") {
  World w;
  Guard g{ w };
  w.commission();
  Rec r(w);
  // The radio stops answering. A restart then fails, and the radio is retried every 5 s from radioReceive().
  w.gate.radioPresent = false;
  CHECK(req(r, w.gate, "config.set", "\"params\":{\"tx_power\":5}")["ok"] == true);
  const LogEv *f = w.gate.last("radio_fail");
  CHECK(f);
  CHECK_EQ(f->a, 0);
  uint32_t retry = (f->t + 5000) | 1;  // the next try's pass (RETRY_MS after the failed one ended)
  uint32_t busyAtRetry = 0;
  r.each = [&] {
    if (w.now == retry) busyAtRetry = w.gate.busyUntil;
  };
  // A relay test due to end 10 ms into that retry.
  CHECK(r.until([&] { return w.now == retry - 191; }, 6000));
  uint32_t t0;
  CHECK(req(r, w.gate, "relay.test", "\"k\":1,\"ms\":200", &t0)["ok"] == true);
  CHECK_EQ(retry - t0, 190);
  r.run(1000);
  r.each = nullptr;
  CHECK(busyAtRetry - retry >= 20);  // the retry did hold that pass
  CHECK_IN(r.last(1).len(), 200, 201);
  w.checkClean();
}

// ---- watchdog -----------------------------------------------------------------------------------------------------

// [watchdog]
TEST(gate_relays_watchdog_burst_of_saves_one_per_loop_pass) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission();
  Rec r(w);
  uint32_t boots = w.gate.boots;
  int bootLogs = w.gate.count("boot"), erases = w.gate.flashErases;
  // 20 saves of ~0.43 s each: more than the watchdog's 8 s if one loop pass took them all.
  const int N = 20;
  std::vector<int> ids;
  for (int i = 0; i < N; i++) ids.push_back(sendReq(w, w.gate, "config.save"));
  std::vector<uint32_t> at(N, 0);
  std::vector<bool> got(N, false);
  CHECK(r.until([&] {
    bool all = true;
    for (int i = 0; i < N; i++) {
      if (!got[i] && w.gate.replies.count(ids[i])) {
        got[i] = true;
        at[i] = w.now;
        CHECK(w.gate.replies[ids[i]]["ok"] == true);
      }
      all = all && got[i];
    }
    return all;
  }, 30000));
  for (int i = 1; i < N; i++) CHECK(at[i] - at[i - 1] >= 400);  // one per pass, the watchdog kicked between
  CHECK(at[N - 1] - at[0] >= 8000);
  CHECK_EQ(w.gate.flashErases - erases, N);
  CHECK_EQ(w.gate.boots, boots);
  CHECK_EQ(w.gate.count("boot"), bootLogs);
  JsonDocument s = w.gate.status();
  CHECK_IN(s["loop_max_us"].as<uint32_t>(), 400000, 1500000);
  CHECK(s["reset_cause"] == "software");  // still commission()'s reset
  CHECK(r.poll([&] { return w.house.status()["link"]["verified"] == true && w.gate.status()["link"]["verified"] == true; },
               20000));
  w.checkClean();
}

// [watchdog]
TEST(gate_relays_watchdog_burst_of_saves_on_the_uart_one_per_loop_pass) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("uart_console", 1));
  });
  Rec r(w);
  uint32_t boots = w.gate.boots;
  int bootLogs = w.gate.count("boot"), erases = w.gate.flashErases;
  // The same burst on the UART console (CRC-tagged, as that port requires): its requests are taken one per pass as
  // well, however many wait in Serial1's buffer. (The adapter is attached only for this exchange.)
  w.gate.uartAdapter = true;
  const int N = 20;
  std::vector<int> ids;
  for (int i = 0; i < N; i++) ids.push_back(sendReq(w, w.gate, "config.save", "", 1));
  std::vector<uint32_t> at(N, 0);
  std::vector<bool> got(N, false);
  CHECK(r.until([&] {
    bool all = true;
    for (int i = 0; i < N; i++) {
      if (!got[i] && w.gate.replies.count(ids[i])) {
        got[i] = true;
        at[i] = w.now;
        CHECK(w.gate.replies[ids[i]]["ok"] == true);
      }
      all = all && got[i];
    }
    return all;
  }, 30000));
  w.gate.uartAdapter = false;
  for (int i = 1; i < N; i++) CHECK(at[i] - at[i - 1] >= 400);  // one per pass, the watchdog kicked between
  CHECK(at[N - 1] - at[0] >= 8000);
  CHECK_EQ(w.gate.flashErases - erases, N);
  CHECK_EQ(w.gate.boots, boots);
  CHECK_EQ(w.gate.count("boot"), bootLogs);
  JsonDocument s = w.gate.status();
  CHECK_IN(s["loop_max_us"].as<uint32_t>(), 400000, 1500000);
  CHECK(s["reset_cause"] == "software");  // still commission()'s reset
  r.run(5000);
  CHECK_EQ(w.gate.boots, boots);
  w.checkClean();
}

// [watchdog]
TEST(gate_relays_watchdog_worst_pass_factory_reset_on_both_ports) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission([](Board &b) {
    if (b.idx == 1) CHECK(b.set("uart_console", 1));
  });
  Rec r(w);
  uint32_t boots = w.gate.boots;
  // The longest request (two sector erases and a radio restart) on both console ports at once: one per port per
  // pass, so this is the longest pass the console can cause. (The adapter is attached only for this exchange: with
  // both ports open every log line arrives twice.)
  w.gate.uartAdapter = true;
  int u = sendReq(w, w.gate, "config.reset", "", 0);
  int a = sendReq(w, w.gate, "config.reset", "", 1);
  uint32_t atU = 0, atA = 0;
  CHECK(r.until([&] {
    if (!atU && w.gate.replies.count(u)) atU = w.now;
    if (!atA && w.gate.replies.count(a)) atA = w.now;
    return atU && atA;
  }, 5000));
  w.gate.uartAdapter = false;
  CHECK(w.gate.replies[u]["ok"] == true);
  CHECK(w.gate.replies[a]["ok"] == true);
  CHECK_EQ(atU, atA);  // the same pass
  CHECK_IN(w.gate.busyUntil - atU, 1600, 4000);  // four erases and two radio restarts: well short of 8 s
  CHECK_IN(w.gate.status()["loop_max_us"].as<uint32_t>(), 1600000, 4000000);
  r.run(10000);
  CHECK_EQ(w.gate.boots, boots);
  CHECK_EQ(w.gate.count("boot", PM_RCAUSE_WDT), 0);
  w.checkClean();
}

// [watchdog]
TEST(gate_relays_watchdog_boot_with_boot_counter_rollover) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission();
  Rec r(w);
  // Fill the boot counter's sector (append-only 4-byte slots in sectors 2 and 3) so the next boot has to erase the
  // other one: the longest boot the flash can cause.
  const uint32_t s2 = 2 * 4096, s3 = 3 * 4096, slots = 4096 / 4;
  auto get32 = [&](uint32_t addr) {
    const uint8_t *p = &w.gate.flash[addr];
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
  };
  uint32_t maxV = 0, slot = 0;
  for (; slot < slots && get32(s2 + 4 * slot) != 0xFFFFFFFFu; slot++)
    if (get32(s2 + 4 * slot) > maxV) maxV = get32(s2 + 4 * slot);
  CHECK(maxV >= 2);
  CHECK(slot < slots);
  for (uint32_t i = 0; i < 4096; i++) CHECK(w.gate.flash[s3 + i] == 0xFF);
  uint32_t v = maxV;
  for (; slot < slots; slot++) {
    v++;
    for (int i = 0; i < 4; i++) w.gate.flash[s2 + 4 * slot + i] = (uint8_t)(v >> (8 * i));
  }
  uint32_t boots = w.gate.boots;
  int erases = w.gate.flashErases;
  w.gate.reset(PM_RCAUSE_EXT);
  CHECK(r.until([&] { return w.gate.boots == boots + 1; }, 2000));
  uint32_t setupMs = w.gate.busyUntil - w.now;
  CHECK_IN(setupMs, 400, 1500);
  CHECK_EQ(w.gate.flashErases - erases, 1);
  const LogEv *b = w.gate.last("boot");
  CHECK(b);
  CHECK_EQ(b->a, PM_RCAUSE_EXT);
  CHECK_IN(b->t - b->at, 400, 1000);  // the erase ran before the boot was logged
  r.run(5000);
  CHECK_EQ(req(r, w.gate, "info")["boot_count"].as<uint32_t>(), v + 1);
  CHECK_EQ(get32(s3), v + 1);
  // The boot after that just appends.
  boots = w.gate.boots;
  erases = w.gate.flashErases;
  w.gate.reset(PM_RCAUSE_EXT);
  CHECK(r.until([&] { return w.gate.boots == boots + 1; }, 2000));
  CHECK(w.gate.busyUntil - w.now + 350 <= setupMs);  // no erase this time
  CHECK_EQ(w.gate.flashErases, erases);
  r.run(5000);
  CHECK_EQ(req(r, w.gate, "info")["boot_count"].as<uint32_t>(), v + 2);
  CHECK_EQ(w.gate.count("boot", PM_RCAUSE_WDT), 0);
  CHECK(w.gate.status()["reset_cause"] == "reset_pin");
  w.checkClean();
}

// [watchdog] [no-stall-in-pulse]
TEST(gate_relays_watchdog_repeated_remote_writes) {
  World w;
  Guard g{ w };
  w.gate.eraseMs = 400;
  w.commission();
  Rec r(w);
  uint32_t boots = w.gate.boots;
  int erases = w.gate.flashErases;
  for (int i = 0; i < 6; i++) {
    size_t ev0 = w.house.events.size();
    CHECK(req(r, w.house, "remote.set", "\"name\":\"heartbeat_s\",\"value\":" + std::to_string(31 + i))["ok"] == true);
    CHECK(r.until([&] { return eventSince(w.house, "remote_set", ev0) != nullptr; }, 15000));
    CHECK((*eventSince(w.house, "remote_set", ev0))["ok"] == true);
  }
  CHECK_EQ(w.gate.get("heartbeat_s"), 36);
  CHECK_EQ(w.gate.flashErases - erases, 6);
  CHECK_EQ(w.gate.count("cfg_remote"), 6);
  CHECK_EQ(w.gate.boots, boots);
  CHECK_IN(w.gate.status()["loop_max_us"].as<uint32_t>(), 400000, 1500000);
  r.run(2000);
  w.checkClean();
}
