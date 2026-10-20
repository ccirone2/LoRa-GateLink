#pragma once
#include <Arduino.h>

// A log of fixed-size records in a run of SPI flash sectors (the flash map: config.h), written in turn: the link
// history's (histlog.cpp) and the event log's (evlog.cpp). Each sector is cut into slots; entering a sector erases it
// first, which drops the oldest records (the log wraps). A record:
//   magic(2) fmt(1) kind(1) lseq(4) idx(4) aux(2) 0(2) data(dataLen) crc(4, over everything before it)
// lseq numbers every record written, so the newest is the one with the largest; idx and aux are the owner's. The
// envelope (magic, kind, lseq, crc, the length) is the same whatever the fmt, so a firmware with another data layout
// still finds the newest record and writes after it.
//
// Every access holds the radio module in reset (FlashAccess, config.h): ~0.5 s off the air and a stalled loop, so
// callers never write while a relay pulses (appRelaysPulsing).
#define FLOG_HDR 16
#define FLOG_MAX_REC 256  // the longest record (FLOG_HDR + dataLen + 4) any log has: buffers are this size

enum FlogRead : uint8_t { FL_ERASED, FL_GARBLED, FL_INVALID, FL_VALID };

struct FlashLog {
  uint16_t sector0, sectors;  // where it lives
  uint16_t slot;              // slot size: a power of two that divides a page
  uint16_t dataLen;           // the data each record carries
  uint16_t magic;
  // State, set by flogOpen
  bool on;           // records may be written: the chip answered and its first read at boot wasn't garbled
  uint16_t head;     // the slot the next record goes to
  uint32_t nextSeq;  // and its lseq
};

uint16_t flogSlots(const FlashLog &l);
inline uint16_t flogRecLen(const FlashLog &l) {
  return FLOG_HDR + l.dataLen + 4;
}
// Reads slot s's record into buf (flogRecLen bytes).
FlogRead flogRead(const FlashLog &l, uint16_t s, uint8_t *buf);
// At boot, inside a FlashAccess: finds the newest valid record (its slot, or -1 for none) and reads it into top, then
// sets where the next one goes (after it, past any torn ones, up to the next sector) and turns the log on. False if a
// read was garbled (the log stays off until the next boot). buf is scratch.
bool flogOpen(FlashLog &l, int32_t &newest, uint8_t *top, uint8_t *buf);
// Appends a record (data: dataLen bytes, or null for zeros) in its own FlashAccess. False if it wasn't written or didn't
// read back intact; the next record goes to the next slot either way.
bool flogWrite(FlashLog &l, uint8_t fmt, uint8_t kind, uint32_t idx, uint16_t aux, const uint8_t *data);

inline uint16_t flogGet16(const uint8_t *p) {
  return p[0] | p[1] << 8;
}
inline uint32_t flogGet32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline void flogPut16(uint8_t *p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
inline void flogPut32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}
