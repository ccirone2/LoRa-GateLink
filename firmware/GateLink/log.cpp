#include "log.h"
#include "board.h"
#include "console.h"
#include "crc32.h"
#include "evlog.h"

// The ring, in the RAM resets other than power-on keep (log.h). Its header is checked by a CRC; after a power-on (or
// a bootloader that touched it) it reads as no ring.
struct LogKeep {
  uint32_t magic;
  uint16_t head;     // where the next entry goes
  uint16_t count;    // entries held
  uint16_t unsaved;  // the newest this many aren't in the flash journal yet
  uint16_t pad;
  uint32_t check;    // CRC-32 of the above
  LogEntry ring[LOG_SIZE];
};
static_assert(sizeof(LogKeep) == 4 * LOG_KEEP_WORDS, "the kept ring is LOG_KEEP_WORDS");
static_assert(4 + LOG_KEEP_WORDS == BOARD_KEEP_WORDS, "the reset memo (4 words), then the ring, fill the kept RAM");
#define KEEP_MAGIC 0x6C6F6721UL

static LogKeep *k;
static uint16_t boot;          // this boot's count, low 16 bits
static uint32_t unsavedSince;  // when the oldest unsaved entry was logged (as far as this boot knows)
static uint32_t savedAt;       // the last write to the journal

static const char *const NAMES[] = {
  "boot", "radio_fail", "link_up", "link_down", "session", "mac_fail", "replay",
  "tx_giveup", "cmd_sent", "cmd_suppressed", "cmd_dropped", "cmd_rx", "cmd_dup",
  "pulse", "gate_state", "travel_timeout", "ctrl", "sync", "resync", "cfg_remote", "input", "cmd_refused", "ctrl_power", "lbt_forced", "cfg", "supply",
  "cmd_hold", "health", "hang", "cmd_late", "missed_move",
};
static_assert(sizeof(NAMES) / sizeof(NAMES[0]) == EV_COUNT, "one name per LogCode");

static uint32_t keepCheck() {
  return crc32(k, offsetof(LogKeep, check));
}

static void seal() {
  k->check = keepCheck();
}

void logBegin(uint32_t bootCount) {
  k = (LogKeep *)(boardKeep() + 4);
  boot = (uint16_t)bootCount;
  uint32_t now = millis();
  savedAt = now - LOG_SAVE_GAP_MS;
  unsavedSince = now - LOG_SAVE_WAIT_MS;  // what the boot before left unsaved goes out at once
  bool kept = k->magic == KEEP_MAGIC && k->check == keepCheck() && k->head < LOG_SIZE && k->count <= LOG_SIZE
              && k->unsaved <= k->count;
  if (!kept) {
    k->magic = KEEP_MAGIC;
    k->head = k->count = k->unsaved = k->pad = 0;
  }
  seal();
}

// Repeats that can come in floods (forged or replayed frames, a busy channel, a chattering input) go into the newest
// entry while it isn't in the journal yet, counted in rep, so a flood can't push everything else out of the ring.
static bool coalesces(const LogEntry &last, LogCode code, int32_t a) {
  if (last.code != code || last.rep == 255) return false;
  return code == EV_MAC_FAIL || code == EV_REPLAY || code == EV_LBT_FORCED || (code == EV_INPUT && last.a == a);
}

void logEvent(LogCode code, int32_t a, int32_t b) {
  LogEntry e = { millis(), code, 0, boot, a, b };
  if (!k) {  // before logBegin: the console only
    consoleEmitLog(e);
    return;
  }
  LogEntry &last = k->ring[(k->head + LOG_SIZE - 1) % LOG_SIZE];
  if (k->unsaved && coalesces(last, code, a)) {
    last.rep++;
  } else {
    if (!k->unsaved) unsavedSince = e.t;
    k->ring[k->head] = e;
    k->head = (k->head + 1) % LOG_SIZE;
    if (k->count < LOG_SIZE) k->count++;
    if (k->unsaved < k->count) k->unsaved++;
    seal();
  }
  consoleEmitLog(e);
}

const char *logCodeName(uint8_t code) {
  return code < EV_COUNT ? NAMES[code] : "?";
}

size_t logCount() {
  return k ? k->count : 0;
}

const LogEntry &logAt(size_t i) {
  return k->ring[(k->head + LOG_SIZE - k->count + i) % LOG_SIZE];
}

bool logSaveDue(uint32_t now) {
  if (!k || !k->unsaved || !evLogOn()) return false;
  // Ages, compared unsigned: a stamp weeks old (a quiet site logs nothing) reads as old, where a signed comparison
  // would read it as in the future past 2^31 ms and never write again. Past 2^32 ms one may read as recent: a write a
  // minute (or ten) late, no more. One stamped by a handler after the loop's now reads as very old: a write early.
  return now - savedAt >= LOG_SAVE_GAP_MS && (k->unsaved >= ELOG_PER_REC || now - unsavedSince >= LOG_SAVE_WAIT_MS);
}

// Writes the oldest unsaved entries, up to a record's worth. A failed write isn't retried (as histSave): those entries
// stay in the ring only. Any left waiting are older than unsavedSince says already: they follow a gap later.
void logSave(uint32_t now) {
  uint8_t n = k->unsaved < ELOG_PER_REC ? k->unsaved : ELOG_PER_REC;
  LogEntry batch[ELOG_PER_REC];
  for (uint8_t i = 0; i < n; i++) batch[i] = logAt(k->count - k->unsaved + i);
  evLogAppend(batch, n);
  k->unsaved -= n;
  seal();
  savedAt = now;
}
