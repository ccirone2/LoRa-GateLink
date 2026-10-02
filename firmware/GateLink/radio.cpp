#include "radio.h"
#include "config.h"
#include "log.h"
#include <LoRa.h>
#include <SHA256.h>

static bool ok = false;
static bool begun = false;
static uint32_t faults = 0;

// Direct SX127x register access (the LoRa library keeps its own private), same bus settings.
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define IRQ_RX_DONE 0x40
#define IRQ_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08
#define OPMODE_LORA_TX 0x83         // long-range mode | TX
#define OPMODE_LORA_RX_SINGLE 0x86  // long-range mode | RX single

static uint8_t regAccess(uint8_t addr, uint8_t value) {
  LORA_DEFAULT_SPI.beginTransaction(SPISettings(LORA_DEFAULT_SPI_FREQUENCY, MSBFIRST, SPI_MODE0));
  digitalWrite(LORA_DEFAULT_SS_PIN, LOW);
  LORA_DEFAULT_SPI.transfer(addr);
  uint8_t r = LORA_DEFAULT_SPI.transfer(value);
  digitalWrite(LORA_DEFAULT_SS_PIN, HIGH);
  LORA_DEFAULT_SPI.endTransaction();
  return r;
}
static uint8_t readReg(uint8_t addr) { return regAccess(addr & 0x7F, 0x00); }
static void writeReg(uint8_t addr, uint8_t v) { regAccess(addr | 0x80, v); }

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

uint32_t radioFaults() {
  return faults;
}

bool radioSend(const uint8_t *buf, size_t len) {
  if (!ok) return false;
  LoRa.beginPacket();
  LoRa.write(buf, len);
  LoRa.endPacket(true);  // async: we poll below with a deadline; next radioReceive() re-enters RX
  // The library's blocking endPacket() waits forever for TX done. A supply dip during TX can
  // reset the radio, which then never reports it and the watchdog reboots the board.
  uint32_t start = millis();
  uint32_t limit = radioAirtimeMs(len) + 200;
  for (;;) {
    if (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE) {
      writeReg(REG_IRQ_FLAGS, IRQ_TX_DONE);
      return true;
    }
    bool stillTx = readReg(REG_OP_MODE) == OPMODE_LORA_TX;
    if (!stillTx && (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE)) continue;  // finished between reads
    if (!stillTx || (int32_t)(millis() - start) >= (int32_t)limit) break;
  }
  faults++;
  logEvent(EV_RADIO_FAIL, 1, faults);
  radioBegin();
  return false;
}

// Our own RX-single polling instead of LoRa.parsePacket(). That one writes the IRQ flags back while a
// packet is still arriving (clearing ValidHeader mid-packet), after which RX_DONE could survive its clear
// and the next poll returned the same FIFO contents again (seen as `replay` with a == b). Here the flags
// are only read and cleared once the radio has dropped back to standby, when they can no longer change.
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr) {
  if (!ok) return 0;
  if (readReg(REG_OP_MODE) == OPMODE_LORA_RX_SINGLE) return 0;  // listening, or a packet is arriving
  // Standby: RX single ended (packet or timeout), or we transmitted / re-initialised since.
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  size_t n = 0;
  if ((irq & IRQ_RX_DONE) && !(irq & IRQ_CRC_ERROR)) {
    uint8_t len = readReg(REG_RX_NB_BYTES);
    if (len <= max) {  // drop oversize packets
      writeReg(REG_FIFO_ADDR_PTR, readReg(REG_FIFO_RX_CURRENT_ADDR));
      while (n < len) buf[n++] = readReg(REG_FIFO);
      rssi = LoRa.packetRssi();
      snr = LoRa.packetSnr();
    }
  }
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_FIFO_ADDR_PTR, 0);
  writeReg(REG_OP_MODE, OPMODE_LORA_RX_SINGLE);
  return n;
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
