// The whole site on a hostile or busy channel: another transmitter on our frequency, SF and sync word, and outages.
#include "world.h"
#include <Arduino.h>

// A short frame of another network (net_id 0x43 against our 0x42), as a neighbour's LoRa would send it: heard, then
// dropped on its header. 13 bytes: ~41 ms at SF9/500 kHz.
static Bytes neighbourFrame() {
  Bytes f(13, 0x5A);
  f[0] = 1;     // PROTO_VER
  f[1] = 6;     // PING
  f[2] = 0x43;  // net_id
  return f;
}

// [busy-channel] A neighbour sending a short frame every 100 ms leaves ~59 ms gaps, room for our frames but shorter
// than the new-frame backoff (73..106 ms at SF9/500 kHz, counted from the last frame heard): it used to hold the
// house's command off until its TTL ran out. Now the gate opens within a couple of seconds.
TEST(link_command_gets_through_a_neighbour_sending_every_100ms) {
  World w;
  w.commission();
  bool flood = true;
  uint32_t next = w.now;
  w.user(true);
  uint32_t asked = w.now;
  CHECK(w.runUntil([&] {
    if (flood && (int32_t)(w.now - next) >= 0) {
      w.airSend(neighbourFrame());
      next = w.now + 100;
    }
    return w.gate.count("cmd_rx") > 0;
  }, 10000));
  CHECK(w.now - asked < 3000);
  flood = false;
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 3000));
  CHECK(w.house.count("lbt_forced") + w.gate.count("lbt_forced") >= 1);
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
}

// [busy-channel] The gate's status reports keep getting through a busy neighbour too. A frame every 130..140 ms
// leaves 89..99 ms gaps: room for a STATUS (47 bytes, ~77 ms on air) sent within a few ms of a gap opening, but the
// backoff (73..106 ms after the last frame heard) started it too late in the gap, so every one collided and the house
// lost the link (timeout 100 s). Now it starts as a gap opens. Three phases of the neighbour against our boot, so it
// can't pass by alignment luck. (Gaps not longer than our frames plus the few ms we take to notice one are jamming:
// 120 ms apart leaves 79 ms, and then the house losing the link, and failing safe, is right.) And no frame of ours
// starts while a neighbour's is on the air and detectable (6 ms in at SF9/500 kHz): the busy cap once let one go
// straight into it, kept from an earlier frame.
TEST(link_stays_up_through_five_minutes_of_neighbour_traffic) {
  for (uint32_t phase : { 0u, 37u, 71u }) {
    World w;
    w.commission();
    uint32_t next = w.now + phase, until = w.now + 300000, checked = w.now;
    int intoBusy = 0;
    auto checkAir = [&] {  // our frames since the last look, against the neighbour's on the air (the last minute's)
      for (const AirFrame &ours : w.air) {
        if (ours.from < 0 || (int32_t)(ours.start - checked) <= 0) continue;
        for (const AirFrame &n : w.air)
          if (n.from < 0 && (int32_t)(ours.start - (n.start + 6)) >= 0 && (int32_t)(ours.start - n.end) < 0) {
            intoBusy++;
            w.trace.add(ours.start, std::string(ours.from == w.house.idx ? "house" : "gate") + " frame type " +
                                        std::to_string(ours.type()) + " sent into a neighbour's (" +
                                        std::to_string(n.start) + ".." + std::to_string(n.end) + ")");
          }
      }
      checked = w.now;
    };
    while ((int32_t)(w.now - until) < 0) {
      if ((int32_t)(w.now - next) >= 0) {
        w.airSend(neighbourFrame());
        next = w.now + 130 + (w.now % 3) * 5;  // 130..140 ms apart
      }
      w.step();
      if (w.now % 1000 == 0) checkAir();
    }
    checkAir();
    CHECK_EQ(intoBusy, 0);
    CHECK_EQ(w.house.count("link_down"), 0);
    CHECK(w.houseLinkUp());
    CHECK(w.sensorClosed());
    CHECK(w.gate.count("lbt_forced") >= 1);  // the gaps were too short for the backoff: the cap is what got through
  }
}

// [outage] A five-second outage as the user opens, then a lossy channel that loses the first command frame after it
// and every other one: with the resends spread over the TTL (four left after 5 s) it still arrives. With pure
// doubling (before 0.13.7) only the last resend, at 9.7 s, was left, and losing it lost the command.
TEST(link_command_survives_a_5s_outage_then_a_lossy_channel) {
  World w;
  w.commission();
  uint32_t start = w.now;
  uint32_t n = 0;
  w.drop = [&](const AirFrame &f) {
    if ((int32_t)(w.now - (start + 5000)) < 0) return true;  // outage, both ways
    return f.from == w.house.idx && f.type() == 4 && (n++ % 2) == 0;  // then every other CMD, the first one too
  };
  w.user(true);
  CHECK(w.runUntil([&] { return w.gate.count("cmd_rx") > 0; }, 10000));
  w.drop = nullptr;
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
}

// [busy-channel] [restarts] A command held on a replayed HELLO (cmd_hold) waits for the verified session to answer
// the house's challenge, not for the channel, so the hold mustn't count toward the wait cap: released, it takes its
// backoff after the frame that released it (the HELLO_ACK), not the capped path that goes within a few ms of a
// frame's end, logged lbt_forced. Found on the bench (0.13.7): a 1.8 s hold logged lbt_forced. A neighbour's frames
// keep the command's first resend waiting for the channel as the HELLO arrives, so its wait has begun before the hold.
TEST(link_command_held_on_a_replayed_hello_keeps_its_backoff) {
  const uint8_t CMD = 4;
  World w;
  w.commission();
  // Restart the gate's link (a radio param change): a new session, and its boot HELLO is now an old session's.
  int tx = w.gate.get("tx_power");
  int sessions = w.house.status()["link"]["sessions"];
  for (int p : { tx - 1, tx })
    CHECK(w.gate.request("config.set", "\"params\":{\"tx_power\":" + std::to_string(p) + "}")["ok"] == true);
  CHECK(w.runUntil([&] {
    return w.house.status()["link"]["sessions"].as<int>() > sessions && w.gate.status()["link"]["verified"] == true;
  }, 20000));
  w.run(5000);
  CHECK(w.gate.request("debug.mute", "\"ms\":3000")["ok"] == true);  // the command goes unheard: it stays pending
  size_t c0 = w.sent(w.house, CMD).size();
  int held0 = w.house.count("cmd_hold", 1), released0 = w.house.count("cmd_hold", 0), pulses0 = w.gate.count("pulse");
  w.user(true);
  CHECK(w.runUntil([&] { return w.sent(w.house, CMD).size() > c0; }, 3000));
  uint32_t first = w.sent(w.house, CMD).back()->end;
  // Neighbour frames 60 ms apart from 200 ms after it (its first resend is due at about a 32nd of the TTL), each
  // gap shorter than the backoff, for less than the wait cap.
  for (uint32_t at = first + 200; at <= first + 380; at += 60) {
    w.run(at - w.now);
    w.airSend(neighbourFrame());
  }
  w.run(45);
  bool held = false;
  for (int i = 0; i < 4 && !held; i++) {
    w.gate.request("debug.replay", "\"hello\":true");
    held = w.runUntil([&] { return w.house.count("cmd_hold", 1) > held0; }, 200);
  }
  CHECK(held);
  int forced0 = w.house.count("lbt_forced");
  CHECK(w.runUntil([&] { return w.house.count("cmd_hold", 0) > released0; }, 10000));
  CHECK(w.runUntil([&] { return w.gate.count("pulse") > pulses0; }, 10000));
  CHECK_EQ(w.house.count("lbt_forced"), forced0);
  // The command's first frame after the release started at least the turnaround after the frame before it ended.
  const AirFrame *after = nullptr;
  for (const AirFrame *f : w.sent(w.house, CMD))
    if (f->start >= w.house.last("cmd_hold")->at) { after = f; break; }
  CHECK(after != nullptr);
  uint32_t prevEnd = 0;
  for (const AirFrame &f : w.air)
    if (f.end <= after->start && f.end > prevEnd) prevEnd = f.end;
  CHECK(after->start - prevEnd >= 25);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  CHECK_EQ(w.gate.count("pulse"), pulses0 + 1);
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
}

static uint32_t seqOf(const Bytes &f) { return f[9] | f[10] << 8 | f[11] << 16 | (uint32_t)f[12] << 24; }

// [sensor-closed-only-known] A STATUS kept from the house (jammed at it while recorded) and played back once the gate
// has opened (threat model withheld-gate-frames-replayed). The link takes it, a seq it hasn't seen inside its window,
// but it's older than the last STATUS the house took, so it changes nothing: the house goes on showing open, the
// contact sensor stays open, and it doesn't count as hearing from the gate. Before 0.13.9 it set the house back to
// closed, closing the sensor with the gate open, and kept the link up for another link timeout.
TEST(link_withheld_status_delivered_late_changes_nothing) {
  World w;
  w.commission();
  CHECK(w.sensorClosed());
  // The gate's next STATUS (closed), every send of it.
  Bytes kept;
  w.drop = [&](const AirFrame &f) {
    if (f.from != w.gate.idx || f.type() != 5) return false;
    if (kept.empty()) kept = f.b;
    return seqOf(f.b) == seqOf(kept);
  };
  CHECK(w.runUntil([&] { return !kept.empty(); }, 40000));
  CHECK_EQ(kept[13], GS_CLOSED_);  // ST_STATE
  w.run(11000);  // the gate gives up on it (STATUS TTL 10 s)
  // Someone else opens the gate; the house takes its new STATUS.
  w.extPress(true, 500);
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 20000));
  CHECK(!w.sensorClosed());
  // From here on nothing from the gate gets through. Just before the link timeout runs out, the kept STATUS arrives.
  w.drop = [&](const AirFrame &f) { return f.from == w.gate.idx; };
  uint32_t taken = w.now, timeout = w.houseLinkTimeoutMs();
  w.run(timeout - 5000);
  CHECK(w.houseLinkUp());
  int replays = w.house.count("replay");
  w.inject(w.house, kept);
  w.run(200);
  CHECK_EQ(w.house.count("replay"), replays + 1);
  CHECK_EQ(w.houseSees(), GS_OPEN_);
  CHECK(!w.sensorClosed());
  CHECK(w.house.status()["gate"] == "open");
  // Nor does it keep the link up: down as the timeout after the open STATUS runs out.
  CHECK(w.runUntil([&] { return !w.houseLinkUp(); }, 6000));
  CHECK_IN(w.now - taken, timeout - 100, timeout + 100);
  CHECK(w.opener.atOpen());
}

static std::string testKeyHex() {
  std::string s;
  char b[3];
  for (uint8_t v : TEST_KEY) snprintf(b, sizeof(b), "%02x", v), s += b;
  return s;
}

// [restarts] Session ids are drawn from the boot count and the draws this boot, so they never repeat, even with an
// entropy source that's stuck: this gate's radioRandom32() returns one value every time. Reboots, a radio setting and
// the key each start a new session, and the house verifies each, never taking the gate's frames for replays. Before
// 0.13.9 the ids were random: this gate came back with the same session (and the same starting seq), and the house
// dropped its frames as replays.
TEST(link_sessions_never_repeat_across_reboots_and_restarts_even_with_a_stuck_rng) {
  World w;
  w.gate.stuckRng = 0x5A5A5A5Au;
  w.commission();
  std::vector<int32_t> seen = { w.house.last("session")->a };
  auto restarted = [&](const char *what) {
    int n = w.house.count("session");
    if (!w.runUntil([&] { return w.house.count("session") > n; }, 20000))
      throw Failure(std::string("the house verified no new gate session after ") + what);
    int32_t s = w.house.last("session")->a;
    for (int32_t o : seen)
      if (o == s) throw Failure(std::string("the gate's session repeated after ") + what);
    seen.push_back(s);
    w.run(5000);
  };
  for (int i = 0; i < 3; i++) {
    w.gate.reset(PM_RCAUSE_SYST);
    restarted("a reboot");
  }
  CHECK(w.gate.request("config.set", "\"params\":{\"tx_power\":5}")["ok"] == true);
  restarted("a radio setting");
  CHECK(w.gate.request("key.set", "\"key\":\"" + testKeyHex() + "\"")["ok"] == true);
  restarted("key.set");
  w.gate.reset(PM_RCAUSE_SYST);
  restarted("a reboot");
  CHECK_EQ(w.house.status()["link"]["replay"].as<int>(), 0);
  // And the link works: the user opens the gate.
  w.user(true);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  CHECK(w.runUntil([&] { return w.houseSees() == GS_OPEN_; }, 5000));
}
