// histlog.cpp over the fake SPI flash: the link history's log in sectors 4-19 (appended buckets, loaded back at boot),
// its wrap and sector erases, power cuts and failed writes, garbled reads, hist.clear's CLEARED record, records of
// another format, and a board without the chip. Each "boot" is histLogBegin on the flash as it is (the firmware's
// RAM state is rebuilt from it).
#include "sim.h"
#include "extflash.h"
#include "histlog.h"

#define BASE (HLOG_SECTOR0 * EXTFLASH_SECTOR)
#define PER_SECTOR (int)(EXTFLASH_SECTOR / HLOG_SLOT)  // 32
#define SLOTS (HLOG_SECTORS * PER_SECTOR)              // 512
#define REC 88                                        // a record's bytes (histlog.h); the rest of its slot is unused
#define DEPTH 96                                      // HIST_DEPTH

static uint32_t crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
  }
  return ~c;
}

typedef std::vector<std::pair<uint32_t, Bytes>> Taken;
static Taken got;
static void take(uint32_t idx, const uint8_t *d) { got.push_back({ idx, Bytes(d, d + HLOG_DATA) }); }

struct Loaded {
  bool had;
  uint16_t period;
  uint32_t next;
  Taken b;
};

// A boot: the log as the flash holds it.
static Loaded boot(uint32_t max = DEPTH) {
  got.clear();
  Loaded l{ false, 0, 0, {} };
  l.had = histLogBegin(max, take, l.period, l.next);
  l.b = got;
  return l;
}

// Bucket idx's contents, as the tests append them.
static Bytes data(uint32_t idx) {
  Bytes d(HLOG_DATA);
  for (size_t i = 0; i < d.size(); i++) d[i] = (uint8_t)(idx * 7 + i * 13 + 1);
  return d;
}

static bool append(uint32_t idx, uint16_t period = 3600) {
  Bytes d = data(idx);
  return histLogAppend(idx, period, d.data());
}

static void appendAll(uint32_t from, uint32_t to, uint16_t period = 3600) {
  for (uint32_t i = from; i <= to; i++)
    if (!append(i, period)) throw Failure("append " + std::to_string(i) + " failed on a healthy chip");
}

static uint8_t *slot(int s) { return flash.mem + BASE + s * HLOG_SLOT; }
static bool erased(int s) {
  for (int i = 0; i < REC; i++)
    if (slot(s)[i] != 0xFF) return false;
  return true;
}
static uint32_t slotIdx(int s) { return getU32(slot(s) + 8); }

// The run loaded: buckets newest .. newest - n + 1, newest first, as appended.
static void checkRun(const Loaded &l, uint32_t newest, uint32_t n, uint16_t period = 3600) {
  CHECK(l.had);
  CHECK_EQ(l.period, period);
  CHECK_EQ(l.next, newest + 1);
  CHECK_EQ(l.b.size(), n);
  for (uint32_t i = 0; i < n; i++) {
    CHECK_EQ(l.b[i].first, newest - i);
    CHECK(l.b[i].second == data(newest - i));
  }
}

// A record as histlog.cpp writes it, at slot s, for records from elsewhere.
static void writeRaw(int s, uint8_t fmt, uint8_t kind, uint32_t lseq, uint32_t idx, uint16_t period) {
  uint8_t *r = slot(s);
  memset(r, 0xFF, HLOG_SLOT);
  r[0] = 0x48;
  r[1] = 0x4C;
  r[2] = fmt;
  r[3] = kind;
  putU32(r + 4, lseq);
  putU32(r + 8, idx);
  r[12] = (uint8_t)period;
  r[13] = (uint8_t)(period >> 8);
  r[14] = r[15] = 0;
  Bytes d = data(idx);
  memcpy(r + 16, d.data(), HLOG_DATA);
  putU32(r + REC - 4, crc32(r, REC - 4));
}

TEST(hist_log_blank_has_nothing_and_starts_in_its_first_sector) {
  Loaded l = boot();
  CHECK(!l.had);
  CHECK(histLogOn());
  CHECK(append(0));
  CHECK_EQ(flash.erases, 1);  // its sector, first
  CHECK_EQ(slotIdx(0), 0);
  CHECK(erased(1));
  checkRun(boot(), 0, 1);
}

TEST(hist_log_reboot_loads_the_newest_run_and_goes_on) {
  boot();
  appendAll(0, 9);
  checkRun(boot(), 9, 10);
  appendAll(10, 12);
  checkRun(boot(), 12, 13);
  // Nothing outside its sectors: config records and the boot counter (sectors 0-3) untouched.
  for (uint32_t a = 0; a < BASE; a++) CHECK_EQ(flash.mem[a], 0xFF);
}

TEST(hist_log_loads_at_most_max) {
  boot();
  appendAll(0, 199);
  checkRun(boot(), 199, DEPTH);
  checkRun(boot(5), 199, 5);
}

TEST(hist_log_wraps_erasing_only_the_oldest_sector) {
  boot();
  for (uint32_t i = 0; i < SLOTS + 40; i++) {
    CHECK(append(i));
    // Entering a sector erases it, once: the oldest records (a lap ago) go, the newest stay.
    CHECK_EQ(flash.erases, (int)(i / PER_SECTOR) + 1);
    int s = i % SLOTS;
    CHECK_EQ(slotIdx(s), i);
    if (s % PER_SECTOR != PER_SECTOR - 1) CHECK(erased(s + 1));
    if (i >= DEPTH && i % 37 == 0) checkRun(boot(), i, DEPTH);  // reboots along the way, across the wrap too
  }
  // Records 512..551: sector 4 (slots 0..31) and sector 5's first 8; sector 5's records from the lap before are gone,
  // sector 6 still holds 64..95.
  CHECK_EQ(slotIdx(31), 543);
  CHECK_EQ(slotIdx(32 + 7), 551);
  CHECK(erased(32 + 8));
  CHECK_EQ(slotIdx(64), 64);
  checkRun(boot(), SLOTS + 39, DEPTH);  // from sector 5 back across the log's end into sector 18
}

TEST(hist_log_power_cut_mid_record_is_skipped_and_nothing_else_lost) {
  boot();
  appendAll(0, 4);
  flash.cutNextProgram = true;  // the power goes as bucket 5 is written: the board resets
  CHECK(!append(5));
  CHECK(!erased(5));
  checkRun(boot(), 4, 5);  // the torn record isn't loaded
  // The next record goes after the torn one, and the run goes on across it.
  CHECK(append(5));
  CHECK_EQ(slotIdx(6), 5);
  checkRun(boot(), 5, 6);
  appendAll(6, 40);
  checkRun(boot(), 40, 41);

  // At a sector's first slot: the reboot finds the newest record in the sector before, and the next write erases
  // that sector again.
  flash.reset();
  boot();
  appendAll(0, 30);  // slots 0..30
  flash.cutNextProgram = true;
  CHECK(!append(31));  // slot 31, sector 4's last
  checkRun(boot(), 30, 31);
  CHECK(append(31));   // sector 5's first slot
  CHECK_EQ(slotIdx(32), 31);
  flash.cutNextProgram = true;
  CHECK(!append(32));  // slot 33
  checkRun(boot(), 31, 32);
  appendAll(32, 70);
  checkRun(boot(), 70, 71);
}

TEST(hist_log_power_cut_on_a_sectors_first_record) {
  boot();
  appendAll(0, 31);  // sector 4 full
  int erases = flash.erases;
  flash.cutNextProgram = true;  // bucket 32: sector 5 erased, its first slot torn, and the board resets
  CHECK(!append(32));
  CHECK_EQ(flash.erases, erases + 1);
  checkRun(boot(), 31, 32);
  CHECK(append(32));  // sector 5 erased again: the torn record goes
  CHECK_EQ(flash.erases, erases + 2);
  CHECK_EQ(slotIdx(32), 32);
  CHECK(erased(33));
  checkRun(boot(), 32, 33);
}

// Without a reset in between, a failed write's bucket is missing from the log: the run the next boot loads ends there.
TEST(hist_log_a_failed_write_leaves_a_gap) {
  boot();
  appendAll(0, 4);
  flash.cutNextProgram = true;
  CHECK(!append(5));
  CHECK(append(6));
  CHECK_EQ(slotIdx(6), 6);
  checkRun(boot(), 6, 1);

  // A sector's first write failing, the later ones in it not: the newest is still found.
  flash.reset();
  boot();
  appendAll(0, 31);
  flash.cutNextProgram = true;
  CHECK(!append(32));  // sector 5, slot 0
  appendAll(33, 34);
  Loaded l = boot();
  checkRun(l, 34, 2);
  CHECK(append(35));
  CHECK_EQ(slotIdx(32 + 3), 35);

  // The very first one failing.
  flash.reset();
  boot();
  flash.cutNextProgram = true;
  CHECK(!append(0));
  appendAll(1, 3);
  checkRun(boot(), 3, 3);

  // ... with the slot left erased (the write never started), the first sector's records after it found all the same.
  flash.reset();
  boot();
  appendAll(0, 31);
  memset(slot(32), 0xFF, HLOG_SLOT);  // as if bucket 32's write never started
  writeRaw(33, HLOG_FMT, 1, 34, 33, 3600);
  writeRaw(34, HLOG_FMT, 1, 35, 34, 3600);
  checkRun(boot(), 34, 2);
  CHECK(append(35));
  CHECK_EQ(slotIdx(35), 35);
  checkRun(boot(), 35, 3);
}

// The newest record is the valid one with the largest lseq: torn or failed writes above it (more of them than the
// records read in full first) don't hide it.
TEST(hist_log_newest_found_past_invalid_records_above_it) {
  boot();
  appendAll(0, 9);
  for (int s = 20; s < 26; s++) {  // six records with higher lseqs, each with a bad CRC
    writeRaw(s, HLOG_FMT, 1, 1000 + s, s, 3600);
    slot(s)[40] ^= 0x55;
  }
  Loaded l = boot();
  checkRun(l, 9, 10);
  CHECK(append(10));  // after the newest valid one, so after its lseq too
  CHECK_EQ(slotIdx(10), 10);
  checkRun(boot(), 10, 11);
}

TEST(hist_log_erase_failure_retries_at_the_next_record) {
  boot();
  appendAll(0, 31);
  flash.failErases = 1;
  CHECK(!append(32));  // not written; sector 5 still to erase
  CHECK(erased(32));
  CHECK(append(33));
  CHECK_EQ(slotIdx(32), 33);
  checkRun(boot(), 33, 1);
}

TEST(hist_log_garbled_read_is_read_again) {
  boot();
  appendAll(0, 9);
  flash.garbleReads = 1;
  checkRun(boot(), 9, 10);
  CHECK(histLogOn());
}

// Garbled twice (and the chip's id with them: the bus, not the flash): as with the boot counter, the read could hide
// the newest record, and writing on from the wrong place would erase history or repeat lseqs. Nothing is loaded or
// written until the next boot, which finds it all.
TEST(hist_log_garbled_twice_writes_nothing_until_the_next_boot) {
  boot();
  appendAll(0, 9);
  flash.garbleReads = 3;  // two reads of slot 0's header, then the id
  Loaded l = boot();
  CHECK(!l.had);
  CHECK(l.b.empty());
  CHECK(!histLogOn());
  std::vector<uint8_t> before(flash.mem, flash.mem + sizeof(flash.mem));
  int erases = flash.erases, programs = flash.programs;
  CHECK(!append(10));
  CHECK(!histLogClear(60));
  CHECK_EQ(flash.erases, erases);
  CHECK_EQ(flash.programs, programs);
  CHECK(!memcmp(before.data(), flash.mem, sizeof(flash.mem)));
  checkRun(boot(), 9, 10);
  CHECK(histLogOn());
}

// Zeros the flash really holds (an erase cut short by a power cut may leave a sector so, and another program may have
// left data there) read the same as a garbled bus, but the chip still gives its id: they're passed over like any slot
// that isn't a valid record, the log stays on, and entering that sector erases it. (Taken for a garbled bus, every
// boot gave up on them and nothing could write the log again, hist.clear included.)
TEST(hist_log_zeros_in_the_flash_are_not_a_garbled_bus) {
  boot();
  appendAll(0, 9);
  memset(slot(0), 0, HLOG_SLOT);                                       // bucket 0's record
  memset(flash.mem + BASE + EXTFLASH_SECTOR, 0, EXTFLASH_SECTOR);      // all of the next sector, 5
  memset(flash.mem + BASE + 15 * EXTFLASH_SECTOR, 0, EXTFLASH_SECTOR); // and the last
  Loaded l = boot();
  checkRun(l, 9, 9);  // 9 .. 1
  CHECK(histLogOn());
  appendAll(10, 40);  // into sector 5, erased first
  CHECK(erased(41));
  checkRun(boot(), 40, 40);
  CHECK(histLogClear(60));
  Loaded c = boot();
  CHECK(c.had);
  CHECK_EQ(c.period, 60);
  CHECK_EQ(c.next, 0);

  // A blank log but for a zeroed sector: on, and it starts in its first sector.
  flash.reset();
  memset(flash.mem + BASE + 7 * EXTFLASH_SECTOR, 0, EXTFLASH_SECTOR);
  CHECK(!boot().had);
  CHECK(histLogOn());
  CHECK(append(0));
  CHECK_EQ(slotIdx(0), 0);
  checkRun(boot(), 0, 1);
}

TEST(hist_log_clear_starts_an_empty_history_of_its_period) {
  boot();
  appendAll(0, 9);
  CHECK(histLogClear(60));
  Loaded l = boot();
  CHECK(l.had);
  CHECK_EQ(l.period, 60);
  CHECK_EQ(l.next, 0);
  CHECK(l.b.empty());
  appendAll(0, 2, 60);
  checkRun(boot(), 2, 3, 60);  // not the hour-long buckets from before the clear
}

TEST(hist_log_records_of_another_format_are_not_loaded) {
  boot();
  appendAll(0, 4);
  // A newer firmware's buckets after them (another bucket layout), then a downgrade.
  for (int s = 5; s < 8; s++) writeRaw(s, 2, 1, 6 + s - 5 + 100, s, 3600);
  Loaded l = boot();
  CHECK(!l.had);  // nothing this firmware can go on from: it starts over
  CHECK(append(0));
  CHECK_EQ(slotIdx(8), 0);  // after them, not over them
  CHECK_EQ(slotIdx(7), 7);
  checkRun(boot(), 0, 1);
}

TEST(hist_log_lseq_ran_out_writes_nothing) {
  boot();
  appendAll(0, 2);
  writeRaw(3, HLOG_FMT, 1, 0xFFFFFFFFu, 3, 3600);
  Loaded l = boot();
  checkRun(l, 3, 4);
  CHECK(!histLogOn());
  CHECK(!append(4));
  CHECK(erased(4));
}

TEST(hist_log_without_the_chip) {
  flash.present = false;
  Loaded l = boot();
  CHECK(!l.had);
  CHECK(!histLogOn());
  CHECK(!append(0));
  CHECK(histLogClear(60));  // nothing kept there to clear
  CHECK_EQ(flash.programs, 0);
  CHECK_EQ(flash.erases, 0);
}

// A run ends at a bucket that doesn't follow on (a gap, or one written before the newer ones: lseq out of order).
TEST(hist_log_run_ends_at_a_bucket_out_of_order) {
  boot();
  appendAll(0, 9);
  writeRaw(3, HLOG_FMT, 1, 1000, 3, 3600);  // bucket 3 again, but newer than everything after it
  writeRaw(10, HLOG_FMT, 1, 1001, 10, 3600);
  checkRun(boot(), 10, 7);  // 10 .. 4: slot 3's lseq is above slot 4's
}
