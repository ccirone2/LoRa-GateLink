// What the fuzz targets share (harness.cpp): fresh firmware state per input, the provisioned flash images, booting
// the board, playing its peer over the radio, and running the loop.
//
// Fresh state: in the libFuzzer build (FUZZ_SNAPSHOT) the firmware's globals sit in two sections (sections.h) that
// are copied back before every input. The g++ replay build has no such sections; replay_main.cpp runs each input
// in a fork()ed child of a process that never ran firmware code, and the harness checks it is used that way.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include "hal.h"

enum FuzzRole : uint8_t { FR_HOUSE = 0, FR_GATE = 1 };

// The link key the provisioned images carry (16 bytes).
extern const uint8_t FUZZ_KEY[16];
#define WRAP_START_MS (0u - 35000u)  // millis() wraps ~20-30 s into an input that starts there

// Call from LLVMFuzzerInitialize: harnessSetup() first (RAM snapshots, provisioned images, warm boards), then
// harnessSelfTest() with two sample inputs for the target, which checks that every input starts afresh (the same
// input gives the same output whatever ran before it). The self-test needs the snapshots: a no-op without them.
typedef int (*FuzzOne)(const uint8_t *data, size_t size);
void harnessSetup();
void harnessSelfTest(FuzzOne one, const Bytes &sampleA, const Bytes &sampleB);

// Start an input from fresh firmware RAM and a fresh HAL (clock at startMs). Nothing runs yet.
void startBlank(uint32_t startMs);
// The provisioned flash image (role and FUZZ_KEY saved) for a role.
const Bytes &provisionedImage(FuzzRole role);
// Start an input from a board that booted with the image and the given inputs, nothing else.
void startCold(FuzzRole role, bool wrap, uint8_t pins);
// Start an input from a board that is up, verified with the peer and settled: the gate has sent its first
// STATUS (ACKed), the house has had one (gate closed) and is armed. Inputs: house IN2 (controller power); gate IN2
// (closed limit) and IN3 (AC).
void startWarm(FuzzRole role, bool wrap);

// Boot the board as GateLink.ino does (relays off, then appSetup).
void boot(uint8_t resetCause);
void setPins(uint8_t pins);  // bits 0..3: IN1..IN4

// The loop. Every pass: the clock moves on stepMs, the watchdog is reset, appLoop() runs, then the monitors.
// A boardReset() unwinds out of these as BoardReset. They stop (return false) once the input's pass budget is
// spent.
bool pass(uint32_t stepMs = 1);
bool advance(uint32_t ms);  // 1 ms steps while a relay is on or pulsing, coarser otherwise
bool settle();               // until the board has taken every queued frame and console byte, then two more
void finish();               // relays still on at the end of an input: run until they release
bool budgetLeft();

// The peer the harness plays (the other board), with a session of its own.
struct Peer {
  uint32_t session = 0;
  uint32_t seq = 0;  // the last seq used
  uint32_t boardSession = 0;
  bool haveChallenge = false;
  uint32_t challenge = 0, prevChallenge = 0;  // the board's latest HELLO challenges
  bool haveSeq[16] = {};
  uint32_t lastSeq[16] = {};  // the board's latest frame per type
  std::vector<Bytes> sent;    // our frames, for replays (the last 32)
  std::vector<Bytes> heard;   // the board's frames (the last 16)
};
extern Peer peer;

// Look at what the board sent since the last call (called by pass()).
void peerScan();
// Build an authenticated frame from the peer: header for this board, HMAC-SHA256 tag (truncated to 8 bytes) with
// the board's current key, as link.cpp computeTag.
Bytes peerFrame(uint8_t type, const uint8_t *payload, size_t len, uint32_t session, uint32_t seq);
void deliver(const Bytes &frame, int16_t rssi = -60, float snr = 9.0f);  // into the board's receive queue
void sendAuth(uint8_t type, const uint8_t *payload, size_t len);  // from our session, next seq
void answerChallenge();  // HELLO_ACK echoing the board's latest challenge
void ackLast(uint8_t type, uint8_t result);  // ACK the board's latest frame of a type
// A STATUS payload (roles.h layout, 26 bytes): gate closed, AC on, heartbeat 30 s, travel 60 s.
Bytes statusPayload(uint8_t state, uint8_t inputs);

// Console
void consoleLine(int port, const std::string &line);  // bytes + '\n' into the port's receive queue
std::string withCrc(const std::string &json);          // appends ,"crc":"xxxxxxxx" as console.cpp checks it

// Reads an input front to back; past the end everything reads 0.
struct Reader {
  const uint8_t *p;
  size_t n;
  bool empty() const { return n == 0; }
  uint8_t u8() {
    if (!n) return 0;
    n--;
    return *p++;
  }
  uint16_t u16() {
    uint16_t lo = u8();
    return (uint16_t)(lo | (u8() << 8));
  }
  uint32_t u32() {
    uint32_t lo = u16();
    return lo | ((uint32_t)u16() << 16);
  }
  Bytes bytes(size_t len) {
    if (len > n) len = n;
    Bytes b(p, p + len);
    p += len;
    n -= len;
    return b;
  }
};
