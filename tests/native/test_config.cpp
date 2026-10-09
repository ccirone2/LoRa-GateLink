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
