#include "radio.h"
#include "config.h"
#include "log.h"
#include <LoRa.h>
#include <SHA256.h>

static bool ok = false;
static bool begun = false;
static uint32_t faults = 0;
static uint32_t crcErrors = 0;  // frames received with a bad CRC, since boot
static uint32_t rxDone = 0;     // frames received, good or not, since boot
static int32_t lastFei = 0;     // frequency error of the last good frame (Hz)
static uint32_t retryAt = 0;  // while !ok: when to try radioBegin() again
#define RETRY_MS 5000
static bool restartDue = false;  // a fault: radioRecover() re-initialises the radio
// Transmission in progress. TX is asynchronous: a frame takes up to seconds at SF12, and blocking for it held
// up the loop (relay pulses ran long by the airtime, and back-to-back frames could reach the watchdog).
static volatile bool txActive = false;
static volatile uint32_t txEndAt;  // when the last TX finished (or was abandoned)
static uint32_t txStart, txLimit;

// Direct SX127x register access (the LoRa library keeps its own private), same bus settings.
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FIFO_ADDR_PTR 0x0D
#define REG_FIFO_RX_CURRENT_ADDR 0x10
#define REG_IRQ_FLAGS 0x12
#define REG_RX_NB_BYTES 0x13
#define REG_MODEM_STAT 0x18
#define REG_RSSI_VALUE 0x1B
#define RSSI_OFFSET_HF 157  // HF port: freq_hz is never below 862 MHz
#define REG_DIO_MAPPING_1 0x40
#define DIO0_TX_DONE 0x40
// Signal detected | signal synchronized | header info valid. Bit 2 (RX on-going) is set whenever the
// receiver is on, so it says nothing about the channel.
#define MODEM_STAT_BUSY 0x0B
#define IRQ_RX_DONE 0x40
#define IRQ_CRC_ERROR 0x20
#define IRQ_TX_DONE 0x08
#define OPMODE_LORA_FSTX 0x82       // long-range mode | frequency synthesis (TX starting)
#define OPMODE_LORA_TX 0x83         // long-range mode | TX
#define OPMODE_LORA_RX_CONT 0x85    // long-range mode | RX continuous
#define OPMODE_LONG_RANGE 0x80      // LoRa mode; clear after a radio reset (FSK is the power-on default)

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

static void startRx() {
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, OPMODE_LORA_RX_CONT);
}

// The radio reset itself (supply dip) or stopped answering: count it, log it, and take it down until radioRecover()
// starts it over. Not from here: LoRa.begin() stops the loop ~0.5 s, which must wait while a relay pulses.
static void fault(int32_t kind) {
  faults++;
  logEvent(EV_RADIO_FAIL, kind, faults);
  ok = false;
  txActive = false;
  txEndAt = millis();
  restartDue = true;
}

static void finishTx() {
  startRx();
  txActive = false;
  txEndAt = millis();
}

// DIO0 rises on TX done: straight back into RX. The peer answers ~25 ms after our frame ends, and waiting
// for the loop could miss its preamble when the loop stalls (USB writes took up to ~40 ms). SPI is safe here:
// usingInterrupt() masks this interrupt during every transaction on the bus, ours and the library's.
static void onDio0() {
  if (txActive && (readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE)) finishTx();
}

void radioRestart() {
  if (begun) radioBegin();
}

bool radioBegin() {
  txActive = false;
  restartDue = false;
  if (begun) LoRa.end();
  begun = true;
  ok = LoRa.begin(cfg.freq_hz);
  if (!ok) {
    if (!retryAt) logEvent(EV_RADIO_FAIL, 0, faults);  // once, not on every failed retry
    retryAt = (millis() + RETRY_MS) | 1;
    return false;
  }
  retryAt = 0;
  int irq = digitalPinToInterrupt(LORA_DEFAULT_DIO0_PIN);
  LORA_DEFAULT_SPI.usingInterrupt(irq);
  attachInterrupt(irq, onDio0, RISING);
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

bool radioRecoverDue() {
  return restartDue || (!ok && retryAt && (int32_t)(millis() - retryAt) >= 0);
}

void radioRecover() {
  if (!radioRecoverDue()) return;
  // After a fault, start over; a radio that failed to initialise is retried, so a transient fault doesn't need a
  // reboot (logged when it comes back).
  bool retry = !restartDue;
  if (radioBegin() && retry) logEvent(EV_RADIO_FAIL, 3, faults);
}

uint32_t radioFaults() {
  return faults;
}

uint32_t radioCrcErrors() {
  return crcErrors;
}

int32_t radioLastFei() {
  return lastFei;
}

uint32_t radioRxDoneCount() {
  return rxDone;
}

bool radioSend(const uint8_t *buf, size_t len) {
  if (radioTxBusy() || !ok) return false;  // in this order: radioTxBusy() can find a fault
  LoRa.beginPacket();
  LoRa.write(buf, len);
  writeReg(REG_DIO_MAPPING_1, DIO0_TX_DONE);
  txStart = millis();
  txLimit = radioAirtimeMs(len) + 200;
  txActive = true;
  LoRa.endPacket(true);  // async; the library's blocking endPacket() waits forever if the radio resets
  return true;
}

// Polled fallback for the DIO0 interrupt, and the deadline: a supply dip during TX can reset the radio,
// which then never reports TX done. That is logged as a fault and the radio is re-initialised.
bool radioTxBusy() {
  if (!txActive) return false;
  // FSTX first: the synthesizer starts up for a moment after endPacket() before TX proper.
  uint8_t mode = readReg(REG_OP_MODE);
  bool stillTx = mode == OPMODE_LORA_TX || mode == OPMODE_LORA_FSTX;
  bool done = readReg(REG_IRQ_FLAGS) & IRQ_TX_DONE;  // read after the mode: catches a TX just finished
  if (!txActive) return false;  // the interrupt finished it between the reads (and cleared the flags)
  // A dead SPI bus reads 0xFF everywhere, TX_DONE included.
  if (done && mode != 0xFF) {
    finishTx();
  } else if (!stillTx || (int32_t)(millis() - txStart) >= (int32_t)txLimit) {
    fault(1);
  }
  return txActive;
}

uint32_t radioTxEndAt() {
  return txEndAt;
}

// Our own RX polling instead of LoRa.parsePacket(). That one writes the IRQ flags back while a packet is
// still arriving (clearing ValidHeader mid-packet), after which RX_DONE could survive its clear and the next
// poll returned the same FIFO contents again (seen as `replay` with a == b). Here the flags are touched only
// once RX_DONE is up, and only the bits read are cleared.
// RX continuous, not RX-single: RX-single gives up after 100 symbols and sits in standby until the loop
// re-arms it, and a frame whose preamble straddled that moment was lost (~2 % of frames at SF9, more at
// SF12, both directions).
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr) {
  if (radioTxBusy() || !ok) return 0;  // down (radioRecover() restarts it), or a fault radioTxBusy() just found
  uint8_t mode = readReg(REG_OP_MODE);
  if (mode != OPMODE_LORA_RX_CONT) {
    // Out of LoRa mode (or no answer) means the radio reset: startRx() alone can't fix that, as LoRa mode can
    // only be entered from sleep, and the board would stay deaf without ever logging a fault.
    if (!(mode & OPMODE_LONG_RANGE) || mode == 0xFF) fault(2);
    else startRx();  // after init
    return 0;
  }
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  if (!(irq & IRQ_RX_DONE)) return 0;  // nothing finished; never touch the flags mid-packet
  rxDone++;
  size_t n = 0;
  if (!(irq & IRQ_CRC_ERROR)) {
    uint8_t len = readReg(REG_RX_NB_BYTES);
    if (len <= max) {  // drop oversize packets
      writeReg(REG_FIFO_ADDR_PTR, readReg(REG_FIFO_RX_CURRENT_ADDR));
      while (n < len) buf[n++] = readReg(REG_FIFO);
      rssi = LoRa.packetRssi();
      snr = LoRa.packetSnr();
      lastFei = LoRa.packetFrequencyError();
    }
  } else {
    crcErrors++;
  }
  writeReg(REG_IRQ_FLAGS, irq);
  return n;
}

bool radioChannelBusy() {
  if (radioTxBusy()) return true;
  if (!ok) return false;
  // A packet we haven't read yet: transmitting now would overwrite it in the FIFO.
  if (readReg(REG_IRQ_FLAGS) & IRQ_RX_DONE) return true;
  if (readReg(REG_OP_MODE) != OPMODE_LORA_RX_CONT) return false;
  return (readReg(REG_MODEM_STAT) & MODEM_STAT_BUSY) != 0;
}

// In-channel RSSI while listening with no LoRa frame under way: the noise floor, plus anything that isn't LoRa
// (FSK bursts such as Z-Wave), which the modem doesn't flag.
int16_t radioNoiseDbm() {
  if (!ok || txActive) return INT16_MIN;
  if (readReg(REG_OP_MODE) != OPMODE_LORA_RX_CONT) return INT16_MIN;
  if (readReg(REG_IRQ_FLAGS) & IRQ_RX_DONE) return INT16_MIN;
  if (readReg(REG_MODEM_STAT) & MODEM_STAT_BUSY) return INT16_MIN;
  return (int16_t)readReg(REG_RSSI_VALUE) - RSSI_OFFSET_HF;
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

static uint8_t randState[32];  // radioRandom32()'s hash chain

void radioAddEntropy(const void *data, size_t len) {
  SHA256 h;
  h.update(randState, sizeof(randState));
  h.update(data, len);
  h.finalize(randState, sizeof(randState));
}

uint32_t radioRandom32() {
  // Wideband RSSI noise only changes while the radio is receiving: sample its LSB in continuous RX, mix in
  // timer jitter, and hash, chained with the previous state. Never leaves RX (that aborted a frame being
  // received and dropped an unread one) and doesn't sample during TX (the chain still changes the result).
  // Called rarely (session ids, challenges, command ids). Seeded per boot (radioAddEntropy) so it differs per boot
  // when the radio is down and timing is all there is.
  uint8_t *state = randState;
  const size_t stateLen = sizeof(randState);
  uint8_t pool[64];
  uint32_t t = micros();
  bool sample = !radioTxBusy() && ok;
  if (sample && readReg(REG_OP_MODE) != OPMODE_LORA_RX_CONT) startRx();
  for (size_t i = 0; i < sizeof(pool); i++) {
    uint8_t b = 0;
    for (int bit = 0; bit < 8; bit++) {
      delayMicroseconds(40);
      b = (b << 1) | ((sample ? LoRa.random() : 0) & 1);
    }
    pool[i] = b ^ (uint8_t)micros();
  }
  SHA256 h;
  h.update(state, stateLen);
  h.update(pool, sizeof(pool));
  h.update(&t, sizeof(t));
  h.finalize(state, stateLen);
  uint32_t out;
  memcpy(&out, state, sizeof(out));
  h.reset();
  h.update(state, stateLen);
  h.finalize(state, stateLen);  // so the output doesn't reveal the next state
  return out;
}
