#pragma once
#include <Arduino.h>

// Thin wrapper over the sandeepmistry LoRa library (raw point-to-point, not LoRaWAN).
bool radioBegin();  // (re)initialise with current cfg radio params
bool radioOk();
void radioSend(const uint8_t *buf, size_t len);
// Polls for a received packet. Returns length (0 if none).
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr);
uint32_t radioAirtimeMs(size_t payloadLen);
uint32_t radioRandom32();
