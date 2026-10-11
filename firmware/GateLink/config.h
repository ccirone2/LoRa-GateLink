#pragma once
#include <Arduino.h>

#define FW_VERSION "0.13.8"
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
// The saves return false if the write didn't verify. Settings in the record that this firmware doesn't know are
// written back as they were.
bool configSave();
// Persist one param, or the key, on top of what is already saved, leaving other unsaved edits unsaved. These also
// return false, writing nothing, if what's saved can't be read back intact (see persisted()).
bool configSaveParam(const ParamDef *p);
bool configSaveKey();
bool configFactoryReset();  // defaults in RAM; saved config and key erased
// Counts this boot in SPI flash and returns the count (0 if the chip doesn't answer). Never repeats: it seeds the
// session id (link.cpp). Kept apart from the config record, so config.reset doesn't restart it.
uint32_t configCountBoot();
// The link key's id: the first 4 bytes of HMAC-SHA256(key, "GateLink key id v1") as 8 lowercase hex digits (out
// holds 9 chars). It tells keys apart (do both boards hold the same one? which backup is it?) without revealing
// the key. docs/key-management.md; the web console and tools/gatelink.py compute it the same way.
void configKeyId(const uint8_t key[16], char out[9]);
// A key key.set refuses as guessable: all 16 bytes equal, bytes counting up or down by one (mod 256), or 8 or
// fewer distinct byte values. A random key is weak about once in 10^10 draws.
bool configKeyWeak(const uint8_t key[16]);
const ParamDef *paramByName(const char *name);
const ParamDef *paramById(uint8_t id);
bool paramValid(const ParamDef *p, int32_t value);  // range check, as paramSet does
bool paramSet(const ParamDef *p, int32_t value);  // range-checked
uint8_t myNodeId();
uint8_t peerNodeId();
