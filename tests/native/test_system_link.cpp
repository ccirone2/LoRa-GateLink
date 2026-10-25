// The whole site on a hostile or busy channel: another transmitter on our frequency, SF and sync word, and outages.
#include "world.h"
#include <Arduino.h>

// A short frame of another network (net_id 0x43 against our 0x42), as a neighbour's LoRa would send it: heard, then
// dropped on its header. 13 bytes: ~41 ms at SF9/500 kHz.
static Bytes neighbourFrame() {
  Bytes f(13, 0x5A);
  f[0] = 2;     // PROTO_VER
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
  CHECK(poll(w, [&] {
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

// [replay] A command recorded and replayed while its seq is still in the gate's ACK memo: the gate never runs it again,
// and re-ACKs it only as often as a sender can resend (10, the most `retries` allows); the rest are replays, counted
// and logged, that draw no transmission (0.19.1, ROADMAP F21). Before, each copy was answered with an ACK.
TEST(link_replayed_command_in_the_ack_memo_is_answered_at_most_10_times) {
  const uint8_t CMD = 4, ACK = 3;
  World w;
  w.commission();
  w.user(true);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  w.run(5000);
  Bytes cmd = w.sent(w.house, CMD).back()->b;
  size_t acks = w.sent(w.gate, ACK).size();
  int pulses = w.gate.count("pulse"), replays = w.gate.count("replay");
  for (int i = 0; i < 14; i++) {
    w.inject(w.gate, cmd);
    w.run(300);
  }
  CHECK_EQ(w.sent(w.gate, ACK).size(), acks + 10);
  CHECK_EQ(w.gate.count("replay"), replays + 4);
  CHECK(w.gate.status()["link"]["replay"] == 4);
  CHECK_EQ(w.gate.count("pulse"), pulses);
  CHECK(w.opener.atOpen());
  CHECK(w.houseLinkUp());
}

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
  CHECK_EQ(w.plain(kept)[0], GS_CLOSED_);  // ST_STATE
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

// [freshness] [pulse-only] A command kept from the gate (jammed at it while recorded: every send of it) and played back
// a minute after the house gave it up (threat model withheld-command-fires-late). Its seq is still inside the gate's
// replay window, so the link takes it, but its deadline (the gate's clock, as the house had it from STATUS) has
// passed: refused (cmd_late, ACKed RES_LATE), no pulse. Before 0.16.0 the gate opened, at a time the attacker chose.
TEST(link_withheld_command_played_back_later_is_refused) {
  World w;
  w.commission();
  Bytes kept;
  w.drop = [&](const AirFrame &f) {
    if (f.from != w.house.idx || f.type() != 4) return false;  // MSG_CMD
    if (kept.empty()) kept = f.b;
    return true;
  };
  w.user(true);
  CHECK(w.runUntil([&] { return w.house.count("cmd_dropped") > 0; }, 15000));  // cmd_ttl_s, then given up
  w.drop = nullptr;
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 90000));  // resynced after mismatch_timeout_s: it never moved
  w.run(60000);
  int replays = w.gate.count("replay");
  w.inject(w.gate, kept);
  w.run(500);
  CHECK_EQ(w.gate.count("replay"), replays);  // in the window: only the deadline stops it
  CHECK_EQ(w.gate.count("cmd_late"), 1);
  CHECK(w.gate.last("cmd_late")->b > 60000);  // ms past its deadline
  CHECK_EQ(w.gate.count("cmd_rx"), 0);
  CHECK_EQ(w.gate.count("pulse"), 0);
  w.run(5000);
  CHECK(w.opener.atClosed());
  CHECK(w.sensorClosed());
}

// [freshness] [pulse-only] The owner opens, then closes a second later. The open command is kept from the gate (every
// send of it jammed there and recorded); the close gets through, and the gate, closed already, takes it. Then the open
// is played back, inside its deadline and inside the replay window (a lower seq it hasn't seen): it must not run, as
// the owner's last word was close. Found by the model checker (tools/verify/tla, CmdOrder): the gate checked a
// command's id only against the last one, so the older command opened the gate.
XFAIL_TEST(link_withheld_older_command_after_a_newer_one_is_refused, "TODO.md: a withheld older command runs after a newer one") {
  World w;
  w.commission();
  CHECK(w.opener.atClosed());
  Bytes kept;
  w.drop = [&](const AirFrame &f) {
    if (f.from != w.house.idx || f.type() != 4 || w.plain(f.b)[2] != 1) return false;  // MSG_CMD, ACT_OPEN
    if (kept.empty()) kept = f.b;
    return true;
  };
  w.user(true);
  CHECK(w.runUntil([&] { return !kept.empty(); }, 2000));
  w.run(1000);
  w.user(false);
  CHECK(w.runUntil([&] { return w.gate.count("cmd_rx") > 0; }, 5000));  // the close, the gate already closed
  CHECK_EQ(w.gate.count("pulse"), 0);
  w.drop = nullptr;
  w.inject(w.gate, kept);
  w.run(3000);
  CHECK_EQ(w.gate.count("pulse"), 0);
  CHECK(w.opener.atClosed());
  CHECK(w.sensorClosed());
}

// [sensor-closed-only-known] Every frame from the gate is kept from the house from just before the gate opens; the
// house's link times out (the sensor opens, as it should). Then the last STATUS the gate sent while closed, which the
// house never took, is played back: a seq later than any it took, so the order check (0.13.9) lets it through, but it
// is over a link timeout old by the gate's own clock in it. It must not close the sensor with the gate open. Found by
// the model checker (tools/verify/tla, SensorTruth): a relay that withholds STATUS and delivers it later held the house
// behind the gate by as long as it liked, not just by the link timeout (threat model withheld-gate-frames-replayed).
XFAIL_TEST(link_status_withheld_past_the_link_timeout_is_refused, "TODO.md: a withheld STATUS is taken however old") {
  World w;
  w.commission();
  CHECK(w.sensorClosed());
  Bytes kept;
  w.drop = [&](const AirFrame &f) {
    if (f.from != w.gate.idx) return false;
    if (f.type() == 5 && kept.empty()) kept = f.b;  // MSG_STATUS
    return true;
  };
  CHECK(w.runUntil([&] { return !kept.empty(); }, 40000));  // the next heartbeat
  CHECK_EQ(w.plain(kept)[0], GS_CLOSED_);
  w.extPress(true, 500);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  CHECK(w.runUntil([&] { return !w.houseLinkUp(); }, w.houseLinkTimeoutMs() + 1000));
  CHECK(!w.sensorClosed());
  w.run(5000);
  int replays = w.house.count("replay");
  w.inject(w.house, kept);
  w.run(500);
  CHECK_EQ(w.house.count("replay"), replays + 1);
  CHECK(!w.houseLinkUp());
  CHECK(!w.sensorClosed());
  CHECK(w.opener.atOpen());
}

// [freshness] [outage] The house's last resend of a command, 9.7 s into its 10 s cmd_ttl_s, is the first to get
// through: the gate still takes it (its deadline is the end of the house's TTL, reckoned from the gate's own clock in
// STATUS, so it isn't refused as late) and opens.
TEST(link_command_resent_at_the_end_of_its_ttl_still_runs) {
  World w;
  w.commission();
  uint32_t asked = w.now;
  w.drop = [&](const AirFrame &f) { return f.from == w.house.idx && f.type() == 4 && (int32_t)(w.now - asked) < 9000; };
  w.user(true);
  CHECK(w.runUntil([&] { return w.gate.count("cmd_rx") > 0; }, 11000));
  CHECK(w.now - asked > 9000);
  CHECK_EQ(w.gate.count("cmd_late"), 0);
  w.drop = nullptr;
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
}

// [freshness] [restarts] The gate reboots and the user switches before its first STATUS (it waits BOOT_SETTLE_MS for
// its inputs): the house knows the new session but not its clock, so it has nothing to date the command in. The
// command waits, never sent undated, and goes once the gate has reported: the gate opens, within cmd_ttl_s of the
// switch.
TEST(link_command_before_a_restarted_gates_first_status_waits_for_it) {
  World w;
  w.commission();
  int sessions = w.house.count("session");
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK(w.runUntil([&] { return w.house.count("session") > sessions; }, 5000));
  CHECK(w.gate.status()["settling"] == true);
  int sent = w.house.count("cmd_sent");
  size_t statuses = w.sent(w.gate, 5).size();
  uint32_t asked = w.now;
  w.user(true);
  w.run(500);
  CHECK_EQ(w.house.count("cmd_sent"), sent);  // waiting for the gate's clock
  CHECK(w.runUntil([&] { return w.house.count("cmd_sent") > sent; }, 10000));
  CHECK(w.sent(w.gate, 5).size() > statuses);  // once the new session's first STATUS is in
  CHECK(w.now - asked < 10000);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
  CHECK_EQ(w.gate.count("cmd_late"), 0);
}

// [freshness] [restarts] The same, with the restarted gate's STATUS kept from the house past cmd_ttl_s: the command is
// dropped, never sent undated, and the controller put back once the gate reports.
TEST(link_command_never_dated_within_its_ttl_is_dropped) {
  World w;
  w.commission();
  int sessions = w.house.count("session");
  w.gate.reset(PM_RCAUSE_SYST);
  uint32_t reset = w.now;
  w.drop = [&](const AirFrame &f) { return f.from == w.gate.idx && f.type() == 5 && w.now - reset < 15000; };
  CHECK(w.runUntil([&] { return w.house.count("session") > sessions; }, 5000));
  int sent = w.house.count("cmd_sent");
  uint32_t asked = w.now;
  w.user(true);
  CHECK(w.runUntil([&] { return w.house.count("cmd_dropped") > 0; }, 12000));
  CHECK_IN(w.now - asked, 9900, 10100);  // cmd_ttl_s from the switch
  CHECK_EQ(w.house.count("cmd_sent"), sent);
  CHECK(w.runUntil([&] { return !w.alarmSwitch(); }, 20000));  // put back once the gate reports (still closed)
  CHECK_EQ(w.gate.count("pulse"), 0);
  CHECK(w.opener.atClosed());
}

// [freshness] [restarts] A house kept from seeing the gate restart (the new session's answers jammed at it) still dates
// commands in the clock of the gate's last boot. Were each boot's clock counted from 0, the new one would come to read
// such a deadline as current, and the command, kept from the gate and played back then, would run (found in the
// security review of 0.16.0). Each boot draws its clock's origin at random, so it lands nowhere near: refused. (The
// simulation's millis() doesn't start over at a reboot, so with one origin its deadline would read as current at once.)
TEST(link_command_dated_in_the_gates_previous_boot_is_refused) {
  World w;
  w.commission();
  int sessions = w.house.count("session");
  bool firstHello = true;
  Bytes kept;
  w.drop = [&](const AirFrame &f) {
    if (f.from == w.gate.idx) {  // all but the restarted gate's first HELLO, so it verifies the house
      if (f.type() != 1 || !firstHello) return true;
      firstHello = false;
      return false;
    }
    if (f.type() != 4) return false;  // and the house's commands at the gate, the first one kept
    if (kept.empty()) kept = f.b;
    return true;
  };
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK(w.runUntil([&] { return !firstHello && w.gate.status()["link"]["verified"] == true; }, 10000));
  CHECK_EQ(w.house.count("session"), sessions);  // the house still has the gate's old session
  w.run(4000);  // the gate has settled
  w.user(true);
  CHECK(w.runUntil([&] { return !kept.empty(); }, 2000));
  int replays = w.gate.count("replay");
  w.inject(w.gate, kept);
  w.run(300);
  CHECK_EQ(w.gate.count("replay"), replays);  // the link takes it: only the session it names stops it
  CHECK_EQ(w.gate.count("cmd_late"), 1);
  CHECK_EQ(w.gate.count("cmd_rx"), 0);
  CHECK_EQ(w.gate.count("pulse"), 0);
  CHECK(w.opener.atClosed());
}

// [freshness] On the board a loop pass takes a millisecond or more before linkPoll, so the STATUS handler stamps its
// clock reading with a millis() later than the loop's `now`. Read unsigned, that reading was ~49.7 days old at once
// and thrown away, and every command waited for a clock that never came (found in review, before the bench).
TEST(link_commands_dated_when_the_status_handler_runs_after_the_loops_now) {
  World w;
  w.house.preLinkUs = 1200;
  w.commission();
  w.user(true);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  w.run(6000);  // past the sync window K1's change opened
  w.user(false);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 30000));
  CHECK_EQ(w.house.count("cmd_sent"), 2);
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
}

// [freshness] A STATUS whose first sends were lost reaches the house 8 s after it was built, its clock reading 8 s
// behind. Taken, it would date the next command 8 s early, and the command's resends late in its lifetime would be
// refused. A reading that far behind the one in use isn't taken: a command whose first sends are lost too still runs.
TEST(link_a_resent_status_doesnt_set_the_houses_reckoning_back) {
  World w;
  w.commission();
  w.run(2000);
  uint32_t first = 0, firstAt = 0;
  w.drop = [&](const AirFrame &f) {
    if (f.from != w.gate.idx || f.type() != 5) return false;
    if (!first) first = seqOf(f.b), firstAt = f.start;
    return seqOf(f.b) == first && (int32_t)(f.start - firstAt) < 8000;
  };
  auto taken = [&] {
    for (const AirFrame &f : w.air)
      if (f.from == w.gate.idx && f.type() == 5 && first && seqOf(f.b) == first && f.delivered && !f.dropped)
        return true;
    return false;
  };
  CHECK(w.runUntil(taken, 45000));
  CHECK(w.now - firstAt > 8000);
  uint32_t asked = w.now;
  w.drop = [&](const AirFrame &f) { return f.from == w.house.idx && f.type() == 4 && w.now - asked < 6000; };
  w.user(true);
  CHECK(w.runUntil([&] { return w.gate.count("cmd_rx") > 0 || w.gate.count("cmd_late") > 0; }, 11000));
  CHECK_EQ(w.gate.count("cmd_late"), 0);
  CHECK(w.now - asked > 6000);
  w.drop = nullptr;
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
}

// [freshness] [outage] At SF12/125 kHz, CR 4/8 a STATUS takes ~3.5 s on the air and a command ~2 s: the house's
// reckoning of the gate's clock lags it by the one, and the command takes the other to arrive, so its last resend
// (27 s into a cmd_ttl_s of 30, about the least SF12 needs: a send and its ACK take ~4.5 s) arrives seconds past its
// deadline. The gate's slack covers both airtimes: it still runs.
TEST(link_command_resent_late_in_its_ttl_at_sf12_still_runs) {
  World w;
  w.commission();
  CHECK(w.house.request("config.set", "\"params\":{\"cmd_ttl_s\":30}")["ok"] == true);
  for (Board *b : { &w.house, &w.gate })
    CHECK(b->request("config.set", "\"params\":{\"sf\":12,\"bw_hz\":125000,\"cr\":8}")["ok"] == true);
  CHECK(w.runUntil([&] { return !w.houseLinkUp(); }, 5000));
  CHECK(w.runUntil([&] { return w.houseLinkUp(); }, 120000));  // relinked, and the gate's STATUS taken
  w.run(20000);
  uint32_t asked = w.now;
  w.drop = [&](const AirFrame &f) { return f.from == w.house.idx && f.type() == 4 && w.now - asked < 24000; };
  w.user(true);
  CHECK(w.runUntil([&] { return w.gate.count("cmd_rx") > 0 || w.gate.count("cmd_late") > 0; }, 35000));
  CHECK_EQ(w.gate.count("cmd_late"), 0);
  CHECK(w.now - asked > 26000);
  w.drop = nullptr;
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 40000));
}

// [freshness] [restarts] The gate's link starts over (a radio setting changed at its console) without a reboot: its new
// session verified, it reports at once, so the house has its clock again within a second or two and a command right
// after runs. (It used to report only at its next heartbeat, and the command waited out cmd_ttl_s and was dropped.)
TEST(link_command_right_after_the_gates_link_restart_runs) {
  World w;
  w.commission();
  int sessions = w.house.count("session");
  CHECK(w.gate.request("config.set", "\"params\":{\"tx_power\":5}")["ok"] == true);
  CHECK(w.runUntil([&] { return w.house.count("session") > sessions; }, 10000));
  uint32_t verified = w.now;
  w.user(true);
  CHECK(w.runUntil([&] { return w.house.count("cmd_sent") > 0; }, 10000));
  CHECK(w.now - verified < 3000);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 20000));
  CHECK_EQ(w.house.count("cmd_dropped"), 0);
}

// Another controller opens the gate and closes it again (8 s of travel each way) while everything from the gate is jammed
// at the house for `jamMs` from the first press (0: nothing is).
static void hiddenCycle(World &w, uint32_t jamMs = 0) {
  uint32_t from = w.now;
  if (jamMs) w.drop = [&w, from, jamMs](const AirFrame &f) { return f.from == w.gate.idx && w.now - from < jamMs; };
  w.extPress(true, 300);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  w.run(2000);
  w.extPress(false, 300);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
}

// The sensor opens within ms and closes again 5 s later (MISSED_OPEN_MS).
static void sensorBlips(World &w, uint32_t ms) {
  uint32_t opened = 0, closed = 0;
  CHECK(w.runUntil([&] {
    if (!opened && !w.sensorClosed()) opened = w.now | 1;
    if (opened && !closed && w.sensorClosed()) closed = w.now | 1;
    return closed != 0;
  }, ms));
  CHECK_IN(closed - opened, 4990, 5010);
}

// The sensor stays closed for ms.
static bool sensorStaysClosed(World &w, uint32_t ms) {
  return !w.runUntil([&] { return !w.sensorClosed(); }, ms);
}

// [sensor-closed-only-known] An open-and-close cycle hidden by jamming (every report from the gate jammed at the house,
// for less than the link timeout: link-loss-concealment-window) left the sensor closed throughout, and the alarm never
// saw the gate open. The gate counts its departures from closed in STATUS; its next one gets through showing closed
// with the count one up, and the house opens the sensor for 5 s, so the alarm records the move late (log missed_move).
TEST(link_move_hidden_by_jamming_is_shown_late) {
  World w;
  w.commission();
  CHECK(w.sensorClosed());
  hiddenCycle(w, 40000);
  CHECK(w.sensorClosed());  // the house never knew
  CHECK(w.houseLinkUp());   // within the link timeout
  CHECK_EQ(w.house.count("missed_move"), 0);
  sensorBlips(w, 45000);  // the jam ends 40 s in; the gate's next heartbeat
  CHECK_EQ(w.house.count("missed_move"), 1);
  CHECK_EQ(w.house.last("missed_move")->a, 1);
  CHECK(w.opener.atClosed());
  CHECK(sensorStaysClosed(w, 40000));  // shown once: the next reports change nothing
  CHECK_EQ(w.house.count("missed_move"), 1);
}

// [sensor-closed-only-known] The same cycle with nothing jammed: the house heard every report, the sensor opened and
// closed with the gate, and nothing is shown again afterwards.
TEST(link_move_the_house_heard_is_not_shown_again) {
  World w;
  w.commission();
  hiddenCycle(w);
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
  CHECK(sensorStaysClosed(w, 40000));
  CHECK_EQ(w.house.count("missed_move"), 0);
}

// [sensor-closed-only-known] The house hears the gate open, then its reports are jammed while it closes, opens again
// and closes: the sensor showed open throughout (the house last heard open), so the alarm missed nothing, and when the
// closed report gets through the sensor just closes, with no missed move shown.
TEST(link_move_while_the_sensor_showed_open_is_not_shown_again) {
  World w;
  w.commission();
  w.extPress(true, 300);
  CHECK(w.runUntil([&] { return w.opener.atOpen() && w.houseSees() == GS_OPEN_; }, 15000));
  CHECK(!w.sensorClosed());
  uint32_t from = w.now;
  w.drop = [&](const AirFrame &f) { return f.from == w.gate.idx && w.now - from < 40000; };
  w.extPress(false, 300);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  w.extPress(true, 300);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  w.extPress(false, 300);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(!w.sensorClosed());
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 45000));
  CHECK(sensorStaysClosed(w, 10000));
  CHECK_EQ(w.house.count("missed_move"), 0);
}

// [sensor-closed-only-known] Jammed for longer than the link timeout: the sensor had already opened on the link loss
// (linkloss_open), so the alarm saw the gate not closed through the cycle, and there is nothing to show afterwards.
TEST(link_move_hidden_by_a_link_loss_was_shown_already) {
  World w;
  w.commission();
  hiddenCycle(w, 110000);
  CHECK(w.runUntil([&] { return !w.houseLinkUp(); }, 110000));
  CHECK(!w.sensorClosed());
  CHECK(w.runUntil([&] { return w.houseLinkUp() && w.sensorClosed(); }, 45000));
  CHECK(sensorStaysClosed(w, 10000));
  CHECK_EQ(w.house.count("missed_move"), 0);
}

// [sensor-closed-only-known] With linkloss_open 0 the sensor stays closed through a link loss, so the alarm never saw a
// cycle hidden by it: shown once the link is back.
TEST(link_move_hidden_by_a_link_loss_with_linkloss_open_0_is_shown_late) {
  World w;
  w.commission([](Board &b) {
    if (b.idx == 0) CHECK(b.set("linkloss_open", 0));
  });
  w.limits.sensorTruth = false;  // its lag limit assumes linkloss_open 1: here lagging is the setting's point
  hiddenCycle(w, 110000);
  CHECK(w.runUntil([&] { return !w.houseLinkUp(); }, 110000));
  CHECK(w.sensorClosed());
  sensorBlips(w, 45000);
  CHECK_EQ(w.house.count("missed_move"), 1);
}

// [sensor-closed-only-known] [restarts] A gate that reboots starts its count over, which says nothing about a move: no
// sensor blip. (A move hidden by cutting the gate board's power is lost with its count: logged as a new session.)
TEST(link_gate_reboot_is_not_a_missed_move) {
  World w;
  w.commission();
  hiddenCycle(w);  // the gate's count is 1
  w.run(5000);
  int sessions = w.house.count("session");
  w.gate.reset(PM_RCAUSE_SYST);
  CHECK(w.runUntil([&] { return w.house.count("session") > sessions; }, 10000));
  size_t statuses = w.sent(w.gate, 5).size();
  CHECK(w.runUntil([&] { return w.sent(w.gate, 5).size() > statuses; }, 12000));  // its first STATUS, count 0
  CHECK(sensorStaysClosed(w, 5000));
  CHECK_EQ(w.house.count("missed_move"), 0);
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
