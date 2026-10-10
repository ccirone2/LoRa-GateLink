// Shared by the fuzz targets (harness.h).
#include "harness.h"
#include <ArduinoJson.h>
#include <SHA256.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <functional>
#include "app.h"
#include "config.h"
#include "crc32.h"
#include "link.h"
#include "roles.h"

#ifndef FUZZ_SNAPSHOT
#define FUZZ_SNAPSHOT 0
#endif
#if FUZZ_SNAPSHOT && !defined(__clang__)
#error "FUZZ_SNAPSHOT needs clang (#pragma clang section, sections.h)"
#endif

const uint8_t FUZZ_KEY[16] = { 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,
                               0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
static const uint32_t SERIAL_NO[4] = { 0x5EED0000, 0x5EED0001, 0x5EED0002, 0x5EED0003 };
#define HDR_LEN 13
#define TAG_LEN 8
#define PASS_BUDGET 40000  // loop passes per input (the tail that lets a pulse end comes on top)

Peer peer;
static Bytes images[2];

[[noreturn]] static void die(const char *what) {
  fprintf(stderr, "gatelink-fuzz harness: %s\n", what);
  abort();
}

// --- fresh firmware RAM ------------------------------------------------------------------------------------------------
#if FUZZ_SNAPSHOT
extern "C" {
extern char __start_gl_bss[] __attribute__((weak)), __stop_gl_bss[] __attribute__((weak));
extern char __start_gl_data[] __attribute__((weak)), __stop_gl_data[] __attribute__((weak));
}

// The sections hold ASan redzones between the globals: copy them without instrumentation (and without memcpy,
// which ASan checks), word by word where aligned.
#define NOSAN __attribute__((no_sanitize("address", "undefined")))
NOSAN static void rawCopy(void *dst, const void *src, size_t n) {
  volatile uint8_t *d = (volatile uint8_t *)dst;
  const volatile uint8_t *s = (const volatile uint8_t *)src;
  size_t i = 0;
  if (((uintptr_t)d % 8) == 0 && ((uintptr_t)s % 8) == 0) {
    for (; i + 8 <= n; i += 8) *(volatile uint64_t *)(d + i) = *(const volatile uint64_t *)(s + i);
  }
  for (; i < n; i++) d[i] = s[i];
}

NOSAN static uint64_t rawHash(const void *p, size_t n, uint64_t h) {
  const volatile uint8_t *b = (const volatile uint8_t *)p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ULL;
  return h;
}

struct Ram {
  std::vector<uint8_t> bss, data;
};

static size_t bssLen() { return (size_t)(__stop_gl_bss - __start_gl_bss); }
static size_t dataLen() { return (size_t)(__stop_gl_data - __start_gl_data); }

static void save(Ram &r) {
  r.bss.resize(bssLen());
  r.data.resize(dataLen());
  rawCopy(r.bss.data(), __start_gl_bss, bssLen());
  rawCopy(r.data.data(), __start_gl_data, dataLen());
}

static void load(const Ram &r) {
  rawCopy(__start_gl_bss, r.bss.data(), bssLen());
  rawCopy(__start_gl_data, r.data.data(), dataLen());
}

static uint64_t ramHash() {
  uint64_t h = rawHash(__start_gl_bss, bssLen(), 1469598103934665603ULL);
  return rawHash(__start_gl_data, dataLen(), h);
}

static bool inRam(const void *p) {
  const char *c = (const char *)p;
  return (c >= __start_gl_bss && c < __stop_gl_bss) || (c >= __start_gl_data && c < __stop_gl_data);
}

static Ram pristine;
struct Warm {
  Ram ram;
  Hal hal;
  Peer peer;
};
static Warm warm[2][2];  // [role][wrap]
#else
// Without snapshots each input needs a process whose firmware RAM is untouched: replay_main.cpp forks one per input.
static bool used = false;
static void freshProcess() {
  if (used) die("this build has no RAM snapshots: one input per process (run it through replay_main)");
  used = true;
}
#endif

// --- the board ---------------------------------------------------------------------------------------------------------
void setPins(uint8_t pins) {
  for (int i = 0; i < 4; i++) hal.in[HP_IN1 + i] = (pins >> i) & 1;
}

void boot(uint8_t resetCause) {
  hal.kickAt = hal.ms;
  appRelaysBegin();
  appSetup(resetCause, SERIAL_NO);
  halCheckPass();
  peerScan();
}

bool budgetLeft() { return hal.passes < PASS_BUDGET; }

bool pass(uint32_t stepMs) {
  if (!budgetLeft()) return false;
  hal.ms += stepMs;
  hal.kickAt = hal.ms;  // GateLink.ino loop(): Watchdog.reset(), then appLoop()
  hal.passes++;
  appLoop();
  halCheckPass();
  peerScan();
  return true;
}

static bool relaysBusy() {
  return appRelaysPulsing() || (activeRole == ROLE_GATE && (hal.out[HP_K1] || hal.out[HP_K2]));
}

// Long waits (link timeouts, heartbeats) would cost tens of thousands of 1 ms passes, so while no relay pulses the
// loop runs every IDLE_STEP_MS: a slow loop, which the firmware must cope with anyway (USB writes, flash saves). A
// pulse is timed in 1 ms passes, so the relay monitors see it end when the board would.
#define IDLE_STEP_MS 50
bool advance(uint32_t ms) {
  uint32_t end = hal.ms + ms;
  while ((int32_t)(end - hal.ms) > 0) {
    uint32_t left = end - hal.ms;
    if (!pass(relaysBusy() ? 1 : left < IDLE_STEP_MS ? left : IDLE_STEP_MS)) return false;
  }
  return true;
}

bool settle() {
  for (int i = 0; i < 3000; i++) {
    bool waiting = (!hal.rxq.empty() && hal.radioUp) || !hal.rx[0].empty() || (hal.uartOn && !hal.rx[1].empty());
    if (!waiting) break;
    if (!pass(1)) return false;
  }
  return pass(1) && pass(1);
}

void finish() {
  // A pulse still running when the input ends: let it end, so an overrun is seen. On top of the input's budget.
  hal.passes = 0;
  for (int i = 0; i < 6000 && relaysBusy(); i++) pass(1);
}

static bool runUntil(const std::function<bool()> &cond, uint32_t maxMs, uint32_t step = 1) {
  uint32_t end = hal.ms + maxMs;
  while (!cond()) {
    if ((int32_t)(hal.ms - end) >= 0 || !pass(step)) return cond();
  }
  return true;
}

static JsonDocument boardStatus() {
  JsonDocument d;
  appFillStatus(d.to<JsonObject>());
  return d;
}

// --- the peer ----------------------------------------------------------------------------------------------------------
static void peerReset(FuzzRole role) {
  peer = Peer();
  peer.session = 0x5EE50000u | role;
  peer.seq = 0x1000;
}

void peerScan() {
  for (const Bytes &f : hal.tx) {
    if (f.size() >= HDR_LEN + TAG_LEN) {
      uint8_t type = f[1];
      peer.boardSession = getU32(&f[5]);
      if (type < 16) {
        peer.haveSeq[type] = true;
        peer.lastSeq[type] = getU32(&f[9]);
      }
      if (type == MSG_HELLO && f.size() >= HDR_LEN + 4 + TAG_LEN) {
        peer.prevChallenge = peer.challenge;
        peer.challenge = getU32(&f[HDR_LEN]);
        peer.haveChallenge = true;
      }
    }
    peer.heard.push_back(f);
    if (peer.heard.size() > 16) peer.heard.erase(peer.heard.begin());
  }
  hal.tx.clear();
}

static void tagOf(const uint8_t *buf, size_t len, uint8_t *tag) {
  SHA256 h;
  h.resetHMAC(cfg.key, sizeof(cfg.key));
  h.update(buf, len);
  h.finalizeHMAC(cfg.key, sizeof(cfg.key), tag, TAG_LEN);
}

Bytes peerFrame(uint8_t type, const uint8_t *payload, size_t len, uint32_t session, uint32_t seq) {
  Bytes f(HDR_LEN + len + TAG_LEN);
  f[0] = 1;  // PROTO_VER
  f[1] = type;
  f[2] = (uint8_t)cfg.net_id;
  f[3] = peerNodeId();  // from the board's point of view: the peer sends, the board receives
  f[4] = myNodeId();
  putU32(&f[5], session);
  putU32(&f[9], seq);
  if (len) memcpy(&f[HDR_LEN], payload, len);
  tagOf(f.data(), HDR_LEN + len, &f[HDR_LEN + len]);
  return f;
}

void deliver(const Bytes &frame, int16_t rssi, float snr) {
  // A radio that is off or held in reset hears nothing; the FIFO holds one frame and a newer one overwrites it.
  if (!hal.radioUp || hal.radioHeld) {
    halTraceFrame("RX lost (radio off)", frame);
    return;
  }
  halTraceFrame("RX", frame);
  hal.rxq.clear();
  hal.rxq.push_back({ frame, rssi, snr });
}

void sendAuth(uint8_t type, const uint8_t *payload, size_t len) {
  Bytes f = peerFrame(type, payload, len, peer.session, ++peer.seq);
  peer.sent.push_back(f);
  if (peer.sent.size() > 32) peer.sent.erase(peer.sent.begin());
  deliver(f);
}

void answerChallenge() {
  uint8_t p[4];
  putU32(p, peer.challenge);
  sendAuth(MSG_HELLO_ACK, p, 4);
}

void ackLast(uint8_t type, uint8_t result) {
  uint8_t p[5];
  putU32(p, type < 16 ? peer.lastSeq[type] : 0);
  p[4] = result;
  sendAuth(MSG_ACK, p, 5);
}

Bytes statusPayload(uint8_t state, uint8_t inputs) {
  Bytes p(ST_LEN, 0);
  p[ST_STATE] = state;
  p[ST_INPUTS] = inputs;
  putU32(&p[ST_UPTIME], hal.ms / 1000);
  putU16(&p[ST_RSSI], (uint16_t)-60);
  p[ST_SNR] = 9;
  putU16(&p[ST_HEARTBEAT], 30);
  p[ST_NOISE] = (uint8_t)(int8_t)-112;
  p[ST_NOISE_MAX] = (uint8_t)(int8_t)-105;
  putU16(&p[ST_TRAVEL], 60);
  return p;
}

// --- console ------------------------------------------------------------------------------------------------------------
void consoleLine(int port, const std::string &line) {
  for (char c : line) hal.rx[port].push_back((uint8_t)c);
  hal.rx[port].push_back('\n');
}

std::string withCrc(const std::string &json) {
  if (json.empty() || json.back() != '}') return json;
  char tail[24];
  snprintf(tail, sizeof(tail), ",\"crc\":\"%08x\"}", crc32(json.data(), json.size()));
  return json.substr(0, json.size() - 1) + tail;
}

// --- starting an input --------------------------------------------------------------------------------------------------
const Bytes &provisionedImage(FuzzRole role) { return images[role]; }

// Runs the firmware: a blank board given a role and the key, saved as the console's config.save would.
static Bytes makeImage(FuzzRole role) {
  halReset(1000);
  boot(PM_RCAUSE_POR);
  const ParamDef *p = paramByName("role");
  if (!p || !paramSet(p, role == FR_HOUSE ? ROLE_HOUSE : ROLE_GATE)) die("can't set the role");
  memcpy(cfg.key, FUZZ_KEY, sizeof(cfg.key));
  cfg.key_set = 1;
  if (!configSave()) die("provisioning: configSave failed");
  return hal.flash;
}

// The board up and verified with the peer, settled (see startWarm).
static void warmUp(FuzzRole role, bool wrap) {
  halReset(wrap ? WRAP_START_MS : 1000);
  hal.flash = images[role];
  setPins(role == FR_HOUSE ? 0x2 : 0x6);  // house: IN2 controller power; gate: IN2 closed limit, IN3 AC
  boot(PM_RCAUSE_POR);
  peerReset(role);
  if (!runUntil([] { return peer.haveChallenge; }, 3000)) die("warm boot: the board sent no HELLO");
  answerChallenge();
  settle();
  if (!linkPeerVerified()) die("warm boot: the board didn't verify the peer");
  if (role == FR_GATE) {
    if (!runUntil([] { return peer.haveSeq[MSG_STATUS]; }, 12000, 5)) die("warm boot: the gate sent no STATUS");
    ackLast(MSG_STATUS, RES_OK);
    settle();
    JsonDocument s = boardStatus();
    if (s["settling"] != false || s["gate"] != "closed" || linkPending(SLOT_STATUS))
      die("warm boot: the gate isn't settled and closed with its STATUS ACKed");
  } else {
    Bytes st = statusPayload(GS_CLOSED, STI_IN2 | STI_IN3);
    sendAuth(MSG_STATUS, st.data(), st.size());
    settle();
    advance(14000);  // ctrl_settle_ms (10 s from boot), then sync_window_ms after the first STATUS
    JsonDocument s = boardStatus();
    if (s["armed"] != true || s["sync_window"] != false || s["link_up"] != true || s["gate"] != "closed")
      die("warm boot: the house isn't armed and synced to a closed gate");
  }
  hal.passes = 0;
}

void startBlank(uint32_t startMs) {
#if FUZZ_SNAPSHOT
  load(pristine);
#else
  freshProcess();
#endif
  halReset(startMs);
}

void startCold(FuzzRole role, bool wrap, uint8_t pins) {
  startBlank(wrap ? WRAP_START_MS : 1000);
  hal.flash = images[role];
  setPins(pins);
  peerReset(role);
  boot(PM_RCAUSE_POR);
}

void startWarm(FuzzRole role, bool wrap) {
#if FUZZ_SNAPSHOT
  const Warm &w = warm[role][wrap];
  load(w.ram);
  hal = w.hal;
  peer = w.peer;
  hal.passes = 0;
#else
  freshProcess();
  warmUp(role, wrap);
#endif
}

// --- init and the self-test -------------------------------------------------------------------------------------------
#if FUZZ_SNAPSHOT
static void selfTest(FuzzOne one, const Bytes &a, const Bytes &b) {
  // Every firmware global must be in the sections (a file compiled without sections.h would leak state).
  const void *globals[] = { &cfg, &activeRole, &k1, &k2, &in1, &in2, &in3, &in4 };
  for (const void *g : globals)
    if (!inRam(g)) die("a firmware global is outside gl_bss/gl_data: is every firmware file built with sections.h?");
  // The warm boot is deterministic: twice from pristine RAM, the same RAM and output.
  load(pristine);
  warmUp(FR_GATE, false);
  uint64_t r1 = ramHash(), o1 = hal.outHash;
  load(pristine);
  warmUp(FR_GATE, false);
  if (ramHash() != r1 || hal.outHash != o1) die("self-test: two warm boots from pristine RAM differ");
  // An input gives the same output whatever ran before it.
  one(a.data(), a.size());
  uint64_t ha = hal.outHash;
  uint32_t la = hal.lineCount, ta = hal.txCount;
  one(b.data(), b.size());
  int32_t changed = cfg.pulse_ms;
  one(a.data(), a.size());
  if (hal.outHash != ha || hal.lineCount != la || hal.txCount != ta)
    die("self-test: the same input gave different output after another input ran");
  if (ha == Hal().outHash) die("self-test: sample A produced no output");
  // A global the second sample changed is back to its initial value for the next input.
  startBlank(1000);
  if (cfg.pulse_ms != 0 || activeRole != ROLE_UNSET) die("self-test: pristine RAM not restored");
  startWarm(FR_GATE, false);
  if (cfg.pulse_ms != 500) die("self-test: warm RAM not restored");
  fprintf(stderr,
          "gatelink-fuzz: fresh firmware RAM per input verified (gl_bss %zu B, gl_data %zu B; sample B left "
          "pulse_ms %d)\n",
          bssLen(), dataLen(), (int)changed);
}
#endif

// GATELINK_FUZZ_DUMP_IMAGES=<dir>: write the provisioned flash images there (house.bin, gate.bin), for make_seeds.py.
static void dumpImages() {
  const char *dir = getenv("GATELINK_FUZZ_DUMP_IMAGES");
  if (!dir) return;
  for (int role = 0; role < 2; role++) {
    std::string path = std::string(dir) + (role == FR_HOUSE ? "/house.bin" : "/gate.bin");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f || fwrite(images[role].data(), 1, images[role].size(), f) != images[role].size()) die("can't write the images");
    fclose(f);
  }
}

void harnessSelfTest(FuzzOne one, const Bytes &sampleA, const Bytes &sampleB) {
#if FUZZ_SNAPSHOT
  selfTest(one, sampleA, sampleB);
  load(pristine);
#else
  (void)one, (void)sampleA, (void)sampleB;
#endif
}

void harnessSetup() {
#if FUZZ_SNAPSHOT
  save(pristine);
  images[FR_HOUSE] = makeImage(FR_HOUSE);
  load(pristine);
  images[FR_GATE] = makeImage(FR_GATE);
  dumpImages();
  for (int role = 0; role < 2; role++) {
    for (int wrap = 0; wrap < 2; wrap++) {
      load(pristine);
      warmUp((FuzzRole)role, wrap);
      save(warm[role][wrap].ram);
      warm[role][wrap].hal = hal;
      warm[role][wrap].peer = peer;
    }
  }
  load(pristine);
#else
  // Keep this process's firmware RAM untouched: provision in a child and take the images back through a pipe.
  int fd[2];
  if (pipe(fd)) die("pipe");
  pid_t pid = fork();
  if (pid < 0) die("fork");
  if (pid == 0) {
    close(fd[0]);
    for (int role = 0; role < 2; role++) {
      Bytes img = makeImage((FuzzRole)role);
      if (write(fd[1], img.data(), img.size()) != (ssize_t)img.size()) _exit(1);
    }
    _exit(0);
  }
  close(fd[1]);
  for (int role = 0; role < 2; role++) {
    images[role].assign(HAL_FLASH_BYTES, 0xFF);
    size_t got = 0;
    while (got < HAL_FLASH_BYTES) {
      ssize_t n = read(fd[0], images[role].data() + got, HAL_FLASH_BYTES - got);
      if (n <= 0) die("provisioning child failed");
      got += (size_t)n;
    }
  }
  close(fd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) die("provisioning child failed");
  dumpImages();
#endif
}
