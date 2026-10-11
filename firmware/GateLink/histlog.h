#pragma once
#include <Arduino.h>

// The link history's log in the SPI flash: history.cpp keeps the buckets in RAM, and each one it completes is
// appended here, so a reset doesn't lose them. Sectors HLOG_SECTOR0.. of the chip (the flash map: config.h), cut into
// 128-byte slots (two to a page, 32 to a sector) written in turn; entering a sector erases it first, which drops the
// oldest records (the log wraps). A record:
//   magic(2) fmt(1) kind(1) lseq(4) idx(4) period_s(2) 0(2) data(HLOG_DATA) crc(4, over everything before it)
// lseq numbers every record written, so the newest is the one with the largest; idx is the bucket's number. hist.clear
// writes a CLEARED record: the history starts again at bucket 0 with its period. The envelope (magic, kind, lseq, crc,
// the length) is the same whatever the fmt, so a firmware with another bucket layout still finds the newest record and
// writes after it; it just doesn't load records of another fmt.
//
// Every access holds the radio module in reset (FlashAccess, config.h): ~0.5 s off the air and a stalled loop, so the
// caller never writes while a relay pulses (appRelaysPulsing), and at most once per bucket.
#define HLOG_SECTOR0 4
#define HLOG_SECTORS 16
#define HLOG_SLOT 128
#define HLOG_DATA 68  // a Bucket (history.cpp)
#define HLOG_FMT 1    // the data's layout: bump it when Bucket changes (records of another fmt aren't loaded)

// Receives a loaded bucket: its number and HLOG_DATA bytes.
typedef void (*HistLogTake)(uint32_t idx, const uint8_t *data);

// At boot, with the config loaded. Finds the newest record. If it holds history (a bucket, or a CLEARED record), sets
// period and next (the bucket number to go on from) and returns true, after handing take() the buckets of the newest
// run: consecutive numbers ending at next - 1, of one period and this fmt, newest first, at most `max`. A torn record
// in the run (a power cut mid-write) is passed over. False if there's nothing to go on from (period, next untouched).
bool histLogBegin(uint32_t max, HistLogTake take, uint16_t &period, uint32_t &next);
// Records can be written: the chip answered and its first read at boot wasn't garbled (else not until the next boot).
bool histLogOn();
// Appends a completed bucket. False if it wasn't written or didn't read back intact (that bucket is then missing from
// the log: the next boot loads only the buckets after it); the next record goes to the next slot either way.
bool histLogAppend(uint32_t idx, uint16_t period, const uint8_t *data);
// Appends a CLEARED record. True also without the chip (nothing is kept there to clear).
bool histLogClear(uint16_t period);
