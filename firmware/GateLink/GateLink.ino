// GateLink: LoRa bridge between an Alarm.com/Shelly Wave 1 at the house and a
// LiftMaster CSW24UL gate opener. One firmware for both boards; the role (house or
// gate) is chosen in the stored config via the web UI.
//
// Board: Arduino MKR WAN 1310 + MKR Relay Proto Shield.
// Build: arduino-cli compile --fqbn arduino:samd:mkrwan1310 firmware/GateLink
//
// The application is in app.cpp; this file adds only what needs the chip itself (board.h): the watchdog, the
// reset cause, the serial number, the software reset and the free RAM.
#include <Adafruit_SleepyDog.h>
#include "app.h"
#include "board.h"

void boardKick() {
  Watchdog.reset();
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

void setup() {
  uint8_t resetCause = PM->RCAUSE.reg;
  appRelaysBegin();
  Watchdog.enable(8000);  // see appSetup()
  const uint32_t serial[4] = { *(volatile uint32_t *)0x0080A00C, *(volatile uint32_t *)0x0080A040,
                               *(volatile uint32_t *)0x0080A044, *(volatile uint32_t *)0x0080A048 };
  appSetup(resetCause, serial);
}

void loop() {
  Watchdog.reset();
  appLoop();
}
