#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Glue implemented in GateLink.ino, used by the console.
void appFillStatus(JsonObject o);
bool appPing();
void appRelayTest(uint8_t k, uint32_t ms);
void appRestartRadio();
void appIdentify(uint32_t ms);  // strobe the LED so the board can be picked out on the bench
