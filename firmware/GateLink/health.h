#pragma once
#include <Arduino.h>

// The board's health, and the fault output on D5 (fault_out) that tells the alarm system about it: HIGH while
// healthy, LOW otherwise, so a dead board, a cut wire or a reset reads as a fault too. A problem must last
// fault_hold_s before D5 drops (a brief blip doesn't trip the alarm); recovery raises it at once. After boot it stays
// LOW until the role has decided (house: armed; gate: inputs settled, the first STATUS sent).
//
// Diagnostics for the alarm only: this reads the roles' state, and nothing in the roles reads it. D5 is not K1/K2 and
// commands nothing.

// Problems, as bits (status `health` names them, log `health` b carries them).
enum Problem : uint8_t {
  PROB_STARTING = 0x01,  // the role hasn't decided yet since boot (or no role is set)
  PROB_RADIO = 0x02,     // the radio isn't working (radioOk() false)
  PROB_LINK = 0x04,      // the link is down (appLinkUp)
  PROB_AC = 0x08,        // gate: no AC power (IN3, with power_sense); house: the gate's STATUS says so
  PROB_NO_POWER = 0x10,  // the gate reads no_power (no AC and no limit)
  PROB_FAULT = 0x20,     // the gate reads fault (both limits)
};
#define PROBLEM_COUNT 6

void healthBegin();  // at boot, once the config is loaded: D5 LOW (fault_out on) or an input with its pull-down
// Every loop pass, after the roles: re-evaluates the problems and drives D5 (also following a changed fault_out or
// fault_hold_s at once). linkUp: appLinkUp(now) (false with no role).
void healthPoll(uint32_t now, bool linkUp);
uint8_t healthProblems();  // as of the last poll
const char *healthName(uint8_t bit);  // name of problem bit number `bit` (0 = PROB_STARTING)
int8_t healthFaultOut();   // D5 as driven: 1 HIGH, 0 LOW, -1 not driven (fault_out off)
