// The fuzz targets' board (hal.h): the firmware's hardware calls, and the invariant monitors on its pins.
#include "hal.h"
#include <ArduinoJson.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <Arduino.h>
#include "board.h"
#include "config.h"
#include "console_io.h"
#include "extflash.h"
#include "log.h"
#include "radio.h"
#include "roles.h"
#include "supply.h"

Hal hal;

static uint64_t next64(uint64_t &s) {  // xorshift64*, as world.cpp
  s ^= s >> 12;
  s ^= s << 25;
  s ^= s >> 27;
  return s * 0x2545F4914F6CDD1DULL;
}

void halHash(const void *p, size_t n) {
  const uint8_t *b = (const uint8_t *)p;
  for (size_t i = 0; i < n; i++) hal.outHash = (hal.outHash ^ b[i]) * 1099511628211ULL;
}

void halReset(uint32_t startMs) {
  hal = Hal();
  hal.ms = startMs;
  hal.kickAt = startMs;
  hal.prng = 0x243F6A8885A308D3ULL;
  hal.rng = 0x9E3779B97F4A7C15ULL;
  hal.strictPulse = getenv("GATELINK_FUZZ_STRICT_PULSE") != nullptr;
  hal.knownBugs = getenv("GATELINK_FUZZ_KNOWN_BUGS") != nullptr;
  hal.trace = getenv("GATELINK_FUZZ_TRACE") != nullptr;
}

void halTrace(const char *fmt, ...) {
  if (!hal.trace) return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%10u  ", hal.ms);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

void halTraceFrame(const char *dir, const Bytes &f) {
  if (!hal.trace) return;
  std::string hex;
  char b[4];
  for (uint8_t x : f) {
    snprintf(b, sizeof(b), "%02x", x);
    hex += b;
  }
  halTrace("%s type %u len %zu: %s", dir, f.size() > 1 ? f[1] : 0, f.size(), hex.c_str());
}

void halTrap(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "\n==GATELINK INVARIANT== t=%u ms: ", hal.ms);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  fflush(stderr);
  __builtin_trap();
}

static bool after(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

// --- the clock ------------------------------------------------------------------------------------------------------
// The firmware waited (delay, flash, radio init). The loop's watchdog is 8 s: a pass that blocks that long resets
// the board, so it's a breach here.
static void block(uint32_t us) {
  hal.us += us;
  hal.ms += hal.us / 1000;
  hal.us %= 1000;
  if (!after(hal.kickAt + HAL_WATCHDOG_MS, hal.ms))
    halTrap("watchdog: the loop blocked %u ms without a watchdog reset", hal.ms - hal.kickAt);
}

uint32_t millis() { return hal.ms; }
uint32_t micros() { return hal.ms * 1000u + hal.us; }
void delay(uint32_t ms) { block(ms * 1000u); }
void delayMicroseconds(uint32_t us) { block(us); }
long random(long max) { return max <= 0 ? 0 : (long)(next64(hal.prng) % (uint64_t)max); }
long random(long min, long max) { return max <= min ? min : min + random(max - min); }
void randomSeed(unsigned long seed) { hal.prng = seed ? seed : 1; }

// --- gate relay monitors ----------------------------------------------------------------------------------------------
// What the gate may keep a relay on for, from sources independent of what the firmware logs: pulse_ms, or the ms of
// a console relay.test for that relay read just before (its pulse may wait out the 100 ms interlock first): one that
// ran (its reply said ok), or the one being handled (the pulse starts before the reply goes out).
static uint32_t allowance(int k, uint32_t now) {
  uint32_t a = (uint32_t)cfg.pulse_ms;
  for (const Hal::TestAsk &t : hal.test[k])
    if (t.valid && (uint32_t)(now - t.at) <= 300 && t.ms > a) a = t.ms;
  for (const Hal::PendingAsk &p : hal.asked)
    if (p.valid && p.k == k + 1 && (uint32_t)(now - p.at) <= 300 && p.ms > a) a = p.ms;
  return a;
}

// The reply to a port's relay.test: if it ran, keep it (in a free slot, else over the oldest).
static void onTestReply(int port, bool ok) {
  Hal::PendingAsk &p = hal.asked[port];
  if (!p.valid) return;
  p.valid = false;
  if (!ok) return;
  if (p.k == 2) hal.houseK2Test = true;
  Hal::TestAsk *slot = nullptr;
  for (Hal::TestAsk &t : hal.test[p.k - 1])
    if (!slot || !t.valid || (slot->valid && (int32_t)(t.at - slot->at) < 0)) {
      slot = &t;
      if (!t.valid) break;
    }
  *slot = { true, p.at, p.ms };
}

// A stall the harness doesn't hold against the relays (a known firmware bug, see radioInit): their timers, and the
// relay.test requests they are measured against, move on by it.
static void excuseStall(uint32_t ms) {
  for (RelayMon &r : hal.relay) {
    if (!r.on) continue;
    r.onAt += ms;
    r.pulseAt += ms;
  }
  for (auto &asks : hal.test)
    for (Hal::TestAsk &t : asks) t.at += ms;
  for (Hal::PendingAsk &p : hal.asked) p.at += ms;
}

static void checkOnTime(int k, uint32_t now) {
  const RelayMon &r = hal.relay[k];
  if (!r.on) return;
  uint32_t onFor = now - r.onAt;
  if (onFor <= r.allowMs + HAL_PULSE_SLACK_MS) return;
  if (r.repulsed && !hal.strictPulse) {
    // Pulsed again while on (a second command, or a test): that pulse runs its own length from its start.
    uint32_t since = now - r.pulseAt;
    if (since <= r.pulseAllowMs + HAL_PULSE_SLACK_MS) return;
    halTrap("gate K%d still on %u ms after its latest pulse started (allowed %u + %u ms; on %u ms in all)", k + 1,
            since, r.pulseAllowMs, HAL_PULSE_SLACK_MS, onFor);
  }
  halTrap("gate K%d on for %u ms (allowed %u + %u ms%s)", k + 1, onFor, r.allowMs, HAL_PULSE_SLACK_MS,
          r.repulsed ? "; it was pulsed again while on" : "");
}

static void relayEdge(int k, bool on) {
  if (activeRole != ROLE_GATE) return;
  RelayMon &r = hal.relay[k], &o = hal.relay[1 - k];
  uint32_t now = hal.ms;
  if (on) {
    if (o.on) halTrap("gate K%d energized while K%d is on", k + 1, 2 - k);
    if (o.offSeen && now - o.offAt < HAL_INTERLOCK_MS)
      halTrap("gate K%d energized %u ms after K%d released (interlock %u ms)", k + 1, now - o.offAt, 2 - k,
              HAL_INTERLOCK_MS);
    r.on = true;
    r.onAt = now;
    r.allowMs = allowance(k, now);
    r.repulsed = false;
  } else {
    checkOnTime(k, now);
    r.on = false;
    r.offSeen = true;
    r.offAt = now;
  }
}

// The firmware logged `pulse` (a = relay, b = ms): a pulse was asked for. If the relay is already on (it went on
// before this call), it's a second pulse that restarts the timer. In the very millisecond it went on, it may be a
// second request too (a delayed start goes on at the top of the loop pass, a console relay.test is read later in
// the same pass): then what it may stay on for is the longer of the two, from that same start.
static void onPulseEvent(int32_t relay, int32_t ms) {
  if (activeRole != ROLE_GATE || (relay != 1 && relay != 2)) return;
  RelayMon &r = hal.relay[relay - 1];
  if (!r.on) return;  // it starts later (interlock): the allowance is taken when it goes on
  uint32_t a = allowance(relay - 1, hal.ms);
  if (hal.ms == r.onAt) {
    if (a > r.allowMs) r.allowMs = a;
    return;
  }
  r.repulsed = true;
  r.pulseAt = hal.ms;
  r.pulseAllowMs = a;
  (void)ms;
}

// House K2 is the alarm's contact sensor: closed only while the house knows the gate as closed and, with
// linkloss_open, the link is up (CLAUDE.md). A console relay.test of K2 overrides it until K2 next releases (the
// end of the test pulse; the house then sets it as the gate's state says).
static void checkHouseSensor() {
  if (activeRole != ROLE_HOUSE || !hal.out[HP_K2] || cfg.sensor_invert || hal.houseK2Test) return;
  if (hal.houseView != GS_CLOSED || (!hal.houseLink && cfg.linkloss_open))
    halTrap("house K2 (contact sensor) closed while the house knows the gate as %d, link %s", (int)hal.houseView,
            hal.houseLink ? "up" : "down");
}

void halCheckPass() {
  for (int k = 0; k < 2; k++) checkOnTime(k, hal.ms);
  checkHouseSensor();
}

// --- pins -----------------------------------------------------------------------------------------------------------
void pinMode(uint32_t pin, uint32_t mode) {
  if (pin < HP_COUNT) hal.mode[pin] = (uint8_t)mode;
}

void digitalWrite(uint32_t pin, uint32_t value) {
  if (pin >= HP_COUNT) return;
  bool v = value != 0;
  if ((pin == HP_K1 || pin == HP_K2) && v != hal.out[pin]) {
    halTrace("K%u %s", pin, v ? "on" : "off");
    relayEdge(pin - HP_K1, v);
    if (pin == HP_K2 && !v) hal.houseK2Test = false;
  }
  hal.out[pin] = v;
}

int digitalRead(uint32_t pin) {
  if (pin >= HP_IN1 && pin <= HP_IN4) return hal.in[pin] ? HIGH : LOW;
  return LOW;  // pull-down: nothing connected reads inactive
}

void analogWrite(uint32_t pin, int value) {
  if (pin == HP_LED) hal.led = value;
}

// --- board.h ----------------------------------------------------------------------------------------------------------
void boardKick() { hal.kickAt = hal.ms; }

void boardReset() {
  // The reset drops the relays now: what they were on for up to here counts. Known bug (README.md): the console's
  // reboot flushes its reply, waits 100 ms and resets, even while a relay pulses, so the pulse can run up to 100 ms
  // long. Unless knownBugs, judge the relays as they were before that wait.
  uint32_t at = hal.ms;
  if (!hal.knownBugs && hal.rebootFlushed && hal.ms - hal.rebootFlushAt <= 100) at = hal.rebootFlushAt;
  for (int k = 0; k < 2; k++) checkOnTime(k, at);
  throw BoardReset{ PM_RCAUSE_SYST };
}

uint32_t boardFreeRam() { return 10000; }

// --- radio.h ----------------------------------------------------------------------------------------------------------
uint32_t halAirtimeMs(size_t len) {  // SX1276 time on air, as world.cpp (explicit header, CRC on)
  int sf = cfg.sf, bw = cfg.bw_hz, cr = cfg.cr;
  if (sf < 6 || sf > 12 || bw <= 0) halTrap("radio params out of range: sf %d bw %d", sf, bw);
  double tsym = (double)(1u << sf) / bw * 1000.0;
  int de = tsym > 16.0 ? 1 : 0;
  double pl = ceil((8.0 * len - 4 * sf + 28 + 16) / (4.0 * (sf - 2 * de)));
  double symbols = 8 + 4.25 + 8 + (pl > 0 ? pl * cr : 0);
  return (uint32_t)ceil(symbols * tsym);
}

// radio.cpp's radioBegin(). `recovery`: called by radio.cpp itself, to recover from a fault or retry a radio that
// didn't answer. Those run whenever they're due, also while a gate relay pulses, and the ~0.45 s they block the loop
// holds the pulse that much longer: a known firmware bug (README.md), which the relay monitors excuse unless
// knownBugs. Every other start (boot, a radio setting, the key, after a flash access) waits for the relays.
static bool radioInit(bool recovery) {
  hal.radioBegun = true;
  hal.radioHeld = false;
  hal.rxq.clear();     // the reset loses whatever was in the FIFO
  hal.txEnd = hal.ms;  // and abandons a TX
  uint32_t t0 = hal.ms;
  block(HAL_RADIO_INIT_US);
  if (recovery && !hal.knownBugs) excuseStall(hal.ms - t0);
  hal.radioUp = hal.radioPresent;
  if (!hal.radioUp) {
    if (!hal.retryAt) logEvent(EV_RADIO_FAIL, 0, (int32_t)hal.faults);
    hal.retryAt = (hal.ms + 5000) | 1;
    return false;
  }
  hal.retryAt = 0;
  return true;
}

bool radioBegin() { return radioInit(false); }

void halRadioFault() {
  if (!hal.radioUp) return;
  hal.faults++;
  logEvent(EV_RADIO_FAIL, 1, (int32_t)hal.faults);
  radioInit(true);
  hal.txEnd = hal.ms;
}

void radioRestart() {
  if (hal.radioBegun) radioBegin();
}

bool radioOk() { return hal.radioUp; }

bool radioTxBusy() { return hal.radioUp && !after(hal.ms, hal.txEnd); }

bool radioSend(const uint8_t *buf, size_t len) {
  if (!hal.radioUp || hal.radioHeld || radioTxBusy()) return false;
  if (len == 0 || len > 255) halTrap("radioSend of %zu bytes", len);
  hal.tx.emplace_back(buf, buf + len);
  hal.txCount++;
  halTraceFrame("TX", hal.tx.back());
  uint8_t tag = 'T';
  halHash(&tag, 1);
  halHash(buf, len);
  hal.txEnd = hal.ms + halAirtimeMs(len);
  return true;
}

uint32_t radioTxEndAt() { return hal.txEnd; }
uint32_t radioFaults() { return hal.faults; }
uint32_t radioCrcErrors() { return hal.crcErr; }
uint32_t radioRxDoneCount() { return hal.rxDone; }
int32_t radioLastFei() { return 0; }

bool radioChannelBusy() {
  if (!hal.radioUp) return false;
  return radioTxBusy() || !hal.rxq.empty() || hal.jammed;  // a frame waiting unread holds the channel too
}

size_t radioReceive(uint8_t *buf, size_t max, int16_t &rssi, float &snr) {
  if (!hal.radioUp) {
    if (hal.retryAt && after(hal.ms, hal.retryAt) && radioInit(true)) logEvent(EV_RADIO_FAIL, 3, (int32_t)hal.faults);
    return 0;
  }
  if (hal.radioHeld || radioTxBusy() || hal.rxq.empty()) return 0;
  RxFrame f = hal.rxq.front();
  hal.rxq.pop_front();
  hal.rxDone++;
  if (f.b.empty()) {
    hal.crcErr++;
    return 0;
  }
  if (f.b.size() > max) return 0;  // the radio drops oversize packets
  memcpy(buf, f.b.data(), f.b.size());
  rssi = f.rssi;
  snr = f.snr;
  return f.b.size();
}

int16_t radioNoiseDbm() {
  if (!hal.radioUp || radioTxBusy() || !hal.rxq.empty() || hal.jammed) return INT16_MIN;
  return hal.noise;
}

uint32_t radioAirtimeMs(size_t payloadLen) { return halAirtimeMs(payloadLen); }

uint32_t radioRandom32() {
  block(20500);  // 512 RSSI samples 40 us apart
  return (uint32_t)(next64(hal.rng) >> 32);
}

void radioAddEntropy(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) hal.rng = (hal.rng ^ p[i]) * 0x100000001B3ULL;
  if (!hal.rng) hal.rng = 1;
}

// --- extflash.h --------------------------------------------------------------------------------------------------------
static void flashRange(uint32_t addr, size_t len) {
  if (addr > HAL_FLASH_BYTES || len > HAL_FLASH_BYTES - addr)
    halTrap("flash access at 0x%x+%zu, beyond sectors 0..3", addr, len);
}

void extFlashHoldModem() {
  hal.radioHeld = true;  // the module's reset resets the SX1276 too: a frame on its way in or out is lost
  hal.radioUp = false;
  hal.rxq.clear();
  hal.txEnd = hal.ms;
  block(1000);
}

bool extFlashBegin() {
  extFlashHoldModem();
  return hal.flashPresent;
}

bool extFlashPresent() { return hal.flashPresent; }
uint32_t extFlashId() { return hal.flashPresent ? 0xEF4015 : 0; }

void extFlashRead(uint32_t addr, uint8_t *buf, size_t len) {
  flashRange(addr, len);
  if (!hal.flashPresent) {
    memset(buf, 0xFF, len);
    return;
  }
  memcpy(buf, hal.flash.data() + addr, len);
  block((uint32_t)len * 8 + 40);  // 1 MHz SPI
}

bool extFlashEraseSector(uint32_t addr) {
  if (!hal.flashPresent) return false;
  addr &= ~(EXTFLASH_SECTOR - 1);
  flashRange(addr, EXTFLASH_SECTOR);
  memset(hal.flash.data() + addr, 0xFF, EXTFLASH_SECTOR);
  block(45000);  // datasheet typical
  return true;
}

bool extFlashProgram(uint32_t addr, const uint8_t *buf, size_t len) {
  if (!hal.flashPresent || len == 0 || (addr % EXTFLASH_PAGE) + len > EXTFLASH_PAGE) return false;
  flashRange(addr, len);
  for (size_t i = 0; i < len; i++) hal.flash[addr + i] &= buf[i];  // NOR: programming only clears bits
  block(800 + (uint32_t)len * 8);
  return true;
}

// --- supply.h ----------------------------------------------------------------------------------------------------------
void supplyBegin() {
  hal.supplySeen = !hal.supplyKnown || hal.supplyGood;
  hal.supplyPollAt = hal.ms;
  logEvent(EV_SUPPLY, hal.supplyKnown ? hal.supplySeen : -1, hal.supplyKnown ? (hal.supplySeen ? 0x04 : 0) : -1);
}

void supplyPoll(uint32_t now) {
  if (!hal.supplyKnown || !after(now, hal.supplyPollAt + 5)) return;
  hal.supplyPollAt = now;
  if (hal.supplyGood == hal.supplySeen) return;
  hal.supplySeen = hal.supplyGood;
  logEvent(EV_SUPPLY, hal.supplySeen, hal.supplySeen ? 0x04 : 0);
}

bool supplyKnown() { return hal.supplyKnown; }
bool supplyGood() { return hal.supplySeen; }

// --- console_io.h ------------------------------------------------------------------------------------------------------
// A request line as the firmware read it. relay.test may pulse for up to 5 s: note what it asked for (read as
// console.cpp does), so the relay monitor allows it. It counts once its reply says it ran (onTestReply): console.cpp
// answers each request before it reads the next line on that port, so the next reply there is this one's.
static void onRequestLine(int port, const std::string &line) {
  hal.asked[port].valid = false;
  // Read exactly as console.cpp does: the command as a C string (so "relay.test\u0000" is relay.test there too),
  // after the JSON escapes are decoded (so a plain-text search for the name would miss "relay.test").
  JsonDocument req;
  if (deserializeJson(req, line.c_str())) return;
  const char *cmd = req["cmd"] | "";
  if (strcmp(cmd, "relay.test")) return;
  int32_t k = req["k"] | 0;
  int32_t ms = req["ms"].isNull() ? 500 : req["ms"].is<int32_t>() ? req["ms"].as<int32_t>() : -1;
  if ((k != 1 && k != 2) || ms < 50 || ms > 5000) return;
  hal.asked[port] = { true, (uint8_t)k, hal.ms, (uint32_t)ms };
}

// Every line the board prints must be one JSON object: a reply ("ok") or an event. On the host no line is cut.
static void onOutputLine(int port, const std::string &line) {
  hal.lineCount++;
  halTrace("%s%s", port ? "uart: " : "", line.c_str());
  uint8_t tag[2] = { 'L', (uint8_t)port };
  halHash(tag, 2);
  halHash(line.data(), line.size());
  if (hal.keepLines) hal.lines.push_back(port ? "uart:" + line : line);
  JsonDocument d;
  DeserializationError e = deserializeJson(d, line, DeserializationOption::NestingLimit(64));
  if (e) halTrap("console line isn't JSON (%s): %.300s", e.c_str(), line.c_str());
  if (!d.is<JsonObject>() || !(d["event"].is<const char *>() || d["ok"].is<bool>()))
    halTrap("console line is neither a reply nor an event: %.300s", line.c_str());
  if (!d["event"].is<const char *>()) onTestReply(port, d["ok"] == true);
  if (port != CON_USB || d["event"] != "log") return;
  const char *ev = d["ev"] | "";
  if (!strcmp(ev, "pulse")) {
    onPulseEvent(d["a"] | 0, d["b"] | 0);
  } else if (!strcmp(ev, "boot")) {
    hal.houseView = GS_UNKNOWN;
    hal.houseLink = false;
  } else if (activeRole == ROLE_HOUSE) {
    if (!strcmp(ev, "gate_state")) hal.houseView = d["a"] | 0;
    else if (!strcmp(ev, "link_up")) hal.houseLink = true;
    else if (!strcmp(ev, "link_down")) hal.houseLink = false;
  }
}

void conIoBegin() {}
void conIoUart(bool on) { hal.uartOn = on; }

bool conIoOpen(uint8_t port) { return port == CON_USB ? hal.usbHost : hal.uartOn && hal.uartAdapter; }

int conIoRead(uint8_t port) {
  if (port >= CON_PORTS) halTrap("conIoRead port %u", port);
  std::deque<uint8_t> &q = hal.rx[port];
  if (q.empty()) return -1;
  int c = q.front();
  q.pop_front();
  std::string &l = hal.lineIn[port];
  if (c == '\n') {
    halTrace("%s<- %s", port ? "uart: " : "", l.c_str());
    onRequestLine(port, l);
    l.clear();
  } else if (c != '\r' && l.size() < 4096) {
    l.push_back((char)c);
  }
  return c;
}

// Only the console's reboot flushes (its reply, before it waits 100 ms and resets).
void conIoFlush(uint8_t) {
  hal.rebootFlushed = true;
  hal.rebootFlushAt = hal.ms;
}

void conIoLineWrite(uint8_t port, const uint8_t *data, size_t n) {
  if (port >= CON_PORTS) halTrap("conIoLineWrite port %u", port);
  hal.lineOut[port].append((const char *)data, n);
}

bool conIoLineEnd(uint8_t port) {
  if (port >= CON_PORTS) halTrap("conIoLineEnd port %u", port);
  std::string all;
  all.swap(hal.lineOut[port]);
  if (all.empty() || all.back() != '\n') halTrap("console line without its newline: %.300s", all.c_str());
  size_t s = 0;
  while (s < all.size()) {
    size_t e = all.find('\n', s);
    if (e > s) onOutputLine(port, all.substr(s, e - s));
    s = e + 1;
  }
  return true;
}
