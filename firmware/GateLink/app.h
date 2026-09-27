#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Glue implemented in GateLink.ino, used by the console.
void appFillStatus(JsonObject o);
bool appPing();
void appRelayTest(uint8_t k, uint32_t ms);
void appRestartRadio();
