#pragma once
#include <Arduino.h>

// Authenticated point-to-point link over LoRa.
//
// Frame: ver | type | net_id | src | dst | session(u32) | seq(u32) | payload | tag(8)
// tag = HMAC-SHA256(key, header+payload) truncated to 8 bytes.
//
// Replay protection without persistent counters: each node picks a random session
// id at boot. A frame is accepted only if its session matches the peer session we
// have verified and its seq is new: above the HELLO_ACK that verified the session,
// and not seen before in a 32-frame sliding window. Unknown sessions are verified
// with a HELLO challenge that the peer must echo in a MAC'd HELLO_ACK; until one
// answers, the verified session stays in place (a replayed HELLO can't drop it).

enum MsgType : uint8_t {
  MSG_HELLO = 1,      // challenge u32
  MSG_HELLO_ACK = 2,  // challenge u32
  MSG_ACK = 3,        // acked seq u32, result u8
  MSG_CMD = 4,        // cmd_id u16, action u8                (reliable, house->gate)
  MSG_STATUS = 5,     // see roles.h                          (reliable, gate->house)
  MSG_PING = 6,       // ping_id u16
  MSG_PONG = 7,       // ping_id u16, rssi i16, snr i8
  MSG_DIAG_REQ = 8,   //                                       (house->gate)
  MSG_DIAG = 9,       // see sendDiag() in role_gate.cpp      (gate->house)
  MSG_CFG_SET = 10,   // param id u8, value i32               (reliable, house->gate)
};

enum Slot : uint8_t { SLOT_CMD = 0, SLOT_STATUS, SLOT_CFG, SLOT_COUNT };

// ACK result codes
enum AckResult : uint8_t {
  RES_OK = 0,
  RES_ALREADY = 1,   // command matched current state; no pulse
  RES_BAD = 2,       // malformed / rejected
  // 3 was RES_BUSY (never produced); the numbering is part of the wire format
  RES_NO_POWER = 4,  // no AC power (IN3 power sense); no pulse
  RES_NOT_SAVED = 5,  // CFG_SET applied, but the flash save failed: reverts at the next reboot
};

struct RxMsg {
  uint8_t type;
  uint32_t seq;
  const uint8_t *payload;
  uint8_t len;
  int16_t rssi;
  float snr;
};

struct LinkStats {
  uint32_t tx, rx, macFail, replay, retries, giveups, sessions;
  uint32_t lbtDefers, lbtForced;  // frames held for a busy channel; sent anyway after the cap
  int16_t lastRssi;
  float lastSnr;
  uint32_t lastRxAt;  // millis of last authenticated frame from peer (0 = never)
};

typedef void (*RxHandler)(const RxMsg &msg);
// Called when a reliable send completes (acked) or gives up (acked = false).
typedef void (*AckHandler)(Slot slot, uint8_t type, bool acked, uint8_t result);

void linkBegin(RxHandler rx, AckHandler ack);
void linkPoll(uint32_t now);
void linkSend(uint8_t type, const uint8_t *payload, uint8_t len);
// Queue a reliable message; replaces any message pending in the same slot.
void linkSendReliable(Slot slot, uint8_t type, const uint8_t *payload, uint8_t len, uint32_t ttlMs);
bool linkPending(Slot slot);
// Acknowledge a reliable message (call from RxHandler).
void linkAck(uint32_t seq, uint8_t result);
const LinkStats &linkStats();
bool linkPeerVerified();
// Debug: retransmit the last frame verbatim (peer must reject it as a replay).
void linkDebugReplay();

// Little-endian helpers
inline void putU16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void putU32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
inline uint16_t getU16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline uint32_t getU32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// Signed elapsed-time check: safe across millis() wrap, and when t was stamped slightly after `now` (handlers
// called from linkPoll use millis(), the loop's `now` is older).
inline bool elapsed(uint32_t now, uint32_t t, uint32_t ms) { return (int32_t)(now - t) >= (int32_t)ms; }
