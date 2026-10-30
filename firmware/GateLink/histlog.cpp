#include "histlog.h"
#include "config.h"
#include "extflash.h"
#include "flashlog.h"

#define KIND_BUCKET 1
#define KIND_CLEARED 2
static_assert(FLOG_HDR + HLOG_DATA + 4 <= HLOG_SLOT && EXTFLASH_PAGE % HLOG_SLOT == 0,
              "a record fits its slot, and slots tile a page");

static FlashLog lg = { HLOG_SECTOR0, HLOG_SECTORS, HLOG_SLOT, HLOG_DATA, 0x4C48 /* "HL" */, false, 0, 0 };

bool histLogBegin(uint32_t max, HistLogTake take, uint16_t &period, uint32_t &next) {
  lg.on = false;
  if (!extFlashPresent()) return false;
  FlashAccess fa;
  uint8_t buf[FLOG_MAX_REC], top[FLOG_MAX_REC];
  int32_t newest;
  if (!flogOpen(lg, newest, top, buf) || newest < 0) return false;
  uint16_t slots = flogSlots(lg);
  uint16_t p = flogGet16(top + 12);
  uint32_t idx = flogGet32(top + 8);
  if (top[3] == KIND_CLEARED) {
    period = p;
    next = 0;
    return true;
  }
  // UINT32_MAX - 1 too: the next bucket would be UINT32_MAX, a number erased flash also reads as (found by fuzz_config;
  // a corrupt record, as no board counts that far).
  if (top[3] != KIND_BUCKET || top[2] != HLOG_FMT || idx >= UINT32_MAX - 1) return false;
  period = p;
  next = idx + 1;
  // The run, back from the newest: each older record must be the bucket before, written before it. A torn record is
  // passed over (the head skipped it, so the numbers go on across it); an erased slot, a CLEARED record or any other
  // record ends it.
  take(idx, top + FLOG_HDR);
  uint32_t n = 1, lastSeq = flogGet32(top + 4);
  for (uint16_t k = 1; k < slots && n < max && idx > 0; k++) {
    FlogRead r = flogRead(lg, (newest + slots - k) % slots, buf);
    if (r == FL_INVALID) continue;
    if (r != FL_VALID || flogGet32(buf + 4) >= lastSeq || buf[3] != KIND_BUCKET || buf[2] != HLOG_FMT
        || flogGet16(buf + 12) != p || flogGet32(buf + 8) != idx - 1)
      break;
    take(--idx, buf + FLOG_HDR);
    n++;
    lastSeq = flogGet32(buf + 4);
  }
  return true;
}

bool histLogOn() {
  return lg.on;
}

bool histLogAppend(uint32_t idx, uint16_t period, const uint8_t *data) {
  return flogWrite(lg, HLOG_FMT, KIND_BUCKET, idx, period, data);
}

bool histLogClear(uint16_t period) {
  if (!extFlashPresent()) return true;
  return flogWrite(lg, HLOG_FMT, KIND_CLEARED, 0, period, nullptr);
}
