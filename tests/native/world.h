// The whole site on the PC: both boards running their complete firmware (everything but the radio, flash, charger
// and console drivers, and GateLink.ino), the opener, the controller (Shelly) and its power, the contact sensor and
// the radio channel between them, on one simulated clock. Invariant monitors watch every millisecond (see
// Monitors below), so any scenario that breaks a behavioural invariant from CLAUDE.md fails, not just the test
// written for it.
//
// Each board is a copy of build/node.so (the firmware plus node/api.cpp) loaded with dlopen, so the two have their
// own globals; a reset loads it again (fresh RAM) while the board's SPI flash, kept here, survives. The firmware's
// hardware calls (Arduino core, radio.h, extflash.h, supply.h, board.h, console_io.h) land in world.cpp, on the
// board making the call (Board::current()).
#pragma once
#include <ArduinoJson.h>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "testing.h"

typedef std::vector<uint8_t> Bytes;

// Pins (pins.h; A1..A4 as in stubs/Arduino.h)
enum : uint8_t { P_K1 = 1, P_K2 = 2, P_LED = 6, P_IN1 = 16, P_IN2 = 17, P_IN3 = 18, P_IN4 = 19, P_COUNT = 32 };

// GateState / Cause / Action numbers (roles.h)
enum : int { GS_UNKNOWN_ = 0, GS_CLOSED_, GS_OPEN_, GS_BETWEEN_, GS_FAULT_, GS_NO_POWER_ };

struct World;

// What a board printed on its console, parsed.
struct LogEv {
  uint32_t t;  // board millis
  uint32_t at;  // world time
  std::string ev;
  int32_t a, b;
};

struct AirFrame {
  int from;  // board index, or -1 for a transmitter of the test's own (an intruder, a neighbour)
  Bytes b;
  uint32_t start, end;
  bool dropped = false, corrupt = false, delivered = false;
  uint8_t type() const { return b.size() > 1 ? b[1] : 0; }
};

// One MKR WAN 1310 with its relay shield, radio, flash chip and charger.
struct Board {
  enum State { OFF, BOOTLOADER, RUN };
  World &w;
  int idx;
  std::string name;
  std::string so;  // this board's copy of node.so
  // Firmware entry points (node/api.cpp)
  void *dl = nullptr;
  void (*fnRelaysBegin)() = nullptr;
  void (*fnSetup)(uint8_t, const uint32_t *) = nullptr;
  void (*fnLoop)() = nullptr;
  bool (*fnParamSet)(const char *, int32_t) = nullptr;
  bool (*fnParamGet)(const char *, int32_t *) = nullptr;
  bool (*fnSave)() = nullptr;
  bool (*fnSetKey)(const uint8_t *) = nullptr;
  size_t (*fnStatus)(char *, size_t) = nullptr;
  int32_t (*fnRole)() = nullptr;
  void (*fnLog)(uint8_t, int32_t, int32_t) = nullptr;

  State state = OFF;
  uint32_t bootAt = 0;      // BOOTLOADER: when setup() runs
  uint8_t resetCause = 0;   // PM->RCAUSE for the next setup()
  uint32_t serial[4];
  uint32_t boots = 0;
  // Clock while the board runs: millis() is clockMs, delay() moves it on; the board isn't called again until the
  // world catches up (a blocked loop).
  uint32_t clockMs = 0, clockUs = 0, busyUntil = 0;
  bool wdtBite = false;  // the watchdog would have reset it: done once the call into the firmware returns
  uint32_t kickAt = 0;  // last watchdog reset (board millis)
  uint64_t prng = 1;    // random()
  uint64_t rng = 1;     // radioRandom32()
  uint32_t stuckRng = 0;  // nonzero: radioRandom32() returns this every time (a dead entropy source)

  // Supply
  bool lipo = false;
  bool cut = false;         // its own feed cut (on top of the site rail)
  uint32_t holdUpMs = 100;  // how long it runs on after VIN goes, without the LiPo
  bool vinNow = true;       // set by the world each step
  uint32_t vinLostAt = 0;   // 0 = VIN present
  bool pmicKnown = true;
  bool pmicGood = true;     // charger's power good, as last polled
  uint32_t pmicPollAt = 0;

  // Pins
  uint8_t mode[P_COUNT] = {};
  bool out[P_COUNT] = {};
  int led = -1;

  // Radio
  bool radioPresent = true;  // a module that answers
  bool radioUp = false, radioBegun = false, radioHeld = false;  // held: the flash access holds the module in reset
  bool radioFault = false;    // test: the radio resets; the next poll finds it out of LoRa mode (radio_fail a=2)
  bool radioRestartDue = false;  // radio.cpp after a fault: down until radioRecover() starts it over
  uint32_t txStart = 0, txEnd = 0;  // the last frame sent (equal: none on the air)
  std::deque<Bytes> rxq;  // at most one frame waits in the FIFO (a newer one overwrites it); empty = bad CRC
  uint32_t rxDone = 0, crcErr = 0, faults = 0;
  int16_t rssi = -60, noise = -118;
  float snr = 9.0f;

  // SPI flash (sectors 0..15)
  bool flashPresent = true;
  std::vector<uint8_t> flash;
  uint32_t eraseMs = 45;  // datasheet typical (max 400)
  int flashErases = 0, flashPrograms = 0;
  bool cutNextProgram = false;

  // Console: what we send it (per port), what it printed
  bool usbHost = true;      // a program has the USB port open (DTR)
  bool uartAdapter = false; // a USB-to-UART adapter on Serial1
  bool uartOn = false;
  std::deque<uint8_t> rx[2];
  std::string lineOut[2];
  std::vector<std::string> lines;  // every complete line, both ports (UART lines prefixed "uart:")
  std::vector<LogEv> logs;
  std::vector<JsonDocument> events;  // {"event":...} other than log
  std::map<int, JsonDocument> replies;  // by request id, until request() takes them
  int nextId = 1;

  Board(World &w, int idx, const char *name);
  ~Board();
  static Board *current();

  // Life cycle
  void load();                 // (re)load the firmware: RAM starts over
  void powerOn();              // power-on reset
  void powerOff();             // dead: outputs drop, radio off
  void reset(uint8_t rcause);  // watchdog / software / reset pin: outputs drop for the bootloader's ~0.5 s
  bool running() const { return state == RUN; }
  void tick();                 // one world millisecond: boot or run one loop pass
  void block(uint32_t us);     // the firmware waited (delay, flash, radio init)

  // Outputs as the hardware has them (a board that's off or in its bootloader drives nothing)
  bool coil(int k) const { return state == RUN && mode[k == 1 ? P_K1 : P_K2] == 1 && out[k == 1 ? P_K1 : P_K2]; }

  // Test access
  JsonDocument status();       // appFillStatus, straight from the firmware
  JsonDocument request(const std::string &cmd, const std::string &argsJson = "", uint32_t timeoutMs = 3000);
  void send(const std::string &line, int port = 0);  // raw bytes onto the console (no wait)
  bool set(const char *param, int32_t v);  // paramSet, unsaved (no radio restart: use request() for radio params)
  int32_t get(const char *param);
  int count(const char *ev) const;
  int count(const char *ev, int32_t a) const;
  const LogEv *last(const char *ev) const;
  bool waitLog(const char *ev, uint32_t maxMs, size_t since = 0);
  void onLine(int port, const std::string &line);

  template <class F> auto call(F f) -> decltype(f());
};

// The CSW24UL opener as the GateSim models it (tools/GateSim): OPEN heads for open, CLOSE for closed, reversing
// mid-travel; limits drop as soon as it leaves them; it runs on AC or its battery, and with neither it's dead
// (limits off, pulses ignored). An OPEN input held (siren, AES) keeps it open: CLOSE is ignored meanwhile.
struct Opener {
  uint32_t travelMs = 8000;
  int32_t pos = 0;  // ms of travel: 0 closed .. travelMs open
  int dir = 0;      // +1 opening, -1 closing
  bool ac = true, battery = true;
  bool stuck = false;  // jams just off the limit it leaves
  bool deaf = false;   // ignores its OPEN/CLOSE inputs (GateSim fault "deaf")
  int force[4] = { -1, -1, -1, -1 };  // gate IN1..IN4 forced off/on (-1 follows the model)
  bool in4 = false;
  // Inputs, debounced like the GateSim (20 ms)
  struct Sense {
    bool raw = false, stable = false;
    uint32_t since = 0, pressedAt = 0;
  } open, close;
  // Someone else on the inputs (AES Prime Edge, siren sensor, local button): held until
  uint32_t extOpenUntil = 0, extCloseUntil = 0;
  bool extOpenHold = false;  // siren holding OPEN indefinitely
  std::vector<std::pair<uint32_t, uint32_t>> presses[2];  // (start, length) per input: [0] OPEN, [1] CLOSE

  bool alive() const { return ac || battery; }
  bool atOpen() const { return alive() && dir == 0 && pos >= (int32_t)travelMs; }
  bool atClosed() const { return alive() && dir == 0 && pos == 0; }
  bool closedTruth() const { return atClosed(); }
  void command(int want, World &w, const char *src);
  void update(World &w, bool openIn, bool closeIn);
  bool in(int n) const;  // gate board input n (1..4) as wired: limits wetted by the opener, IN3 across the AC supply
};

// The Shelly Wave 1 on the house's 12 V rail: its relay is the Alarm.com switch (house IN1), its SW input follows
// house K1, and the rail also feeds the IN2 opto and the house board.
struct Shelly {
  enum Mode { FOLLOW, TOGGLE, DETACHED };
  Mode mode = FOLLOW;  // "toggle switch, contact closed = ON / open = OFF" (docs/hardware.md)
  bool relay = false;
  bool booted = true;
  uint32_t bootAt = 0;
  bool sw = false;          // SW level as last acted on
  bool swRaw = false;
  uint32_t swSince = 0;
  uint32_t bootMs = 1500;
  uint32_t relayDropMs = 460;  // relay contact falls this long into a 12 V cut (bench)
  uint32_t optoDropMs = 2100;  // the IN2 opto lags further (rail capacitors)
  uint32_t optoUpMs = 50;
  bool restoreSw = true;  // at boot the relay takes the SW level (else off)
  uint32_t relayDropAt = 0, optoAt = 0;
  bool opto = true;
  void update(World &w, bool rail, bool swIn);
};

// A record of everything that happened, printed when a test fails.
struct Trace {
  std::deque<std::string> lines;
  void add(uint32_t t, const std::string &s);
  void dump(size_t n = 120) const;
};

struct World {
  uint32_t now;
  Board house, gate;
  Opener opener;
  Shelly shelly;
  std::vector<AirFrame> air;
  Trace trace;
  bool traceFrames = false;

  // Site power
  bool rail12 = true;  // house 12 V: Shelly, IN2 opto, house board's buck
  uint32_t rail12OffAt = 0;
  enum GateFeed { FEED_FIXED, FEED_ACC, FEED_PSU } gateFeed = FEED_FIXED;  // gate board's buck fed from
  bool gateRail = true;  // FEED_FIXED: on unless cut
  uint32_t pgLagMs = 250;  // the charger reports VIN lost this long into a cut (bench: ~0.2 s before the relay)

  // Radio channel
  std::function<bool(const AirFrame &)> drop;     // lost on the way
  std::function<bool(const AirFrame &)> corrupt;  // heard with a bad CRC

  // User and test actions, for the provenance monitor
  struct UserCmd {
    uint32_t at;
    bool on;
    bool used;
  };
  std::vector<UserCmd> userCmds;
  struct RelayTest {
    uint32_t at;
    int k;
    uint32_t ms;
  };
  std::vector<RelayTest> relayTests[2];  // console relay.test per board

  explicit World(uint32_t start = 1000);
  ~World();

  // Bring both boards up with roles and the shared key, saved (plus whatever `configure` sets on each, also saved),
  // and wait for the link and the first STATUS; with settle, until the house is armed and its sync window over.
  void commission(const std::function<void(Board &)> &configure = nullptr, bool settle = true);
  void step();
  void run(uint32_t ms);
  bool runUntil(const std::function<bool()> &cond, uint32_t maxMs);
  Board &board(int i) { return i == 0 ? house : gate; }

  // The site
  void user(bool on);              // Alarm.com switch (the Shelly's relay, over Z-Wave)
  void extPress(bool openInput, uint32_t ms);  // another controller pulses OPEN/CLOSE
  void setRail12(bool on);
  void setGateRail(bool on);
  bool sensorClosed() const;       // the 2GIG contact sensor (house K2, sensor_invert 0)
  int houseSees() const { return houseView; }  // the gate state the house last logged (GS_*)
  int gateSees() const { return gateView; }    // the gate state the gate last logged
  bool houseLinkUp() const { return houseLink; }  // from the house's link_up / link_down log
  uint32_t houseLinkTimeoutMs();   // max(house link_timeout_s, 2.5 x gate heartbeat_s), as houseLinkTimeoutMs()
  bool alarmSwitch() const { return shelly.relay; }
  int gateTruth() const;           // GS_* from the opener's position and power
  void airSend(const Bytes &frame, int from = -1);  // a frame on the air from a transmitter of our own
  void inject(Board &to, const Bytes &frame) { to.rxq.clear(), to.rxq.push_back(frame); }
  std::vector<const AirFrame *> sent(const Board &from, uint8_t type) const;
  uint32_t airtimeMs(size_t len, int sf = 9, int bwHz = 500000, int cr = 5) const;

  // Monitors: the behavioural invariants, checked every millisecond. A breach is recorded (and traced); tests end
  // with checkClean() (the destructor of a test that passed checks too).
  struct Limits {
    uint32_t pulseMaxMs = 0;          // 0: the gate's pulse_ms + slack; a relay.test may run longer
    uint32_t sensorGraceMs = 3000;    // house K2 may lag the gate leaving its closed limit by this much
    bool provenance = true;           // every command traced to a user action
    bool sensorTruth = true;
    bool notClosedDisplay = true;
  } limits;
  std::vector<std::string> violations;
  void violate(const std::string &what);
  void checkClean();

  // Monitor state
  struct CoilTrack {
    bool on = false;
    uint32_t onAt = 0, offAt = 0, maxMs = 0;
    bool reported = false;
  } gk[2];
  uint32_t k2WrongSince = 0;
  int houseView = GS_UNKNOWN_;  // the house's idea of the gate (its gate_state log)
  uint32_t houseViewAt = 0;
  int gateView = GS_UNKNOWN_;
  bool houseLink = false;
  uint32_t k2InconsistentSince = 0;
  uint32_t displayWrongSince = 0;
  uint32_t resyncUntil = 0;  // the house is cycling K1 to resync the controller
  std::map<int32_t, int> pulsesPerCmd;
  int32_t lastGateCmdRx = -1;
  uint32_t lastGateCmdRxAt = 0;
  void monitor();
  void onLog(Board &b, const LogEv &e);
};

extern World *world;

// The shared link key (16 bytes) commission() gives both boards.
extern const uint8_t TEST_KEY[16];
