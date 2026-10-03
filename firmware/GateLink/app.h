#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Glue implemented in GateLink.ino, used by the console.
void appFillStatus(JsonObject o);
bool appPing();
void appRelayTest(uint8_t k, uint32_t ms);
void appRestartRadio();
void appIdentify(uint32_t ms);
// Heard from the peer within the link timeout (house: houseLinkTimeoutMs(), gate: link_timeout_s).
bool appLinkUp(uint32_t now);  // strobe the LED so the board can be picked out on the bench
