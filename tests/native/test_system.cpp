// The whole site: both boards' firmware, the opener and the controller (world.h). Normal operation end to end.
#include "world.h"

TEST(sys_commission_link_and_first_status) {
  World w;
  w.commission();
  JsonDocument h = w.house.status(), g = w.gate.status();
  CHECK(h["gate"] == "closed");
  CHECK(g["gate"] == "closed");
  CHECK(h["link_up"] == true);
  CHECK(!w.house.coil(1));     // not closed: K1 off
  CHECK(w.sensorClosed());     // contact sensor closed
  CHECK(!w.alarmSwitch());
}

TEST(sys_open_and_close_from_the_controller) {
  World w;
  w.commission();
  w.user(true);
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  CHECK(w.runUntil([&] { return w.house.status()["gate"] == "open"; }, 3000));
  CHECK(w.house.coil(1));
  CHECK(!w.sensorClosed());
  CHECK(w.gate.last("gate_state")->b == 1);  // cause lora
  w.run(4000);  // past the sync window K1 opened (an edge inside it is taken as the controller following K1)
  w.user(false);
  CHECK(w.runUntil([&] { return w.opener.atClosed(); }, 15000));
  CHECK(w.runUntil([&] { return w.sensorClosed(); }, 3000));
  CHECK(!w.house.coil(1));
  CHECK(w.alarmSwitch() == false);
  CHECK_EQ(w.gate.count("cmd_rx"), 2);
  CHECK_EQ(w.opener.presses[0].size(), 1);
  CHECK_EQ(w.opener.presses[1].size(), 1);
}

