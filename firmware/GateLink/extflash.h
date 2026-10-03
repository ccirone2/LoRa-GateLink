// The MKR WAN 1310's on-board SPI NOR flash (W25Q16JV, 2 MB): CS = FLASH_CS, on SPI1 (SERCOM4) with the radio.
// Firmware uploads don't touch it, so config kept here survives them.
//
// The radio module's own MCU (unused: we drive its SX1276 directly) shares SERCOM4 lines with the chip, and
// while it runs, flash traffic is garbled (JEDEC id reads 0, writes don't verify). Every access must be made
// with extFlashHoldModem() asserted. That resets the SX1276 as well, so a running radio needs radioRestart()
// afterwards. usingInterrupt() on SPI1 keeps the radio's DIO0 handler off the bus meanwhile.
#pragma once
#include <Arduino.h>

#define EXTFLASH_SECTOR 4096u
#define EXTFLASH_PAGE 256u

void extFlashHoldModem();  // holds the radio module in reset (LoRa.begin() releases it)
bool extFlashBegin();      // holds the module, wakes the chip, checks its JEDEC id; false if it doesn't answer
bool extFlashPresent();
uint32_t extFlashId();     // JEDEC id as read (manufacturer << 16 | type << 8 | capacity)
void extFlashRead(uint32_t addr, uint8_t *buf, size_t len);
bool extFlashEraseSector(uint32_t addr);                          // blocks up to ~400 ms
bool extFlashProgram(uint32_t addr, const uint8_t *buf, size_t len);  // within one page
