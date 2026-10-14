#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "io.h"
#include "link.h"

enum GateState : uint8_t { GS_UNKNOWN = 0, GS_CLOSED, GS_OPEN, GS_BETWEEN, GS_FAULT, GS_NO_POWER, GS_COUNT };
enum Cause : uint8_t { CAUSE_NONE = 0, CAUSE_LORA, CAUSE_EXTERNAL };
enum Action : uint8_t { ACT_OPEN = 1, ACT_CLOSE = 2 };
enum TravelResult : uint8_t { TR_NONE = 0, TR_REACHED, TR_TIMEOUT, TR_ALREADY };

// STATUS payload layout (gate -> house)
#define ST_STATE 0
#define ST_INPUTS 1   // STI_* bits
#define ST_CAUSE 2
#define ST_RESULT 3
#define ST_CMD_ID 4   // u16
#define ST_UPTIME 6   // u32 seconds
#define ST_RSSI 10    // i16, RSSI at gate of last frame from house (0 = none yet)
#define ST_SNR 12     // i8
#define ST_TARGET 13
#define ST_HEARTBEAT 14  // u16, gate heartbeat_s: the house's link timeout must cover it
#define ST_LEN_V1 16     // up to 0.3.x; the fields below were added in 0.4.0
#define ST_RETRIES 16    // u16, gate link retries (running total, low 16 bits; resets when its link restarts)
#define ST_GIVEUPS 18    // u16, gate link giveups (same)
#define ST_CRC 20        // u16, CRC errors at the gate radio (same)
#define ST_NOISE 22      // i8, gate noise floor since its previous STATUS, dBm (0 = no sample)
#define ST_NOISE_MAX 23  // i8, its peak
#define ST_LEN_V2 24     // 0.4.0 up to 0.12.x; the field below was added in 0.13.0
#define ST_TRAVEL 24     // u16, gate travel_timeout_s: the house holds K1 through a travel for as long
#define ST_LEN_V3 26     // 0.13.0 up to 0.15.x; the field below was added in 0.16.0
#define ST_CLOCK 26      // u48, the gate's clock (ms, from a random origin each boot): commands' deadlines are in it
#define ST_LEN_V4 32     // 0.16.x; the field below was added in 0.17.0
#define ST_LEFT_CLOSED 32  // u8, times the gate's reported state has left closed since its boot (mod 256)
#define ST_LEN 33

// ST_INPUTS bits: the gate's inputs and relays as it sees them
#define STI_IN1 0x01      // open limit
#define STI_IN2 0x02      // close limit
#define STI_K1 0x04
#define STI_K2 0x08
#define STI_IN3 0x10      // AC power sense
#define STI_IN4 0x20
#define STI_AC_LOST 0x40  // 0.7.0
#define STI_REBOOTS 0x80  // 0.15.0

// CMD payload (house -> gate): cmd_id u16, action u8, then the deadline (link.h). 9 bytes keep the frame at 30, which
// fits the gaps a busy neighbour leaves (test_system_link.cpp): a 34-byte one no longer did.
#define CMD_DEADLINE 3  // u48, the gate's clock by which it must arrive: when it was sent plus the house's cmd_ttl_s
#define CMD_LEN 9
// The gate takes a command until CMD_LATE_SLACK_MS past its deadline, plus a STATUS's and a command's time on the air
// (the house's reckoning of the gate's clock lags it a little: a STATUS's backoff and airtime, the crystals drifting
// apart since; and the command takes its airtime to arrive), and refuses one whose
// deadline lies further ahead than the longest cmd_ttl_s allows (CMD_TTL_MAX_S, its PARAMS maximum: test_config
// checks they agree).
#define CMD_TTL_MAX_S 120
#define CMD_LATE_SLACK_MS 2000
#define CMD_AHEAD_MAX_MS (CMD_TTL_MAX_S * 1000L + CMD_LATE_SLACK_MS)

// DIAG payload layout (gate -> house, answering DIAG_REQ; built by sendDiag, parsed by consoleEventDiag)
#define DIAG_FW 0        // u8 x3: major, minor, patch
#define DIAG_UPTIME 3    // u32 seconds
#define DIAG_COUNTERS 7  // u16 x DC_COUNT, saturating, in DiagCounter order
enum DiagCounter : uint8_t { DC_TX = 0, DC_RX, DC_MAC_FAIL, DC_REPLAY, DC_RETRIES, DC_GIVEUPS, DC_COUNT };
#define DIAG_HDR (DIAG_COUNTERS + 2 * DC_COUNT)  // then (param id u8, value i32) per remote-writable param
static_assert(DIAG_HDR == 19, "DIAG header changed: both boards must be updated together");

extern Input in1, in2, in3, in4;
extern Relay k1, k2;

// Debounce IN3/IN4 (gate IN3 = AC power sense) and log edges. Returns true if either changed.
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
void houseRemoteSet(uint8_t id, int32_t value);
uint32_t houseLinkTimeoutMs();
uint32_t houseStatusAt();  // millis() when the last STATUS was taken (0 = none yet): the link's liveness (appLinkUp)
void houseLinkRestarted();  // our link started over (a key or radio setting): down until the gate's next STATUS
// For health.cpp, which reads these and is read by nothing here: armed at least once since boot (the first STATUS
// plus its sync window; a K1 relay test disarming it for a while doesn't count), the gate's state as last reported
// (GS_UNKNOWN before its first STATUS), and whether that STATUS said AC lost.
bool houseDecided();
uint8_t houseGateState();
bool houseGateAcLost();
bool houseGateRebootLoop();  // the gate's latest STATUS says it is in a reboot loop

void gateBegin();
void gateLoop(uint32_t now);
void gateOnRx(const RxMsg &m);
void gateOnAck(Slot slot, uint8_t type, bool acked, uint8_t result);
void gateStatus(JsonObject o);
bool gateRelayTest(uint8_t k, uint32_t ms);  // false: that relay is pulsing already (busy)
void gateDebugRebootAfterCmd();  // bench: the next pulsed command resets the gate before its ACK
uint32_t gateLinkTimeoutMs();  // nothing heard from the house for this long: the link is down (appLinkUp)
// For health.cpp (as above): the inputs have settled since boot and the first STATUS has gone out (not `settling`),
// the state as reported, and AC power (IN3, or true with power_sense off).
bool gateDecided();
uint8_t gateCurrentState();
bool gateAcPresent();
