// The simulated site (world.h): both boards' hardware as the firmware sees it, the devices around them, and the
// invariant monitors.
#include "world.h"
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
#include <unistd.h>
#include <exception>
#include <filesystem>
#include <Arduino.h>
#include "board.h"
#include "console_io.h"
#include "extflash.h"
#include "log.h"
#include "radio.h"
#include "supply.h"

World *world = nullptr;
const uint8_t TEST_KEY[16] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                               0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
static Board *cur = nullptr;
static std::vector<std::string> leftover;  // violations of the last World, checked by the after-test hook

// A board reset from inside the firmware (boardReset, a watchdog bite): unwinds to Board::tick.
struct NodeReset {
  uint8_t cause;
};

static bool after(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
static uint64_t next64(uint64_t &s) {  // xorshift64*
  s ^= s >> 12;
  s ^= s << 25;
  s ^= s >> 27;
  return s * 0x2545F4914F6CDD1DULL;
}
static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char *f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

// --- trace -----------------------------------------------------------------------------------------------------
void Trace::add(uint32_t t, const std::string &s) {
  lines.push_back(fmt("%10u  ", t) + s);
  if (lines.size() > 4000) lines.pop_front();
}
void Trace::dump(size_t n) const {
  printf("      --- last %zu trace lines ---\n", n < lines.size() ? n : lines.size());
  for (size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); i++) printf("      %s\n", lines[i].c_str());
}

// --- boards ----------------------------------------------------------------------------------------------------
static std::string soDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) throw Failure("can't find the test program's directory");
  buf[n] = 0;
  return std::filesystem::path(buf).parent_path().string();
}

// The per-board copies of node.so this process made, removed when it exits.
static struct SoCopies {
  std::vector<std::string> files;
  ~SoCopies() {
    for (const std::string &f : files) unlink(f.c_str());
  }
} soCopies;

Board::Board(World &w, int idx, const char *name) : w(w), idx(idx), name(name) {
  static std::string dir = soDir();
  // Named by process too: test shards run in parallel from one build directory (make run-systests).
  so = dir + "/node-" + name + "-" + std::to_string(getpid()) + ".so";
  static bool copied[2] = { false, false };
  if (!copied[idx]) {
    // Two files, so dlopen gives each board its own copy of the firmware's globals.
    std::filesystem::copy_file(dir + "/node.so", so, std::filesystem::copy_options::overwrite_existing);
    soCopies.files.push_back(so);
    copied[idx] = true;
  }
  flash.assign(16 * 4096, 0xFF);
  for (int i = 0; i < 4; i++) serial[i] = 0x5EED0000u + idx * 16 + i;
  rng = 0x9E3779B97F4A7C15ULL + idx;
  holdUpMs = idx == 0 ? 100 : 600;  // bench: house ~0.1 s, gate ~0.6 s without a LiPo
  busyUntil = w.now;  // timestamps start at the world's time: 0 would read as far ahead once it passes 2^31
}

Board::~Board() {
  if (dl) dlclose(dl);
}

Board *Board::current() {
  return cur;
}

template <class T> static void sym(void *dl, const char *name, T &out) {
  out = (T)dlsym(dl, name);
  if (!out) throw Failure(std::string("node.so lacks ") + name);
}

void Board::load() {
  if (dl) dlclose(dl);
  dl = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!dl) throw Failure(std::string("dlopen: ") + dlerror());
  sym(dl, "node_relays_begin", fnRelaysBegin);
  sym(dl, "node_setup", fnSetup);
  sym(dl, "node_loop", fnLoop);
  sym(dl, "node_param_set", fnParamSet);
  sym(dl, "node_param_get", fnParamGet);
  sym(dl, "node_save", fnSave);
  sym(dl, "node_set_key", fnSetKey);
  sym(dl, "node_status", fnStatus);
  sym(dl, "node_role", fnRole);
  sym(dl, "node_log", fnLog);
}

static void dropOutputs(Board &b) {
  memset(b.mode, 0, sizeof(b.mode));
  memset(b.out, 0, sizeof(b.out));
  b.led = -1;
  b.radioUp = b.radioBegun = b.radioHeld = b.radioFault = b.radioRestartDue = false;
  b.rxq.clear();
  b.rx[0].clear();
  b.rx[1].clear();
  b.lineOut[0].clear();
  b.lineOut[1].clear();
  b.uartOn = false;
}

void Board::powerOn() {
  load();
  dropOutputs(*this);
  state = BOOTLOADER;
  resetCause = PM_RCAUSE_POR;
  bootAt = w.now + 30;
  busyUntil = w.now;
  w.trace.add(w.now, name + ": power on");
}

void Board::powerOff() {
  state = OFF;
  dropOutputs(*this);
  w.trace.add(w.now, name + ": power off");
}

void Board::reset(uint8_t rcause) {
  load();
  dropOutputs(*this);
  state = BOOTLOADER;
  resetCause = rcause;
  bootAt = w.now + 500;  // the bootloader's double-tap check, on every reset but power-on
  busyUntil = w.now;
  w.trace.add(w.now, name + fmt(": reset (rcause 0x%02x)", rcause));
}

struct UseBoard {  // makes a board current for a call into its firmware
  Board *prev;
  explicit UseBoard(Board *b) : prev(cur) { cur = b; }
  ~UseBoard() { cur = prev; }
};

template <class F> auto Board::call(F f) -> decltype(f()) {
  UseBoard use(this);
  if (!after(w.now, busyUntil)) {
    clockMs = busyUntil;
  } else {
    clockMs = w.now;
    clockUs = 0;
  }
  struct Done {
    Board &b;
    ~Done() { b.busyUntil = after(b.clockMs, b.w.now) ? b.clockMs : b.w.now; }
  } done{ *this };
  return f();
}

void Board::block(uint32_t us) {
  clockUs += us;
  clockMs += clockUs / 1000;
  clockUs %= 1000;
  // Not thrown from here: this may run inside a destructor (FlashAccess restarting the radio). The reset happens
  // once the call into the firmware returns.
  if (state == RUN && !wdtBite && after(clockMs, kickAt + 8000)) {
    w.violate(name + fmt(": loop blocked %u ms without a watchdog reset", clockMs - kickAt));
    wdtBite = true;
  }
}

void Board::tick() {
  // Supply: VIN, else the LiPo, else the hold-up of the board's capacitors.
  if (vinNow) vinLostAt = 0;
  else if (!vinLostAt) vinLostAt = w.now | 1;
  bool alive = vinNow || lipo || !after(w.now, vinLostAt + holdUpMs);
  pmicGood = !vinLostAt || !after(w.now, vinLostAt + w.pgLagMs);
  if (!alive) {
    if (state != OFF) powerOff();
    return;
  }
  if (state == OFF) {
    powerOn();
    return;
  }
  try {
    if (state == BOOTLOADER) {
      if (!after(w.now, bootAt)) return;
      state = RUN;
      boots++;
      call([&] {
        kickAt = clockMs;
        fnRelaysBegin();
        fnSetup(resetCause, serial);
      });
    } else {
      if (!after(w.now, busyUntil)) return;  // still inside a blocking call
      call([&] {
        kickAt = clockMs;  // GateLink.ino loop(): Watchdog.reset()
        fnLoop();
      });
    }
  } catch (const NodeReset &r) {
    wdtBite = false;
    reset(r.cause);
    return;
  }
  if (wdtBite) {
    wdtBite = false;
    reset(PM_RCAUSE_WDT);
  }
}

JsonDocument Board::status() {
  if (!dl || state != RUN) throw Failure(name + ": status of a board that isn't running");
  static char buf[16384];
  size_t n = call([&] { return fnStatus(buf, sizeof(buf)); });
  JsonDocument d;
  deserializeJson(d, buf, n);
  return d;
}

void Board::send(const std::string &line, int port) {
  for (char c : line) rx[port].push_back((uint8_t)c);
  rx[port].push_back('\n');
}

JsonDocument Board::request(const std::string &cmd, const std::string &args, uint32_t timeoutMs) {
  int id = nextId++;
  std::string line = "{\"id\":" + std::to_string(id) + ",\"cmd\":\"" + cmd + "\"" + (args.empty() ? "" : "," + args) + "}";
  if (cmd == "relay.test") {
    JsonDocument a;
    deserializeJson(a, "{" + args + "}");
    w.relayTests[idx].push_back({ w.now, a["k"] | 0, a["ms"] | 500u });
  }
  w.trace.add(w.now, name + " <- " + line);
  send(line);
  if (!w.runUntil([&] { return replies.count(id) > 0; }, timeoutMs)) throw Failure(name + ": no reply to " + line);
  JsonDocument d = replies[id];
  replies.erase(id);
  return d;
}

bool Board::set(const char *param, int32_t v) {
  UseBoard use(this);
  return fnParamSet(param, v);
}

int32_t Board::get(const char *param) {
  UseBoard use(this);
  int32_t v = 0;
  if (!fnParamGet(param, &v)) throw Failure(std::string("no param ") + param);
  return v;
}

int Board::count(const char *ev) const {
  int n = 0;
  for (const LogEv &e : logs) n += e.ev == ev;
  return n;
}

int Board::count(const char *ev, int32_t a) const {
  int n = 0;
  for (const LogEv &e : logs) n += e.ev == ev && e.a == a;
  return n;
}

const LogEv *Board::last(const char *ev) const {
  for (size_t i = logs.size(); i-- > 0;)
    if (logs[i].ev == ev) return &logs[i];
  return nullptr;
}

bool Board::waitLog(const char *ev, uint32_t maxMs, size_t since) {
  return w.runUntil([&] {
    for (size_t i = since; i < logs.size(); i++)
      if (logs[i].ev == ev) return true;
    return false;
  }, maxMs);
}

void Board::onLine(int port, const std::string &line) {
  lines.push_back(port ? "uart:" + line : line);
  JsonDocument d;
  if (deserializeJson(d, line)) {
    w.trace.add(w.now, name + ": unparsable console line: " + line);
    return;
  }
  // Events go to both ports: take them once, from USB while a host has it open.
  if (port == CON_UART && usbHost && d["event"].is<const char *>()) return;
  if (d["event"] == "log") {
    LogEv e = { d["t"] | 0u, w.now, d["ev"] | "", d["a"] | 0, d["b"] | 0 };
    logs.push_back(e);
    w.trace.add(w.now, name + fmt(": %s a=%d b=%d", e.ev.c_str(), e.a, e.b));
    w.onLog(*this, e);
  } else if (d["event"].is<const char *>()) {
    events.push_back(d);
  } else if (d["id"].is<int>()) {
    replies[d["id"].as<int>()] = d;
  }
}

// --- the Arduino core, for the current board -------------------------------------------------------------------
uint32_t millis() {
  return cur ? cur->clockMs : world ? world->now : 0;
}
uint32_t micros() {
  return cur ? cur->clockMs * 1000u + cur->clockUs : (world ? world->now : 0) * 1000u;
}
void delay(uint32_t ms) {
  if (cur) cur->block(ms * 1000u);
}
void delayMicroseconds(uint32_t us) {
  if (cur) cur->block(us);
}
long random(long max) {
  return max <= 0 || !cur ? 0 : (long)(next64(cur->prng) % (uint64_t)max);
}
long random(long min, long max) {
  return max <= min ? min : min + random(max - min);
}
void randomSeed(unsigned long seed) {
  if (cur) cur->prng = seed ? seed : 1;
}
void pinMode(uint32_t pin, uint32_t mode) {
  if (cur && pin < P_COUNT) cur->mode[pin] = (uint8_t)mode;
}
void digitalWrite(uint32_t pin, uint32_t value) {
  if (!cur || pin >= P_COUNT) return;
  if (cur->out[pin] != (bool)value && (pin == P_K1 || pin == P_K2))
    world->trace.add(world->now, cur->name + fmt(": K%u %s", pin, value ? "on" : "off"));
  cur->out[pin] = value;
}
int digitalRead(uint32_t pin) {
  if (!cur || !world) return LOW;
  Board &b = *cur;
  World &w = *world;
  if (&b == &w.gate) {
    if (pin >= P_IN1 && pin <= P_IN4) return w.opener.in(pin - P_IN1 + 1);
  } else {
    if (pin == P_IN1) return w.shelly.booted && w.shelly.relay;
    if (pin == P_IN2) return w.shelly.opto;
  }
  return LOW;  // pull-down: nothing connected reads inactive
}
void analogWrite(uint32_t pin, int value) {
  if (cur && pin == P_LED) cur->led = value;
}

// --- board.h -----------------------------------------------------------------------------------------------------
void boardKick() {
  if (cur) cur->kickAt = cur->clockMs;
}
void boardReset() {
  throw NodeReset{ PM_RCAUSE_SYST };
}
uint32_t boardFreeRam() {
  return 10000;
}

// --- radio.h: an SX1276 per board on the shared channel ----------------------------------------------------------
struct RadioCfg {
  int32_t freq, sf, bw, cr, sync;
};
static RadioCfg radioCfg(Board &b) {
  return { b.get("freq_hz"), b.get("sf"), b.get("bw_hz"), b.get("cr"), b.get("sync_word") };
}
static std::map<const Board *, RadioCfg> radioCfgs;  // as last initialised

uint32_t World::airtimeMs(size_t len, int sf, int bw, int cr) const {
  double tsym = (double)(1u << sf) / bw * 1000.0;
  int de = tsym > 16.0 ? 1 : 0;
  double pl = ceil((8.0 * len - 4 * sf + 28 + 16) / (4.0 * (sf - 2 * de)));
  double symbols = 8 + 4.25 + 8 + (pl > 0 ? pl * cr : 0);
  return (uint32_t)ceil(symbols * tsym);
}
static uint32_t boardAirtime(const Board &b, size_t len) {
  auto it = radioCfgs.find(&b);
  if (it == radioCfgs.end()) return world->airtimeMs(len);
  return world->airtimeMs(len, it->second.sf, it->second.bw, it->second.cr);
}
static uint32_t detectMs(const Board &b) {  // a preamble is flagged a few symbols in
  auto it = radioCfgs.find(&b);
  int sf = it == radioCfgs.end() ? 9 : it->second.sf, bw = it == radioCfgs.end() ? 500000 : it->second.bw;
  return (uint32_t)ceil(5.0 * (1u << sf) / bw * 1000.0);
}
static bool sameChannel(const Board &a, const Board &b) {
  auto x = radioCfgs.find(&a), y = radioCfgs.find(&b);
  if (x == radioCfgs.end() || y == radioCfgs.end()) return true;
  const RadioCfg &p = x->second, &q = y->second;
  return p.freq == q.freq && p.sf == q.sf && p.bw == q.bw && p.sync == q.sync;
}

static uint32_t retryAt[2];
bool radioBegin() {
  Board &b = *cur;
  b.radioBegun = true;
  b.radioHeld = false;
  b.radioRestartDue = false;
  b.rxq.clear();
  b.txStart = b.txEnd = b.clockMs;  // abandons a TX
  b.block(470000);      // LoRa.begin() on the MKR WAN 1310: delay(200 + 200 + 50) on the module's reset, 2 x 10 ms on the SX1276's
  b.radioUp = b.radioPresent;
  if (!b.radioUp) {
    if (!retryAt[b.idx]) b.fnLog(EV_RADIO_FAIL, 0, (int32_t)b.faults);
    retryAt[b.idx] = (b.clockMs + 5000) | 1;
    return false;
  }
  retryAt[b.idx] = 0;
  radioCfgs[&b] = radioCfg(b);
  return true;
}
void radioRestart() {
  if (cur->radioBegun) radioBegin();
}
bool radioOk() {
  return cur->radioUp;
}
// As radio.cpp: after a fault, or every 5 s while the radio didn't start, the firmware (appLoop) restarts it here.
bool radioRecoverDue() {
  Board &b = *cur;
  return b.radioRestartDue || (!b.radioUp && retryAt[b.idx] && after(b.clockMs, retryAt[b.idx]));
}
void radioRecover() {
  Board &b = *cur;
  if (!radioRecoverDue()) return;
  bool retry = !b.radioRestartDue;
  if (radioBegin() && retry) b.fnLog(EV_RADIO_FAIL, 3, (int32_t)b.faults);
}
bool radioTxBusy() {
  // Unsigned: a frame sent weeks ago must not read as still on the air once the signed difference flips.
  return cur->radioUp && cur->clockMs - cur->txStart < cur->txEnd - cur->txStart;
}
bool radioSend(const uint8_t *buf, size_t len) {
  Board &b = *cur;
  if (!b.radioUp || b.radioHeld || radioTxBusy()) return false;
  AirFrame f;
  f.from = b.idx;
  f.b.assign(buf, buf + len);
  f.start = b.clockMs;
  f.end = b.clockMs + boardAirtime(b, len);
  World &w = *world;
  if (w.drop) f.dropped = w.drop(f);
  if (w.corrupt) f.corrupt = w.corrupt(f);
  b.txStart = f.start;
  b.txEnd = f.end;
  if (w.traceFrames) w.trace.add(w.now, b.name + fmt(": TX type %u len %zu%s", f.type(), len, f.dropped ? " (dropped)" : ""));
  w.air.push_back(f);
  return true;
}
uint32_t radioTxEndAt() {
  return cur->txEnd;
}
uint32_t radioFaults() {
  return cur->faults;
}
uint32_t radioCrcErrors() {
  return cur->crcErr;
}
uint32_t radioRxDoneCount() {
  return cur->rxDone;
}
int32_t radioLastFei() {
  return 0;
}
bool radioChannelBusy() {
  Board &b = *cur;
  if (!b.radioUp) return false;
  if (radioTxBusy() || !b.rxq.empty()) return true;
  for (const AirFrame &f : world->air) {
    if (f.from == b.idx || f.dropped || f.delivered) continue;
    if (f.from >= 0 && !sameChannel(b, world->board(f.from))) continue;
    if (after(b.clockMs, f.start + detectMs(b)) && !after(b.clockMs, f.end)) return true;
  }
  return false;
}
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr) {
  Board &b = *cur;
  if (!b.radioUp) return 0;  // radioRecover() restarts it
  if (b.radioFault) {
    // radio.cpp fault(2): out of LoRa mode, a reset seen in RX. Counted, logged, and down until radioRecover().
    b.radioFault = false;
    b.faults++;
    b.fnLog(EV_RADIO_FAIL, 2, (int32_t)b.faults);
    b.radioUp = false;
    b.radioRestartDue = true;
    b.rxq.clear();
    b.txStart = b.txEnd = b.clockMs;
    return 0;
  }
  if (b.radioHeld || radioTxBusy() || b.rxq.empty()) return 0;
  Bytes f = b.rxq.front();
  b.rxq.pop_front();
  b.rxDone++;
  if (f.empty()) {
    b.crcErr++;
    return 0;
  }
  if (f.size() > max) return 0;
  memcpy(buf, f.data(), f.size());
  rssi = b.rssi;
  snr = b.snr;
  return f.size();
}
int16_t radioNoiseDbm() {
  Board &b = *cur;
  if (!b.radioUp || radioTxBusy() || !b.rxq.empty()) return INT16_MIN;
  return b.noise;
}
uint32_t radioAirtimeMs(size_t payloadLen) {
  return boardAirtime(*cur, payloadLen);
}
uint32_t radioRandom32() {
  cur->block(20500);  // 512 RSSI samples 40 us apart
  if (cur->stuckRng) return cur->stuckRng;
  return (uint32_t)(next64(cur->rng) >> 32);
}
void radioAddEntropy(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) cur->rng = (cur->rng ^ p[i]) * 0x100000001B3ULL;
  if (!cur->rng) cur->rng = 1;
}

// --- extflash.h ------------------------------------------------------------------------------------------------
static void flashRange(Board &b, uint32_t addr, size_t len) {
  if (addr + len > b.flash.size()) throw Failure(b.name + fmt(": flash access at 0x%x+%zu beyond the faked chip", addr, len));
}
void extFlashHoldModem() {
  Board &b = *cur;
  b.radioHeld = true;  // the module's reset also resets the SX1276: any frame on its way in or out is lost
  b.radioUp = false;
  b.rxq.clear();
  b.txStart = b.txEnd = b.clockMs;
  b.block(1000);
}
bool extFlashBegin() {
  extFlashHoldModem();
  return cur->flashPresent;
}
bool extFlashPresent() {
  return cur->flashPresent;
}
uint32_t extFlashId() {
  return cur->flashPresent ? 0xEF4015 : 0;
}
void extFlashRead(uint32_t addr, uint8_t *buf, size_t len) {
  Board &b = *cur;
  flashRange(b, addr, len);
  if (!b.flashPresent) {
    memset(buf, 0xFF, len);
    return;
  }
  memcpy(buf, b.flash.data() + addr, len);
  b.block((uint32_t)len * 8 + 40);  // 1 MHz SPI
}
bool extFlashEraseSector(uint32_t addr) {
  Board &b = *cur;
  if (!b.flashPresent) return false;
  addr &= ~(EXTFLASH_SECTOR - 1);
  flashRange(b, addr, EXTFLASH_SECTOR);
  memset(b.flash.data() + addr, 0xFF, EXTFLASH_SECTOR);
  b.flashErases++;
  b.block(b.eraseMs * 1000);
  return true;
}
bool extFlashProgram(uint32_t addr, const uint8_t *buf, size_t len) {
  Board &b = *cur;
  if (!b.flashPresent || len == 0 || (addr % EXTFLASH_PAGE) + len > EXTFLASH_PAGE) return false;
  flashRange(b, addr, len);
  size_t n = len;
  if (b.cutNextProgram) {
    b.cutNextProgram = false;
    n = len / 2;
  }
  for (size_t i = 0; i < n; i++) b.flash[addr + i] &= buf[i];  // NOR: programming only clears bits
  b.flashPrograms++;
  b.block(800 + (uint32_t)len * 8);
  return n == len;
}

// --- supply.h: the charger's power good --------------------------------------------------------------------------
static bool supplyGoodNow[2] = { true, true };
void supplyBegin() {
  Board &b = *cur;
  supplyGoodNow[b.idx] = !b.pmicKnown || b.pmicGood;
  b.pmicPollAt = b.clockMs;
  b.fnLog(EV_SUPPLY, b.pmicKnown ? supplyGoodNow[b.idx] : -1, b.pmicKnown ? (supplyGoodNow[b.idx] ? 0x04 : 0) : -1);
}
void supplyPoll(uint32_t now) {
  Board &b = *cur;
  if (!b.pmicKnown || !after(now, b.pmicPollAt + 5)) return;
  b.pmicPollAt = now;
  if (b.pmicGood == supplyGoodNow[b.idx]) return;
  supplyGoodNow[b.idx] = b.pmicGood;
  b.fnLog(EV_SUPPLY, b.pmicGood, b.pmicGood ? 0x04 : 0);
}
bool supplyKnown() {
  return cur->pmicKnown;
}
bool supplyGood() {
  return supplyGoodNow[cur->idx];
}

// --- console_io.h ----------------------------------------------------------------------------------------------
void conIoBegin() {}
void conIoUart(bool on) {
  cur->uartOn = on;
}
bool conIoOpen(uint8_t port) {
  return port == CON_USB ? cur->usbHost : cur->uartOn && cur->uartAdapter;
}
int conIoRead(uint8_t port) {
  std::deque<uint8_t> &q = cur->rx[port];
  if (q.empty()) return -1;
  int c = q.front();
  q.pop_front();
  return c;
}
void conIoFlush(uint8_t) {}
void conIoLineWrite(uint8_t port, const uint8_t *data, size_t n) {
  cur->lineOut[port].append((const char *)data, n);
}
bool conIoLineEnd(uint8_t port) {
  std::string all;
  all.swap(cur->lineOut[port]);
  size_t s = 0;
  while (s < all.size()) {
    size_t e = all.find('\n', s);
    if (e == std::string::npos) e = all.size();
    if (e > s) cur->onLine(port, all.substr(s, e - s));
    s = e + 1;
  }
  return true;
}

// --- the opener --------------------------------------------------------------------------------------------------
void Opener::command(int want, World &w, const char *src) {
  if (!alive() || deaf) {
    w.trace.add(w.now, fmt("opener: %s from %s ignored (%s)", want > 0 ? "open" : "close", src, deaf ? "deaf" : "no power"));
    return;
  }
  w.trace.add(w.now, fmt("opener: %s from %s", want > 0 ? "open" : "close", src));
  if ((want > 0 && pos >= (int32_t)travelMs) || (want < 0 && pos == 0)) {
    dir = 0;
    return;
  }
  dir = want;
  if (stuck) {
    pos = want > 0 ? (pos == 0 ? 1 : pos) : (pos >= (int32_t)travelMs ? (int32_t)travelMs - 1 : pos);
    dir = 0;
  }
}

void Opener::update(World &w, bool openIn, bool closeIn) {
  uint32_t now = w.now;
  openIn = openIn || extOpenHold || (extOpenUntil && !after(now, extOpenUntil));
  closeIn = closeIn || (extCloseUntil && !after(now, extCloseUntil));
  auto sense = [&](Sense &s, bool raw, int which) {
    if (raw != s.raw) {
      s.raw = raw;
      s.since = now;
    }
    if (s.raw == s.stable || now - s.since < 20) return 0;
    s.stable = s.raw;
    if (s.stable) s.pressedAt = s.since;
    else presses[which].push_back({ s.pressedAt, s.since - s.pressedAt });
    return s.stable ? 1 : -1;
  };
  int eo = sense(open, openIn, 0), ec = sense(close, closeIn, 1);
  if (eo > 0) command(+1, w, "OPEN input");
  if (ec > 0) {
    if (open.stable) w.trace.add(now, "opener: close ignored while OPEN is held");
    else command(-1, w, "CLOSE input");
  }
  if (!alive()) {
    dir = 0;  // stops dead, and doesn't resume when power returns
    return;
  }
  if (dir > 0 && ++pos >= (int32_t)travelMs) {
    pos = travelMs;
    dir = 0;
  } else if (dir < 0 && --pos <= 0) {
    pos = 0;
    dir = 0;
  }
}

bool Opener::in(int n) const {
  if (force[n - 1] >= 0) return force[n - 1];
  switch (n) {
    case 1: return atOpen();
    case 2: return atClosed();  // the closed-limit relay's NC contact, wetted by the opener's 24 V
    case 3: return ac;
    default: return in4;
  }
}

// --- the controller (Shelly) ---------------------------------------------------------------------------------------
void Shelly::update(World &w, bool rail, bool swIn) {
  uint32_t now = w.now;
  if (!rail) {
    if (booted && !relayDropAt) relayDropAt = (now + relayDropMs) | 1;
    if (opto && !optoAt) optoAt = (now + optoDropMs) | 1;
  } else {
    if (relayDropAt) {
      // Back before the relay dropped: the cut was short enough to ride through.
      relayDropAt = 0;
    }
    if (!opto && !optoAt) optoAt = (now + optoUpMs) | 1;
    if (opto && optoAt) optoAt = 0;  // a dip the opto never saw
    if (!booted && !bootAt) bootAt = (now + bootMs) | 1;
  }
  if (relayDropAt && after(now, relayDropAt)) {
    relayDropAt = 0;
    booted = false;
    relay = false;
    bootAt = 0;
    w.trace.add(now, "shelly: unpowered (relay dropped)");
  }
  if (optoAt && after(now, optoAt)) {
    optoAt = 0;
    opto = rail;
    w.trace.add(now, fmt("shelly: IN2 opto %s", opto ? "on" : "off"));
  }
  if (!booted && bootAt && rail && after(now, bootAt)) {
    bootAt = 0;
    booted = true;
    sw = swRaw = swIn;
    swSince = now;
    relay = restoreSw && mode == FOLLOW ? swIn : false;
    w.trace.add(now, fmt("shelly: booted, relay %s", relay ? "on" : "off"));
  }
  if (!booted) return;
  if (swIn != swRaw) {
    swRaw = swIn;
    swSince = now;
  }
  if (swRaw != sw && now - swSince >= 30) {
    sw = swRaw;
    bool was = relay;
    if (mode == FOLLOW) relay = sw;
    else if (mode == TOGGLE) relay = !relay;
    if (relay != was) w.trace.add(now, fmt("shelly: SW %s -> relay %s", sw ? "on" : "off", relay ? "on" : "off"));
  }
}

// --- the world -----------------------------------------------------------------------------------------------------
World::World(uint32_t start) : now(start), house(*this, 0, "house"), gate(*this, 1, "gate") {
  if (world) throw Failure("one World at a time");
  world = this;
  radioCfgs.clear();
  retryAt[0] = retryAt[1] = 0;
  supplyGoodNow[0] = supplyGoodNow[1] = true;
  house.vinNow = gate.vinNow = true;
}

World::~World() {
  if (std::uncaught_exceptions() || !violations.empty()) trace.dump();
  leftover = violations;
  radioCfgs.clear();
  world = nullptr;
  cur = nullptr;
}

static TestHooks checkLeftover(nullptr, [] {
  if (leftover.empty()) return;
  std::string all = "invariant monitors:";
  for (const std::string &v : leftover) all += "\n        " + v;
  leftover.clear();
  throw Failure(all);
});

void World::violate(const std::string &what) {
  std::string v = fmt("t=%u ", now) + what;
  trace.add(now, "VIOLATION " + what);
  if (violations.size() < 50) violations.push_back(v);
}

void World::checkClean() {
  if (violations.empty()) return;
  std::string all = "invariant monitors:";
  for (const std::string &v : violations) all += "\n        " + v;
  violations.clear();
  throw Failure(all);
}

void World::commission(const std::function<void(Board &)> &configure, bool settle) {
  for (Board *b : { &house, &gate }) {
    b->vinNow = true;
    b->tick();  // power on
  }
  CHECK(runUntil([&] { return house.running() && gate.running(); }, 1000));
  run(5);
  for (Board *b : { &house, &gate }) {
    CHECK(b->set("role", b->idx == 0 ? 1 : 2));
    if (configure) configure(*b);
    b->call([&] { return b->fnSetKey(TEST_KEY); });
    CHECK(b->call([&] { return b->fnSave(); }));
    b->reset(PM_RCAUSE_SYST);
  }
  CHECK(runUntil([&] { return house.running() && gate.running(); }, 2000));
  CHECK(runUntil([&] { return house.status()["link"]["verified"] == true && gate.status()["link"]["verified"] == true; },
                 10000));
  CHECK(runUntil([&] { return house.status()["gate"] != "unknown"; }, 15000));
  // The gate logs gate_state only on a change, not the state it settles on at boot: start gateSees() from its status.
  static const char *const names[] = { "unknown", "closed", "open", "between", "fault", "no_power" };
  std::string gs = gate.status()["gate"] | "unknown";
  for (int i = 0; i < 6; i++)
    if (gs == names[i]) gateView = i;
  if (settle) {
    CHECK(runUntil([&] {
      JsonDocument s = house.status();
      return s["armed"] == true && s["sync_window"] == false;
    }, 20000));
  }
}

void World::step() {
  now++;
  bool openIn = gate.coil(1), closeIn = gate.coil(2);
  opener.update(*this, openIn, closeIn);
  shelly.update(*this, rail12, house.coil(1));
  house.vinNow = rail12 && !house.cut;
  gate.vinNow = !gate.cut && (gateFeed == FEED_FIXED ? gateRail : gateFeed == FEED_ACC ? opener.alive() : opener.ac);
  for (AirFrame &f : air) {
    if (f.delivered || !after(now, f.end)) continue;
    f.delivered = true;
    if (f.dropped) continue;
    for (Board *to : { &house, &gate }) {
      if (to->idx == f.from || to->state != Board::RUN || !to->radioUp || to->radioHeld) continue;
      if (f.from >= 0 && !sameChannel(*to, board(f.from))) continue;
      bool lost = false, garbled = f.corrupt;
      for (const AirFrame &o : air) {
        if (&o == &f || !((int32_t)(o.start - f.end) < 0 && (int32_t)(f.start - o.end) < 0)) continue;  // wrap-safe
        if (o.from == to->idx) lost = true;  // half duplex: deaf while transmitting
        else garbled = true;                 // two transmitters at once
      }
      if (lost) continue;
      to->rxq.clear();  // a newer frame overwrites one still unread in the FIFO
      to->rxq.push_back(garbled ? Bytes() : f.b);
    }
  }
  if (air.size() > 512) {  // keep the last minute's frames for sent()
    std::vector<AirFrame> keep;
    for (const AirFrame &f : air)
      if (!f.delivered || !after(now, f.end + 60000)) keep.push_back(f);
    air.swap(keep);
  }
  house.tick();
  gate.tick();
  monitor();
}

void World::run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) step();
}

bool World::runUntil(const std::function<bool()> &cond, uint32_t maxMs) {
  for (uint32_t i = 0; i < maxMs; i++) {
    if (cond()) return true;
    step();
  }
  return cond();
}

void World::user(bool on) {
  userCmds.push_back({ now, on, false });
  if (!shelly.booted) {
    trace.add(now, fmt("user: switch %s, but the controller is down", on ? "on" : "off"));
    return;
  }
  shelly.relay = on;
  trace.add(now, fmt("user: switch %s", on ? "on" : "off"));
}

void World::extPress(bool openInput, uint32_t ms) {
  (openInput ? opener.extOpenUntil : opener.extCloseUntil) = (now + ms) | 1;
  trace.add(now, fmt("external: %s pressed %u ms", openInput ? "OPEN" : "CLOSE", ms));
}

void World::setRail12(bool on) {
  rail12 = on;
  trace.add(now, fmt("house 12 V rail %s", on ? "on" : "off"));
}

void World::setGateRail(bool on) {
  gateRail = on;
  trace.add(now, fmt("gate board feed %s", on ? "on" : "off"));
}

bool World::sensorClosed() const {
  return house.coil(2);
}

uint32_t World::houseLinkTimeoutMs() {
  uint32_t ms = (uint32_t)house.get("link_timeout_s") * 1000, hb = (uint32_t)gate.get("heartbeat_s") * 2500;
  return hb > ms ? hb : ms;
}

int World::gateTruth() const {
  if (opener.atOpen() && opener.atClosed()) return GS_FAULT_;
  if (opener.atClosed()) return GS_CLOSED_;
  if (opener.atOpen()) return GS_OPEN_;
  if (!opener.ac) return GS_NO_POWER_;
  return GS_BETWEEN_;
}

void World::airSend(const Bytes &frame, int from) {
  AirFrame f;
  f.from = from;
  f.b = frame;
  f.start = now;
  f.end = now + airtimeMs(frame.size());
  air.push_back(f);
}

std::vector<const AirFrame *> World::sent(const Board &from, uint8_t type) const {
  std::vector<const AirFrame *> out;
  for (const AirFrame &f : air)
    if (f.from == from.idx && f.type() == type) out.push_back(&f);
  return out;
}

// --- monitors ------------------------------------------------------------------------------------------------------
// A console relay.test of relay k on that board, asked for at most `within` ms before `at`.
static bool recentRelayTest(World &w, int board, int k, uint32_t at, uint32_t within, uint32_t *ms = nullptr) {
  const std::vector<World::RelayTest> &tests = w.relayTests[board];
  for (auto it = tests.rbegin(); it != tests.rend(); ++it) {  // the newest first
    const World::RelayTest &t = *it;
    if (t.k == k && after(at, t.at) && !after(at, t.at + within)) {
      if (ms) *ms = t.ms;
      return true;
    }
  }
  return false;
}

void World::onLog(Board &b, const LogEv &e) {
  if (e.ev == "boot") {
    if (&b == &house) {
      houseView = GS_UNKNOWN_;
      houseLink = false;
    } else {
      gateView = GS_UNKNOWN_;
    }
  }
  if (&b == &house) {
    if (e.ev == "gate_state") {
      houseView = e.a;
      houseViewAt = now;
    } else if (e.ev == "link_up" || e.ev == "link_down") {
      houseLink = e.ev == "link_up";
    } else if (e.ev == "resync") {
      resyncUntil = now + (uint32_t)house.get("resync_ms") + 50;
    } else if (e.ev == "cmd_sent" && limits.provenance) {
      // Every command the house sends must come from the user moving the switch (Alarm.com), never from our own
      // sync edges, a controller losing power, or the level the Shelly shows at boot.
      bool on = e.a == 1;
      uint32_t window = (uint32_t)house.get("debounce_ms") + (uint32_t)house.get("ctrl_confirm_ms") + 100;
      bool found = false;
      for (UserCmd &u : userCmds) {
        if (u.used || u.on != on || after(u.at, now + 1) || after(now, u.at + window)) continue;
        u.used = found = true;
        break;
      }
      if (!found) violate(fmt("house sent %s (cmd %d) with no matching user action", on ? "OPEN" : "CLOSE", e.b));
    }
  } else {
    if (e.ev == "cmd_rx") {
      lastGateCmdRx = e.b;
      lastGateCmdRxAt = now;
      bool sent = false;
      for (const LogEv &h : house.logs) sent |= h.ev == "cmd_sent" && h.b == e.b && h.a == e.a;
      if (!sent) violate(fmt("gate took command %d that the house never sent", e.b));
    } else if (e.ev == "pulse") {
      if (lastGateCmdRxAt == now && lastGateCmdRx >= 0) {
        if (++pulsesPerCmd[lastGateCmdRx] > 1) violate(fmt("gate pulsed twice for command %d", lastGateCmdRx));
      } else if (!recentRelayTest(*this, 1, e.a, now, 200)) {
        violate(fmt("gate pulsed K%d without a command or relay test", e.a));
      }
    } else if (e.ev == "gate_state") {
      gateView = e.a;
      // Gate state comes only from its limit inputs.
      if (e.a == GS_OPEN_ && !opener.in(1)) violate("gate reported open with the open limit off");
      if (e.a == GS_CLOSED_ && !opener.in(2)) violate("gate reported closed with the closed limit off");
    }
  }
}

void World::monitor() {
  // Gate relays: pulsed only, never both, never one within INTERLOCK_MS (100) of the other releasing.
  for (int k = 0; k < 2; k++) {
    CoilTrack &c = gk[k], &o = gk[1 - k];
    bool on = gate.coil(k + 1);
    if (on && !c.on) {
      c.on = true;
      c.onAt = now;
      c.reported = false;
      if (o.on) violate(fmt("gate K%d energized while K%d is", k + 1, 2 - k));
      else if (o.offAt && !after(now, o.offAt + 100))
        violate(fmt("gate K%d energized %u ms after K%d released (interlock 100 ms)", k + 1, now - o.offAt, 2 - k));
      uint32_t testMs = 0;
      bool test = recentRelayTest(*this, 1, k + 1, now, 300, &testMs);
      c.maxMs = limits.pulseMaxMs ? limits.pulseMaxMs : (test ? testMs : (uint32_t)gate.get("pulse_ms")) + 25;
      // As the gate sees it: IN3 (which a test may force), not the mains itself.
      if (!test && gate.get("power_sense") && !opener.in(3)) violate(fmt("gate K%d pulsed without AC power (IN3 off)", k + 1));
    } else if (!on && c.on) {
      c.on = false;
      c.offAt = now;
    }
    if (c.on && !c.reported && after(now, c.onAt + c.maxMs)) {
      c.reported = true;
      violate(fmt("gate K%d held over %u ms (pulses only)", k + 1, c.maxMs));
    }
  }

  // House K2 (contact sensor) shows closed only when the gate is known closed: the house's last report says closed
  // and (with linkloss_open) the link is up; and it never lags the real gate by more than the link timeout.
  bool houseK2Test = recentRelayTest(*this, 0, 2, now, 5100);
  if (limits.sensorTruth && house.running() && sensorClosed() && !houseK2Test) {
    bool known = houseView == GS_CLOSED_ && (houseLink || !house.get("linkloss_open"));
    if (known || house.get("sensor_invert")) {
      if (k2InconsistentSince != 0x7FFFFFFF) k2InconsistentSince = 0;
    } else if (!k2InconsistentSince) {
      k2InconsistentSince = now | 1;
    } else if (k2InconsistentSince != 0x7FFFFFFF && after(now, k2InconsistentSince + 50)) {
      violate(fmt("contact sensor closed while the house knows the gate as %d, link %s", houseView,
                  houseLink ? "up" : "down"));
      k2InconsistentSince = 0x7FFFFFFF;
    }
    if (!opener.atClosed() && !house.get("sensor_invert")) {
      if (!k2WrongSince) k2WrongSince = now | 1;
      else if (k2WrongSince != 0x7FFFFFFF && after(now, k2WrongSince + houseLinkTimeoutMs() + limits.sensorGraceMs)) {
        violate(fmt("contact sensor shows closed %u ms after the gate left its closed limit", now - k2WrongSince));
        k2WrongSince = 0x7FFFFFFF;
      }
    } else if (k2WrongSince != 0x7FFFFFFF) {
      k2WrongSince = 0;
    }
  } else {
    if (k2WrongSince != 0x7FFFFFFF) k2WrongSince = 0;
    if (k2InconsistentSince != 0x7FFFFFFF) k2InconsistentSince = 0;
  }

  // While the house knows the gate as no_power or fault: not-closed (K1 energized, K2 open).
  bool unknown = houseView == GS_NO_POWER_ || houseView == GS_FAULT_;
  bool houseTest = recentRelayTest(*this, 0, 1, now, 5100) || houseK2Test;
  if (limits.notClosedDisplay && house.running() && unknown && after(now, houseViewAt + 50) && !houseTest
      && after(now, resyncUntil)) {
    bool k1Expected = house.get("ctrl_sync") != 0;
    if ((k1Expected && !house.coil(1)) || sensorClosed()) {
      if (!displayWrongSince) displayWrongSince = now | 1;
      else if (after(now, displayWrongSince + 20)) {
        violate(fmt("house shows the gate %s as closed (K1 %d, K2 %d)", houseView == GS_FAULT_ ? "fault" : "no_power",
                    house.coil(1), house.coil(2)));
        displayWrongSince = 0x7FFFFFFF;
      }
    } else if (displayWrongSince != 0x7FFFFFFF) {
      displayWrongSince = 0;
    }
  } else if (displayWrongSince != 0x7FFFFFFF) {
    displayWrongSince = 0;
  }
}
