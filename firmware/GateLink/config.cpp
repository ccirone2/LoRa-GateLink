#include "config.h"
#include <FlashStorage.h>

// Note: program flash is erased when new firmware is uploaded, so config (and key)
// must be re-applied after a firmware update. The web UI can export/import config.

#define CFG_MAGIC 0x47544C4Bu  // "GTLK"
#define CFG_VERSION 2

Config cfg;
int32_t activeRole = ROLE_UNSET;
FlashStorage(cfgStore, Config);

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
  { 12, "cmd_ttl_s", &Config::cmd_ttl_s, 2, 120, P_REMOTE },
  { 13, "debounce_ms", &Config::debounce_ms, 10, 1000, P_REMOTE },
  { 14, "in1_invert", &Config::in1_invert, 0, 1, P_REMOTE },
  { 15, "in2_invert", &Config::in2_invert, 0, 1, P_REMOTE },
  { 16, "pulse_ms", &Config::pulse_ms, 100, 5000, P_REMOTE },
  { 17, "travel_timeout_s", &Config::travel_timeout_s, 5, 300, P_REMOTE },
  { 18, "ctrl_sync", &Config::ctrl_sync, 0, 1, 0 },
  { 19, "sync_window_ms", &Config::sync_window_ms, 500, 10000, 0 },
  { 20, "resync_ms", &Config::resync_ms, 200, 5000, 0 },
  { 21, "mismatch_timeout_s", &Config::mismatch_timeout_s, 10, 600, 0 },
  { 22, "sensor_invert", &Config::sensor_invert, 0, 1, 0 },
  { 23, "linkloss_open", &Config::linkloss_open, 0, 1, 0 },
  { 24, "in3_invert", &Config::in3_invert, 0, 1, P_REMOTE },
  { 25, "in4_invert", &Config::in4_invert, 0, 1, P_REMOTE },
};
const size_t PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

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
  c.pulse_ms = 500;
  c.travel_timeout_s = 60;
  c.ctrl_sync = 1;
  c.sync_window_ms = 3000;
  c.resync_ms = 1000;
  c.mismatch_timeout_s = 75;
  c.sensor_invert = 0;
  c.linkloss_open = 1;
  c.key_set = 0;
}

bool configLoad() {
  Config c;
  cfgStore.read(&c);
  if (c.magic != CFG_MAGIC || c.version != CFG_VERSION || c.crc != configCrc(c)) {
    configDefaults(cfg);
    return false;
  }
  cfg = c;
  return true;
}

void configSave() {
  cfg.magic = CFG_MAGIC;
  cfg.version = CFG_VERSION;
  cfg.crc = configCrc(cfg);
  cfgStore.write(cfg);
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

bool paramSet(const ParamDef *p, int32_t value) {
  if (!p || value < p->minV || value > p->maxV) return false;
  if (p->field == &Config::bw_hz && value != 125000 && value != 250000 && value != 500000) return false;
  int32_t old = cfg.*(p->field);
  cfg.*(p->field) = value;
  // The house declares the link down after link_timeout_s without frames; the gate's
  // heartbeat must comfortably fit inside it.
  if (cfg.heartbeat_s * 2 > cfg.link_timeout_s) {
    cfg.*(p->field) = old;
    return false;
  }
  return true;
}

uint8_t myNodeId() {
  return activeRole == ROLE_GATE ? NODE_GATE : NODE_HOUSE;
}

uint8_t peerNodeId() {
  return activeRole == ROLE_GATE ? NODE_HOUSE : NODE_GATE;
}
