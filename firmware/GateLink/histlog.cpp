#include "histlog.h"
#include "config.h"
#include "crc32.h"
#include "extflash.h"

#define MAGIC 0x4C48u  // "HL"
#define KIND_BUCKET 1
#define KIND_CLEARED 2
#define HDR 16
#define REC_LEN (HDR + HLOG_DATA + 4)
#define PER_SECTOR (EXTFLASH_SECTOR / HLOG_SLOT)
#define SLOTS ((uint16_t)(HLOG_SECTORS * PER_SECTOR))
static_assert(REC_LEN <= HLOG_SLOT && EXTFLASH_PAGE % HLOG_SLOT == 0, "a record fits its slot, and slots tile a page");

enum Read : uint8_t { R_ERASED, R_GARBLED, R_INVALID, R_VALID };

static bool on = false;   // records may be written
static uint16_t head;     // the slot the next record goes to
static uint32_t nextSeq;  // and its lseq

static void put16(uint8_t *p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}

static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

static uint16_t get16(const uint8_t *p) {
  return p[0] | p[1] << 8;
}

static uint32_t get32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t slotAddr(uint16_t s) {
  return HLOG_SECTOR0 * EXTFLASH_SECTOR + (uint32_t)s * HLOG_SLOT;
}

static bool all(const uint8_t *b, size_t n, uint8_t v) {
  for (size_t i = 0; i < n; i++)
    if (b[i] != v) return false;
  return true;
}

// Reads the first n bytes of slot s into buf; false if the read was garbled on the bus (extflash.h): it started with 4
// zero bytes twice and the chip's id didn't read back either. No record starts so (the magic), and a write cut short
// leaves bits at 1, never at 0 where it meant 1; but an erase cut short may leave a sector at zeros, and so may another
// program's data: with the chip answering, the zeros are what the flash holds, a slot like any that isn't a record.
// On a garbled read the caller gives up (as the boot counter does: it could hide the newest record).
static bool readSlot(uint16_t s, uint8_t *buf, size_t n) {
  for (int tries = 0; tries < 2; tries++) {
    extFlashRead(slotAddr(s), buf, n);
    if (!all(buf, 4, 0)) return true;
  }
  return extFlashAnswers();
}

// Reads slot s's record into buf.
static Read readRec(uint16_t s, uint8_t *buf) {
  if (!readSlot(s, buf, REC_LEN)) return R_GARBLED;
  if (all(buf, REC_LEN, 0xFF)) return R_ERASED;
  if (get16(buf) != MAGIC || get32(buf + REC_LEN - 4) != crc32(buf, REC_LEN - 4)) return R_INVALID;
  return R_VALID;
}

// The newest valid record's slot (-1: none) and lseq; false if a read was garbled. Every slot's header (magic to
// lseq, 8 bytes) is read, which keeps the boot quick; only a whole record shows whether it's valid, so the few with the
// largest lseqs are read in full, largest first, and the first valid one is the newest (a torn or failed write is the
// most a power cut leaves above it). If those are all invalid, every slot is read in full.
#define CANDIDATES 4
static bool findNewest(int32_t &newest, uint32_t &seq, uint8_t *buf) {
  uint16_t cand[CANDIDATES];
  uint32_t candSeq[CANDIDATES];
  uint8_t nc = 0;
  for (uint16_t s = 0; s < SLOTS; s++) {
    if (!readSlot(s, buf, 8)) return false;
    if (get16(buf) != MAGIC) continue;
    uint32_t q = get32(buf + 4);
    uint8_t i = nc < CANDIDATES ? nc++ : CANDIDATES;  // where it goes, sorted largest first
    while (i > 0 && candSeq[i - 1] < q) {
      if (i < CANDIDATES) {
        cand[i] = cand[i - 1];
        candSeq[i] = candSeq[i - 1];
      }
      i--;
    }
    if (i < CANDIDATES) {
      cand[i] = s;
      candSeq[i] = q;
    }
  }
  newest = -1;
  for (uint8_t i = 0; i < nc; i++) {
    Read r = readRec(cand[i], buf);
    if (r == R_GARBLED) return false;
    if (r == R_VALID) {
      newest = cand[i];
      seq = candSeq[i];
      return true;
    }
  }
  if (nc < CANDIDATES) return true;  // every slot with the magic was read: none is valid
  for (uint16_t s = 0; s < SLOTS; s++) {
    Read r = readRec(s, buf);
    if (r == R_GARBLED) return false;
    if (r == R_VALID && (newest < 0 || get32(buf + 4) > seq)) {
      newest = s;
      seq = get32(buf + 4);
    }
  }
  return true;
}

bool histLogBegin(uint32_t max, HistLogTake take, uint16_t &period, uint32_t &next) {
  on = false;
  if (!extFlashPresent()) return false;
  FlashAccess fa;
  uint8_t buf[REC_LEN], top[REC_LEN];
  int32_t newest;
  uint32_t seq = 0;
  if (!findNewest(newest, seq, buf) || (newest >= 0 && readRec(newest, top) != R_VALID)) return false;
  // Slots after the newest record that aren't erased (one a power cut tore, or a write that failed) are passed over:
  // the next record goes after them, up to the next sector, which it erases first.
  head = newest < 0 ? 0 : (newest + 1) % SLOTS;
  while (head % PER_SECTOR) {
    Read r = readRec(head, buf);
    if (r == R_GARBLED) return false;
    if (r == R_ERASED) break;
    head = (head + 1) % SLOTS;
  }
  nextSeq = newest < 0 ? 1 : seq + 1;  // 0: lseq ran out (only a record from elsewhere can read that high)
  on = nextSeq != 0;
  if (newest < 0) return false;
  uint16_t p = get16(top + 12);
  uint32_t idx = get32(top + 8);
  if (top[3] == KIND_CLEARED) {
    period = p;
    next = 0;
    return true;
  }
  if (top[3] != KIND_BUCKET || top[2] != HLOG_FMT || idx == UINT32_MAX) return false;
  period = p;
  next = idx + 1;
  // The run, back from the newest: each older record must be the bucket before, written before it. A torn record is
  // passed over (the head skipped it, so the numbers go on across it); an erased slot, a CLEARED record or any other
  // record ends it.
  take(idx, top + HDR);
  uint32_t n = 1, lastSeq = seq;
  for (uint16_t k = 1; k < SLOTS && n < max && idx > 0; k++) {
    Read r = readRec((newest + SLOTS - k) % SLOTS, buf);
    if (r == R_INVALID) continue;
    if (r != R_VALID || get32(buf + 4) >= lastSeq || buf[3] != KIND_BUCKET || buf[2] != HLOG_FMT
        || get16(buf + 12) != p || get32(buf + 8) != idx - 1)
      break;
    take(--idx, buf + HDR);
    n++;
    lastSeq = get32(buf + 4);
  }
  return true;
}

bool histLogOn() {
  return on;
}

static bool writeRec(uint8_t kind, uint32_t idx, uint16_t period, const uint8_t *data) {
  if (!on) return false;
  uint8_t buf[REC_LEN], check[REC_LEN];
  put16(buf, MAGIC);
  buf[2] = HLOG_FMT;
  buf[3] = kind;
  put32(buf + 4, nextSeq);
  put32(buf + 8, idx);
  put16(buf + 12, period);
  put16(buf + 14, 0);
  if (data) memcpy(buf + HDR, data, HLOG_DATA);
  else memset(buf + HDR, 0, HLOG_DATA);
  put32(buf + REC_LEN - 4, crc32(buf, REC_LEN - 4));
  FlashAccess fa;
  uint32_t addr = slotAddr(head);
  // Entering a sector: erase it first. It holds the oldest records (the log wraps here).
  if (head % PER_SECTOR == 0 && !extFlashEraseSector(addr)) return false;
  bool ok = extFlashProgram(addr, buf, REC_LEN);
  if (ok) {
    extFlashRead(addr, check, REC_LEN);
    ok = !memcmp(buf, check, REC_LEN);
  }
  head = (head + 1) % SLOTS;
  on = ++nextSeq != 0;
  return ok;
}

bool histLogAppend(uint32_t idx, uint16_t period, const uint8_t *data) {
  return writeRec(KIND_BUCKET, idx, period, data);
}

bool histLogClear(uint16_t period) {
  if (!extFlashPresent()) return true;
  return writeRec(KIND_CLEARED, 0, period, nullptr);
}
