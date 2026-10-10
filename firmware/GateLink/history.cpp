#include "history.h"
#include "histlog.h"
#include "link.h"
#include "radio.h"

#define NOISE_EVERY_MS 250
#define NO_MIN INT16_MAX

struct Bucket {
  int32_t rssiSum, snrSumQ, noiseSum;  // SNR in quarter dB, as the radio reports it
  int32_t peerRssiSum, peerSnrSum, peerNoiseSum;
  uint16_t tx, rx, retries, crcErr, lbtDefers, downS;
  uint16_t rssiN, noiseN, peerN, peerNoiseN, peerRetries, peerCrc;
  int16_t rssiMin, noiseMax, peerRssiMin, peerNoiseMax;
  uint8_t giveups, macFail, lbtForced, sessions, faults, peerGiveups;
  int8_t snrMinQ, peerSnrMin;
  uint32_t boot;  // the boot it was recorded in (appBootCount; 0 = unknown)
};
static_assert(sizeof(Bucket) == HLOG_DATA, "a bucket is what the flash log keeps");

static Bucket ring[HIST_DEPTH + 1];
static uint32_t period = HIST_PERIOD_S;
// Seconds since bucket 0 started, as if the history had run without a break (a boot goes on at cur * period).
static uint32_t secs;
static uint32_t secAt;  // millis() at the last whole second
static uint32_t cur;    // number of the bucket in progress
static uint32_t base;   // the oldest bucket there is: 0, or the oldest loaded from the flash at boot
static uint32_t saveNext;  // the next completed bucket to write to the flash (== cur: none waiting)
static uint32_t bootNo;
static uint32_t noiseAt;
static int16_t pendingNoise = NOISE_NONE;  // last reading, kept once no frame turned out to be starting
static uint32_t pendingRxDone;

// Totals at the last poll: their increase goes into the bucket in progress.
static uint32_t pTx, pRx, pRetries, pGiveups, pMac, pLbtDefers, pLbtForced, pSessions, pCrc, pFaults;
static bool peerPrev;  // the last STATUS carried the gate's counters
static uint16_t pPeerRetries, pPeerGiveups, pPeerCrc;

// Noise since the last histNoiseTake(), and a smoothed value (1/256 dB) for status.
static int32_t takeSum;
static uint16_t takeN;
static int16_t takeMax;
static int32_t noiseEma;
static bool haveEma;

static Bucket &bucket() {
  return ring[cur % (HIST_DEPTH + 1)];
}

static void startBucket() {
  Bucket &b = bucket();
  memset(&b, 0, sizeof(b));
  b.rssiMin = b.peerRssiMin = NO_MIN;
  b.noiseMax = b.peerNoiseMax = NOISE_NONE;
  b.snrMinQ = b.peerSnrMin = INT8_MAX;
  b.boot = bootNo;
}

// The history goes on at bucket `next` (base: the oldest there is), its first second from now.
static void startAt(uint32_t periodS, uint32_t next, uint32_t oldest) {
  period = periodS;
  cur = next;
  base = oldest;
  saveNext = next;
  secs = next * periodS;
  secAt = millis();
  startBucket();
}

static void add16(uint16_t &c, uint32_t d) {
  uint32_t v = c + d;
  c = v > 0xFFFF ? 0xFFFF : v;
}

static void add8(uint8_t &c, uint32_t d) {
  uint32_t v = c + d;
  c = v > 0xFF ? 0xFF : v;
}

// Increase of a running total since the last call. A total that went down was reset (link restart): count it
// from zero.
static uint32_t grew(uint32_t v, uint32_t &prev) {
  uint32_t d = v >= prev ? v - prev : v;
  prev = v;
  return d;
}

static uint16_t grew16(uint16_t v, uint16_t &prev) {
  uint16_t d = v >= prev ? v - prev : v;
  prev = v;
  return d;
}

static int8_t clamp8(int32_t v) {
  return v < INT8_MIN ? INT8_MIN : v > INT8_MAX - 1 ? INT8_MAX - 1 : v;  // INT8_MAX marks "no sample"
}

HistClear histClear(uint32_t periodS) {
  if (periodS < 60 || periodS > 3600) return HIST_BAD_PERIOD;
  startAt(periodS, 0, 0);
  return histLogClear((uint16_t)periodS) ? HIST_CLEARED : HIST_NOT_SAVED;
}

uint32_t histPeriod() {
  return period;
}

static uint32_t loadedFrom;  // the oldest bucket histLogBegin handed over (newest first)

static void takeBucket(uint32_t idx, const uint8_t *data) {
  memcpy(&ring[idx % (HIST_DEPTH + 1)], data, sizeof(Bucket));
  loadedFrom = idx;
}

void histBegin(uint32_t bootCount) {
  bootNo = bootCount;
  uint16_t p = HIST_PERIOD_S;
  uint32_t next = 0;
  loadedFrom = UINT32_MAX;
  // What the flash holds, if it makes sense here: a period hist.clear can set, and numbers that leave secs room to go
  // on (an hourly history fills half of it in 68 years).
  if (histLogBegin(HIST_DEPTH, takeBucket, p, next) && p >= 60 && p <= 3600 && (uint64_t)next * p <= UINT32_MAX / 2
      && (loadedFrom == UINT32_MAX || loadedFrom < next))
    startAt(p, next, loadedFrom == UINT32_MAX ? next : loadedFrom);
  else
    startAt(HIST_PERIOD_S, 0, 0);
  noiseAt = millis();
}

static void addNoise(int16_t n) {
  Bucket &b = bucket();
  if (b.noiseN < 0xFFFF) {
    b.noiseSum += n;
    b.noiseN++;
  }
  if (n > b.noiseMax) b.noiseMax = n;
  if (takeN < 0xFFFF) {
    takeSum += n;
    takeN++;
  }
  if (takeN == 1 || n > takeMax) takeMax = n;
  noiseEma = haveEma ? noiseEma + (n * 256 - noiseEma) / 16 : n * 256;
  haveEma = true;
}

// A reading taken before the modem detected a frame's preamble is that frame (the peer's frames showed up as
// noise peaks at their own RSSI). So a reading counts only if, by the next one, no frame has ended since and
// none is on the air; while one is, it waits for it to end.
static void sampleNoise() {
  if (pendingNoise != NOISE_NONE) {
    if (radioRxDoneCount() != pendingRxDone) {
      pendingNoise = NOISE_NONE;
    } else if (radioChannelBusy()) {
      return;
    } else {
      addNoise(pendingNoise);
    }
  }
  pendingRxDone = radioRxDoneCount();
  pendingNoise = radioNoiseDbm();
}

void histPoll(uint32_t now, bool linkUp) {
  Bucket &b = bucket();
  const LinkStats &st = linkStats();
  add16(b.tx, grew(st.tx, pTx));
  uint32_t rx = grew(st.rx, pRx);
  add16(b.rx, rx);
  // linkPoll takes at most one frame per loop, so the last RSSI/SNR are that frame's.
  if (rx && b.rssiN < 0xFFFF) {
    int16_t snrQ = (int16_t)lroundf(st.lastSnr * 4);
    b.rssiSum += st.lastRssi;
    b.snrSumQ += snrQ;
    b.rssiN++;
    if (st.lastRssi < b.rssiMin) b.rssiMin = st.lastRssi;
    if (b.snrMinQ == INT8_MAX || snrQ < b.snrMinQ) b.snrMinQ = clamp8(snrQ);
  }
  add16(b.retries, grew(st.retries, pRetries));
  add8(b.giveups, grew(st.giveups, pGiveups));
  add8(b.macFail, grew(st.macFail, pMac));
  add16(b.lbtDefers, grew(st.lbtDefers, pLbtDefers));
  add8(b.lbtForced, grew(st.lbtForced, pLbtForced));
  add8(b.sessions, grew(st.sessions, pSessions));
  add16(b.crcErr, grew(radioCrcErrors(), pCrc));
  add8(b.faults, grew(radioFaults(), pFaults));

  if (elapsed(now, noiseAt, NOISE_EVERY_MS)) {
    noiseAt = now;
    sampleNoise();
  }

  // A stalled loop catches up second by second; the link state then counts for all of them.
  while (elapsed(now, secAt, 1000)) {
    secAt += 1000;
    if (!linkUp) add16(bucket().downS, 1);
    if (++secs % period == 0) {
      cur++;
      startBucket();
    }
  }
}

bool histSaveDue() {
  return saveNext != cur && histLogOn();
}

void histSave() {
  if (cur - saveNext > HIST_DEPTH) saveNext = cur - HIST_DEPTH;  // left the ring meanwhile
  histLogAppend(saveNext, (uint16_t)period, (const uint8_t *)&ring[saveNext % (HIST_DEPTH + 1)]);
  saveNext++;  // written or not: a failing chip costs one try per bucket, not one per loop pass
}

void histPeer(const PeerReport &r) {
  Bucket &b = bucket();
  if (r.rssi != 0 && b.peerN < 0xFFFF) {
    b.peerRssiSum += r.rssi;
    b.peerSnrSum += r.snr;
    b.peerN++;
    if (r.rssi < b.peerRssiMin) b.peerRssiMin = r.rssi;
    if (b.peerSnrMin == INT8_MAX || r.snr < b.peerSnrMin) b.peerSnrMin = clamp8(r.snr);
  }
  if (!r.ext) {
    peerPrev = false;
    return;
  }
  if (r.noiseAvg != 0 && b.peerNoiseN < 0xFFFF) {
    b.peerNoiseSum += r.noiseAvg;
    b.peerNoiseN++;
    if (r.noiseMax > b.peerNoiseMax) b.peerNoiseMax = r.noiseMax;
  }
  // The first report after our boot (or from older gate firmware) only sets the baseline: its totals cover the
  // gate's whole uptime.
  uint16_t dr = grew16(r.retries, pPeerRetries), dg = grew16(r.giveups, pPeerGiveups), dc = grew16(r.crcErr, pPeerCrc);
  if (peerPrev) {
    add16(b.peerRetries, dr);
    add8(b.peerGiveups, dg);
    add16(b.peerCrc, dc);
  }
  peerPrev = true;
}

int16_t histNoiseNow() {
  if (!haveEma) return NOISE_NONE;
  return (int16_t)((noiseEma + (noiseEma < 0 ? -128 : 128)) / 256);
}

void histNoiseTake(int8_t &avg, int8_t &max) {
  if (!takeN) {
    avg = max = 0;
    return;
  }
  int32_t a = (takeSum - (int32_t)takeN / 2) / (int32_t)takeN;  // rounded (the sum is negative)
  avg = a < INT8_MIN ? INT8_MIN : a;
  max = takeMax < INT8_MIN ? INT8_MIN : takeMax;
  takeSum = 0;
  takeN = 0;
}

// Averages rounded to 0.25 dB: exact in binary, so they print short.
static void addAvg(JsonArray row, int32_t sum, uint32_t n, float scale) {
  if (n) row.add(roundf(sum * 4.0f / (n * scale)) / 4);
  else row.add(nullptr);
}

static void addLevel(JsonArray row, int32_t v, bool have, float scale) {
  if (have) row.add(v / scale);
  else row.add(nullptr);
}

static const char *const FIELDS[] = {
  "idx", "tx", "rx", "retries", "giveups", "crc_err", "mac_fail", "lbt_defers", "lbt_forced", "sessions",
  "radio_faults", "down_s", "rssi_min", "rssi_avg", "snr_min", "snr_avg", "noise_avg", "noise_max", "peer_n",
  "peer_rssi_min", "peer_rssi_avg", "peer_snr_min", "peer_snr_avg", "peer_noise_avg", "peer_noise_max",
  "peer_retries", "peer_giveups", "peer_crc_err", "boot",
};

void histGet(JsonObject res, int32_t from, int32_t n) {
  uint32_t oldest = cur > HIST_DEPTH ? cur - HIST_DEPTH : 0;
  if (base > oldest) oldest = base;
  res["period_s"] = period;
  res["now_s"] = secs;
  res["oldest"] = oldest;
  res["current"] = cur;
  res["persist"] = histLogOn();
  JsonArray fields = res["fields"].to<JsonArray>();
  for (const char *f : FIELDS) fields.add(f);
  uint32_t first = from < 0 || (uint32_t)from < oldest ? oldest : from;
  if (n < 1 || n > HIST_PAGE) n = HIST_PAGE;
  JsonArray rows = res["rows"].to<JsonArray>();
  for (uint32_t i = first; i <= cur && i < first + n; i++) {
    const Bucket &b = ring[i % (HIST_DEPTH + 1)];
    JsonArray r = rows.add<JsonArray>();
    r.add(i);
    r.add(b.tx);
    r.add(b.rx);
    r.add(b.retries);
    r.add(b.giveups);
    r.add(b.crcErr);
    r.add(b.macFail);
    r.add(b.lbtDefers);
    r.add(b.lbtForced);
    r.add(b.sessions);
    r.add(b.faults);
    r.add(b.downS);
    addLevel(r, b.rssiMin, b.rssiN, 1);
    addAvg(r, b.rssiSum, b.rssiN, 1);
    addLevel(r, b.snrMinQ, b.rssiN, 4);
    addAvg(r, b.snrSumQ, b.rssiN, 4);
    addAvg(r, b.noiseSum, b.noiseN, 1);
    addLevel(r, b.noiseMax, b.noiseN, 1);
    r.add(b.peerN);
    addLevel(r, b.peerRssiMin, b.peerN, 1);
    addAvg(r, b.peerRssiSum, b.peerN, 1);
    addLevel(r, b.peerSnrMin, b.peerN, 1);
    addAvg(r, b.peerSnrSum, b.peerN, 1);
    addAvg(r, b.peerNoiseSum, b.peerNoiseN, 1);
    addLevel(r, b.peerNoiseMax, b.peerNoiseN, 1);
    r.add(b.peerRetries);
    r.add(b.peerGiveups);
    r.add(b.peerCrc);
    r.add(b.boot);
  }
}
