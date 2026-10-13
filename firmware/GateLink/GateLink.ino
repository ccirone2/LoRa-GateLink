// GateLink: LoRa bridge between an Alarm.com/Shelly Wave 1 at the house and a
// LiftMaster CSW24UL gate opener. One firmware for both boards; the role (house or
// gate) is chosen in the stored config via the web UI.
//
// Board: Arduino MKR WAN 1310 + MKR Relay Proto Shield.
// Build: arduino-cli compile --fqbn arduino:samd:mkrwan1310 firmware/GateLink
//
// The application is in app.cpp; this file adds only what needs the chip itself (board.h): the watchdog and its
// early warning, the RAM kept across resets, the reset cause, the serial number, the software reset and the free RAM.
#include "app.h"
#include "board.h"

// Kept across every reset but power-on: a section the startup code neither loads nor zeroes. The core's linker
// script has no .noinit, so this is an orphan NOBITS section, which ld places after .bss and before the heap (the
// "@" makes the assembler ignore the "aw",%progbits GCC appends). The bootloader's own RAM is the bottom of RAM and
// its stack below 0x20007C00, both clear of this.
static uint32_t keep[BOARD_KEEP_WORDS] __attribute__((section(".noinit,\"aw\",%nobits@")));

uint32_t *boardKeep() {
  return keep;
}

void boardKick() {
  while (WDT->STATUS.bit.SYNCBUSY) {}
  WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}

void boardReset() {
  NVIC_SystemReset();
}

// Gap between the heap's high-water mark and the stack: what's left for the deepest stack and a bigger reply.
extern "C" char *sbrk(int incr);
uint32_t boardFreeRam() {
  char top;
  return &top - sbrk(0);
}

// The watchdog: 8 s (8192 cycles of a 1.024 kHz clock), with the early warning half way (4096 cycles), in normal
// mode. GCLK2 (unused by the core) divides the 32 kHz ultra-low-power oscillator by 32 for it.
static void watchdogBegin() {
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(2) | GCLK_GENDIV_DIV(4);
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_DIVSEL;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT | GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK2;
  WDT->CTRL.reg = 0;
  while (WDT->STATUS.bit.SYNCBUSY) {}
  WDT->CONFIG.reg = WDT_CONFIG_PER_8K;
  WDT->EWCTRL.reg = WDT_EWCTRL_EWOFFSET_4K;
  WDT->INTFLAG.reg = WDT_INTFLAG_EW;
  WDT->INTENSET.reg = WDT_INTENSET_EW;
  NVIC_ClearPendingIRQ(WDT_IRQn);
  NVIC_SetPriority(WDT_IRQn, 0);
  NVIC_EnableIRQ(WDT_IRQn);
  WDT->CTRL.reg = WDT_CTRL_ENABLE;
  while (WDT->STATUS.bit.SYNCBUSY) {}
}

// The early warning: 4 s without a kick. The exception frame on the main stack holds the interrupted program
// counter (word 6); appWatchdogWarning only writes RAM. The reset still follows 4 s later unless the loop recovers.
extern "C" void watchdogWarning(uint32_t *frame) {
  WDT->INTFLAG.reg = WDT_INTFLAG_EW;
  appWatchdogWarning(frame[6]);
}

extern "C" __attribute__((naked)) void WDT_Handler() {
  __asm__ volatile("mrs r0, msp\n\tldr r1, =watchdogWarning\n\tbx r1\n");
}

void setup() {
  uint8_t resetCause = PM->RCAUSE.reg;
  appRelaysBegin();
  watchdogBegin();  // see appSetup()
  const uint32_t serial[4] = { *(volatile uint32_t *)0x0080A00C, *(volatile uint32_t *)0x0080A040,
                               *(volatile uint32_t *)0x0080A044, *(volatile uint32_t *)0x0080A048 };
  appSetup(resetCause, serial);
}

void loop() {
  boardKick();
  appLoop();
}
