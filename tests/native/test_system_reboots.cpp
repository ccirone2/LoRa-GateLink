// Reset breadcrumbs and reboot loops (app.cpp's reset memo, in the RAM a reset keeps; ROADMAP F9): after a watchdog
// reset the board reports where its early warning found the loop (status hang_stage / hang_pc, log `hang`); three
// watchdog or brown-out resets in a row, each within REBOOT_SETTLE_MS of the boot before, are a reboot loop (health
// `reboots`, D5 with fault_out on), until a boot has run REBOOT_SETTLE_MS. The gate reports its loop in STATUS
// (ST_INPUTS bit 7), and the house raises `reboots` for it too: its D5 is the one wired to the alarm.
#include <Arduino.h>
#include <stdio.h>
#include <exception>
#include <string>
#include "world.h"

namespace {

const uint32_t HOLD_MS = 10000;     // fault_hold_s default
const uint32_t SETTLE_MS = 3600000;  // app.h REBOOT_SETTLE_MS

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

bool hasProblem(Board &b, const char *name) {
  JsonDocument s = b.status();
  for (JsonVariantConst v : s["health"].as<JsonArrayConst>())
    if (v == name) return true;
  return false;
}

// Resets b with `rcause` and waits until it has booted again (its `boot` log).
void resetAndBoot(World &w, Board &b, uint8_t rcause) {
  size_t n = b.logs.size();
  b.reset(rcause);
  CHECK(w.runUntil([&] {
    for (size_t i = n; i < b.logs.size(); i++)
      if (b.logs[i].ev == "boot") return true;
    return false;
  }, 3000));
  w.run(1000);  // past the radio's start (setup blocks ~0.5 s) and a few loop passes, which evaluate health
}

void faultOutOn(Board &b) {
  CHECK(b.set("fault_out", 1));
}

}  // namespace

// The early warning's note survives the watchdog reset: status and the boot log say where the loop hung. A warning
// whose stall ended (the loop kicked again) leaves nothing for a later reset; other resets show none.
TEST(watchdog_reset_reports_where_it_hung) {
  Site w;
  w.commission();
  JsonDocument s = w.gate.status();
  CHECK(s["hang_stage"].isNull());
  CHECK(s["hang_pc"].isNull());
  CHECK_EQ(s["crash_resets"].as<int>(), 0);

  w.gate.watchdogWarning(0x00012345);
  resetAndBoot(w, w.gate, PM_RCAUSE_WDT);
  s = w.gate.status();
  CHECK(s["reset_cause"] == "watchdog");
  CHECK(s["hang_stage"] == "core");  // between passes
  CHECK_EQ(s["hang_pc"].as<uint32_t>(), 0x00012345u);
  CHECK_EQ(s["crash_resets"].as<int>(), 1);
  const LogEv *hang = w.gate.last("hang");
  CHECK(hang);
  CHECK_EQ((uint32_t)hang->a, 0x00012345u);
  CHECK_EQ(hang->b, 11);  // STG_CORE
  CHECK_EQ(w.gate.count("hang"), 1);

  // A stall the warning saw that ended: the next loop pass drops the note, so this reset has no hang.
  w.run(1000);
  w.gate.watchdogWarning(0x00054321);
  w.run(10);
  resetAndBoot(w, w.gate, PM_RCAUSE_WDT);
  s = w.gate.status();
  CHECK(s["hang_stage"].isNull());
  CHECK_EQ(w.gate.count("hang"), 1);
  CHECK_EQ(s["crash_resets"].as<int>(), 2);

  // A deliberate reset with a note pending: no hang (only a watchdog reset follows a warning), and the streak ends.
  w.gate.watchdogWarning(0x00011111);
  resetAndBoot(w, w.gate, PM_RCAUSE_SYST);
  s = w.gate.status();
  CHECK(s["hang_stage"].isNull());
  CHECK_EQ(s["crash_resets"].as<int>(), 0);
  w.checkClean();
}

// [fault-out] Three crash resets in a row are a reboot loop: `reboots` on the board, in its STATUS, at the house, and
// D5 low at both once it has lasted fault_hold_s. It clears once a boot has run REBOOT_SETTLE_MS (1 h).
SLOW_TEST(gate_reboot_loop_reaches_the_house_and_clears_after_an_hour) {
  Site w;
  w.commission(faultOutOn);
  resetAndBoot(w, w.gate, PM_RCAUSE_WDT);
  w.run(20000);
  resetAndBoot(w, w.gate, PM_RCAUSE_BOD33);
  w.run(20000);
  CHECK(!hasProblem(w.gate, "reboots"));
  CHECK_EQ(w.gate.status()["crash_resets"].as<int>(), 2);
  resetAndBoot(w, w.gate, PM_RCAUSE_WDT);
  CHECK_EQ(w.gate.status()["crash_resets"].as<int>(), 3);
  CHECK(hasProblem(w.gate, "reboots"));
  CHECK(poll(w, [&] { return w.house.status()["remote"]["reboot_loop"] == true; }, 15000));
  CHECK(hasProblem(w.house, "reboots"));
  w.run(HOLD_MS + 100);
  CHECK_EQ(w.house.faultOut(), 0);
  CHECK_EQ(w.gate.faultOut(), 0);
  CHECK(w.house.status()["link_up"] == true);  // the link itself is fine

  // An hour after its last boot the gate has settled: the loop is over, at the gate and then at the house.
  const LogEv *boot = w.gate.last("boot");
  w.run(SETTLE_MS - (w.now - boot->at) + 100);
  CHECK(!hasProblem(w.gate, "reboots"));
  CHECK_EQ(w.gate.faultOut(), 1);
  CHECK(poll(w, [&] { return w.house.status()["remote"]["reboot_loop"] == false; }, 70000));
  CHECK(!hasProblem(w.house, "reboots"));
  CHECK_EQ(w.house.faultOut(), 1);

  // The next crash reset starts a new streak.
  resetAndBoot(w, w.gate, PM_RCAUSE_WDT);
  CHECK_EQ(w.gate.status()["crash_resets"].as<int>(), 1);
  w.checkClean();
}

// Deliberate resets (the console's reboot, the reset pin) and power cuts are never a loop: they end a streak. A crash
// reset after a boot that ran an hour starts one over.
TEST(deliberate_resets_and_power_cuts_end_a_streak) {
  Site w;
  w.commission();
  resetAndBoot(w, w.house, PM_RCAUSE_WDT);
  resetAndBoot(w, w.house, PM_RCAUSE_WDT);
  CHECK_EQ(w.house.status()["crash_resets"].as<int>(), 2);
  resetAndBoot(w, w.house, PM_RCAUSE_EXT);
  CHECK_EQ(w.house.status()["crash_resets"].as<int>(), 0);
  resetAndBoot(w, w.house, PM_RCAUSE_WDT);
  resetAndBoot(w, w.house, PM_RCAUSE_WDT);
  CHECK_EQ(w.house.status()["crash_resets"].as<int>(), 2);
  // A power cut: the kept RAM is garbage afterwards, which reads as no memo.
  w.house.cut = true;
  w.run(2000);
  w.house.cut = false;
  CHECK(w.runUntil([&] { return w.house.running() && w.house.last("boot") && w.house.last("boot")->a == PM_RCAUSE_POR; }, 3000));
  CHECK_EQ(w.house.status()["crash_resets"].as<int>(), 0);
  resetAndBoot(w, w.house, PM_RCAUSE_WDT);
  CHECK_EQ(w.house.status()["crash_resets"].as<int>(), 1);
  CHECK(!hasProblem(w.house, "reboots"));
  w.checkClean();
}

// A long loop stays a loop: the count stops at 255 rather than wrapping back below the threshold.
TEST(crash_resets_saturate_at_255) {
  Site w;
  w.commission();
  for (int i = 0; i < 257; i++) {
    size_t n = w.house.logs.size();
    w.house.reset(PM_RCAUSE_WDT);
    CHECK(w.runUntil([&] {
      for (size_t j = n; j < w.house.logs.size(); j++)
        if (w.house.logs[j].ev == "boot") return true;
      return false;
    }, 3000));
  }
  w.run(1000);
  CHECK_EQ(w.house.status()["crash_resets"].as<int>(), 255);
  CHECK(hasProblem(w.house, "reboots"));
}
