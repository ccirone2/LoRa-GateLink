// evlog.cpp over the fake SPI flash: the event journal in sectors 20-35 (records of up to 14 entries, read back newest
// first by log.get rec), its wrap and sector erases, power cuts and failed writes, a reboot going on after the newest
// record, and a board without the chip. Each "boot" is evLogBegin on the flash as it is.
#include "sim.h"
#include "evlog.h"
#include "extflash.h"
#include "histlog.h"

#define BASE (ELOG_SECTOR0 * EXTFLASH_SECTOR)
#define PER_SECTOR (int)(EXTFLASH_SECTOR / ELOG_SLOT)  // 16
#define SLOTS (ELOG_SECTORS * PER_SECTOR)              // 256

static_assert(ELOG_SECTOR0 == HLOG_SECTOR0 + HLOG_SECTORS, "the journal follows the history log");

// Record r's entries: n of them, each telling which record and entry it is.
static std::vector<LogEntry> rec(uint32_t r, uint8_t n = ELOG_PER_REC) {
  std::vector<LogEntry> v;
  for (uint8_t i = 0; i < n; i++)
    v.push_back(LogEntry{ r * 1000 + i, (uint8_t)(i % EV_COUNT), (uint8_t)i, (uint16_t)(r & 0xFFFF), (int32_t)r,
                          -(int32_t)i });
  return v;
}

static bool append(uint32_t r, uint8_t n = ELOG_PER_REC) {
  std::vector<LogEntry> v = rec(r, n);
  return evLogAppend(v.data(), n);
}

static void appendAll(uint32_t from, uint32_t to) {
  for (uint32_t r = from; r <= to; r++)
    if (!append(r)) throw Failure("append " + std::to_string(r) + " failed on a healthy chip");
}

// The k-th newest record (k = 1 the newest), followed back by the cursor as log.get does: its entries, or 0.
static uint8_t getK(uint16_t k, LogEntry *out) {
  uint32_t before = UINT32_MAX, seq = 0;
  uint8_t n = 0;
  for (uint16_t i = 0; i < k; i++) {
    n = evLogGet(before, out, seq);
    if (!n) return 0;
    before = seq;
  }
  return n;
}

// The k-th newest record read back is record r, with n entries.
static void checkGet(uint16_t k, uint32_t r, uint8_t n = ELOG_PER_REC) {
  LogEntry out[ELOG_PER_REC];
  CHECK_EQ(getK(k, out), n);
  std::vector<LogEntry> want = rec(r, n);
  for (uint8_t i = 0; i < n; i++) {
    CHECK_EQ(out[i].t, want[i].t);
    CHECK_EQ(out[i].code, want[i].code);
    CHECK_EQ(out[i].rep, want[i].rep);
    CHECK_EQ(out[i].boot, want[i].boot);
    CHECK_EQ(out[i].a, want[i].a);
    CHECK_EQ(out[i].b, want[i].b);
  }
}

static uint8_t get(uint16_t k) {
  LogEntry out[ELOG_PER_REC];
  return getK(k, out);
}

TEST(ev_log_blank_has_nothing_and_starts_in_its_first_sector) {
  flash.reset();
  evLogBegin();
  CHECK(evLogOn());
  CHECK_EQ(get(1), 0);
  CHECK(append(0, 3));
  CHECK_EQ(flash.erases, 1);
  checkGet(1, 0, 3);
  CHECK_EQ(get(2), 0);
}

TEST(ev_log_reads_newest_first_and_goes_on_after_a_reboot) {
  flash.reset();
  evLogBegin();
  appendAll(0, 9);
  for (uint16_t k = 1; k <= 10; k++) checkGet(k, 10 - k);
  CHECK_EQ(get(11), 0);
  evLogBegin();  // a reboot: the next record goes after the newest
  appendAll(10, 12);
  checkGet(1, 12);
  checkGet(4, 9);
  checkGet(13, 0);
}

// Past its 256 slots the journal wraps: entering a sector erases it, so the oldest sector's 16 records go.
TEST(ev_log_wrap_keeps_the_newest_and_erases_one_sector_at_a_time) {
  flash.reset();
  evLogBegin();
  appendAll(0, SLOTS + 4);  // 261 records: the first sector holds 256..260, its other 11 slots erased
  CHECK_EQ(flash.erases, ELOG_SECTORS + 1);
  checkGet(1, SLOTS + 4);
  // Back from 260: 260..256 in sector 0, then sector 15 down to sector 1 (records 255..16); sector 0's erased slots
  // after the head end it there.
  checkGet(5, SLOTS);
  checkGet(6, SLOTS - 1);
  checkGet(5 + SLOTS - PER_SECTOR, PER_SECTOR);
  CHECK_EQ(get(5 + SLOTS - PER_SECTOR + 1), 0);
  evLogBegin();
  checkGet(1, SLOTS + 4);
}

// A power cut mid-write tears that record: it reads invalid and is passed over; the next boot writes after it.
TEST(ev_log_power_cut_mid_write_loses_that_record_only) {
  flash.reset();
  evLogBegin();
  appendAll(0, 4);
  flash.cutNextProgram = true;
  CHECK(!append(5));
  evLogBegin();
  checkGet(1, 4);
  CHECK(append(6));
  checkGet(1, 6);
  checkGet(2, 4);
  checkGet(6, 0);
}

// An erase that fails writes nothing and doesn't move on: the next try erases that sector again.
TEST(ev_log_failed_erase_writes_nothing) {
  flash.reset();
  evLogBegin();
  appendAll(0, PER_SECTOR - 1);  // sector 0 full
  flash.failErases = 1;
  CHECK(!append(PER_SECTOR));
  checkGet(1, PER_SECTOR - 1);
  CHECK(append(PER_SECTOR + 1));
  checkGet(1, PER_SECTOR + 1);
  checkGet(2, PER_SECTOR - 1);
}

// A garbled first read at boot leaves the journal off until the next boot: it could hide the newest record.
TEST(ev_log_garbled_boot_read_keeps_it_off) {
  flash.reset();
  evLogBegin();
  appendAll(0, 2);
  flash.garbleReads = 3;  // a slot's header twice, and the id
  evLogBegin();
  CHECK(!evLogOn());
  CHECK(!append(3));
  CHECK_EQ(get(1), 0);
  evLogBegin();
  CHECK(evLogOn());
  checkGet(1, 2);
}

// The history log's sectors are left alone, and records there aren't taken for the journal's.
TEST(ev_log_keeps_to_its_own_sectors) {
  flash.reset();
  evLogBegin();
  int erases = flash.erases;
  appendAll(0, SLOTS * 2);
  for (uint32_t a = 0; a < BASE; a++)
    if (flash.mem[a] != 0xFF) throw Failure("the journal wrote below its first sector");
  CHECK_EQ(flash.erases - erases, 2 * ELOG_SECTORS + 1);
}

// The cursor is an lseq, so records written between two pages don't shift them: paging on after a write goes on
// from where it was, and the new record is the newest.
TEST(ev_log_cursor_is_stable_across_writes) {
  flash.reset();
  evLogBegin();
  appendAll(0, 4);
  LogEntry out[ELOG_PER_REC];
  uint32_t seq = 0;
  CHECK_EQ(evLogGet(UINT32_MAX, out, seq), ELOG_PER_REC);
  CHECK_EQ(out[0].a, 4);
  uint32_t first = seq;
  appendAll(5, 6);
  CHECK_EQ(evLogGet(first, out, seq), ELOG_PER_REC);
  CHECK_EQ(out[0].a, 3);  // the record before the first page's, not record 4 again
  CHECK_EQ(evLogGet(UINT32_MAX, out, seq), ELOG_PER_REC);
  CHECK_EQ(out[0].a, 6);
  CHECK_EQ(evLogGet(1, out, seq), 0);  // nothing before the first record
}

TEST(ev_log_without_the_chip) {
  flash.reset();
  flash.present = false;
  evLogBegin();
  CHECK(!evLogOn());
  CHECK(!append(0));
  CHECK_EQ(get(1), 0);
  CHECK_EQ(flash.programs, 0);
  flash.present = true;
}
