#include "config.h"
#include "extflash.h"
#include "radio.h"
#include <FlashStorage.h>

// Config is kept in the on-board SPI flash, which firmware uploads don't touch. It's stored as (param id,
// value) pairs plus the key, so a firmware with added, removed or reordered settings still loads every
// setting it knows (the rest take their defaults). PARAMS ids are therefore permanent: if a setting's
// meaning or units change, give it a new id. Only if the SPI flash doesn't answer does config fall back to
// program flash (FlashStorage, a struct image checked against CFG_VERSION), which every upload erases.

#define CFG_MAGIC 0x47544C4Bu  // "GTLK"
#define CFG_VERSION 5

const char FW_MARKER[] = FW_MARKER_PREFIX FW_VERSION;
Config cfg;
int32_t activeRole = ROLE_UNSET;
FlashStorage(cfgStore, Config);
static uint8_t source = CFG_DEFAULTS;
static int32_t dropped = 0;

const ParamDef PARAMS[] = {
  { 1, "role", &Config::role, 0, 2, P_REBOOT },
  { 2, "net_id", &Config::net_id, 0, 255, 0 },
  { 3, "freq_hz", &Config::freq_hz, 862000000, 928000000, P_RADIO },
  { 4, "sf", &Config::sf, 7, 12, P_RADIO },
  { 5, "bw_hz", &Config::bw_hz, 125000, 500000, P_RADIO },
  { 6, "cr", &Config::cr, 5, 8, P_RADIO },
  { 7, "tx_power", &Config::tx_power, 2, 20, P_RADIO },
  { 8, "sync_word", &Config::sync_word, 0, 255, P_RADIO },
  { 9, "retries", &Config::retries, 0, 10, P_REMOTE },
  { 10, "heartbeat_s", &Config::heartbeat_s, 5, 3600, P_REMOTE },
  { 11, "link_timeout_s", &Config::link_timeout_s, 15, 10800, P_REMOTE },
  { 12, "cmd_ttl_s", &Config::cmd_ttl_s, 2, 120, 0 },  // house only
  { 13, "debounce_ms", &Config::debounce_ms, 10, 1000, P_REMOTE },
  // Not remote: inverted, a dead opto or cut wire reads active (e.g. a closed limit). Keep at 0.
  { 14, "in1_invert", &Config::in1_invert, 0, 1, 0 },
  { 15, "in2_invert", &Config::in2_invert, 0, 1, 0 },
  { 16, "pulse_ms", &Config::pulse_ms, 100, 5000, P_REMOTE },
  { 17, "travel_timeout_s", &Config::travel_timeout_s, 5, 300, P_REMOTE },
  { 18, "ctrl_sync", &Config::ctrl_sync, 0, 1, 0 },
  { 19, "sync_window_ms", &Config::sync_window_ms, 500, 10000, 0 },
  { 20, "resync_ms", &Config::resync_ms, 200, 5000, 0 },
  { 21, "mismatch_timeout_s", &Config::mismatch_timeout_s, 10, 600, 0 },
  { 22, "sensor_invert", &Config::sensor_invert, 0, 1, 0 },
  { 23, "linkloss_open", &Config::linkloss_open, 0, 1, 0 },
  { 24, "in3_invert", &Config::in3_invert, 0, 1, 0 },
  { 25, "in4_invert", &Config::in4_invert, 0, 1, 0 },
  { 26, "power_sense", &Config::power_sense, 0, 1, P_REMOTE },
  { 27, "ctrl_power_sense", &Config::ctrl_power_sense, 0, 1, 0 },
  { 28, "ctrl_confirm_ms", &Config::ctrl_confirm_ms, 0, 5000, 0 },
  { 29, "ctrl_settle_ms", &Config::ctrl_settle_ms, 0, 60000, 0 },
  { 30, "uart_console", &Config::uart_console, 0, 1, 0 },
};
const size_t PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

// SPI flash record, written to one page of sector 0 or 1 alternately (the newer seq wins), so losing power
// mid-save leaves the previous record:
//   magic(4) fmt(1) count(1) seq(4) crc(4, over the rest) key(16) key_set(1) count x { id(1) value(4) }
#define REC_MAGIC 0x31434C47u  // "GLC1"
#define REC_FMT 1
#define REC_HDR 31
#define REC_MAX_PARAMS ((EXTFLASH_PAGE - REC_HDR) / 5)
#define REC_SECTORS 2
static_assert(sizeof(PARAMS) / sizeof(PARAMS[0]) <= REC_MAX_PARAMS, "config record outgrew one flash page");
// Flash access holds the radio module in reset, which resets the radio: it's re-initialised afterwards (about
// 0.5 s off the air, in LoRa.begin()'s reset delays). Saves are rare (console, remote writes).
struct FlashAccess {
  FlashAccess() {
    if (extFlashPresent()) extFlashHoldModem();
  }
  ~FlashAccess() {
    if (extFlashPresent()) radioRestart();
  }
};
static int8_t spiSector = -1;  // sector holding the newest valid record (-1 = none)
static uint32_t spiSeq = 0;

static uint32_t crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
  }
  return ~crc;
}

static uint32_t configCrc(const Config &c) {
  return crc32((const uint8_t *)&c, offsetof(Config, crc));
}

static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

static uint32_t get32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t encode(const Config &c, uint32_t seq, uint8_t *buf) {
  put32(buf, REC_MAGIC);
  buf[4] = REC_FMT;
  buf[5] = PARAM_COUNT;
  put32(buf + 6, seq);
  memcpy(buf + 14, c.key, 16);
  buf[30] = c.key_set ? 1 : 0;
  uint8_t *p = buf + REC_HDR;
  for (size_t i = 0; i < PARAM_COUNT; i++, p += 5) {
    p[0] = PARAMS[i].id;
    put32(p + 1, (uint32_t)(c.*(PARAMS[i].field)));
  }
  size_t len = p - buf;
  put32(buf + 10, crc32(buf + 14, len - 14));
  return len;
}

static bool paramValid(const ParamDef *p, int32_t value);

// Applies a record on top of c (which holds the defaults). Returns false if buf holds no valid record.
static bool decode(const uint8_t *buf, Config &c, uint32_t &seq, int32_t &skipped) {
  if (get32(buf) != REC_MAGIC || buf[4] != REC_FMT || buf[5] > REC_MAX_PARAMS) return false;
  size_t len = REC_HDR + 5 * buf[5];
  if (get32(buf + 10) != crc32(buf + 14, len - 14)) return false;
  seq = get32(buf + 6);
  memcpy(c.key, buf + 14, 16);
  c.key_set = buf[30] ? 1 : 0;
  skipped = 0;
  for (const uint8_t *p = buf + REC_HDR; p < buf + len; p += 5) {
    const ParamDef *d = paramById(p[0]);
    int32_t v = (int32_t)get32(p + 1);
    if (d && paramValid(d, v)) c.*(d->field) = v;
    else skipped++;  // setting no longer exists, or its range shrank
  }
  return true;
}

// Loads the newest valid record from SPI flash on top of the defaults.
static bool spiRead(Config &c, int32_t &skipped) {
  uint8_t buf[EXTFLASH_PAGE];
  spiSector = -1;
  for (int8_t s = 0; s < REC_SECTORS; s++) {
    Config t;
    configDefaults(t);
    uint32_t seq;
    int32_t sk;
    extFlashRead(s * EXTFLASH_SECTOR, buf, sizeof(buf));
    if (!decode(buf, t, seq, sk)) continue;
    if (spiSector >= 0 && (int32_t)(seq - spiSeq) <= 0) continue;
    spiSector = s;
    spiSeq = seq;
    c = t;
    skipped = sk;
  }
  return spiSector >= 0;
}

// Writes into the sector not holding the newest record, then reads it back.
static bool spiWrite(const Config &c) {
  uint8_t buf[EXTFLASH_PAGE], check[EXTFLASH_PAGE];
  uint32_t seq = spiSector >= 0 ? spiSeq + 1 : 1;
  int8_t sector = spiSector == 0 ? 1 : 0;
  uint32_t addr = sector * EXTFLASH_SECTOR;
  size_t len = encode(c, seq, buf);
  if (!extFlashEraseSector(addr) || !extFlashProgram(addr, buf, len)) return false;
  extFlashRead(addr, check, len);
  if (memcmp(buf, check, len)) return false;
  spiSector = sector;
  spiSeq = seq;
  return true;
}

static bool valid(const Config &c) {
  return c.magic == CFG_MAGIC && c.version == CFG_VERSION && c.crc == configCrc(c);
}

void configDefaults(Config &c) {
  memset(&c, 0, sizeof(c));
  c.magic = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.role = ROLE_UNSET;
  c.net_id = 0x42;
  c.freq_hz = 915000000;
  c.sf = 9;
  c.bw_hz = 500000;
  c.cr = 5;
  c.tx_power = 17;
  c.sync_word = 0x12;
  c.retries = 5;
  c.heartbeat_s = 30;
  c.link_timeout_s = 100;
  c.cmd_ttl_s = 10;
  c.debounce_ms = 50;
  c.power_sense = 1;
  c.pulse_ms = 500;
  c.travel_timeout_s = 60;
  c.ctrl_sync = 1;
  c.sync_window_ms = 3000;
  c.resync_ms = 1000;
  c.mismatch_timeout_s = 75;
  c.sensor_invert = 0;
  c.linkloss_open = 1;
  c.ctrl_power_sense = 1;
  c.ctrl_confirm_ms = 3000;  // > the ~1.65 s the IN2 opto lags the controller's relay on a 12 V cut
  c.ctrl_settle_ms = 10000;
  c.uart_console = 0;
  c.key_set = 0;
}

bool configLoad() {
  configDefaults(cfg);
  source = CFG_DEFAULTS;
  dropped = 0;
  Config c;
  if (extFlashBegin()) {
    if (!spiRead(c, dropped)) return false;
  } else {
    cfgStore.read(&c);
    if (!valid(c)) return false;
  }
  cfg = c;
  source = extFlashPresent() ? CFG_FROM_SPI : CFG_FROM_INTERNAL;
  return true;
}

uint8_t configSource() {
  return source;
}

int32_t configDropped() {
  return dropped;
}

const char *configStoreName() {
  return extFlashPresent() ? "spi" : "internal";
}

static bool store(Config &c) {
  c.magic = CFG_MAGIC;
  c.version = CFG_VERSION;
  c.crc = configCrc(c);
  if (extFlashPresent()) return spiWrite(c);
  cfgStore.write(c);
  return true;
}

bool configSave() {
  FlashAccess fa;
  return store(cfg);
}

// What's saved, or the running config if nothing valid is saved yet (nothing to protect then).
static Config persisted() {
  Config c;
  if (extFlashPresent()) {
    int32_t skipped;
    if (!spiRead(c, skipped)) c = cfg;
  } else {
    cfgStore.read(&c);
    if (!valid(c)) c = cfg;
  }
  return c;
}

bool configSaveParam(const ParamDef *p) {
  FlashAccess fa;
  Config c = persisted();
  c.*(p->field) = cfg.*(p->field);
  return store(c);
}

bool configSaveKey() {
  FlashAccess fa;
  Config c = persisted();
  memcpy(c.key, cfg.key, sizeof(c.key));
  c.key_set = cfg.key_set;
  return store(c);
}

bool configFactoryReset() {
  configDefaults(cfg);
  if (!extFlashPresent()) return store(cfg);
  // Erase both records, so the old key doesn't linger in the older one.
  FlashAccess fa;
  bool ok = true;
  for (int8_t s = 0; s < REC_SECTORS; s++) ok &= extFlashEraseSector(s * EXTFLASH_SECTOR);
  spiSector = -1;
  return ok;
}

const ParamDef *paramByName(const char *name) {
  for (size_t i = 0; i < PARAM_COUNT; i++)
    if (strcmp(PARAMS[i].name, name) == 0) return &PARAMS[i];
  return nullptr;
}

const ParamDef *paramById(uint8_t id) {
  for (size_t i = 0; i < PARAM_COUNT; i++)
    if (PARAMS[i].id == id) return &PARAMS[i];
  return nullptr;
}

static bool paramValid(const ParamDef *p, int32_t value) {
  if (!p || value < p->minV || value > p->maxV) return false;
  if (p->field == &Config::bw_hz && value != 125000 && value != 250000 && value != 500000) return false;
  // No heartbeat_s vs link_timeout_s check here: only the gate uses heartbeat_s and only the house
  // link_timeout_s, and the house stretches its timeout to the gate's heartbeat (see houseLinkTimeoutMs).
  return true;
}

bool paramSet(const ParamDef *p, int32_t value) {
  if (!paramValid(p, value)) return false;
  cfg.*(p->field) = value;
  return true;
}

uint8_t myNodeId() {
  return activeRole == ROLE_GATE ? NODE_GATE : NODE_HOUSE;
}

uint8_t peerNodeId() {
  return activeRole == ROLE_GATE ? NODE_HOUSE : NODE_GATE;
}
