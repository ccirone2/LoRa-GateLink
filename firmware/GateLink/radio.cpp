#include "radio.h"
#include "config.h"
#include <LoRa.h>
#include <SHA256.h>

static bool ok = false;
static bool begun = false;

bool radioBegin() {
  if (begun) LoRa.end();
  begun = true;
  ok = LoRa.begin(cfg.freq_hz);
  if (!ok) return false;
  LoRa.setSpreadingFactor(cfg.sf);
  LoRa.setSignalBandwidth(cfg.bw_hz);
  LoRa.setCodingRate4(cfg.cr);
  LoRa.setTxPower(cfg.tx_power);
  LoRa.setSyncWord(cfg.sync_word);
  LoRa.setPreambleLength(8);
  LoRa.enableCrc();
  return true;
}

bool radioOk() {
  return ok;
}

void radioSend(const uint8_t *buf, size_t len) {
  if (!ok) return;
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket();  // blocks until TX done; next parsePacket() re-enters RX
}

size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr) {
  if (!ok) return 0;
  int len = LoRa.parsePacket();
  if (len <= 0) return 0;
  size_t n = 0;
  while (LoRa.available()) {
    int b = LoRa.read();
    if (n < max) buf[n++] = (uint8_t)b;
  }
  rssi = LoRa.packetRssi();
  snr = LoRa.packetSnr();
  return (size_t)len > max ? 0 : n;  // drop oversize packets
}

uint32_t radioAirtimeMs(size_t payloadLen) {
  // Semtech SX1276 time-on-air formula, explicit header, CRC on.
  float tsym = (float)(1UL << cfg.sf) / (float)cfg.bw_hz * 1000.0f;
  int de = tsym > 16.0f ? 1 : 0;
  float tpre = (8 + 4.25f) * tsym;
  float num = 8.0f * payloadLen - 4.0f * cfg.sf + 28 + 16;
  float den = 4.0f * (cfg.sf - 2 * de);
  int nPayload = 8 + max((int)ceilf(num / den) * (cfg.cr), 0);
  return (uint32_t)ceilf(tpre + nPayload * tsym);
}

uint32_t radioRandom32() {
  // Wideband RSSI noise only changes while the radio is receiving: sample its LSB in
  // continuous RX, mix in timer jitter, and hash. Called rarely (session ids, challenges).
  static uint32_t counter = 0;
  uint8_t pool[64];
  uint32_t t = micros();
  if (ok) LoRa.receive();
  for (size_t i = 0; i < sizeof(pool); i++) {
    uint8_t b = 0;
    for (int bit = 0; bit < 8; bit++) {
      delayMicroseconds(40);
      b = (b << 1) | ((ok ? LoRa.random() : 0) & 1);
    }
    pool[i] = b ^ (uint8_t)micros();
  }
  if (ok) LoRa.idle();
  SHA256 h;
  h.update(pool, sizeof(pool));
  h.update(&t, sizeof(t));
  counter++;
  h.update(&counter, sizeof(counter));
  uint32_t out;
  h.finalize(&out, sizeof(out));
  return out;
}
