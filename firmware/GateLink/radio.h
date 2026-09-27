#pragma once
#include <Arduino.h>

// Thin wrapper over the sandeepmistry LoRa library (raw point-to-point, not LoRaWAN).
bool radioBegin();  // (re)initialise with current cfg radio params
bool radioOk();
// Transmits and waits (bounded) for TX done. On a stuck or reset radio it logs a fault,
// re-initialises the radio and returns false instead of hanging until the watchdog fires.
bool radioSend(const uint8_t *buf, size_t len);
uint32_t radioFaults();  // TX faults since boot
// Polls for a received packet. Returns length (0 if none).
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr);
uint32_t radioAirtimeMs(size_t payloadLen);
uint32_t radioRandom32();
