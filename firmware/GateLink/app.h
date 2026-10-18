#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// The application around the roles: boot, the main loop, PING/PONG, dispatch by role, the status LED and the
// status report, the reset memo (app.cpp). GateLink.ino only adds the watchdog and the chip registers.
void appRelaysBegin();  // first thing at boot: relays (and the LED) off
// The rest of the boot, once the watchdog runs. resetCause: PM->RCAUSE; serial: the chip's 128-bit serial number.
void appSetup(uint8_t resetCause, const uint32_t serial[4]);
void appLoop();  // one pass of the main loop
// From the watchdog's early-warning interrupt, 4 s into a loop pass that hasn't kicked it (the reset follows at 8 s):
// notes the interrupted program counter and the loop's stage for the next boot. Only writes RAM.
void appWatchdogWarning(uint32_t pc);
// In a reboot loop: REBOOT_LOOP_RESETS watchdog or brown-out resets in a row, each within REBOOT_SETTLE_MS of the
// boot before, and this boot hasn't run REBOOT_SETTLE_MS yet. Health problem `reboots`; the gate reports it in STATUS.
bool appRebootLoop();
// A few flags the role keeps for the boot after a crash (a watchdog or brown-out reset), in the reset memo: appKept()
// is what the boot before last kept, 0 after a deliberate reset (software, the reset pin) or a power-on.
uint8_t appKept();
void appKeep(uint8_t flags);
#define KEEP_CLOSED_UNPROVEN 0x01  // gate: the both-limits hold (role_gate.cpp closedUnproven)
#define REBOOT_LOOP_RESETS 3
#define REBOOT_SETTLE_MS 3600000UL

// Used by the console.
void appFillStatus(JsonObject o);
bool appPing();
bool appRelayTest(uint8_t k, uint32_t ms);  // false: on the gate, that relay is pulsing already (busy)
// A relay pulse is running or waiting for its interlock start. Flash saves and radio restarts block the loop for
// up to ~1 s, and Relay::update doesn't run meanwhile, so they wait until this is false (the pulse would stretch):
// console requests (console.cpp blocksLoop), remote config writes (role_gate.cpp), radio recovery and the history's
// writes to the flash (appLoop).
bool appRelaysPulsing();
void appRestartRadio();
void appIdentify(uint32_t ms);  // strobe the LED so the board can be picked out on the bench
uint32_t appBootCount();  // this boot's number (configCountBoot); 0 if the flash chip didn't answer or it ran out
// Heard from the peer within the link timeout: the house, a STATUS taken (houseStatusAt) within houseLinkTimeoutMs();
// the gate, any authenticated frame within link_timeout_s.
bool appLinkUp(uint32_t now);
