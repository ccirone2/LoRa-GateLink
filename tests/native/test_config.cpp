// config.cpp over the fake SPI flash: round trips, the two alternating records, power cuts and corruption,
// records from other firmware, partial saves, the program-flash fallback and the boot counter.
#include "sim.h"
#include <FlashStorage.h>
#include "extflash.h"

extern FlashStorageClass<Config> cfgStore;  // config.cpp's fallback store

#define REC_HDR 31

static uint32_t crc32(const uint8_t *d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
  }
  return ~c;
}

// A record as config.cpp writes it (magic fmt count seq crc key key_set {id value}...), for records from other
// firmware.
static void writeRecord(int sector, uint32_t seq, const std::vector<std::pair<uint8_t, int32_t>> &params) {
  uint8_t *b = flash.mem + sector * EXTFLASH_SECTOR;
  memset(b, 0xFF, EXTFLASH_SECTOR);
  putU32(b, 0x31434C47u);
  b[4] = 1;
  b[5] = (uint8_t)params.size();
  putU32(b + 6, seq);
  for (int i = 0; i < 16; i++) b[14 + i] = (uint8_t)i;
  b[30] = 1;
  uint8_t *p = b + REC_HDR;
  for (auto &kv : params) {
    p[0] = kv.first;
    putU32(p + 1, (uint32_t)kv.second);
    p += 5;
  }
  putU32(b + 10, crc32(b + 14, p - b - 14));
}

static uint32_t recordSeq(int sector) { return getU32(flash.mem + sector * EXTFLASH_SECTOR + 6); }

// Ids in a record written to flash.
static std::vector<uint8_t> recordIds(int sector) {
  const uint8_t *b = flash.mem + sector * EXTFLASH_SECTOR;
  std::vector<uint8_t> ids;
  for (int i = 0; i < b[5]; i++) ids.push_back(b[REC_HDR + 5 * i]);
  return ids;
}

static int newestSector() {
  if (recordSeq(1) == 0xFFFFFFFFu) return 0;  // erased
  if (recordSeq(0) == 0xFFFFFFFFu) return 1;
  return (int32_t)(recordSeq(1) - recordSeq(0)) > 0 ? 1 : 0;
}

static void fresh() {
  cfgStore.erase();
  CHECK(!configLoad());  // blank: defaults
  CHECK_EQ(configSource(), CFG_DEFAULTS);
}

static void saveWith(int32_t pulse) {
  cfg.pulse_ms = pulse;
  CHECK(configSave());
}

TEST(config_round_trip) {
  fresh();
  cfg.role = ROLE_GATE;
  cfg.pulse_ms = 750;
  cfg.travel_timeout_s = 120;
  cfg.bw_hz = 250000;
  for (int i = 0; i < 16; i++) cfg.key[i] = (uint8_t)(0x30 + i);
  cfg.key_set = 1;
  Config saved = cfg;
  CHECK(configSave());
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(configSource(), CFG_FROM_SPI);
  CHECK_EQ(configDropped(), 0);
  for (size_t i = 0; i < PARAM_COUNT; i++) CHECK_EQ(cfg.*(PARAMS[i].field), saved.*(PARAMS[i].field));
  CHECK(memcmp(cfg.key, saved.key, 16) == 0);
  CHECK_EQ(cfg.key_set, 1);
  CHECK(strcmp(configStoreName(), "spi") == 0);
}

TEST(config_records_alternate_and_the_newest_wins) {
  fresh();
  saveWith(600);
  saveWith(700);
  saveWith(800);
  CHECK_EQ(recordSeq(0), 3);
  CHECK_EQ(recordSeq(1), 2);
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 800);
}

TEST(config_power_cut_mid_save_keeps_the_previous_record) {
  fresh();
  saveWith(600);
  flash.cutNextProgram = true;
  cfg.pulse_ms = 700;
  CHECK(!configSave());  // the read-back doesn't verify
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 600);
  saveWith(800);  // and the next save goes on from there
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 800);
}

TEST(config_corrupt_newest_record_falls_back_to_the_older) {
  fresh();
  saveWith(600);
  saveWith(700);
  int s = newestSector();
  flash.mem[s * EXTFLASH_SECTOR + REC_HDR + 1] ^= 0x01;  // a bit flipped in a value
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 600);
}

TEST(config_unknown_and_out_of_range_settings_are_dropped) {
  // A record from other firmware: an id this one doesn't know, a value outside today's range. Both are counted,
  // the rest loads, and the setting out of range keeps its default.
  fresh();
  writeRecord(0, 5, { { 16, 700 }, { 200, 5 }, { 17, 9999 }, { 1, ROLE_HOUSE } });
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 700);
  CHECK_EQ(cfg.role, ROLE_HOUSE);
  CHECK_EQ(cfg.travel_timeout_s, 60);
  CHECK_EQ(configDropped(), 2);
  CHECK_EQ(cfg.heartbeat_s, 30);  // missing from the record: default
}

TEST(config_save_param_leaves_other_edits_unsaved) {
  fresh();
  cfg.retries = 5;
  saveWith(600);
  cfg.pulse_ms = 900;
  cfg.retries = 7;  // a console edit, not saved
  CHECK(configSaveParam(paramByName("pulse_ms")));
  CHECK_EQ(cfg.retries, 7);  // the running config is untouched
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 900);
  CHECK_EQ(cfg.retries, 5);
}

TEST(config_save_key_leaves_other_edits_unsaved) {
  fresh();
  saveWith(600);
  cfg.pulse_ms = 900;
  for (int i = 0; i < 16; i++) cfg.key[i] = (uint8_t)(0x55 + i);
  cfg.key_set = 1;
  CHECK(configSaveKey());
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 600);
  CHECK_EQ(cfg.key_set, 1);
  CHECK_EQ(cfg.key[15], 0x55 + 15);
}

TEST(config_factory_reset_erases_both_records) {
  fresh();
  cfg.key_set = 1;
  saveWith(600);
  saveWith(700);
  CHECK(configFactoryReset());
  CHECK_EQ(cfg.pulse_ms, 500);
  CHECK_EQ(cfg.key_set, 0);
  for (int i = 0; i < 2 * (int)EXTFLASH_SECTOR; i++) CHECK_EQ(flash.mem[i], 0xFF);
  CHECK(!configLoad());
  saveWith(650);  // saving starts again from a blank chip
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 650);
}

static const uint8_t KEY_A[16] = { 0x8a, 0x3f, 0x1c, 0x6e, 0x9b, 0x2d, 0x4f, 0x70,
                                    0xa1, 0xc3, 0xe5, 0xf7, 0x09, 0x2b, 0x4d, 0x6f };
static const uint8_t KEY_B[16] = { 0x13, 0xd2, 0x77, 0x40, 0xe9, 0x5c, 0x31, 0xb8,
                                    0x06, 0x9f, 0x6a, 0xc4, 0x28, 0xfd, 0x85, 0x5e };

static void setKey(const uint8_t key[16]) {
  memcpy(cfg.key, key, 16);
  cfg.key_set = 1;
}

// Anywhere on the chip, whole or either half (what a power cut halfway through overwriting it would leave).
static bool flashHolds(const uint8_t key[16]) {
  for (size_t i = 0; i + 8 <= sizeof(flash.mem); i++)
    if (!memcmp(flash.mem + i, key, 8) || !memcmp(flash.mem + i, key + 8, 8)) return true;
  return false;
}

static bool loadsKey(const uint8_t key[16]) {
  configDefaults(cfg);
  return configLoad() && cfg.key_set == 1 && !memcmp(cfg.key, key, 16);
}

// key.set writes the new record into the other sector, so the one before still held the old key, readable from the
// chip (threat model old-key-in-older-record), until the next save. Once the new record verifies, the old key is
// overwritten. A save that keeps the key keeps the older record, the fallback if the newest goes bad.
TEST(config_key_change_leaves_no_old_key_in_flash) {
  fresh();
  setKey(KEY_A);
  CHECK(configSave());
  CHECK(configSave());  // both sectors hold key A
  CHECK(flashHolds(KEY_A));
  setKey(KEY_B);
  CHECK(configSaveKey());
  CHECK(!flashHolds(KEY_A));
  CHECK(loadsKey(KEY_B));
  cfg.pulse_ms = 700;
  CHECK(configSaveParam(paramByName("pulse_ms")));
  CHECK(configSave());
  CHECK(!flashHolds(KEY_A));
  CHECK(loadsKey(KEY_B));
  CHECK_EQ(cfg.pulse_ms, 700);
  // A full save of a changed key too (config.save after a key.set whose save failed).
  setKey(KEY_A);
  CHECK(configSave());
  CHECK(!flashHolds(KEY_B));
  CHECK(loadsKey(KEY_A));
}

// Power cuts during a key change: before the new record verifies, the old one (old key) is what loads, and the save
// reported failure; after, the new key loads, and what's left of the old one goes with the next save.
TEST(config_key_change_power_cuts_leave_a_valid_record) {
  auto start = [] {
    flash.reset();  // a board as new
    fresh();
    setKey(KEY_A);
    CHECK(configSave());
    CHECK(configSave());
    setKey(KEY_B);
  };
  auto nextSave = [] {
    cfg.pulse_ms = 800;
    CHECK(configSave());
    CHECK(!flashHolds(KEY_A));
    CHECK(loadsKey(KEY_B));
  };
  // Cut writing the new record.
  start();
  flash.cutNextProgram = true;
  CHECK(!configSaveKey());
  CHECK(loadsKey(KEY_A));
  // Cut overwriting the old key: that record fails its CRC, the new one loads.
  start();
  flash.cutNextProgram = true;
  flash.cutAfterPrograms = 1;
  CHECK(configSaveKey());
  CHECK(!flash.cutNextProgram);  // the cut hit the overwrite (the new record's program went through)
  CHECK(loadsKey(KEY_B));
  nextSave();
  // Cut after the new record verified, before the old key was touched: put back the page the overwrite changed.
  start();
  int prev = newestSector();  // the record the new one goes past
  Bytes before(flash.mem + prev * EXTFLASH_SECTOR, flash.mem + prev * EXTFLASH_SECTOR + EXTFLASH_PAGE);
  CHECK(configSaveKey());
  memcpy(flash.mem + prev * EXTFLASH_SECTOR, before.data(), before.size());
  CHECK(loadsKey(KEY_B));
  nextSave();
}

TEST(config_falls_back_to_program_flash_without_the_chip) {
  flash.present = false;
  fresh();
  CHECK(strcmp(configStoreName(), "internal") == 0);
  saveWith(650);
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(configSource(), CFG_FROM_INTERNAL);
  CHECK_EQ(cfg.pulse_ms, 650);
  // A struct image from another CFG_VERSION isn't trusted: defaults.
  Config c;
  cfgStore.read(&c);
  c.version++;
  cfgStore.write(c);
  CHECK(!configLoad());
  CHECK_EQ(cfg.pulse_ms, 500);
}

TEST(config_param_ranges) {
  CHECK(paramValid(paramByName("bw_hz"), 250000));
  CHECK(!paramValid(paramByName("bw_hz"), 300000));
  CHECK(!paramValid(paramByName("pulse_ms"), 99));
  CHECK(paramValid(paramByName("pulse_ms"), 100));
  CHECK(!paramValid(nullptr, 1));
  CHECK(paramById(31) == paramByName("ctrl_power_pmic"));
  // ids are permanent and unique
  for (size_t i = 0; i < PARAM_COUNT; i++)
    for (size_t j = i + 1; j < PARAM_COUNT; j++) CHECK(PARAMS[i].id != PARAMS[j].id);
}

static void keyFromHex(const char *hex, uint8_t key[16]) {
  for (int i = 0; i < 16; i++) {
    unsigned v;
    sscanf(hex + 2 * i, "%2x", &v);
    key[i] = (uint8_t)v;
  }
}

static std::string keyIdOf(const char *hex) {
  uint8_t key[16];
  keyFromHex(hex, key);
  char id[9];
  memset(id, 'x', sizeof(id));
  configKeyId(key, id);
  CHECK_EQ(id[8], 0);
  return id;
}

// The same vectors as tests/web/fixtures/firmware.json (key_id) and the Python tests: HMAC-SHA256(key,
// "GateLink key id v1"), first 4 bytes.
TEST(key_id_is_the_hmac_of_its_label) {
  CHECK(keyIdOf("8a3f1c6e9b2d4f70a1c3e5f7092b4d6f") == "e03fddf7");
  CHECK(keyIdOf("00112233445566778899aabbccddeeff") == "fa60d1a7");
  CHECK(keyIdOf("0f1e2d3c4b5a69788796a5b4c3d2e1f0") == "2fa26167");
}

static bool weak(const char *hex) {
  uint8_t key[16];
  keyFromHex(hex, key);
  return configKeyWeak(key);
}

TEST(weak_keys_are_refused) {
  CHECK(weak("00000000000000000000000000000000"));
  CHECK(weak("ffffffffffffffffffffffffffffffff"));
  CHECK(weak("000102030405060708090a0b0c0d0e0f"));  // counting up
  CHECK(weak("f8f9fafbfcfdfeff0001020304050607"));  // up, through the wrap
  CHECK(weak("0f0e0d0c0b0a09080706050403020100"));  // down
  CHECK(weak("01020304050607080102030405060708"));  // 8 distinct values
  CHECK(weak("deadbeefdeadbeefdeadbeefdeadbeef"));  // 4
  CHECK(weak("31323334353637383132333435363738"));  // ASCII "1234567812345678"
  CHECK(!weak("02030405060708090102030405060708"));  // 9 distinct values
  CHECK(!weak("8a3f1c6e9b2d4f70a1c3e5f7092b4d6f"));
  CHECK(!weak("00112233445566778899aabbccddeeff"));  // steps of 0x11: 16 distinct values
  CHECK(!weak("000102030405060708090a0b0c0d0e10"));  // counts up, but not to the end
  CHECK(!weak("0100ffcfcbcac9c8c7c6c5c4c3c2c1c0"));  // counts down, but not all the way
}

TEST(boot_counter_counts_up_across_sector_rollover) {
  // 4-byte slots, 1024 per sector, two sectors: 2500 boots wrap around twice.
  for (uint32_t i = 1; i <= 2500; i++) CHECK_EQ(configCountBoot(), i);
}

TEST(boot_counter_survives_a_factory_reset) {
  fresh();
  for (int i = 0; i < 5; i++) configCountBoot();
  CHECK(configFactoryReset());
  CHECK_EQ(configCountBoot(), 6);
}

TEST(boot_counter_without_the_chip_is_zero) {
  flash.present = false;
  CHECK_EQ(configCountBoot(), 0);
}

// Boot counter slots: sector 2 from slot 0, 4 bytes each, little-endian.
static uint32_t bootSlot(int slot) { return getU32(flash.mem + 2 * EXTFLASH_SECTOR + 4 * slot); }
static void setBootSlot(int slot, uint32_t v) { putU32(flash.mem + 2 * EXTFLASH_SECTOR + 4 * slot, v); }

// Boots in a row: every count returned is higher than the one before (0 is no count), so session ids, which are
// drawn from it, never repeat.
static void bootsCountUp(int n, uint32_t &last) {
  for (int i = 0; i < n; i++) {
    uint32_t c = configCountBoot();
    if (!c) throw Failure("the boot counter returned 0 on a healthy chip");
    CHECK(c > last);
    last = c;
  }
}

// A slot written by a boot that lost power mid-program reads high (bits not yet cleared stay 1), and its count was
// never returned. Before 0.13.9 the count was the largest slot + 1: a torn slot of 0xFFFFFFFD or more wrapped it to 1
// at every boot after (the session seed repeating), and one like 0xFFFFFF23 got there within ~220 boots. Such slots
// (bit 31 set) are skipped now.
TEST(boot_counter_skips_a_torn_slot_reading_high) {
  uint32_t last = 0;
  bootsCountUp(5, last);
  setBootSlot(5, 0xFFFFFFFDu);  // torn at boot 6
  CHECK_EQ(configCountBoot(), 6);
  CHECK_EQ(bootSlot(6), 6);
  last = 6;
  bootsCountUp(300, last);  // and on, never back to 1
  CHECK_EQ(last, 306);
}

// Firmware before 0.13.9 wrote a slot in one step, took a torn one as the count and went on from there: 0xFFFFFF23
// (torn, meant 7), then 0xFFFFFF24 ... 0xFFFFFFFE and 1 for good. Those slots have bit 31 set, apart from the 1s; the
// count goes on above everything this firmware can see, and the old high counts lie past anything it will reach.
TEST(boot_counter_goes_on_after_the_old_wrap_to_1) {
  uint32_t last = 0;
  bootsCountUp(6, last);
  setBootSlot(6, 0xFFFFFF23u);
  for (int i = 7; i < 7 + 0xDB; i++) setBootSlot(i, 0xFFFFFF23u + (i - 6));  // to 0xFFFFFFFE
  for (int i = 7 + 0xDB; i < 7 + 0xDB + 20; i++) setBootSlot(i, 1);
  CHECK_EQ(bootSlot(7 + 0xDA), 0xFFFFFFFEu);
  CHECK_EQ(configCountBoot(), 7);
  last = 7;
  bootsCountUp(1500, last);  // across both sectors' rollover
}

// A power cut in either step of writing a slot: the first (the count, with bit 31 set) or the commit (bit 31
// cleared). The count of a boot that lost power was never returned; what any boot returns is higher than before.
TEST(boot_counter_power_cut_mid_slot_never_repeats) {
  uint32_t last = 0;
  bootsCountUp(3, last);
  flash.cutNextProgram = true;  // the count's program
  CHECK_EQ(configCountBoot(), 0);
  CHECK(bootSlot(3) & 0x80000000u);  // torn
  bootsCountUp(2, last);
  flash.cutNextProgram = true;
  flash.cutAfterPrograms = 1;  // the commit's
  CHECK_EQ(configCountBoot(), 0);
  CHECK(bootSlot(6) & 0x80000000u);
  bootsCountUp(2000, last);
}

// A committed slot at the top (a count of 2^31, or a slot torn by older firmware after it cleared bit 31): the count
// goes up to BOOT_COUNT_MAX and then runs out, returning 0 (no count: session ids are drawn at random, as without the
// chip), never wrapping to a count used before.
TEST(boot_counter_runs_out_instead_of_wrapping) {
  uint32_t last = 0;
  bootsCountUp(3, last);
  setBootSlot(3, 0x7FFFFFFCu);
  CHECK_EQ(configCountBoot(), 0x7FFFFFFDu);
  CHECK_EQ(configCountBoot(), 0x7FFFFFFEu);
  for (int i = 0; i < 5; i++) CHECK_EQ(configCountBoot(), 0);
  CHECK_EQ(bootSlot(6), 0xFFFFFFFFu);  // nothing written once it ran out
}

// A read garbled by the radio module's MCU (extflash.h) returns zeros. No slot is ever written 0 (counts start at 1,
// and one being written has bit 31 set), so a slot reading 0 is the bus, not the counter: no count this boot, and
// nothing written or erased. Taken as slots, the zeros hid the largest count, which then came round again (session
// ids are drawn from it). Worse with the largest counts in the sector read garbled and the other one full: that one
// held the largest count read, so the sector holding the real largest was erased as the older one, and every count
// it held came round again.
TEST(boot_counter_garbled_read_gives_no_count) {
  uint32_t last = 0;
  bootsCountUp(5, last);
  flash.garbleReads = 1;  // sector 2's first page, where counts 1..5 are
  CHECK_EQ(configCountBoot(), 0);
  CHECK_EQ(bootSlot(5), 0xFFFFFFFFu);
  bootsCountUp(3, last);
  CHECK_EQ(last, 8);

  flash.reset();
  for (int i = 0; i < 1024; i++) putU32(flash.mem + 3 * EXTFLASH_SECTOR + 4 * i, 1 + i);  // sector 3 full: 1..1024
  for (int i = 0; i < 10; i++) setBootSlot(i, 1025 + i);                                  // sector 2: 1025..1034
  CHECK_EQ(configCountBoot(), 1035);
  int erases = flash.erases;
  flash.garbleReads = 1;
  CHECK_EQ(configCountBoot(), 0);
  CHECK_EQ(flash.erases, erases);
  CHECK_EQ(bootSlot(0), 1025);
  last = 1035;
  bootsCountUp(2000, last);
}

// A save of one setting re-reads the record to save on top of. If that read is garbled, the save must not start
// over at seq 1 in sector 0: the older record in sector 1 would then have the higher seq and win at the next boot,
// and the save, reported as done, would be lost. One garbled read is retried.
TEST(config_save_after_a_garbled_read_is_retried) {
  fresh();
  saveWith(600);
  saveWith(600);
  flash.garbleReads = 2;  // both sectors read back as zeros once
  cfg.pulse_ms = 900;
  CHECK(configSaveParam(paramByName("pulse_ms")));
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 900);
}

TEST(config_save_refused_while_the_record_reads_garbled) {
  fresh();
  saveWith(600);
  saveWith(600);
  int programs = flash.programs;
  flash.garbleReads = 4;  // the retry is garbled too
  cfg.pulse_ms = 900;
  CHECK(!configSaveParam(paramByName("pulse_ms")));
  flash.garbleReads = 4;
  CHECK(!configSaveKey());
  CHECK_EQ(flash.programs, programs);  // nothing written
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 600);
  cfg.pulse_ms = 900;  // and the next save works again
  CHECK(configSaveParam(paramByName("pulse_ms")));
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 900);
}

// The newest record, which verified when written, no longer reads intact: saving one setting on top of the older
// record would drop the newer one's change, so it's refused. A full save (config.save) still writes the running
// config.
TEST(config_save_refused_when_the_newest_record_went_bad) {
  fresh();
  saveWith(600);
  saveWith(700);
  int s = newestSector();
  flash.mem[s * EXTFLASH_SECTOR + REC_HDR + 1] ^= 0x01;
  cfg.retries = 3;
  CHECK(!configSaveParam(paramByName("retries")));
  CHECK_EQ(newestSector(), s);  // the older record is untouched
  CHECK(configSave());
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 700);
  CHECK_EQ(cfg.retries, 3);
}

// The boot read was garbled, so the board runs on defaults. A save of one setting reads the record again and
// saves on top of it, not on top of the defaults, with a seq that wins.
TEST(config_save_after_a_garbled_boot_read_keeps_the_record) {
  fresh();
  saveWith(600);
  saveWith(700);
  flash.garbleReads = 2;
  CHECK(!configLoad());
  CHECK_EQ(cfg.pulse_ms, 500);
  cfg.retries = 3;
  CHECK(configSaveParam(paramByName("retries")));
  configDefaults(cfg);
  CHECK(configLoad());
  CHECK_EQ(cfg.pulse_ms, 700);
  CHECK_EQ(cfg.retries, 3);
}

// After a downgrade, settings this firmware doesn't know (saved by a newer one) are written back by every save, so
// the newer firmware finds them again. A known setting out of range is dropped: this firmware saves its own value.
TEST(config_save_keeps_settings_this_firmware_does_not_know) {
  fresh();
  writeRecord(0, 5, { { 16, 700 }, { 200, 5 }, { 201, -7 }, { 17, 9999 } });
  CHECK(configLoad());
  CHECK_EQ(configDropped(), 3);
  auto check = [](const char *what) {
    const uint8_t *b = flash.mem + newestSector() * EXTFLASH_SECTOR;
    int found = 0;
    for (int i = 0; i < b[5]; i++) {
      const uint8_t *p = b + REC_HDR + 5 * i;
      int32_t v = (int32_t)getU32(p + 1);
      if (p[0] == 200) found += v == 5;
      if (p[0] == 201) found += v == -7;
      if (p[0] == 17) CHECK_EQ(v, 60);
    }
    if (found != 2) throw Failure(std::string("unknown settings lost by ") + what);
  };
  cfg.pulse_ms = 800;
  CHECK(configSaveParam(paramByName("pulse_ms")));
  check("configSaveParam");
  CHECK(configSaveKey());
  check("configSaveKey");
  CHECK(configSave());
  check("configSave");
  configDefaults(cfg);
  CHECK(configLoad());  // and this firmware still loads its own
  CHECK_EQ(cfg.pulse_ms, 800);
  CHECK_EQ(configDropped(), 2);
}

TEST(config_factory_reset_forgets_unknown_settings) {
  fresh();
  writeRecord(0, 5, { { 200, 5 } });
  CHECK(configLoad());
  CHECK(configFactoryReset());
  CHECK(configSave());
  for (uint8_t id : recordIds(newestSector())) CHECK(id != 200);
}

// A record full of unknown settings: this firmware's own come first, and the unknown ones fill what's left of the
// page.
TEST(config_unknown_settings_fill_at_most_the_page) {
  fresh();
  std::vector<std::pair<uint8_t, int32_t>> many;
  for (int i = 0; i < 45; i++) many.push_back({ (uint8_t)(200 + i), i });
  writeRecord(0, 5, many);
  CHECK(configLoad());
  CHECK(configSave());
  std::vector<uint8_t> ids = recordIds(newestSector());
  CHECK_EQ((int)ids.size(), 45);
  CHECK_EQ(ids[0], 1);
  configDefaults(cfg);
  CHECK(configLoad());
}
