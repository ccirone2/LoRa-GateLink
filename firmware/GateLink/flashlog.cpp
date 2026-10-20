// Logs of records in the SPI flash (flashlog.h).
#include "flashlog.h"
#include "config.h"
#include "crc32.h"
#include "extflash.h"

uint16_t flogSlots(const FlashLog &l) {
  return l.sectors * (EXTFLASH_SECTOR / l.slot);
}

static uint16_t perSector(const FlashLog &l) {
  return EXTFLASH_SECTOR / l.slot;
}

static uint32_t slotAddr(const FlashLog &l, uint16_t s) {
  return l.sector0 * EXTFLASH_SECTOR + (uint32_t)s * l.slot;
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
static bool readSlot(const FlashLog &l, uint16_t s, uint8_t *buf, size_t n) {
  for (int tries = 0; tries < 2; tries++) {
    extFlashRead(slotAddr(l, s), buf, n);
    if (!all(buf, 4, 0)) return true;
  }
  return extFlashAnswers();
}

bool flogHead(const FlashLog &l, uint16_t s, uint8_t *buf) {
  return readSlot(l, s, buf, 8);
}

FlogRead flogRead(const FlashLog &l, uint16_t s, uint8_t *buf) {
  uint16_t len = flogRecLen(l);
  if (!readSlot(l, s, buf, len)) return FL_GARBLED;
  if (all(buf, len, 0xFF)) return FL_ERASED;
  if (flogGet16(buf) != l.magic || flogGet32(buf + len - 4) != crc32(buf, len - 4)) return FL_INVALID;
  return FL_VALID;
}

// The newest valid record's slot (-1: none) and lseq; false if a read was garbled. Every slot's header (magic to
// lseq, 8 bytes) is read, which keeps the boot quick; only a whole record shows whether it's valid, so the few with the
// largest lseqs are read in full, largest first, and the first valid one is the newest (a torn or failed write is the
// most a power cut leaves above it). If those are all invalid, every slot is read in full.
#define CANDIDATES 4
static bool findNewest(const FlashLog &l, int32_t &newest, uint32_t &seq, uint8_t *buf) {
  uint16_t slots = flogSlots(l);
  uint16_t cand[CANDIDATES];
  uint32_t candSeq[CANDIDATES];
  uint8_t nc = 0;
  for (uint16_t s = 0; s < slots; s++) {
    if (!readSlot(l, s, buf, 8)) return false;
    if (flogGet16(buf) != l.magic) continue;
    uint32_t q = flogGet32(buf + 4);
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
    FlogRead r = flogRead(l, cand[i], buf);
    if (r == FL_GARBLED) return false;
    if (r == FL_VALID) {
      newest = cand[i];
      seq = candSeq[i];
      return true;
    }
  }
  if (nc < CANDIDATES) return true;  // every slot with the magic was read: none is valid
  for (uint16_t s = 0; s < slots; s++) {
    FlogRead r = flogRead(l, s, buf);
    if (r == FL_GARBLED) return false;
    if (r == FL_VALID && (newest < 0 || flogGet32(buf + 4) > seq)) {
      newest = s;
      seq = flogGet32(buf + 4);
    }
  }
  return true;
}

bool flogOpen(FlashLog &l, int32_t &newest, uint8_t *top, uint8_t *buf) {
  l.on = false;
  uint32_t seq = 0;
  if (!findNewest(l, newest, seq, buf) || (newest >= 0 && flogRead(l, newest, top) != FL_VALID)) return false;
  // Slots after the newest record that aren't erased (one a power cut tore, or a write that failed) are passed over:
  // the next record goes after them, up to the next sector, which it erases first.
  uint16_t slots = flogSlots(l);
  l.head = newest < 0 ? 0 : (newest + 1) % slots;
  while (l.head % perSector(l)) {
    FlogRead r = flogRead(l, l.head, buf);
    if (r == FL_GARBLED) return false;
    if (r == FL_ERASED) break;
    l.head = (l.head + 1) % slots;
  }
  l.nextSeq = newest < 0 ? 1 : seq + 1;  // 0: lseq ran out (only a record from elsewhere can read that high)
  l.on = l.nextSeq != 0;
  return true;
}

bool flogWrite(FlashLog &l, uint8_t fmt, uint8_t kind, uint32_t idx, uint16_t aux, const uint8_t *data) {
  if (!l.on) return false;
  uint16_t len = flogRecLen(l);
  uint8_t buf[FLOG_MAX_REC], check[FLOG_MAX_REC];
  flogPut16(buf, l.magic);
  buf[2] = fmt;
  buf[3] = kind;
  flogPut32(buf + 4, l.nextSeq);
  flogPut32(buf + 8, idx);
  flogPut16(buf + 12, aux);
  flogPut16(buf + 14, 0);
  if (data) memcpy(buf + FLOG_HDR, data, l.dataLen);
  else memset(buf + FLOG_HDR, 0, l.dataLen);
  flogPut32(buf + len - 4, crc32(buf, len - 4));
  FlashAccess fa;
  uint32_t addr = slotAddr(l, l.head);
  // Entering a sector: erase it first. It holds the oldest records (the log wraps here).
  bool ok = l.head % perSector(l) || extFlashEraseSector(addr);
  if (ok) {
    ok = extFlashProgram(addr, buf, len);
    if (ok) {
      extFlashRead(addr, check, len);
      ok = !memcmp(buf, check, len);
    }
    l.head = (l.head + 1) % flogSlots(l);
    l.on = ++l.nextSeq != 0;
  }
  return ok;
}
