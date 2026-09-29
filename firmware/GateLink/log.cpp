#include "log.h"
#include "console.h"

static LogEntry ring[LOG_SIZE];
static size_t head = 0;
static size_t count = 0;

static const char *const NAMES[EV_COUNT] = {
  "boot", "radio_fail", "link_up", "link_down", "session", "mac_fail", "replay",
  "tx_giveup", "cmd_sent", "cmd_suppressed", "cmd_dropped", "cmd_rx", "cmd_dup",
  "pulse", "gate_state", "travel_timeout", "ctrl", "sync", "resync", "cfg_remote", "input", "cmd_refused", "ctrl_power",
};

void logEvent(LogCode code, int32_t a, int32_t b) {
  LogEntry &e = ring[head];
  e.t = millis();
  e.code = code;
  e.a = a;
  e.b = b;
  head = (head + 1) % LOG_SIZE;
  if (count < LOG_SIZE) count++;
  consoleEmitLog(e);
}

const char *logCodeName(uint8_t code) {
  return code < EV_COUNT ? NAMES[code] : "?";
}

size_t logCopy(LogEntry *out, size_t max) {
  size_t n = count < max ? count : max;
  size_t start = (head + LOG_SIZE - count) % LOG_SIZE;
  start = (start + (count - n)) % LOG_SIZE;
  for (size_t i = 0; i < n; i++) out[i] = ring[(start + i) % LOG_SIZE];
  return n;
}
