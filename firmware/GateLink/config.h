#pragma once
#include <Arduino.h>

#define FW_VERSION "0.12.3"
#define FW_MARKER_PREFIX "GATELINK_FW="
// FW_MARKER_PREFIX FW_VERSION: the web console looks for it in a .bin to check the file is GateLink and
// read its version. The version is reported from it (fwVersion()) so the linker keeps it in the image.
extern const char FW_MARKER[];
inline const char *fwVersion() { return FW_MARKER + sizeof(FW_MARKER_PREFIX) - 1; }

enum Role : uint8_t { ROLE_UNSET = 0, ROLE_HOUSE = 1, ROLE_GATE = 2 };

// Node addresses are fixed by role.
#define NODE_HOUSE 1
#define NODE_GATE 2

struct Config {
  uint32_t magic;
  uint16_t version;
  // General
  int32_t role;
  int32_t net_id;
  // Radio
  int32_t freq_hz;
  int32_t sf;
  int32_t bw_hz;
  int32_t cr;
  int32_t tx_power;
  int32_t sync_word;
  // Link
  int32_t retries;
  int32_t heartbeat_s;
  int32_t link_timeout_s;
  int32_t cmd_ttl_s;
  // IO
  int32_t debounce_ms;
  int32_t in1_invert;
  int32_t in2_invert;
  int32_t in3_invert;
  int32_t in4_invert;
  int32_t power_sense;  // gate: IN3 = AC power present
  // Gate node
  int32_t pulse_ms;
  int32_t travel_timeout_s;
  // House node
  int32_t ctrl_sync;
  int32_t sync_window_ms;
  int32_t resync_ms;
  int32_t mismatch_timeout_s;
  int32_t sensor_invert;
  int32_t linkloss_open;
  int32_t ctrl_power_sense;  // house: IN2 = controller supply present
  int32_t ctrl_confirm_ms;
  int32_t ctrl_settle_ms;
  int32_t ctrl_power_pmic;  // house: the board's supply (charger power good) counts as controller power too
  // Board
  int32_t uart_console;  // also run the console on Serial1 (pins 13 RX / 14 TX), for bench power tests
  // Security
  uint8_t key[16];
  int32_t key_set;
  uint32_t crc;
};

// Parameter table flags
#define P_RADIO 0x01   // change requires radio re-init
#define P_REMOTE 0x02  // may be written over LoRa (never radio params)
#define P_REBOOT 0x04  // change takes effect after reboot

struct ParamDef {
  uint8_t id;
  const char *name;
  int32_t Config::*field;
  int32_t minV;
  int32_t maxV;
  uint8_t flags;
};

extern Config cfg;
// Role latched at boot; a changed cfg.role only takes effect after a reboot.
extern int32_t activeRole;
extern const ParamDef PARAMS[];
extern const size_t PARAM_COUNT;

// Where the running config came from at boot (log event `cfg`, a).
enum CfgSource : uint8_t { CFG_DEFAULTS = 0, CFG_FROM_SPI = 1, CFG_FROM_INTERNAL = 2 };

void configDefaults(Config &c);
bool configLoad();  // returns false if nothing valid was saved (defaults loaded)
uint8_t configSource();
int32_t configDropped();        // saved settings this firmware didn't accept (unknown id or out of range)
const char *configStoreName();  // "spi" (survives uploads) or "internal" (SPI flash missing; erased by uploads)
// The saves return false if the write didn't verify.
bool configSave();
// Persist one param, or the key, on top of what is already saved, leaving other unsaved edits unsaved.
bool configSaveParam(const ParamDef *p);
bool configSaveKey();
bool configFactoryReset();  // defaults in RAM; saved config and key erased
const ParamDef *paramByName(const char *name);
const ParamDef *paramById(uint8_t id);
bool paramSet(const ParamDef *p, int32_t value);  // range-checked
uint8_t myNodeId();
uint8_t peerNodeId();
