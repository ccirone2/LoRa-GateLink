#include "sim.h"
#include <math.h>
#include "extflash.h"
#include "radio.h"

uint32_t simNow = 0;
Sim *sim = nullptr;
FakeFlash flash;
static Node *cur = nullptr;
static std::vector<LogRec> globalLogs;  // logEvent with no node current (config tests)

// --- Arduino core --------------------------------------------------------------------------------------------
static uint64_t prng = 1;
static uint64_t next64(uint64_t &s) {  // xorshift64*
  s ^= s >> 12;
  s ^= s << 25;
  s ^= s >> 27;
  return s * 0x2545F4914F6CDD1DULL;
}
uint32_t millis() { return simNow; }
uint32_t micros() { return simNow * 1000u; }
void delay(uint32_t ms) { simNow += ms; }
long random(long max) { return max <= 0 ? 0 : (long)(next64(prng) % (uint64_t)max); }
long random(long min, long max) { return max <= min ? min : min + random(max - min); }
void randomSeed(unsigned long seed) { prng = seed ? seed : 1; }

// --- log.h ---------------------------------------------------------------------------------------------------
void logEvent(LogCode code, int32_t a, int32_t b) {
  (cur ? cur->logs : globalLogs).push_back({ simNow, (uint8_t)code, a, b });
}

// --- radio.h: one SX127x per node, over Sim's air ------------------------------------------------------------
uint32_t airtimeMs(size_t len) {
  double tsym = (double)(1u << cfg.sf) / cfg.bw_hz * 1000.0;
  int de = tsym > 16.0 ? 1 : 0;
  double pl = ceil((8.0 * len - 4 * cfg.sf + 28 + 16) / (4.0 * (cfg.sf - 2 * de)));
  double symbols = 8 + 4.25 + 8 + (pl > 0 ? pl * cfg.cr : 0);
  return (uint32_t)ceil(symbols * tsym);
}
static uint32_t detectMs() {  // a preamble is flagged a few symbols in
  return (uint32_t)ceil(5.0 * (1u << cfg.sf) / cfg.bw_hz * 1000.0);
}

bool radioBegin() { return true; }
void radioRestart() {}
bool radioOk() { return cur ? cur->radioUp : true; }
bool radioSend(const uint8_t *buf, size_t len) {
  if (!cur || !cur->radioUp || radioTxBusy()) return false;
  AirFrame f;
  f.from = cur->idx;
  f.b.assign(buf, buf + len);
  f.start = simNow;
  f.end = simNow + airtimeMs(len);
  if (sim->drop) f.dropped = sim->drop(f);
  if (sim->corrupt) f.corrupt = sim->corrupt(f);
  cur->txStart = f.start;
  cur->txEnd = f.end;
  sim->air.push_back(f);
  return true;
}
// Unsigned: a frame sent weeks ago must not read as still on the air once the signed difference flips.
bool radioTxBusy() { return cur && simNow - cur->txStart < cur->txEnd - cur->txStart; }
uint32_t radioTxEndAt() { return cur ? cur->txEnd : 0; }
uint32_t radioFaults() { return 0; }
uint32_t radioCrcErrors() { return 0; }
uint32_t radioRxDoneCount() { return cur ? cur->rxDone : 0; }
int32_t radioLastFei() { return 0; }
bool radioChannelBusy() {
  if (!cur) return false;
  if (!cur->rxq.empty()) return true;
  for (const AirFrame &f : sim->air) {
    if (f.from == cur->idx || f.dropped || f.delivered) continue;
    if ((int32_t)(simNow - (f.start + detectMs())) >= 0 && (int32_t)(simNow - f.end) < 0) return true;
  }
  return false;
}
size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr) {
  if (!cur || cur->rxq.empty()) return 0;
  Bytes f = cur->rxq.front();
  cur->rxq.pop_front();
  cur->rxDone++;
  if (f.empty()) return 0;  // bad CRC
  size_t n = f.size() < max ? f.size() : max;
  memcpy(buf, f.data(), n);
  rssi = -60;
  snr = 9.0f;
  return n;
}
int16_t radioNoiseDbm() { return -120; }
uint32_t radioAirtimeMs(size_t payloadLen) { return airtimeMs(payloadLen); }
uint32_t radioRandom32() {
  if (cur && cur->stuckRng) return cur->stuckRng;
  return cur ? (uint32_t)(next64(cur->rng) >> 32) : (uint32_t)(next64(prng) >> 32);
}
void radioAddEntropy(const void *, size_t) {}

// --- extflash.h ----------------------------------------------------------------------------------------------
void FakeFlash::reset() {
  present = true;
  memset(mem, 0xFF, sizeof(mem));
  garbleReads = 0;
  cutNextProgram = false;
  cutAfterPrograms = 0;
  erases = programs = 0;
}
static void inRange(uint32_t addr, size_t len) {
  if (addr + len > sizeof(flash.mem)) throw Failure("flash access out of the faked sectors");
}
void extFlashHoldModem() {}
bool extFlashBegin() { return flash.present; }
bool extFlashPresent() { return flash.present; }
uint32_t extFlashId() { return flash.present ? 0xEF4015 : 0; }
void extFlashRead(uint32_t addr, uint8_t *buf, size_t len) {
  inRange(addr, len);
  if (flash.garbleReads > 0) {
    flash.garbleReads--;
    memset(buf, 0, len);
    return;
  }
  memcpy(buf, flash.mem + addr, len);
}
bool extFlashEraseSector(uint32_t addr) {
  inRange(addr, EXTFLASH_SECTOR);
  flash.erases++;
  memset(flash.mem + (addr & ~(EXTFLASH_SECTOR - 1)), 0xFF, EXTFLASH_SECTOR);
  return true;
}
bool extFlashProgram(uint32_t addr, const uint8_t *buf, size_t len) {
  inRange(addr, len);
  flash.programs++;
  size_t n = len;
  if (flash.cutNextProgram && flash.cutAfterPrograms-- <= 0) {
    flash.cutNextProgram = false;
    flash.cutAfterPrograms = 0;
    n = len / 2;
  }
  for (size_t i = 0; i < n; i++) flash.mem[addr + i] &= buf[i];  // NOR: programming only clears bits
  return n == len;
}

// --- nodes -----------------------------------------------------------------------------------------------------
Node::Ctx::Ctx(Node &n) : prev(cur), same(cur == &n) {
  if (same) return;
  if (prev) prev->cfg = ::cfg;
  else prevCfg = ::cfg;
  prevRole = activeRole;
  ::cfg = n.cfg;
  activeRole = n.cfg.role;
  cur = &n;
}
Node::Ctx::~Ctx() {
  if (same) return;
  cur->cfg = ::cfg;
  ::cfg = prev ? prev->cfg : prevCfg;
  activeRole = prevRole;
  cur = prev;
}

static void onRxThunk(const RxMsg &m) {
  cur->rx.push_back({ simNow, m.type, m.seq, Bytes(m.payload, m.payload + m.len) });
  if (cur->onRx) {
    cur->onRx(*cur, m);
  } else if (m.type == MSG_CMD || m.type == MSG_STATUS || m.type == MSG_CFG_SET) {
    cur->ack(m.seq, RES_OK);
  }
}
static void onAckThunk(Slot slot, uint8_t type, bool acked, uint8_t result) {
  cur->acks.push_back({ simNow, slot, type, acked, result });
}

void Node::begin() {
  Ctx c(*this);
  txStart = txEnd = simNow;  // radioBegin() abandons a TX
  rxq.clear();
  api.begin(onRxThunk, onAckThunk);
}
void Node::boot(uint32_t count) {
  {
    Ctx c(*this);
    api.setBoot(count, serial);
  }
  begin();
}
void Node::sendReliable(Slot slot, uint8_t type, const Bytes &p, uint32_t ttlMs) {
  Ctx c(*this);
  api.sendReliable(slot, type, p.data(), (uint8_t)p.size(), ttlMs);
}
void Node::send(uint8_t type, const Bytes &p) {
  Ctx c(*this);
  api.send(type, p.data(), (uint8_t)p.size());
}
void Node::ack(uint32_t seq, uint8_t result) {
  Ctx c(*this);
  api.ack(seq, result);
}
void Node::ackLater(uint32_t seq) {
  Ctx c(*this);
  api.ackLater(seq);
}
void Node::refuse(uint32_t seq) {
  Ctx c(*this);
  api.refuse(seq);
}
bool Node::pending(Slot slot) {
  Ctx c(*this);
  return api.pending(slot);
}
LinkStats Node::stats() {
  Ctx c(*this);
  return api.stats();
}
bool Node::verified() {
  Ctx c(*this);
  return api.verified();
}
int Node::count(LogCode code) const {
  int n = 0;
  for (const LogRec &l : logs) n += l.code == code;
  return n;
}
int Node::count(LogCode code, int32_t a) const {
  int n = 0;
  for (const LogRec &l : logs) n += l.code == code && l.a == a;
  return n;
}
int Node::rxCount(uint8_t type) const {
  int n = 0;
  for (const RxRec &r : rx) n += r.type == type;
  return n;
}

static void setupNode(Node &n, int idx, const char *name, int32_t role, uint64_t seed) {
  n.idx = idx;
  n.name = name;
  n.api = role == ROLE_HOUSE ? houseLinkApi() : gateLinkApi();
  configDefaults(n.cfg);
  n.cfg.role = role;
  n.cfg.key_set = 1;
  for (int i = 0; i < 16; i++) n.cfg.key[i] = (uint8_t)(0xA0 + i);
  n.rng = seed;
  for (int i = 0; i < 4; i++) n.serial[i] = 0x5EED0000u + idx * 16 + i;
  n.txStart = n.txEnd = simNow;
}

Sim::Sim(uint32_t start) {
  if (sim) throw Failure("one Sim at a time");
  sim = this;
  simNow = start;
  randomSeed(12345);
  setupNode(house, 0, "house", ROLE_HOUSE, 0x9E3779B97F4A7C15ULL);
  setupNode(gate, 1, "gate", ROLE_GATE, 0xD1B54A32D192ED03ULL);
  house.begin();
  gate.begin();
}
Sim::~Sim() { sim = nullptr; }

void Sim::deliver() {
  for (AirFrame &f : air) {
    if (f.delivered || (int32_t)(simNow - f.end) < 0) continue;
    f.delivered = true;
    if (f.dropped) continue;
    Node &to = node(1 - f.from);
    for (const AirFrame &o : air) {  // half duplex: deaf while transmitting
      if (o.from == to.idx && (int32_t)(o.start - f.end) < 0 && (int32_t)(f.start - o.end) < 0) f.collided = true;
    }
    if (!f.collided && to.radioUp) to.rxq.push_back(f.corrupt ? Bytes() : f.b);
  }
}

void Sim::step(uint32_t ms) {
  simNow += ms;
  deliver();
  for (Node *n : { &house, &gate }) {
    Node::Ctx c(*n);
    n->api.poll(simNow);
  }
}

void Sim::run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) step();
}

bool Sim::runUntil(const std::function<bool()> &cond, uint32_t maxMs) {
  for (uint32_t i = 0; i < maxMs; i++) {
    if (cond()) return true;
    step();
  }
  return cond();
}

std::vector<const AirFrame *> Sim::sent(const Node &from, uint8_t type) const {
  std::vector<const AirFrame *> out;
  for (const AirFrame &f : air)
    if (f.from == from.idx && f.type() == type) out.push_back(&f);
  return out;
}

void Sim::dumpAir(uint32_t since) const {
  static const char *names[] = { "?", "HELLO", "HELLO_ACK", "ACK", "CMD", "STATUS", "PING", "PONG", "DIAG_REQ", "DIAG",
                                 "CFG_SET" };
  for (const AirFrame &f : air) {
    if ((int32_t)(f.start - since) < 0) continue;
    printf("      %10u-%-10u %-5s %-9s session %08x seq %u%s%s", f.start, f.end, f.from ? "gate" : "house",
           f.type() <= MSG_CFG_SET ? names[f.type()] : "?", f.session(), f.seq(), f.dropped ? " dropped" : "",
           f.collided ? " collided" : "");
    if (f.type() == MSG_ACK) printf(" acks %u", getU32(&f.b[13]));
    printf("\n");
  }
}

// --- per test ------------------------------------------------------------------------------------------------
static TestHooks resetEach([] { flash.reset(); },
                           [] {
                             sim = nullptr;
                             cur = nullptr;
                           });
