#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// The application around the roles: boot, the main loop, PING/PONG, dispatch by role, the status LED and the
// status report (app.cpp). GateLink.ino only adds the watchdog and the chip registers.
void appRelaysBegin();  // first thing at boot: relays (and the LED) off
// The rest of the boot, once the watchdog runs. resetCause: PM->RCAUSE; serial: the chip's 128-bit serial number.
void appSetup(uint8_t resetCause, const uint32_t serial[4]);
void appLoop();  // one pass of the main loop

// Used by the console.
void appFillStatus(JsonObject o);
bool appPing();
void appRelayTest(uint8_t k, uint32_t ms);
// A relay pulse is running or waiting for its interlock start. Flash saves and radio restarts block the loop for
// up to ~1 s, and Relay::update doesn't run meanwhile, so they wait until this is false (the pulse would stretch).
bool appRelaysPulsing();
void appRestartRadio();
void appIdentify(uint32_t ms);  // strobe the LED so the board can be picked out on the bench
uint32_t appBootCount();  // this boot's number (configCountBoot); 0 if the flash chip didn't answer
// Heard from the peer within the link timeout (house: houseLinkTimeoutMs(), gate: link_timeout_s).
bool appLinkUp(uint32_t now);
