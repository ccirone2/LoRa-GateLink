#pragma once
#include <Arduino.h>

enum LogCode : uint8_t {
  EV_BOOT = 0,       // a = reset cause (PM RCAUSE bits), b = role
  EV_RADIO_FAIL,     // a = 0 init failed (once until a retry succeeds), 1 TX fault, 2 reset seen in RX (both
                     //   re-initialised), 3 init retry succeeded; b = fault count
  EV_LINK_UP,
  EV_LINK_DOWN,
  EV_SESSION,        // a = peer session accepted
  EV_MAC_FAIL,
  EV_REPLAY,         // a = seq, b = last seq
  EV_TX_GIVEUP,      // a = msg type, b = seq
  EV_CMD_SENT,       // a = action, b = cmd id
  EV_CMD_SUPPRESSED, // a = action, b = gate state
  EV_CMD_DROPPED,    // a = action (link down / ttl)
  EV_CMD_RX,         // a = action, b = cmd id
  EV_CMD_DUP,        // a = cmd id
  EV_PULSE,          // a = relay, b = ms
  EV_GATE_STATE,     // a = state, b = cause
  EV_TRAVEL_TIMEOUT, // a = target state
  EV_CTRL,           // a = level, b = 1 if ignored (controller unpowered)
  EV_SYNC,           // a = level (edge caused by our K1 sync)
  EV_RESYNC,         // a = target level
  EV_CFG_REMOTE,     // a = param id, b = value
  EV_INPUT,          // a = spare input number (house 2/3/4, gate 3/4), b = level
  EV_CMD_REFUSED,    // a = action, b = cmd id (opener unpowered)
  EV_CTRL_POWER,     // a = controller power level, b = action discarded by the power loss (0 = none)
  EV_LBT_FORCED,     // a = msg type, b = ms the channel stayed busy (sent anyway)
  EV_CFG,            // at boot: a = config source (CfgSource: 0 defaults, 1 SPI flash, 2 program flash),
                     //   b = saved settings dropped (unknown or out of range)
  EV_SUPPLY,         // a = board supply (VIN) power good, -1 = charger not answering (at boot only), b = REG08
  EV_CMD_HOLD,       // a = 1 command held (HELLO from an unverified session b), 0 sent after all (verified
                     //     session b answered), 2 dropped (new session b verified: the peer restarted)
  EV_COUNT
};

struct LogEntry {
  uint32_t t;
  uint8_t code;
  int32_t a;
  int32_t b;
};

#define LOG_SIZE 64

void logEvent(LogCode code, int32_t a = 0, int32_t b = 0);
const char *logCodeName(uint8_t code);
// Iterate oldest->newest. Returns number of entries copied.
size_t logCopy(LogEntry *out, size_t max);
