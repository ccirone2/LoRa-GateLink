// fuzz_config: the SPI flash holds whatever the input says (sectors 0..3: the two config records and the boot
// counter), and the board loads it as at boot (configLoad, configCountBoot). Then what must hold whatever was there:
//   - every setting loaded is in range (paramValid), the key flag 0 or 1
//   - the boot counter counts: two boots in a row get two counts, the second higher (it seeds the session id,
//     which must never repeat under one key)
//   - configSave, then configLoad, gives back the same config (round trip)
//   - configSaveParam writes one setting on top of what's saved and leaves another unsaved edit unsaved
//   - configSaveKey keeps the settings; configFactoryReset leaves nothing to load
//
// Input: the flash image from address 0 (sector 0 holds one record at its start, sector 1 the other, sectors 2-3
// the boot counter's 4-byte slots); past the input the flash reads erased (0xFF), and beyond 16 KB it is ignored.
// The last byte also picks which setting configSaveParam writes. A record's CRC-32 is something the fuzzer can't
// find by mutation, so unless flash byte 256 (sector 0's second page, which the firmware never reads) is even, the
// harness first rewrites each record's CRC to match what the record declares (magic and count as they are): the
// settings inside then reach the decoder. Short inputs leave it erased (odd): fixed up.
#include <stdio.h>
#include <vector>
#include "harness.h"
#include "config.h"
#include "crc32.h"

struct Saved {
  std::vector<int32_t> v;
  uint8_t key[16];
  int32_t keySet;
};

static Saved snap() {
  Saved s;
  for (size_t i = 0; i < PARAM_COUNT; i++) s.v.push_back(cfg.*(PARAMS[i].field));
  memcpy(s.key, cfg.key, 16);
  s.keySet = cfg.key_set;
  return s;
}

static void expect(const Saved &want, const char *when) {
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    int32_t got = cfg.*(PARAMS[i].field);
    if (got != want.v[i]) halTrap("%s: %s is %d, expected %d", when, PARAMS[i].name, (int)got, (int)want.v[i]);
  }
  if (memcmp(cfg.key, want.key, 16)) halTrap("%s: the key changed", when);
  if (cfg.key_set != want.keySet) halTrap("%s: key_set is %d, expected %d", when, (int)cfg.key_set, (int)want.keySet);
}

static void checkLoaded(const char *when) {
  for (size_t i = 0; i < PARAM_COUNT; i++) {
    int32_t v = cfg.*(PARAMS[i].field);
    if (!paramValid(&PARAMS[i], v)) halTrap("%s: %s = %d is out of range", when, PARAMS[i].name, (int)v);
  }
  if (cfg.key_set != 0 && cfg.key_set != 1) halTrap("%s: key_set = %d", when, (int)cfg.key_set);
}

// A valid value for p, from seed.
static int32_t validValue(const ParamDef *p, uint32_t seed) {
  if (p->field == &Config::bw_hz) {
    static const int32_t bw[] = { 125000, 250000, 500000 };
    return bw[seed % 3];
  }
  uint64_t span = (uint64_t)((int64_t)p->maxV - p->minV + 1);
  return (int32_t)(p->minV + (int64_t)(seed % span));
}

// Record layout (config.cpp): magic(4) fmt(1) count(1) seq(4) crc(4, over the rest) key(16) key_set(1) count x 5.
static void fixCrcs() {
  for (uint32_t base : { 0u, 4096u }) {
    uint8_t *r = hal.flash.data() + base;
    size_t len = 31 + 5 * (size_t)r[5];
    if (len > 256) continue;  // the decoder refuses it before looking at the CRC
    uint32_t c = crc32(r + 14, len - 14);
    for (int i = 0; i < 4; i++) r[10 + i] = (uint8_t)(c >> (8 * i));
  }
}

static int runInput(const uint8_t *data, size_t size) {
  startBlank(1000);
  if (size) memcpy(hal.flash.data(), data, size < HAL_FLASH_BYTES ? size : HAL_FLASH_BYTES);
  if (hal.flash[256] & 1) fixCrcs();

  bool loaded = configLoad();
  checkLoaded("load");
  if (loaded != (configSource() == CFG_FROM_SPI)) halTrap("configLoad returned %d with source %u", loaded, configSource());
  uint32_t c1 = configCountBoot(), c2 = configCountBoot();
  if (!c1 || !c2) halTrap("boot counter failed on a healthy chip: %u, then %u", c1, c2);
  if (c2 <= c1) {
    // Known bug (README.md, TODO.md): a slot that reads high (a program cut short, a garbled read) makes the count
    // fall back to 1 (crashes/fuzz_config/boot-counter-*), and it can stay 1 at every boot after. Tolerated unless
    // knownBugs; any other repeat traps.
    bool known = c2 == 1;
    if (!known || hal.knownBugs) halTrap("boot counter didn't count up: %u, then %u (session seeds repeat)", c1, c2);
  }

  // Round trip
  Saved before = snap();
  if (!configSave()) halTrap("configSave failed on a healthy chip");
  if (!configLoad()) halTrap("nothing to load after configSave");
  checkLoaded("reload");
  expect(before, "after save and load");

  // One setting saved on top of the record, another edited and left unsaved
  uint8_t pick = size ? data[size - 1] : 0;
  const ParamDef *p = &PARAMS[pick % PARAM_COUNT], *q = &PARAMS[(pick + 1) % PARAM_COUNT];
  uint32_t seed = size >= 5 ? (uint32_t)data[size - 2] << 24 | data[size - 3] << 16 | data[size - 4] << 8 | data[size - 5] : pick;
  int32_t pv = validValue(p, seed), qv = validValue(q, seed * 2654435761u + 1);
  Saved want = snap();
  if (!paramSet(p, pv) || !paramSet(q, qv)) halTrap("paramSet refused a valid value");
  want.v[p - PARAMS] = pv;
  if (!configSaveParam(p)) halTrap("configSaveParam(%s) failed on a healthy chip", p->name);
  if (!configLoad()) halTrap("nothing to load after configSaveParam");
  expect(want, "after configSaveParam");

  // The key, saved on top
  for (int i = 0; i < 16; i++) cfg.key[i] = (uint8_t)(seed >> (i % 4 * 8)) ^ (uint8_t)i;
  cfg.key_set = 1;
  Saved withKey = snap();
  if (!configSaveKey()) halTrap("configSaveKey failed on a healthy chip");
  if (!configLoad()) halTrap("nothing to load after configSaveKey");
  expect(withKey, "after configSaveKey");

  // config.reset: nothing left, defaults in RAM
  if (!configFactoryReset()) halTrap("configFactoryReset failed on a healthy chip");
  if (configLoad()) halTrap("a record survived configFactoryReset");
  checkLoaded("after reset");

  halHash(&cfg, sizeof(cfg));
  halHash(hal.flash.data(), hal.flash.size());
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return runInput(data, size);
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  harnessSetup();
  Bytes b(256, 0x00);  // a garbled record
  harnessSelfTest(runInput, provisionedImage(FR_GATE), b);
  return 0;
}
