// link.cpp: handshake, reliable delivery, replay protection, sessions across restarts, HELLO side effects,
// retry spacing and timer wrap. Two real link instances over the simulated channel (sim.h).
#include "sim.h"

static const Bytes CMD_OPEN = { 0x01, 0x00, 1 };  // cmd_id u16, action

static const AirFrame *last(const std::vector<const AirFrame *> &v) {
  CHECK(!v.empty());
  return v.back();
}

TEST(handshake_verifies_both_sides) {
  Sim s;
  CHECK(s.handshake());
  CHECK_EQ(s.house.stats().sessions, 1);
  CHECK_EQ(s.gate.stats().sessions, 1);
  CHECK_EQ(s.house.stats().macFail + s.gate.stats().macFail, 0);
  CHECK_EQ(s.house.stats().replay + s.gate.stats().replay, 0);
}

TEST(wrong_key_never_verifies) {
  Sim s;
  s.gate.cfg.key[0] ^= 1;
  s.gate.begin();
  s.run(5000);
  CHECK(!s.house.verified());
  CHECK(!s.gate.verified());
  CHECK(s.house.stats().macFail > 0);
  CHECK(s.gate.stats().macFail > 0);
}

TEST(other_net_id_is_ignored_before_authentication) {
  Sim s;
  s.gate.cfg.net_id = 0x43;
  s.gate.begin();
  s.run(5000);
  CHECK(!s.house.verified());
  CHECK_EQ(s.house.stats().macFail, 0);  // dropped on the header, not on the tag
}

TEST(no_key_no_transmissions) {
  Sim s;
  s.house.cfg.key_set = 0;
  s.house.begin();
  s.air.clear();
  s.run(5000);
  CHECK(s.sent(s.house, MSG_HELLO).empty());
  CHECK(!s.gate.verified());
}

TEST(reliable_command_delivered_once_and_acked) {
  Sim s;
  CHECK(s.handshake());
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 2000));
  s.run(3000);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK(s.gate.rx.back().payload == CMD_OPEN);
  CHECK_EQ(s.house.acks.size(), 1);
  CHECK(s.house.acks[0].acked);
  CHECK_EQ(s.house.acks[0].result, RES_OK);
  CHECK_EQ(s.house.stats().retries, 0);
}

TEST(slots_wait_for_verification) {
  // Queued before the handshake: nothing goes out until the peer is verified (its ACK would be dropped anyway).
  Sim s;
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 5000));
  for (const AirFrame *f : s.sent(s.house, MSG_CMD)) CHECK(f->start >= s.sent(s.house, MSG_HELLO_ACK).front()->start);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
}

TEST(lost_ack_is_answered_from_the_memo_not_reprocessed) {
  Sim s;
  CHECK(s.handshake());
  int acksDropped = 0;
  s.drop = [&](const AirFrame &f) { return f.from == 1 && f.type() == MSG_ACK && acksDropped++ < 2; };
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 5000));
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK_EQ(s.house.stats().retries, 2);
  CHECK_EQ(s.gate.stats().replay, 0);
  CHECK(s.house.acks.back().acked);
}

TEST(ack_later_holds_retransmits_quietly) {
  // The gate takes a CFG_SET but answers only later (a relay pulse running, 0.13.2): the house's retries are
  // neither re-processed, answered nor counted as replays, and the late ACK carries the real result.
  Sim s;
  CHECK(s.handshake());
  uint32_t heldSeq = 0;
  s.gate.onRx = [&](Node &n, const RxMsg &m) {
    if (m.type != MSG_CFG_SET) return;
    heldSeq = m.seq;
    n.ackLater(m.seq);
  };
  s.house.sendReliable(SLOT_CFG, MSG_CFG_SET, { 9, 4, 0, 0, 0 }, 10000);
  s.run(3000);
  CHECK(heldSeq != 0);
  CHECK(s.house.stats().retries >= 2);
  CHECK(s.house.pending(SLOT_CFG));
  CHECK(s.sent(s.gate, MSG_ACK).empty());
  CHECK_EQ(s.gate.stats().replay, 0);
  CHECK_EQ(s.gate.rxCount(MSG_CFG_SET), 1);
  s.gate.ack(heldSeq, RES_NOT_SAVED);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CFG); }, 1000));
  CHECK(s.house.acks.back().acked);
  CHECK_EQ(s.house.acks.back().result, RES_NOT_SAVED);
}

TEST(replayed_frame_is_rejected) {
  Sim s;
  CHECK(s.handshake());
  s.house.send(MSG_PING, { 7, 0 });
  s.run(500);
  CHECK_EQ(s.gate.rxCount(MSG_PING), 1);
  s.inject(s.gate, last(s.sent(s.house, MSG_PING))->b);
  s.run(500);
  CHECK_EQ(s.gate.rxCount(MSG_PING), 1);
  CHECK_EQ(s.gate.stats().replay, 1);
  CHECK_EQ(s.gate.count(EV_REPLAY), 1);
}

TEST(tampered_frame_fails_the_mac) {
  Sim s;
  CHECK(s.handshake());
  s.house.send(MSG_PING, { 7, 0 });
  s.run(500);
  Bytes f = last(s.sent(s.house, MSG_PING))->b;
  f[13] ^= 0x01;  // payload
  putU32(&f[9], getU32(&f[9]) + 1);  // a fresh seq, so only the tag can catch it
  s.inject(s.gate, f);
  s.run(500);
  CHECK_EQ(s.gate.rxCount(MSG_PING), 1);
  CHECK_EQ(s.gate.stats().macFail, 1);
}

TEST(frames_from_before_a_gate_restart_are_rejected) {
  // The gate restarts and re-verifies the house's (unchanged) session: only seqs above the new HELLO_ACK's count.
  Sim s;
  CHECK(s.handshake());
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 2000));
  Bytes old = last(s.sent(s.house, MSG_CMD))->b;
  s.gate.begin();
  CHECK(s.runUntil([&] { return s.gate.verified(); }, 10000));
  s.run(500);
  s.inject(s.gate, old);
  s.run(500);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK_EQ(s.gate.stats().replay, 1);
}

TEST(frames_from_before_a_house_restart_are_rejected) {
  // The house restarts with a new session: frames of its old one are never accepted again.
  Sim s;
  CHECK(s.handshake());
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 2000));
  Bytes old = last(s.sent(s.house, MSG_CMD))->b;
  uint32_t oldSession = getU32(&old[5]);
  s.house.begin();
  CHECK(s.runUntil([&] { return s.house.verified() && s.gate.stats().sessions == 2; }, 10000));
  CHECK(last(s.sent(s.house, MSG_HELLO))->session() != oldSession);
  s.inject(s.gate, old);
  s.run(2000);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK(s.gate.verified());
  CHECK_EQ(s.gate.stats().sessions, 2);  // the old session's frame didn't bring it back
}

TEST(sessions_never_repeat_across_restarts) {
  Sim s;
  std::vector<uint32_t> seen;
  for (int i = 0; i < 50; i++) {
    s.gate.begin();
    s.run(800);
    uint32_t session = last(s.sent(s.gate, MSG_HELLO))->session();
    for (uint32_t o : seen) CHECK(o != session);
    seen.push_back(session);
  }
}

TEST(replayed_old_hello_holds_the_command_until_the_live_session_answers) {
  // A recorded HELLO from the gate's previous session arrives while a command waits: it could be the gate having
  // restarted (the command may have run there) or a replay. The house holds the command, challenges, and sends it
  // once the verified session answers, without dropping the link.
  Sim s;
  CHECK(s.handshake());
  Bytes oldHello = last(s.sent(s.gate, MSG_HELLO))->b;
  s.gate.begin();
  CHECK(s.runUntil([&] { return s.gate.verified() && s.house.stats().sessions == 2; }, 10000));
  s.run(1500);  // past the HELLO answer gap
  bool deaf = true;
  s.drop = [&](const AirFrame &f) { return deaf && f.from == 0 && f.type() == MSG_CMD; };
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  s.run(50);
  s.inject(s.house, oldHello);
  s.run(100);
  CHECK_EQ(s.house.count(EV_CMD_HOLD, 1), 1);
  s.run(3000);
  deaf = false;
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 7000));
  CHECK_EQ(s.house.count(EV_CMD_HOLD, 0), 1);
  CHECK_EQ(s.house.count(EV_CMD_HOLD, 2), 0);
  CHECK(s.house.acks.back().acked);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK(s.house.verified());
  CHECK_EQ(s.house.stats().sessions, 2);
}

TEST(gate_restart_with_a_lost_ack_drops_the_command) {
  // The gate ran the command, then restarted before its ACK got through: resending could pulse twice, late.
  Sim s;
  CHECK(s.handshake());
  bool acksLost = true;
  s.drop = [&](const AirFrame &f) { return acksLost && f.from == 1 && f.type() == MSG_ACK; };
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return s.gate.rxCount(MSG_CMD) == 1; }, 2000));
  s.gate.begin();
  acksLost = false;
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 10000));
  CHECK_EQ(s.house.count(EV_CMD_HOLD, 1), 1);
  CHECK_EQ(s.house.count(EV_CMD_HOLD, 2), 1);
  CHECK(!s.house.acks.back().acked);
  s.run(3000);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK(s.house.verified() && s.gate.verified());
}

TEST(replayed_current_hello_flood_does_not_starve_slots) {
  // REVIEW #17: the verified gate's own HELLO replayed 4 times a second. It is answered at most once a second, and
  // the command still goes through (each answer renumbers what waits and used to push it back every time).
  // Faster than the new-frame backoff (up to ~106 ms at SF9/500 kHz) any frame heard holds our new frames off
  // for good, whatever it is: see TODO.md.
  Sim s;
  CHECK(s.handshake());
  Bytes hello = last(s.sent(s.gate, MSG_HELLO))->b;
  s.run(1500);
  size_t acksBefore = s.sent(s.house, MSG_HELLO_ACK).size();
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  for (int i = 0; i < 20; i++) {
    s.inject(s.house, hello);
    s.run(250);
  }
  CHECK(!s.house.pending(SLOT_CMD));
  CHECK(s.house.acks.back().acked);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK(s.sent(s.house, MSG_HELLO_ACK).size() - acksBefore <= 6);
}

// Offsets (ms after the first transmission) of the retries for one reliable message with the peer deaf.
static std::vector<uint32_t> retryOffsets(Sim &s, uint32_t ttl, uint32_t *gaveUpAfter) {
  s.drop = [](const AirFrame &f) { return f.from == 0 && f.type() == MSG_CMD; };
  uint32_t sentAt = simNow;
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, ttl);
  s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, ttl + 1000);
  *gaveUpAfter = s.house.acks.empty() ? 0 : s.house.acks.back().t - sentAt;
  std::vector<uint32_t> out;
  auto frames = s.sent(s.house, MSG_CMD);
  for (const AirFrame *f : frames) out.push_back(f->start - frames.front()->start);
  return out;
}

TEST(retries_double_and_span_the_ttl) {
  // cmd_ttl_s 10, retries 5: resends about 0.3, 0.9, 2.2, 4.7 and 9.7 s after the first (gaps ttl >> 5..1, less
  // up to 1/8 jitter, plus the frame's airtime and the listen-before-talk backoff), then a give-up at the TTL.
  Sim s;
  CHECK(s.handshake());
  s.run(500);
  uint32_t gaveUp;
  std::vector<uint32_t> t = retryOffsets(s, 10000, &gaveUp);
  CHECK_EQ(t.size(), 6);
  uint32_t air = airtimeMs(13 + 3 + 8);
  for (int i = 1; i <= 5; i++) {
    uint32_t gap = t[i] - t[i - 1];
    uint32_t nominal = 10000u >> (6 - i);
    CHECK_IN(gap, nominal - nominal / 8 + air, nominal + air + 150);
  }
  CHECK(t[5] <= 10000);
  CHECK_IN(gaveUp, 10000, 10010);
  CHECK(!s.house.acks.back().acked);
  CHECK_EQ(s.house.count(EV_TX_GIVEUP), 1);
}

TEST(command_outage_coverage) {
  // TODO.md, "Commands don't survive outages longer than ~4.7 s well": how many sends of a command are left
  // after an outage of a given length (starting as it is queued), at the defaults. Each must get through for
  // the command to; the table shows the chance at a 10 % / 30 % per-frame loss after the outage.
  printf("      outage  sends left  P(delivered) at 10%% / 30%% loss\n");
  for (uint32_t outage = 0; outage <= 9500; outage += 500) {
    Sim s;
    CHECK(s.handshake());
    s.run(500);
    uint32_t start = simNow, gaveUp;
    std::vector<uint32_t> t = retryOffsets(s, 10000, &gaveUp);
    uint32_t first = s.sent(s.house, MSG_CMD).front()->start - start;
    int left = 0;
    for (uint32_t o : t) left += first + o >= outage;
    double p10 = 1, p30 = 1;
    for (int i = 0; i < left; i++) p10 *= 0.1, p30 *= 0.3;
    printf("      %4.1f s   %d           %5.1f %% / %5.1f %%\n", outage / 1000.0, left, 100 * (1 - p10), 100 * (1 - p30));
    if (outage < 9500) CHECK(left >= 1);  // a clean channel after the outage still delivers
  }
}

TEST(command_delivered_after_a_long_outage) {
  // An outage of 9 s (both ways) with a clean channel after it: the last retry, just before the TTL, gets through.
  Sim s;
  CHECK(s.handshake());
  s.run(500);
  uint32_t until = simNow + 9000;
  s.drop = [&](const AirFrame &) { return (int32_t)(simNow - until) < 0; };
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 11000));
  CHECK(s.house.acks.back().acked);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
}

TEST(works_across_the_millis_wrap) {
  // Handshake and a retried command across 2^32 ms (~49.7 days of uptime).
  Sim s(0xFFFFFFFFu - 1500);
  CHECK(s.handshake());
  int dropped = 0;
  s.drop = [&](const AirFrame &f) { return f.from == 0 && f.type() == MSG_CMD && dropped++ < 2; };
  s.house.sendReliable(SLOT_CMD, MSG_CMD, CMD_OPEN, 10000);
  CHECK(s.runUntil([&] { return !s.house.pending(SLOT_CMD); }, 11000));
  CHECK(simNow < 0x80000000u);  // wrapped
  CHECK(s.house.acks.back().acked);
  CHECK_EQ(s.gate.rxCount(MSG_CMD), 1);
  CHECK_EQ(s.house.stats().giveups, 0);
}

TEST(silence_never_reads_as_fresh) {
  // REVIEW #1: after 2^31 ms without a frame the signed age used to flip and the link read as up again (house K2
  // stopped failing open). The age is capped: with the gate silent for 30 days, it never reads as recent.
  Sim s;
  CHECK(s.handshake());
  s.drop = [](const AirFrame &f) { return f.from == 1; };
  const uint32_t timeout = 100000;  // link_timeout_s default
  for (int h = 0; h < 30 * 24; h++) {
    s.step(3600000);
    uint32_t at = s.house.stats().lastRxAt;
    CHECK(at != 0);
    CHECK(elapsed(simNow, at, timeout));
  }
}
