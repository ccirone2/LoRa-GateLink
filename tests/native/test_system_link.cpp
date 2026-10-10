// The whole site on a hostile or busy channel: another transmitter on our frequency, SF and sync word, and outages.
#include "world.h"

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

// [busy-channel] The gate's status reports keep getting through a busy neighbour too. A frame every ~125 ms leaves
// ~84 ms gaps: room for a STATUS (47 bytes, ~77 ms on air), but the backoff (73..106 ms after the last frame heard)
// started it too late in the gap, so every one collided and the house lost the link (timeout 100 s). Now it starts
// as a gap opens. (Gaps shorter than our frames are jamming: then the house losing the link, and failing safe, is
// right.)
TEST(link_stays_up_through_ten_minutes_of_neighbour_traffic) {
  World w;
  w.commission();
  uint32_t next = w.now, until = w.now + 600000;
  while ((int32_t)(w.now - until) < 0) {
    if ((int32_t)(w.now - next) >= 0) {
      w.airSend(neighbourFrame());
      next = w.now + 120 + (w.now % 3) * 5;  // 120..130 ms apart
    }
    w.step();
  }
  CHECK_EQ(w.house.count("link_down"), 0);
  CHECK(w.houseLinkUp());
  CHECK(w.sensorClosed());
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
