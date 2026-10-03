#include "extflash.h"
#include <SPI.h>

// Standard SPI NOR commands (W25Q16JV and alike).
#define CMD_WRITE_ENABLE 0x06
#define CMD_READ_STATUS 0x05
#define CMD_READ 0x03
#define CMD_PAGE_PROGRAM 0x02
#define CMD_SECTOR_ERASE 0x20
#define CMD_RELEASE_PD 0xAB
#define CMD_JEDEC_ID 0x9F
#define SR_BUSY 0x01

#define FLASH_BUS SPI1
static const SPISettings FLASH_SPI(1000000, MSBFIRST, SPI_MODE0);
static uint32_t jedecId = 0;
static uint32_t rawId = 0;  // as read, for diagnostics

static void select() {
  FLASH_BUS.beginTransaction(FLASH_SPI);
  digitalWrite(FLASH_CS, LOW);
}

static void deselect() {
  digitalWrite(FLASH_CS, HIGH);
  FLASH_BUS.endTransaction();
}

static void command(uint8_t cmd) {
  select();
  FLASH_BUS.transfer(cmd);
  deselect();
}

static void sendAddr(uint8_t cmd, uint32_t addr) {
  FLASH_BUS.transfer(cmd);
  FLASH_BUS.transfer((uint8_t)(addr >> 16));
  FLASH_BUS.transfer((uint8_t)(addr >> 8));
  FLASH_BUS.transfer((uint8_t)addr);
}

// A dead bus reads 0xFF, i.e. busy forever: the timeout turns that into a failure.
static bool waitReady(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  for (;;) {
    select();
    FLASH_BUS.transfer(CMD_READ_STATUS);
    uint8_t sr = FLASH_BUS.transfer(0);
    deselect();
    if (!(sr & SR_BUSY)) return true;
    if ((int32_t)(millis() - t0) >= (int32_t)timeoutMs) return false;
    delayMicroseconds(50);
  }
}

void extFlashHoldModem() {
  pinMode(LORA_RESET, OUTPUT);
  digitalWrite(LORA_RESET, LOW);
  delay(1);
}

bool extFlashBegin() {
  extFlashHoldModem();
  pinMode(LORA_IRQ_DUMB, OUTPUT);  // the radio's select, on the same bus
  digitalWrite(LORA_IRQ_DUMB, HIGH);
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
  FLASH_BUS.begin();
  command(CMD_RELEASE_PD);
  delayMicroseconds(50);  // tRES1 is 3 us
  select();
  FLASH_BUS.transfer(CMD_JEDEC_ID);
  uint32_t id = (uint32_t)FLASH_BUS.transfer(0) << 16;
  id |= (uint32_t)FLASH_BUS.transfer(0) << 8;
  id |= FLASH_BUS.transfer(0);
  deselect();
  rawId = id;
  uint8_t mfr = id >> 16;
  jedecId = (mfr == 0x00 || mfr == 0xFF || !waitReady(10)) ? 0 : id;
  return jedecId != 0;
}

bool extFlashPresent() {
  return jedecId != 0;
}

uint32_t extFlashId() {
  return rawId;
}

void extFlashRead(uint32_t addr, uint8_t *buf, size_t len) {
  select();
  sendAddr(CMD_READ, addr);
  for (size_t i = 0; i < len; i++) buf[i] = FLASH_BUS.transfer(0);
  deselect();
}

bool extFlashEraseSector(uint32_t addr) {
  if (!jedecId) return false;
  command(CMD_WRITE_ENABLE);
  select();
  sendAddr(CMD_SECTOR_ERASE, addr);
  deselect();
  return waitReady(1000);  // datasheet max 400 ms
}

bool extFlashProgram(uint32_t addr, const uint8_t *buf, size_t len) {
  if (!jedecId || len == 0 || (addr % EXTFLASH_PAGE) + len > EXTFLASH_PAGE) return false;
  command(CMD_WRITE_ENABLE);
  select();
  sendAddr(CMD_PAGE_PROGRAM, addr);
  for (size_t i = 0; i < len; i++) FLASH_BUS.transfer(buf[i]);
  deselect();
  return waitReady(20);  // datasheet max 3 ms
}

