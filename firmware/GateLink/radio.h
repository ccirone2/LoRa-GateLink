#pragma once
#include <Arduino.h>

// Thin wrapper over the sandeepmistry LoRa library (raw point-to-point, not LoRaWAN).
bool radioBegin();  // (re)initialise with current cfg radio params
void radioRestart();  // radioBegin() again if it has been started (after the module was held in reset)
bool radioOk();
// Starts transmitting and returns at once (false if the radio is down or still transmitting). The radio
// goes back to RX continuous as soon as TX is done.
bool radioSend(const uint8_t *buf, size_t len);
// True while a frame is on the air. A TX that overruns its airtime (stuck or reset radio) is logged as a
// fault and the radio is re-initialised.
bool radioTxBusy();
uint32_t radioTxEndAt();  // millis() when the last TX ended
uint32_t radioFaults();  // TX faults since boot
uint32_t radioCrcErrors();  // frames received with a bad CRC since boot
uint32_t radioRxDoneCount();  // frames received (any CRC) since boot
// Listen-before-talk: true while a LoRa frame is being received (preamble detected onward) or a received
// packet is still waiting to be read. Blind for the first few preamble symbols.
bool radioChannelBusy();
// Polls for a received packet. Returns length (0 if none).
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr);
// Noise floor (in-channel RSSI, dBm) while idle in RX; INT16_MIN while transmitting or receiving a frame. The
// modem flags a frame only a few symbols into its preamble, so a reading can still be the start of one.
int16_t radioNoiseDbm();
uint32_t radioAirtimeMs(size_t payloadLen);
uint32_t radioRandom32();
