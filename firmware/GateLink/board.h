#pragma once
#include <Arduino.h>

// What only the board itself can do: implemented in GateLink.ino on the MKR, and by the host simulation in
// tests/native. Everything else (app.cpp and below, except the radio, flash, charger and console drivers) builds on
// the PC too, so the role state machines can be tested there.
void boardKick();         // watchdog reset (8 s)
void boardReset();        // software reset; never returns on the board
uint32_t boardFreeRam();  // bytes between the heap's high-water mark and the stack
