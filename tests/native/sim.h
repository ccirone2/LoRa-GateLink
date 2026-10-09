// Host test harness: a house and a gate running the real link.cpp over a simulated channel, with a shared clock,
// plus a fake SPI NOR flash for config.cpp.
//
// link.cpp keeps its state in file-scope statics, so it is compiled twice, into namespaces `house_link` and
// `gate_link` (link_house.cpp, link_gate.cpp); each node calls its own copy through a LinkApi. The globals the
// link reads (cfg, activeRole) and the fakes it calls (radio*, logEvent, millis) are shared: Node::Ctx swaps the
// node's config in and points the fakes at it for the duration of each call.
#pragma once
#include <Arduino.h>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "config.h"
#include "link_types.h"
#include "log.h"

extern uint32_t simNow;  // millis()

struct LinkApi {
  void (*begin)(RxHandler, AckHandler);
  void (*poll)(uint32_t);
  void (*send)(uint8_t, const uint8_t *, uint8_t);
  void (*sendReliable)(Slot, uint8_t, const uint8_t *, uint8_t, uint32_t);
  bool (*pending)(Slot);
  void (*ack)(uint32_t, uint8_t);
  void (*ackLater)(uint32_t);
  const LinkStats &(*stats)();
  bool (*verified)();
};
LinkApi houseLinkApi();
LinkApi gateLinkApi();

typedef std::vector<uint8_t> Bytes;

struct AirFrame {
  int from;  // node index
  Bytes b;
  uint32_t start, end;
  bool dropped = false;   // lost on the way (Sim::drop): the receiver never hears it
  bool collided = false;  // the receiver was transmitting meanwhile (half duplex)
  bool delivered = false;
  uint8_t type() const { return b[1]; }
  uint32_t session() const { return getU32(&b[5]); }
  uint32_t seq() const { return getU32(&b[9]); }
};

struct LogRec {
  uint32_t t;
  uint8_t code;
  int32_t a, b;
};
struct RxRec {
  uint32_t t;
  uint8_t type;
  uint32_t seq;
  Bytes payload;
};
struct AckRec {
  uint32_t t;
  Slot slot;
  uint8_t type;
  bool acked;
  uint8_t result;
};

struct Node {
  int idx;
  const char *name;
  LinkApi api;
  Config cfg;
  // Radio
  bool radioUp = true;
  uint32_t txEnd = 0;
  std::deque<Bytes> rxq;
  uint64_t rng;
  // What the link handed up
  std::vector<LogRec> logs;
  std::vector<RxRec> rx;
  std::vector<AckRec> acks;
  // Called for each received message (in the node's context). Default: ACK reliable ones with RES_OK.
  std::function<void(Node &, const RxMsg &)> onRx;

  // Makes this node current for the fakes and the link globals while it lives.
  struct Ctx {
    explicit Ctx(Node &n);
    ~Ctx();
    Node *prev;
    bool same;  // already current (a handler calling back into its own link)
    Config prevCfg;
    int32_t prevRole;
  };

  void begin();  // linkBegin: a boot, or a radio/key restart
  void sendReliable(Slot slot, uint8_t type, const Bytes &payload, uint32_t ttlMs);
  void send(uint8_t type, const Bytes &payload);
  void ack(uint32_t seq, uint8_t result);
  void ackLater(uint32_t seq);
  bool pending(Slot slot);
  LinkStats stats();
  bool verified();

  int count(LogCode code) const;
  int count(LogCode code, int32_t a) const;
  int rxCount(uint8_t type) const;
};

struct Sim {
  Node house, gate;
  std::vector<AirFrame> air;
  // Decides, when a frame starts, whether it gets through. Default: everything does.
  std::function<bool(const AirFrame &)> drop;

  explicit Sim(uint32_t start = 1000);
  ~Sim();
  Node &peer(const Node &n) { return n.idx == 0 ? gate : house; }
  Node &node(int idx) { return idx == 0 ? house : gate; }

  void step(uint32_t ms = 1);  // advance the clock, deliver finished frames, poll both nodes
  void run(uint32_t ms);
  // Steps until cond() holds; false if it didn't within maxMs.
  bool runUntil(const std::function<bool()> &cond, uint32_t maxMs);
  bool handshake(uint32_t maxMs = 5000) {
    return runUntil([&] { return house.verified() && gate.verified(); }, maxMs);
  }
  // Straight into `to`'s receiver, as if heard off the air (a recorded frame replayed).
  void inject(Node &to, const Bytes &frame) { to.rxq.push_back(frame); }
  std::vector<const AirFrame *> sent(const Node &from, uint8_t type) const;
  void dumpAir(uint32_t since = 0) const;  // debugging aid: every frame since then, one per line

  void deliver();
};

extern Sim *sim;

// The fake SPI flash (sectors 0-3: config records and boot counter).
struct FakeFlash {
  bool present = true;
  uint8_t mem[4 * 4096];
  int garbleReads = 0;       // the next N reads return zeros (bus garbled by the radio module's MCU)
  bool cutNextProgram = false;  // the next program writes only its first half and fails (power cut mid-save)
  int erases = 0, programs = 0;
  void reset();
};
extern FakeFlash flash;

uint32_t airtimeMs(size_t len);  // LoRa time on air at cfg's SF/BW/CR

// --- minimal test framework ---------------------------------------------------------------------------------
struct TestCase {
  const char *name;
  void (*fn)();
  const char *xfail;  // known failure (reason); the run fails if it unexpectedly passes
};
std::vector<TestCase> &registry();
struct Reg {
  Reg(const char *n, void (*f)(), const char *xfail = nullptr) { registry().push_back({ n, f, xfail }); }
};
struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};
std::string where(const char *file, int line, const std::string &what);

#define TEST(name) \
  static void name(); \
  static Reg reg_##name(#name, name); \
  static void name()
// A test that documents a known bug (REVIEW.md / TODO.md item): it must fail until the fix lands.
#define XFAIL_TEST(name, reason) \
  static void name(); \
  static Reg reg_##name(#name, name, reason); \
  static void name()
#define CHECK(c) \
  do { \
    if (!(c)) throw Failure(where(__FILE__, __LINE__, "CHECK(" #c ")")); \
  } while (0)
#define CHECK_EQ(a, b) \
  do { \
    long long a_ = (long long)(a), b_ = (long long)(b); \
    if (a_ != b_) \
      throw Failure(where(__FILE__, __LINE__, \
                          "CHECK_EQ(" #a ", " #b "): " + std::to_string(a_) + " != " + std::to_string(b_))); \
  } while (0)
// Inclusive range check.
#define CHECK_IN(v, lo, hi) \
  do { \
    long long v_ = (long long)(v), lo_ = (long long)(lo), hi_ = (long long)(hi); \
    if (v_ < lo_ || v_ > hi_) \
      throw Failure(where(__FILE__, __LINE__, \
                          "CHECK_IN(" #v "): " + std::to_string(v_) + " not in [" + std::to_string(lo_) + ", " + \
                              std::to_string(hi_) + "]")); \
  } while (0)
