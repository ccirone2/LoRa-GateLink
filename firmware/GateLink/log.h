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
                     //     session b answered), 2 dropped (new session b verified: the peer restarted), 3 waiting
                     //     for the gate's clock (house: no STATUS yet from gate session b, at most cmd_ttl_s)
  EV_HEALTH,         // fault output D5 changed: a = 1 HIGH (healthy), 0 LOW (needs attention), -1 released (fault_out
                     //   off: an input again); b = the problems then (Problem bits, health.h)
  EV_HANG,           // at boot, after a watchdog reset its early warning saw coming: a = the program counter the
                     //   warning interrupted (4 s into the stall), b = the loop's stage then (status hang_stage)
  EV_CMD_LATE,       // a = cmd id, b = ms past its deadline (negative: a deadline too far ahead); refused, no pulse
  EV_MISSED_MOVE,    // a = times the gate left closed unheard (house: the sensor opens for a moment to show it)
  EV_COUNT
};

// 16 bytes, also as kept in RAM across resets and in the flash journal (evlog.cpp).
struct LogEntry {
  uint32_t t;     // millis() in the boot it was logged in
  uint8_t code;
  uint8_t rep;    // times it came again right after, coalesced into this entry (floods: logEvent)
  uint16_t boot;  // that boot's count (appBootCount, low 16 bits; 0 without one)
  int32_t a;
  int32_t b;
};

#define LOG_SIZE 64
// The ring in RAM every reset but power-on keeps (boardKeep, after the reset memo): the newest LOG_SIZE events,
// those not written to the flash journal yet among them, so a crash or a reboot loses none of them.
#define LOG_KEEP_WORDS (4 + LOG_SIZE * 4)

// At boot, before the first logEvent: takes the ring the boot before left (checked), else starts it empty.
void logBegin(uint32_t bootCount);
void logEvent(LogCode code, int32_t a = 0, int32_t b = 0);
const char *logCodeName(uint8_t code);
// The ring, oldest first: i < logCount().
size_t logCount();
const LogEntry &logAt(size_t i);
// Writing the ring's unsaved entries to the flash journal (evlog.h), from the loop, never while a relay pulses: due
// once ELOG_PER_REC are waiting, or the oldest has waited LOG_SAVE_WAIT_MS, at most once per LOG_SAVE_GAP_MS (a flood
// can't keep the radio off the air). One record per call.
#define LOG_SAVE_WAIT_MS 600000UL
#define LOG_SAVE_GAP_MS 60000UL
bool logSaveDue(uint32_t now);
void logSave(uint32_t now);
// For log.get: ring entries not in the journal yet, and this boot's journal writes and failed ones (a failing chip, or
// a boot that seems to write nothing: one left more than a record's worth writes the rest a minute later).
uint16_t logUnsaved();
uint16_t logSaves();
uint16_t logSaveFails();
