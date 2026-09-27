#pragma once
#include <Arduino.h>

enum LogCode : uint8_t {
  EV_BOOT = 0,       // a = reset cause (PM RCAUSE bits), b = role
  EV_RADIO_FAIL,     // a = 0 init failed, 1 TX fault (radio re-initialised); b = fault count
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
  EV_CTRL,           // a = level
  EV_SYNC,           // a = level (edge caused by our K1 sync)
  EV_RESYNC,         // a = target level
  EV_CFG_REMOTE,     // a = param id, b = value
  EV_INPUT,          // a = spare input number (3/4), b = level
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
