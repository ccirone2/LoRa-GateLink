// The environment the CBMC harnesses run the firmware in: what the Arduino core, the radio and the other drivers would
// answer, as nondeterministic choices (CBMC explores every one). Each harness #includes the firmware's .cpp files it
// checks, so their statics are visible, and defines the functions they call that live elsewhere. A function declared
// but given no body anywhere returns a nondeterministic value and changes nothing (CBMC's default): that suits
// queries such as radioRandom32() or histSaveDue(), and is never used where a result must be consistent.
#pragma once
#include <Arduino.h>
#include "config.h"

// CBMC's nondeterministic values: declared without a body.
extern "C" {
bool nondet_bool();
uint8_t nondet_u8();
uint16_t nondet_u16();
uint32_t nondet_u32();
uint64_t nondet_u64();
int32_t nondet_i32();
}
#define ASSUME(c) __ESBMC_assume(c)
#define CHECK(c, msg) __ESBMC_assert(c, msg)

// Time. `clk` is millis(); `T` is the same clock unwrapped (ms since the harness began, 64 bits), the physical time the
// properties are stated in. The harness starts clk anywhere, so every run may cross millis()'s wrap.
static uint32_t clk;
static uint64_t T;
uint32_t millis() { return clk; }
uint32_t micros() { return clk * 1000u; }
static void advance(uint32_t d) {
  clk += d;
  T += d;
}
void delay(uint32_t ms) { advance(ms); }

// Loop passes. The watchdog resets a board whose pass takes 8 s, so real passes are far shorter; the harnesses let one
// last up to IDLE_MAX_MS (while no relay pulses), so a pass reaches the wrap. It stays under 2^30 ms less a skew: the
// firmware keeps its stamps at most STAMP_CAP_MS (2^30) old once a pass, and a stamp that old plus a pass must still
// read as past (under 2^31) when compared signed. A frame handler's millis() runs up to HANDLER_SKEW_MS ahead of the
// pass's `now`.
#define HANDLER_SKEW_MS 20
#define IDLE_MAX_MS (0x40000000u - 64)
void delayMicroseconds(uint32_t) {}

// The radio's time on air (radio.cpp, the SX1276 formula) for a frame of `len` bytes: only its bound matters here. At
// SF12/125 kHz the longest frame the link sends (121 bytes) takes about 4.3 s.
#define AIRTIME_MAX_MS 4500u
static uint32_t airtime[256];  // drawn once per length, then fixed: the formula is a function
static bool airtimeSet[256];
uint32_t radioAirtimeMs(size_t len) {
  uint8_t i = (uint8_t)(len > 255 ? 255 : len);
  if (!airtimeSet[i]) {
    airtime[i] = nondet_u32();
    ASSUME(airtime[i] <= AIRTIME_MAX_MS);
    airtimeSet[i] = true;
  }
  return airtime[i];
}
