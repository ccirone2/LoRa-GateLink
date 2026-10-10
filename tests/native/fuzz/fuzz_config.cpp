// fuzz_config: the SPI flash holds whatever the input says (sectors 0..3: the two config records and the boot
// counter; 4..19: the link history log), and the board loads it as at boot (configLoad, configCountBoot, the history).
// Then what must hold whatever was there:
//   - every setting loaded is in range (paramValid), the key flag 0 or 1
//   - the boot counter counts: two boots in a row get two counts, the second higher (session ids are drawn from it,
//     and must never repeat under one key), until it runs out (a committed slot at BOOT_COUNT_MAX) and returns 0;
//     a slot reading 0 (never written: a garbled read) gives 0 too
//   - configSave, then configLoad, gives back the same config (round trip)
//   - configSaveParam writes one setting on top of what's saved and leaves another unsaved edit unsaved
//   - configSaveKey keeps the settings and leaves the old key in neither record
//   - configFactoryReset leaves nothing to load
//   - none of that touches the history log's sectors
//   - the history (history.cpp) takes from the log only what makes sense: hist.get reports a period hist.clear can
//     set, and pages of consecutive buckets from `oldest` to `current`
//   - the log (histlog.cpp) hands over at most HIST_DEPTH buckets, newest first, numbered consecutively up to the next
//     one; with nothing to go on from it leaves period and next alone
//   - if it can write (histLogOn: no read garbled, lseq not run out), the first bucket appended after the load always
//     lands, and loading again hands it back first, intact, with the history going on after it; a CLEARED record
//     appended after that, if written, loads as an empty history of its period; if it can't write, appending and
//     clearing write nothing
//
// Input: the flash image from address 0 (sector 0 holds one record at its start, sector 1 the other, sectors 2-3
// the boot counter's 4-byte slots, sectors 4-19 the history log's 128-byte slots); past the input the flash reads
// erased (0xFF), and beyond 80 KB it is ignored. The last byte also picks which setting configSaveParam writes. A
// record's CRC-32 is something the fuzzer can't find by mutation, so unless flash byte 256 (sector 0's second page,
// which the firmware never reads) is even, the harness first rewrites each record's CRC to match what the record
// declares (magic and count as they are): the settings inside then reach the decoder. Likewise each history slot that
// starts with the log's magic gets its CRC fixed unless its byte 100 (past the record, never read) is even. Short
// inputs leave both erased (odd): fixed up.
#include <stdio.h>
#include <vector>
#include "harness.h"
#include "config.h"
#include "crc32.h"
#include "history.h"
#include "histlog.h"

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

// Boot counter (config.cpp): 4-byte slots in sectors 2 and 3, each read up to its first unwritten slot; bit 31 set =
// not committed (skipped). It gives no count (0) once it has run out, a committed slot reading BOOT_COUNT_MAX or more,
// or when a slot reads 0, which no slot is ever written (a garbled read). A slot in the way: one of those. With
// `pastFree`, also past a sector's first unwritten slot, which the first boot may write, so the second reads on.
#define BOOT_COUNT_MAX 0x7FFFFFFEu

static bool bootSlotInTheWay(bool pastFree) {
  for (uint32_t base : { 2 * 4096u, 3 * 4096u }) {
    for (uint32_t a = base; a < base + 4096; a += 4) {
      const uint8_t *p = hal.flash.data() + a;
      uint32_t v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
      if (v == 0xFFFFFFFFu && !pastFree) break;
      if (!v || (!(v & 0x80000000u) && v >= BOOT_COUNT_MAX)) return true;
    }
  }
  return false;
}

// History log (histlog.h): 128-byte slots from sector 4; a record is 88 bytes, its CRC in the last 4, over the rest.
#define HLOG_BASE (HLOG_SECTOR0 * 4096u)
#define HLOG_END (HLOG_BASE + HLOG_SECTORS * 4096u)
#define HLOG_REC 88

static void fixHistCrcs() {
  for (uint32_t a = HLOG_BASE; a < HLOG_END; a += HLOG_SLOT) {
    uint8_t *r = hal.flash.data() + a;
    if (r[0] != 0x48 || r[1] != 0x4C || !(r[100] & 1)) continue;
    uint32_t c = crc32(r, HLOG_REC - 4);
    for (int i = 0; i < 4; i++) r[HLOG_REC - 4 + i] = (uint8_t)(c >> (8 * i));
  }
}

static std::vector<std::pair<uint32_t, Bytes>> taken;

static void takeOne(uint32_t idx, const uint8_t *d) {
  taken.push_back({ idx, Bytes(d, d + HLOG_DATA) });
}

// Loads the log and checks what it handed over. Returns whether it had history (period, next set).
static bool loadLog(const char *when, uint16_t &period, uint32_t &next) {
  taken.clear();
  const uint16_t P0 = 0xBEEF;
  const uint32_t N0 = 0xDEADBEEF;
  period = P0;
  next = N0;
  bool had = histLogBegin(HIST_DEPTH, takeOne, period, next);
  if (!had && (period != P0 || next != N0)) halTrap("%s: histLogBegin found nothing but set period/next", when);
  if (!had && !taken.empty()) halTrap("%s: histLogBegin found nothing but handed over %zu buckets", when, taken.size());
  if (taken.size() > HIST_DEPTH) halTrap("%s: %zu buckets handed over, more than HIST_DEPTH", when, taken.size());
  for (size_t i = 0; i < taken.size(); i++)
    if (taken[i].first != next - 1 - i)
      halTrap("%s: bucket %zu handed over is number %u, expected %u", when, i, (unsigned)taken[i].first,
              (unsigned)(next - 1 - i));
  return had;
}

static uint64_t histRegionHash() {
  uint64_t h = 1469598103934665603ULL;
  for (uint32_t a = HLOG_BASE; a < HLOG_END; a++) h = (h ^ hal.flash[a]) * 1099511628211ULL;
  return h;
}

// What history.cpp makes of the log at boot, through hist.get.
static void checkHistory() {
  histBegin(7);
  JsonDocument d;
  JsonObject res = d.to<JsonObject>();
  histGet(res, -1, HIST_PAGE);
  uint32_t period = res["period_s"], oldest = res["oldest"], current = res["current"];
  if (period < 60 || period > 3600) halTrap("hist.get: period_s %u", (unsigned)period);
  if (oldest > current || current - oldest > HIST_DEPTH)
    halTrap("hist.get: oldest %u, current %u", (unsigned)oldest, (unsigned)current);
  JsonArray rows = res["rows"];
  if (rows.size() == 0 || rows.size() > HIST_PAGE) halTrap("hist.get: %zu rows", rows.size());
  for (size_t i = 0; i < rows.size(); i++)
    if (rows[i][0].as<uint32_t>() != oldest + i) halTrap("hist.get: row %zu is bucket %u", i, rows[i][0].as<unsigned>());
  if (current - oldest < HIST_PAGE && rows[rows.size() - 1][0].as<uint32_t>() != current)
    halTrap("hist.get: the last row isn't the bucket in progress");
}

static void checkLog(const uint8_t *data, size_t size) {
  uint16_t period;
  uint32_t next;
  bool had = loadLog("load", period, next);
  if (!histLogOn()) {
    Bytes before = hal.flash;
    uint8_t z[HLOG_DATA] = {};
    if (histLogAppend(0, 3600, z) || histLogClear(60)) halTrap("the log wrote although it can't");
    if (hal.flash != before) halTrap("the log changed the flash although it can't write");
    return;
  }
  // A bucket appended after the load lands, and comes back first, as written.
  uint32_t idx = had ? next : 0;
  uint16_t p = had ? period : 3600;
  uint8_t d[HLOG_DATA];
  for (size_t i = 0; i < HLOG_DATA; i++) d[i] = size ? (uint8_t)(data[i % size] + i) : (uint8_t)i;
  if (!histLogAppend(idx, p, d)) halTrap("the first bucket appended after a load failed (bucket %u)", (unsigned)idx);
  uint16_t p2;
  uint32_t n2;
  if (!loadLog("reload", p2, n2)) halTrap("nothing loaded after appending bucket %u", (unsigned)idx);
  if (p2 != p || n2 != idx + 1 || taken.empty() || taken[0].first != idx || memcmp(taken[0].second.data(), d, HLOG_DATA))
    halTrap("reload: period %u, next %u, %zu buckets: not bucket %u as appended", (unsigned)p2, (unsigned)n2,
            taken.size(), (unsigned)idx);
  // Cleared: an empty history of its period (if written: the slot after the head may hold anything here).
  if (histLogOn() && histLogClear(120)) {
    if (!loadLog("after clear", p2, n2) || p2 != 120 || n2 != 0 || !taken.empty())
      halTrap("after a clear: period %u, next %u, %zu buckets", (unsigned)p2, (unsigned)n2, taken.size());
  }
}

static int runInput(const uint8_t *data, size_t size) {
  startBlank(1000);
  if (size) memcpy(hal.flash.data(), data, size < HAL_FLASH_BYTES ? size : HAL_FLASH_BYTES);
  if (hal.flash[256] & 1) fixCrcs();
  fixHistCrcs();
  uint64_t histBefore = histRegionHash();

  bool loaded = configLoad();
  checkLoaded("load");
  if (loaded != (configSource() == CFG_FROM_SPI)) halTrap("configLoad returned %d with source %u", loaded, configSource());
  bool inTheWay = bootSlotInTheWay(false), later = bootSlotInTheWay(true);
  uint32_t c1 = configCountBoot(), c2 = configCountBoot();
  // No count only for a slot in the way, and then none from the first boot on, nothing written; else 0 only for the
  // second boot, after the first took the top count or wrote the slot before one in the way.
  if (inTheWay) {
    if (c1 || c2) halTrap("boot counter counted past a slot in the way: %u, then %u", c1, c2);
  } else if (!c1 || (!c2 && c1 != BOOT_COUNT_MAX && !later)) {
    halTrap("boot counter failed on a healthy chip: %u, then %u", c1, c2);
  }
  if (c1 && c2 && c2 <= c1) halTrap("boot counter didn't count up: %u, then %u (session ids repeat)", c1, c2);

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

  // The key, saved on top; the old one is then in neither record
  uint8_t oldKey[16];
  memcpy(oldKey, cfg.key, 16);
  for (int i = 0; i < 16; i++) cfg.key[i] = (uint8_t)(seed >> (i % 4 * 8)) ^ (uint8_t)i;
  cfg.key_set = 1;
  Saved withKey = snap();
  if (!configSaveKey()) halTrap("configSaveKey failed on a healthy chip");
  // (All 0x00, the default, or all 0xFF, erased flash: no key to hide. Overwriting leaves zeros.)
  bool blank = true;
  for (int i = 1; i < 16; i++) blank &= oldKey[i] == oldKey[0];
  blank &= oldKey[0] == 0x00 || oldKey[0] == 0xFF;
  for (uint32_t base : { 0u, 4096u })
    if (!blank && memcmp(oldKey, cfg.key, 16) && !memcmp(hal.flash.data() + base + 14, oldKey, 16))
      halTrap("the old key is still in the record at 0x%x after configSaveKey", base);
  if (!configLoad()) halTrap("nothing to load after configSaveKey");
  expect(withKey, "after configSaveKey");

  // config.reset: nothing left, defaults in RAM
  if (!configFactoryReset()) halTrap("configFactoryReset failed on a healthy chip");
  if (configLoad()) halTrap("a record survived configFactoryReset");
  checkLoaded("after reset");
  if (histRegionHash() != histBefore) halTrap("the config writes changed the history log's sectors");

  checkHistory();
  checkLog(data, size);

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
