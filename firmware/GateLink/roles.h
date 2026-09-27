#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "io.h"
#include "link.h"

enum GateState : uint8_t { GS_UNKNOWN = 0, GS_CLOSED, GS_OPEN, GS_BETWEEN, GS_FAULT };
enum Cause : uint8_t { CAUSE_NONE = 0, CAUSE_LORA, CAUSE_EXTERNAL };
enum Action : uint8_t { ACT_OPEN = 1, ACT_CLOSE = 2 };
enum TravelResult : uint8_t { TR_NONE = 0, TR_REACHED, TR_TIMEOUT, TR_ALREADY };

// STATUS payload layout (gate -> house)
#define ST_STATE 0
#define ST_INPUTS 1   // bit0 in1, bit1 in2, bit2 k1, bit3 k2, bit4 in3, bit5 in4
#define ST_CAUSE 2
#define ST_RESULT 3
#define ST_CMD_ID 4   // u16
#define ST_UPTIME 6   // u32 seconds
#define ST_RSSI 10    // i16, RSSI at gate of last frame from house
#define ST_SNR 12     // i8
#define ST_TARGET 13
#define ST_LEN 14

extern Input in1, in2, in3, in4;
extern Relay k1, k2;

// Debounce the spare inputs (IN3/IN4) and log edges. Returns true if either changed.
bool updateSpareInputs(uint32_t now);

const char *gateStateName(uint8_t s);
const char *causeName(uint8_t c);
const char *resultName(uint8_t r);

void houseBegin();
void houseLoop(uint32_t now);
void houseOnRx(const RxMsg &m);
void houseOnAck(Slot slot, uint8_t type, bool acked, uint8_t result);
void houseStatus(JsonObject o);
void houseRelayTest(uint8_t k, uint32_t ms);
void houseRemoteDiag();
bool houseRemoteSet(uint8_t id, int32_t value);

void gateBegin();
void gateLoop(uint32_t now);
void gateOnRx(const RxMsg &m);
void gateOnAck(Slot slot, uint8_t type, bool acked, uint8_t result);
void gateStatus(JsonObject o);
void gateRelayTest(uint8_t k, uint32_t ms);
