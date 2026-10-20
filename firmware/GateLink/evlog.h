#pragma once
#include <Arduino.h>
#include "log.h"

// The event log's journal in the SPI flash (flashlog.h): log.cpp keeps the newest events in RAM, and writes them here
// in batches of up to ELOG_PER_REC, so a reset or power cut loses at most those not written yet. Sectors ELOG_SECTOR0..
// of the chip (the flash map: config.h), cut into page-sized slots, one record each: ELOG_PER_REC entries of 16 bytes
// (the oldest first; aux = how many are used). Sectors from ELOG_SECTOR0 + ELOG_SECTORS up stay free (F7's staged image).
#define ELOG_SECTOR0 20
#define ELOG_SECTORS 16
#define ELOG_SLOT 256
#define ELOG_ENTRY 16
#define ELOG_PER_REC 14
#define ELOG_FMT 1  // the entry layout (evlog.cpp): bump it when it changes (records of another fmt aren't read)

// At boot, with the config loaded: finds the newest record, so the next goes after it.
void evLogBegin();
bool evLogOn();
// Appends a record of n (1..ELOG_PER_REC) entries. False if it wasn't written or didn't read back intact.
bool evLogAppend(const LogEntry *e, uint8_t n);
// Reads the k-th newest record (k = 1 the newest) into out (ELOG_PER_REC entries), oldest entry first; the number of
// entries, or 0 if there is no such record (fewer were written, or the log is off). One FlashAccess.
uint8_t evLogGet(uint16_t k, LogEntry *out);
