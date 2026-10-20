// The event log's journal in the SPI flash (evlog.h).
#include "evlog.h"
#include "config.h"
#include "extflash.h"
#include "flashlog.h"

#define KIND_EVENTS 1
static_assert(FLOG_HDR + ELOG_PER_REC * ELOG_ENTRY + 4 <= ELOG_SLOT && ELOG_SLOT <= FLOG_MAX_REC
                && EXTFLASH_PAGE % ELOG_SLOT == 0,
              "a record fits its slot, and slots tile a page");
static_assert(sizeof(LogEntry) == ELOG_ENTRY, "an entry as it is kept in RAM");

static FlashLog lg = { ELOG_SECTOR0, ELOG_SECTORS, ELOG_SLOT, ELOG_PER_REC * ELOG_ENTRY, 0x4C45 /* "EL" */, false, 0, 0 };

// An entry: t(4) code(1) rep(1) boot(2) a(4) b(4), little-endian.
static void putEntry(uint8_t *p, const LogEntry &e) {
  flogPut32(p, e.t);
  p[4] = e.code;
  p[5] = e.rep;
  flogPut16(p + 6, e.boot);
  flogPut32(p + 8, (uint32_t)e.a);
  flogPut32(p + 12, (uint32_t)e.b);
}

static void getEntry(const uint8_t *p, LogEntry &e) {
  e.t = flogGet32(p);
  e.code = p[4];
  e.rep = p[5];
  e.boot = flogGet16(p + 6);
  e.a = (int32_t)flogGet32(p + 8);
  e.b = (int32_t)flogGet32(p + 12);
}

void evLogBegin() {
  lg.on = false;
  if (!extFlashPresent()) return;
  FlashAccess fa;
  uint8_t buf[FLOG_MAX_REC], top[FLOG_MAX_REC];
  int32_t newest;
  flogOpen(lg, newest, top, buf);
}

bool evLogOn() {
  return lg.on;
}

bool evLogAppend(const LogEntry *e, uint8_t n) {
  if (!n || n > ELOG_PER_REC) return false;
  uint8_t data[ELOG_PER_REC * ELOG_ENTRY];
  memset(data, 0xFF, sizeof(data));
  for (uint8_t i = 0; i < n; i++) putEntry(data + i * ELOG_ENTRY, e[i]);
  return flogWrite(lg, ELOG_FMT, KIND_EVENTS, 0, n, data);
}

// Back from the newest slot: records written before one another, so their lseqs fall; a torn or failed write is passed
// over, and an erased slot (never written, or the sector the next record will erase) or a rise in lseq (the oldest,
// already overwritten in part) ends the log.
uint8_t evLogGet(uint16_t k, LogEntry *out) {
  if (!lg.on || !k) return 0;
  FlashAccess fa;
  uint16_t slots = flogSlots(lg);
  uint8_t buf[FLOG_MAX_REC];
  uint32_t lastSeq = lg.nextSeq;
  for (uint16_t j = 1; j < slots; j++) {
    FlogRead r = flogRead(lg, (lg.head + slots - j) % slots, buf);
    if (r == FL_INVALID) continue;
    if (r != FL_VALID || flogGet32(buf + 4) >= lastSeq) return 0;
    lastSeq = flogGet32(buf + 4);
    uint16_t n = flogGet16(buf + 12);
    if (buf[2] != ELOG_FMT || buf[3] != KIND_EVENTS || !n || n > ELOG_PER_REC) continue;
    if (--k) continue;
    for (uint16_t i = 0; i < n; i++) getEntry(buf + FLOG_HDR + i * ELOG_ENTRY, out[i]);
    return n;
  }
  return 0;
}
