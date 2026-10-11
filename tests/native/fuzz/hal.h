// One board's hardware for the fuzz targets: everything the firmware calls that isn't firmware (the Arduino core,
// board.h, radio.h, extflash.h, supply.h, console_io.h). Simpler than world.cpp (one board, no site): the harness
// plays the peer by putting frames into the radio's receive queue and reading what the board sent, feeds the
// console and moves the clock. Timings (airtime, flash) are world.cpp's, radio init the board's (LoRa.begin() on
// the MKR WAN 1310 waits 450 ms; world.cpp takes 25 ms), so a loop that blocks while a relay pulses shows up as a
// longer pulse, as it would on the board.
//
// The cheap behavioural invariants are checked here, as the firmware drives the pins (halTrap on a breach).
#pragma once
#include <stdint.h>
#include <deque>
#include <string>
#include <vector>

typedef std::vector<uint8_t> Bytes;

// Pins (pins.h, with A1..A4 as stubs/Arduino.h numbers them)
enum : uint8_t { HP_K1 = 1, HP_K2 = 2, HP_LED = 6, HP_IN1 = 16, HP_IN2, HP_IN3, HP_IN4, HP_COUNT = 32 };

// Sectors 0..19: config records (0, 1), the boot counter (2, 3) and the history log (4..19): config.h's flash map
#define HAL_FLASH_BYTES (20 * 4096u)
#define HAL_WATCHDOG_MS 8000
#define HAL_INTERLOCK_MS 100  // role_gate.cpp INTERLOCK_MS
#define HAL_PULSE_SLACK_MS 50
// LoRa.begin() on the MKR WAN 1310 (LoRa library): the module's reset, delay(200) + delay(200) + delay(50), then
// SPI and register setup. radio.cpp's radioBegin() runs it for every (re)start: CLAUDE.md's "about 0.5 s off the air".
#define HAL_RADIO_INIT_US 451000

// Thrown by boardReset() (the console's reboot, debug.reboot_after_cmd): the input ends there.
struct BoardReset {
  uint8_t cause;
};

struct RxFrame {
  Bytes b;  // empty: received with a bad CRC
  int16_t rssi;
  float snr;
};

// Gate relay monitor (CLAUDE.md: pulsed only, never restarted, never both, interlocked).
struct RelayMon {
  bool on = false;
  uint32_t onAt = 0;
  uint32_t allowMs = 0;    // pulse_ms, or a relay.test's ms, when it went on
  bool offSeen = false;
  uint32_t offAt = 0;
};

struct Hal {
  // Clock: millis() is ms; delay() and the blocking fakes move it on.
  uint32_t ms = 1000, us = 0;
  uint32_t kickAt = 1000;  // last watchdog reset
  uint64_t prng = 1;       // random()
  uint64_t rng = 1;        // radioRandom32()
  // Pins
  uint8_t mode[HP_COUNT] = {};
  bool out[HP_COUNT] = {};
  bool in[HP_COUNT] = {};  // input levels the target sets
  int led = -1;
  // Supply (the charger's power good)
  bool supplyKnown = true, supplyGood = true, supplySeen = true;
  uint32_t supplyPollAt = 0;
  // Radio
  bool radioPresent = true;  // the module answers (false: radioBegin fails, retried every 5 s)
  bool radioUp = false, radioBegun = false, radioHeld = false;
  bool restartDue = false;   // after a fault: down until the firmware calls radioRecover()
  bool jammed = false;       // the channel reads busy (a carrier that never ends: listen-before-talk is forced)
  uint32_t txEnd = 0, retryAt = 0;
  uint32_t rxDone = 0, crcErr = 0, faults = 0;
  int16_t noise = -118;
  std::deque<RxFrame> rxq;   // frames waiting for radioReceive
  std::vector<Bytes> tx;     // frames the board sent, for the harness (it clears them)
  uint32_t txCount = 0;
  // SPI flash
  Bytes flash = Bytes(HAL_FLASH_BYTES, 0xFF);
  bool flashPresent = true;
  // Console
  bool usbHost = true, uartAdapter = true, uartOn = false;
  std::deque<uint8_t> rx[2];
  std::string lineIn[2];   // the request line the firmware is reading (for the relay.test allowance)
  std::string lineOut[2];
  bool keepLines = false;  // keep every output line in `lines`
  std::vector<std::string> lines;
  uint32_t lineCount = 0;
  // Everything the board emitted (console lines, frames), hashed: the determinism self-test compares it.
  uint64_t outHash = 1469598103934665603ULL;
  // Monitors
  RelayMon relay[2];
  // Console relay.test requests per relay (k1, k2) that ran: the last few, read as the firmware reads them, and
  // kept once their reply says ok (a bad crc, a missing crc on the UART, a line too long: refused, not kept).
  struct TestAsk {
    bool valid = false;
    uint32_t at = 0, ms = 0;
  } test[2][4];
  // The relay.test request each port is handling, until its reply comes (the pulse starts before the reply).
  struct PendingAsk {
    bool valid = false;
    uint8_t k = 0;
    uint32_t at = 0, ms = 0;
  } asked[2];
  bool houseK2Test = false;  // house K2 is on for a relay.test: the sensor check is off until K2 next releases
  // GATELINK_FUZZ_KNOWN_BUGS: also trap on the known, reported firmware bugs (README.md, Known bugs). Fuzzing
  // tolerates exactly those, so it can look for others; the replay of fuzz/crashes sets it.
  bool knownBugs = false;
  bool trace = false;        // GATELINK_FUZZ_TRACE: print what happens (console lines, frames, relays) to stderr
  // The house's idea of the gate and the link, from its gate_state / link_up / link_down log events.
  int32_t houseView = 0;  // GS_*
  bool houseLink = false;
  uint64_t passes = 0;
};

extern Hal hal;

// Fresh hardware: clock at startMs, blank flash, inputs low, radio absent until radioBegin.
void halReset(uint32_t startMs);
// Report a breach (or a harness inconsistency) and stop: a crash libFuzzer records.
[[noreturn]] void halTrap(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// After every loop pass: the invariants that depend on time passing.
void halCheckPass();
uint32_t halAirtimeMs(size_t len);
// radio.cpp's fault(): a TX that never finished, or a reset seen in RX. Counted, logged, and down until the firmware
// calls radioRecover() (appLoop, once no relay pulses).
void halRadioFault();
void halHash(const void *p, size_t n);
// With GATELINK_FUZZ_TRACE: a line on stderr, stamped with the board's millis().
void halTrace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void halTraceFrame(const char *dir, const Bytes &f);
