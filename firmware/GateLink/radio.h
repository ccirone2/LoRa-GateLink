#pragma once
#include <Arduino.h>

// Thin wrapper over the sandeepmistry LoRa library (raw point-to-point, not LoRaWAN).
bool radioBegin();  // (re)initialise with current cfg radio params
bool radioOk();
// Starts transmitting and returns at once (false if the radio is down or still transmitting). The radio
// goes back to RX continuous as soon as TX is done.
bool radioSend(const uint8_t *buf, size_t len);
// True while a frame is on the air. A TX that overruns its airtime (stuck or reset radio) is logged as a
// fault and the radio is re-initialised.
bool radioTxBusy();
uint32_t radioTxEndAt();  // millis() when the last TX ended
uint32_t radioFaults();  // TX faults since boot
// Listen-before-talk: true while a LoRa frame is being received (preamble detected onward) or a received
// packet is still waiting to be read. Blind for the first few preamble symbols.
bool radioChannelBusy();
// Polls for a received packet. Returns length (0 if none).
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr);
uint32_t radioAirtimeMs(size_t payloadLen);
uint32_t radioRandom32();
