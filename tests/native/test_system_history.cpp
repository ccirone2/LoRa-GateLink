// The link history kept in the SPI flash (history.cpp, histlog.cpp; invariant tag [history-kept]): each bucket
// completed is written once, never while a relay pulses ([no-stall-in-pulse]: a write stops the loop ~0.5 s and
// restarts the radio), and a reboot loads the completed buckets back as they were, each with the boot it was recorded
// in, the numbering going on. A power cut mid-write loses that bucket and nothing else; hist.clear clears the flash
// too; the log wraps by erasing only its oldest sector; a failing chip costs one try per bucket and nothing more.
// Through the whole simulated site (world.h): both boards' firmware, their flash chips (which survive resets).
//
// Buckets are 60 s here (hist.clear period_s, kept with the history across reboots): a bucket closes 60 s after the
// clear, then every 60 s, and is written in that loop pass (or the one that releases a relay pulsing then).
#include <Arduino.h>
#include <stdio.h>
#include <exception>
#include <functional>
#include "world.h"

namespace {

const uint32_t HIST_BASE = 4 * 4096;  // histlog.h HLOG_SECTOR0: the log's first sector
const uint32_t SLOT = 128;            // HLOG_SLOT
const uint32_t PERIOD = 60;

// The site, failing the test (not aborting the run) on a monitor breach (as in test_system_gate_state.cpp).
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

// Every page of hist.get: the header of the last, and the rows by bucket number, each serialized (to compare).
struct Hist {
  uint32_t period = 0, nowS = 0, oldest = 0, current = 0;
  bool persist = false;
  std::vector<uint32_t> idx;
  std::vector<std::string> rows;
  std::vector<uint32_t> boot;
  std::string row(uint32_t i) const {
    for (size_t k = 0; k < idx.size(); k++)
      if (idx[k] == i) return rows[k];
    throw Failure("no row for bucket " + std::to_string(i));
  }
};

Hist history(Board &b) {
  Hist h;
  int32_t from = -1;
  for (;;) {
    JsonDocument r = b.request("hist.get", from < 0 ? "" : "\"from\":" + std::to_string(from));
    CHECK(r["ok"] == true);
    h.period = r["period_s"];
    h.nowS = r["now_s"];
    h.oldest = r["oldest"];
    h.current = r["current"];
    h.persist = r["persist"] == true;
    JsonArray fields = r["fields"];
    size_t bootAt = 0;
    for (size_t i = 0; i < fields.size(); i++)
      if (fields[i] == "boot") bootAt = i;
    CHECK(bootAt > 0);
    JsonArray rows = r["rows"];
    for (JsonArray row : rows) {
      h.idx.push_back(row[0]);
      h.boot.push_back(row[bootAt]);
      std::string s;
      serializeJson(row, s);
      h.rows.push_back(s);
    }
    if (rows.size() == 0 || h.idx.back() >= h.current) break;
    from = (int32_t)h.idx.back() + 1;
  }
  return h;
}

uint32_t bootCount(Board &b) {
  return b.request("info")["boot_count"];
}

// Flash ops (erase / program) in the history log's sectors since index `from` of b.flashOps.
std::vector<Board::FlashOp> histOps(const Board &b, size_t from, char op = 0) {
  std::vector<Board::FlashOp> out;
  for (size_t i = from; i < b.flashOps.size(); i++)
    if (b.flashOps[i].addr >= HIST_BASE && (!op || b.flashOps[i].op == op)) out.push_back(b.flashOps[i]);
  return out;
}

void clear60(Board &b) {
  CHECK(b.request("hist.clear", "\"period_s\":60")["ok"] == true);
}

// A reset (fresh RAM, the flash as it is), then until the link is back.
void reboot(World &w, Board &b, uint8_t cause = PM_RCAUSE_SYST) {
  b.reset(cause);
  CHECK(w.runUntil([&] { return b.running(); }, 2000));
  CHECK(w.runUntil([&] { return w.house.running() && w.gate.running() && b.status()["link"]["verified"] == true; },
                   15000));
}

void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
  }
  return ~c;
}

// A bucket record as histlog.cpp writes it, in slot s of b's log (histlog.h: magic fmt kind lseq idx period 0 data
// crc). The bucket (history.cpp Bucket, 68 bytes) is zeros but for tx (offset 24) and boot (offset 64): no frames.
void putRecord(Board &b, uint32_t s, uint32_t lseq, uint32_t idx, uint32_t boot) {
  uint8_t *r = b.flash.data() + HIST_BASE + s * SLOT;
  memset(r, 0xFF, SLOT);
  r[0] = 0x48;
  r[1] = 0x4C;
  r[2] = 1;  // HLOG_FMT
  r[3] = 1;  // a bucket
  put32(r + 4, lseq);
  put32(r + 8, idx);
  put16(r + 12, (uint16_t)PERIOD);
  put16(r + 14, 0);
  memset(r + 16, 0, 68);
  put16(r + 16 + 24, (uint16_t)(idx & 0xFFFF));
  put32(r + 16 + 64, boot);
  put32(r + 84, crc32(r, 84));
}

}  // namespace

static void rebootKeepsTheCompletedBuckets(int which) {
  Site w;
  w.commission();
  Board &b = w.board(which);
  clear60(b);
  size_t ops0 = b.flashOps.size();
  w.run(3 * PERIOD * 1000 + 5000);  // buckets 0..2 complete, 3 in progress
  CHECK_EQ(histOps(b, ops0, 'P').size(), 3);  // one write a bucket
  uint32_t boot0 = bootCount(b);
  Hist before = history(b);
  CHECK(before.persist);
  CHECK_EQ(before.period, PERIOD);
  CHECK_EQ(before.oldest, 0);
  CHECK_EQ(before.current, 3);
  for (uint32_t bt : before.boot) CHECK_EQ(bt, boot0);
  size_t ops1 = b.flashOps.size();
  reboot(w, b);
  CHECK(histOps(b, ops1).empty());  // a boot only reads the log
  uint32_t boot1 = bootCount(b);
  CHECK_EQ(boot1, boot0 + 1);
  Hist after = history(b);
  CHECK(after.persist);
  CHECK_EQ(after.period, PERIOD);  // the period goes with the history
  CHECK_EQ(after.oldest, 0);
  CHECK_EQ(after.current, 3);  // bucket 3 was in progress at the reset: the numbers go on from 3
  for (uint32_t i = 0; i < 3; i++) CHECK(after.row(i) == before.row(i));
  CHECK_EQ(after.boot.back(), boot1);
  CHECK_IN(after.nowS, 3 * PERIOD, 3 * PERIOD + 20);  // counted on as if without a break
  // And on: the next bucket closes a period after the boot, and is kept too.
  size_t ops2 = b.flashOps.size();
  w.run(PERIOD * 1000 + 2000);
  CHECK_EQ(histOps(b, ops2, 'P').size(), 1);
  Hist on = history(b);
  CHECK_EQ(on.current, 4);
  CHECK(on.row(2) == before.row(2));
  CHECK_EQ(on.boot[3], boot1);
  w.checkClean();
}

// [history-kept]
TEST(history_gate_reboot_keeps_the_completed_buckets) {
  rebootKeepsTheCompletedBuckets(1);
}

// [history-kept]
TEST(history_house_reboot_keeps_the_completed_buckets) {
  rebootKeepsTheCompletedBuckets(0);
}

// The bucket that closes while a relay pulses is written once the pulse is over: in the pass that releases it, not
// during it (the pulse would run ~0.5 s long), on either board.
static void pulseAcrossRollover(Site &w, Board &b, int k) {
  clear60(b);
  Hist h = history(b);
  CHECK(h.nowS < 50);
  // The bucket closes between 59 - now_s and 60 - now_s seconds from here: a 5 s pulse from 58 - now_s covers it.
  w.run((58 - h.nowS) * 1000);
  size_t ops0 = b.flashOps.size();
  CHECK(b.request("relay.test", "\"k\":" + std::to_string(k) + ",\"ms\":5000")["ok"] == true);
  uint32_t on = 0, off = 0;
  CHECK(w.runUntil([&] {
    if (!on && b.coil(k)) on = w.now;
    if (on && !b.coil(k)) off = w.now;
    return off != 0;
  }, 8000));
  CHECK_IN(off - on, 5000, 5001);
  w.run(2000);
  std::vector<Board::FlashOp> ops = histOps(b, ops0);
  CHECK_EQ(ops.size(), 1);
  CHECK_EQ(ops[0].op, 'P');
  CHECK(ops[0].ok);
  CHECK_EQ(ops[0].at, off);  // after the pulse, at once
  CHECK_EQ(history(b).current, 1);
}

// [no-stall-in-pulse] [history-kept]
TEST(history_bucket_written_after_a_relay_pulse_not_during_it) {
  Site w;
  w.commission();
  pulseAcrossRollover(w, w.house, 1);  // K1 is off while the gate is closed
  w.run(3000);
  pulseAcrossRollover(w, w.gate, 1);   // and opens it
  CHECK(w.runUntil([&] { return w.opener.atOpen(); }, 15000));
  w.checkClean();
}

// [history-kept]
TEST(history_power_cut_mid_write_loses_that_bucket_only) {
  Site w;
  w.commission();
  clear60(w.gate);
  w.run(2 * PERIOD * 1000 + 5000);  // 0 and 1 written
  Hist before = history(w.gate);
  CHECK_EQ(before.current, 2);
  // The power goes as bucket 2 is written: half of it reaches the flash.
  w.gate.cutNextProgram = true;
  size_t ops0 = w.gate.flashOps.size();
  CHECK(w.runUntil([&] { return !histOps(w.gate, ops0, 'P').empty(); }, PERIOD * 1000));
  CHECK(!histOps(w.gate, ops0, 'P')[0].ok);
  reboot(w, w.gate, PM_RCAUSE_POR);
  Hist after = history(w.gate);
  CHECK_EQ(after.oldest, 0);
  CHECK_EQ(after.current, 2);  // the torn bucket isn't loaded: 2 starts over
  for (uint32_t i = 0; i < 2; i++) CHECK(after.row(i) == before.row(i));
  // The next buckets go after the torn record, and the next boot loads them all, across it.
  w.run(2 * PERIOD * 1000 + 2000);
  Hist more = history(w.gate);
  CHECK_EQ(more.current, 4);
  reboot(w, w.gate);
  Hist again = history(w.gate);
  CHECK_EQ(again.oldest, 0);
  CHECK_EQ(again.current, 4);
  for (uint32_t i = 0; i < 2; i++) CHECK(again.row(i) == before.row(i));
  for (uint32_t i = 2; i < 4; i++) CHECK(again.row(i) == more.row(i));
  w.checkClean();
}

// [history-kept]
TEST(history_clear_clears_the_flash_too) {
  Site w;
  w.commission();
  clear60(w.gate);
  w.run(2 * PERIOD * 1000 + 5000);
  CHECK_EQ(history(w.gate).current, 2);
  CHECK(w.gate.request("hist.clear", "\"period_s\":120")["ok"] == true);
  reboot(w, w.gate);
  Hist h = history(w.gate);
  CHECK_EQ(h.period, 120);  // kept with the (empty) history
  CHECK_EQ(h.oldest, 0);
  CHECK_EQ(h.current, 0);
  CHECK_EQ(h.idx.size(), 1);
  CHECK_EQ(h.boot[0], bootCount(w.gate));
  w.checkClean();
}

// [history-kept]
TEST(history_log_wrap_erases_only_its_oldest_sector) {
  Site w;
  w.commission();
  // The gate's log one slot short of full: buckets 0..510 in slots 0..510, recorded in boot 3.
  const uint32_t SLOTS = 16 * 4096 / SLOT;
  for (uint32_t s = 0; s < SLOTS - 1; s++) putRecord(w.gate, s, s + 1, s, 3);
  reboot(w, w.gate);
  Hist loaded = history(w.gate);
  CHECK_EQ(loaded.period, PERIOD);
  CHECK_EQ(loaded.oldest, SLOTS - 1 - 96);
  CHECK_EQ(loaded.current, SLOTS - 1);
  CHECK_EQ(loaded.boot[0], 3);
  // Bucket 511 goes to the last slot; 512 wraps to the first, erasing the first sector (buckets 0..31) first.
  size_t ops0 = w.gate.flashOps.size();
  w.run(2 * PERIOD * 1000 + 2000);
  std::vector<Board::FlashOp> ops = histOps(w.gate, ops0);
  CHECK_EQ(ops.size(), 3);
  CHECK(ops[0].op == 'P' && ops[0].addr == HIST_BASE + (SLOTS - 1) * SLOT && ops[0].ok);
  CHECK(ops[1].op == 'E' && ops[1].addr == HIST_BASE);
  CHECK(ops[2].op == 'P' && ops[2].addr == HIST_BASE && ops[2].ok);
  Hist before = history(w.gate);
  CHECK_EQ(before.current, SLOTS + 1);
  reboot(w, w.gate);
  Hist after = history(w.gate);
  CHECK_EQ(after.oldest, SLOTS + 1 - 96);
  CHECK_EQ(after.current, SLOTS + 1);
  for (uint32_t i = SLOTS + 1 - 96; i <= SLOTS; i++) CHECK(after.row(i) == before.row(i));
  w.checkClean();
}

// [history-kept] Zeros the flash really holds (an erase cut short by a power cut may leave a sector so; another program
// may have left data there) read like a garbled bus, but the chip still answers: the log stays on (`persist`), loads
// what it has, writes on past them (erasing such a sector as it enters it), and the next boot loads that too. Taken for
// a garbled bus, every boot gave up on the log for good, hist.clear included.
TEST(history_zeros_in_the_flash_leave_the_log_on) {
  Site w;
  w.commission();
  clear60(w.gate);
  w.run(2 * PERIOD * 1000 + 5000);  // the CLEARED record in slot 0, buckets 0 and 1 in slots 1 and 2
  Hist before = history(w.gate);
  CHECK_EQ(before.current, 2);
  memset(w.gate.flash.data() + HIST_BASE + 4096, 0, 4096);       // the log's second sector: zeros
  memset(w.gate.flash.data() + HIST_BASE + 15 * 4096, 0, 4096);  // and its last
  reboot(w, w.gate);
  Hist after = history(w.gate);
  CHECK(after.persist);
  CHECK_EQ(after.period, PERIOD);
  CHECK_EQ(after.oldest, 0);
  CHECK_EQ(after.current, 2);
  for (uint32_t i = 0; i < 2; i++) CHECK(after.row(i) == before.row(i));
  // On into the zeroed sector: 29 more buckets fill the first (slots 3..31), the next is the second's first slot.
  size_t ops0 = w.gate.flashOps.size();
  w.run(30 * PERIOD * 1000 + 2000);
  std::vector<Board::FlashOp> erases = histOps(w.gate, ops0, 'E');
  CHECK_EQ(erases.size(), 1);
  CHECK_EQ(erases[0].addr, HIST_BASE + 4096);
  Hist more = history(w.gate);
  CHECK_EQ(more.current, 32);
  reboot(w, w.gate);
  Hist again = history(w.gate);
  CHECK(again.persist);
  CHECK_EQ(again.oldest, 0);
  CHECK_EQ(again.current, 32);
  for (uint32_t i = 0; i < 32; i++) CHECK(again.row(i) == more.row(i));
  w.checkClean();
}

// [history-kept] [watchdog]
TEST(history_failing_flash_costs_one_try_a_bucket) {
  Site w;
  w.commission();
  clear60(w.gate);
  size_t h0 = w.house.logs.size();
  // A chip that stopped programming: each bucket is tried once, and the board runs on.
  w.gate.failPrograms = 1000000;
  size_t ops0 = w.gate.flashOps.size();
  w.run(5 * PERIOD * 1000 + 5000);
  std::vector<Board::FlashOp> ops = histOps(w.gate, ops0, 'P');
  CHECK_EQ(ops.size(), 5);
  for (const Board::FlashOp &o : ops) CHECK(!o.ok);
  Hist h = history(w.gate);
  CHECK(h.persist);
  CHECK_EQ(h.current, 5);  // the history goes on in RAM
  // A chip that stops answering altogether.
  w.gate.failPrograms = 0;
  w.gate.flashPresent = false;
  w.run(3 * PERIOD * 1000);
  CHECK_EQ(history(w.gate).current, 8);
  for (size_t i = h0; i < w.house.logs.size(); i++) CHECK(w.house.logs[i].ev != "link_down");
  JsonDocument s = w.gate.status();
  CHECK(s["link"]["verified"] == true);
  CHECK((s["loop_max_us"] | 0u) < 1500000u);
  w.checkClean();
}
