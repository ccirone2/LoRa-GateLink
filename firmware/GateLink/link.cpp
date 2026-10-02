#include "link.h"
#include "config.h"
#include "radio.h"
#include "log.h"
#include <SHA256.h>

#define PROTO_VER 1
#define HDR_LEN 13
#define TAG_LEN 8
#define MAX_PAYLOAD 100
#define MAX_FRAME (HDR_LEN + MAX_PAYLOAD + TAG_LEN)
#define HELLO_INTERVAL_MS 1000
#define CHALLENGE_LIFE_MS 10000
#define ACK_MEMO 4
#define TURNAROUND_MS 25        // let the peer get back into RX between frames
#define HELLO_RESET_GUARD_MS 2000  // limit how often a (possibly replayed) HELLO can reset verification

struct PendingSlot {
  bool active;
  uint8_t type;
  uint8_t payload[MAX_PAYLOAD];
  uint8_t len;
  uint8_t frame[MAX_FRAME];
  uint8_t frameLen;
  uint32_t seq;
  uint8_t attempts;
  uint32_t nextAt;
  uint32_t ttl;
  uint32_t expiresAt;
};

struct AckMemo {
  bool valid;
  uint32_t seq;
  uint8_t result;
};

static RxHandler rxHandler;
static AckHandler ackHandler;
static LinkStats stats;

static uint32_t mySession;
static uint32_t txSeq;
static bool peerOk;
static uint32_t peerSession;
static uint32_t peerLastSeq;
static uint32_t peerWindow;  // bit i set = peerLastSeq - i already accepted
static uint32_t sessionAt;
static uint32_t challenge;
static uint32_t challengeAt;
static uint32_t lastHelloAt;
static bool helloSent;
static uint32_t helloDueAt;  // deferred HELLO after answering the peer's HELLO

static PendingSlot slots[SLOT_COUNT];
static AckMemo acks[ACK_MEMO];
static uint8_t ackNext;

static uint8_t lastFrame[MAX_FRAME];
static uint8_t lastFrameLen;
static uint32_t lastAirAt;  // end of our last TX or RX

static void computeTag(const uint8_t *buf, size_t len, uint8_t *tag) {
  SHA256 h;
  h.resetHMAC(cfg.key, sizeof(cfg.key));
  h.update(buf, len);
  h.finalizeHMAC(cfg.key, sizeof(cfg.key), tag, TAG_LEN);
}

static bool tagEqual(const uint8_t *a, const uint8_t *b) {
  uint8_t d = 0;
  for (int i = 0; i < TAG_LEN; i++) d |= a[i] ^ b[i];
  return d == 0;
}

static uint8_t buildFrame(uint8_t type, const uint8_t *payload, uint8_t len, uint8_t *out, uint32_t *seqOut) {
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  uint32_t seq = ++txSeq;
  out[0] = PROTO_VER;
  out[1] = type;
  out[2] = (uint8_t)cfg.net_id;
  out[3] = myNodeId();
  out[4] = peerNodeId();
  putU32(out + 5, mySession);
  putU32(out + 9, seq);
  if (len) memcpy(out + HDR_LEN, payload, len);
  computeTag(out, HDR_LEN + len, out + HDR_LEN + len);
  if (seqOut) *seqOut = seq;
  return HDR_LEN + len + TAG_LEN;
}

static void transmit(const uint8_t *frame, uint8_t len) {
  if (!cfg.key_set) return;  // never run the link on the default key
  while (millis() - lastAirAt < TURNAROUND_MS) {}
  radioSend(frame, len);
  lastAirAt = millis();
  memcpy(lastFrame, frame, len);
  lastFrameLen = len;
  stats.tx++;
}

void linkSend(uint8_t type, const uint8_t *payload, uint8_t len) {
  uint8_t frame[MAX_FRAME];
  uint8_t n = buildFrame(type, payload, len, frame, nullptr);
  transmit(frame, n);
}

static void sendHello(uint32_t now, bool force) {
  if (!force && helloSent && now - lastHelloAt < HELLO_INTERVAL_MS) return;
  if (challenge == 0 || now - challengeAt > CHALLENGE_LIFE_MS) {
    do { challenge = radioRandom32(); } while (challenge == 0);
    challengeAt = now;
  }
  uint8_t p[4];
  putU32(p, challenge);
  linkSend(MSG_HELLO, p, 4);
  lastHelloAt = now;
  helloSent = true;
}

// The TTL governs, not the retry count: the gaps double and the `retries` resends spread over the whole
// TTL (cmd_ttl_s 10, retries 5: about 0.3, 0.9, 2.2, 4.7 and 9.7 s), so a lost frame is retried quickly
// and an outage of nearly the TTL is still covered. The slot gives up when the TTL runs out.
static uint32_t retryDelay(const PendingSlot &s) {
  uint32_t minGap = radioAirtimeMs(s.frameLen) + radioAirtimeMs(HDR_LEN + 5 + TAG_LEN) + 100;
  uint32_t n = s.attempts - 1;  // gaps so far; the gap after the last resend is the whole TTL (clamped)
  uint32_t gap = n >= (uint32_t)cfg.retries ? s.ttl : s.ttl >> (cfg.retries - n);
  gap -= random(0, gap / 8 + 1);  // jitter, so two boards retrying at once drift apart; never past the TTL
  return gap < minGap ? minGap : gap;
}

static void reframeSlots(uint32_t now) {
  // Peer lost our session (rebooted); resend pending messages with fresh seq numbers.
  for (auto &s : slots) {
    if (!s.active) continue;
    s.frameLen = buildFrame(s.type, s.payload, s.len, s.frame, &s.seq);
    s.nextAt = now + random(120, 300);
  }
}

void linkBegin(RxHandler rx, AckHandler ack) {
  // Report anything dropped by a restart (key or radio change).
  for (uint8_t i = 0; i < SLOT_COUNT; i++)
    if (slots[i].active && ackHandler) ackHandler((Slot)i, slots[i].type, false, 0);
  rxHandler = rx;
  ackHandler = ack;
  memset(&stats, 0, sizeof(stats));
  memset(slots, 0, sizeof(slots));
  memset(acks, 0, sizeof(acks));
  randomSeed(radioRandom32());
  do { mySession = radioRandom32(); } while (mySession == 0);
  txSeq = 0;
  peerOk = false;
  challenge = 0;
  helloSent = false;
  // Start the handshake ourselves rather than waiting for the peer's next frame.
  helloDueAt = (millis() + random(100, 600)) | 1;
}

void linkSendReliable(Slot slot, uint8_t type, const uint8_t *payload, uint8_t len, uint32_t ttlMs) {
  PendingSlot &s = slots[slot];
  if (len > MAX_PAYLOAD) len = MAX_PAYLOAD;
  s.active = true;
  s.type = type;
  memcpy(s.payload, payload, len);
  s.len = len;
  s.frameLen = buildFrame(type, payload, len, s.frame, &s.seq);
  s.attempts = 0;
  uint32_t now = millis();
  s.nextAt = now;
  s.ttl = ttlMs;
  s.expiresAt = now + ttlMs;
}

void linkCancel(Slot slot) {
  slots[slot].active = false;
}

bool linkPending(Slot slot) {
  return slots[slot].active;
}

void linkAck(uint32_t seq, uint8_t result) {
  acks[ackNext] = { true, seq, result };
  ackNext = (ackNext + 1) % ACK_MEMO;
  uint8_t p[5];
  putU32(p, seq);
  p[4] = result;
  linkSend(MSG_ACK, p, 5);
}

static void handleAck(const uint8_t *p, uint8_t len) {
  if (len < 5) return;
  uint32_t seq = getU32(p);
  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    PendingSlot &s = slots[i];
    if (s.active && s.seq == seq) {
      s.active = false;
      if (ackHandler) ackHandler((Slot)i, s.type, true, p[4]);
    }
  }
}

static void resendAck(uint32_t seq) {
  for (auto &m : acks) {
    if (m.valid && m.seq == seq) {
      uint8_t p[5];
      putU32(p, seq);
      p[4] = m.result;
      linkSend(MSG_ACK, p, 5);
      return;
    }
  }
}

// Sliding-window replay check: accepts each seq at most once, tolerating reordering
// (e.g. a retried frame arriving after a later frame from another slot).
static bool acceptSeq(uint32_t seq) {
  if (seq > peerLastSeq) {
    uint32_t shift = seq - peerLastSeq;
    peerWindow = shift >= 32 ? 0 : peerWindow << shift;
    peerWindow |= 1;
    peerLastSeq = seq;
    return true;
  }
  uint32_t d = peerLastSeq - seq;
  if (d >= 32 || (peerWindow & (1UL << d))) return false;
  peerWindow |= 1UL << d;
  return true;
}

static void handleFrame(uint8_t *buf, size_t len, int16_t rssi, float snr, uint32_t now) {
  lastAirAt = millis();
  if (!cfg.key_set) return;
  if (len < HDR_LEN + TAG_LEN || len > MAX_FRAME) return;
  if (buf[0] != PROTO_VER || buf[2] != (uint8_t)cfg.net_id) return;
  if (buf[3] != peerNodeId() || buf[4] != myNodeId()) return;

  uint8_t tag[TAG_LEN];
  size_t body = len - TAG_LEN;
  computeTag(buf, body, tag);
  if (!tagEqual(tag, buf + body)) {
    stats.macFail++;
    logEvent(EV_MAC_FAIL, buf[1], rssi);
    return;
  }

  uint8_t type = buf[1];
  uint32_t session = getU32(buf + 5);
  uint32_t seq = getU32(buf + 9);
  const uint8_t *payload = buf + HDR_LEN;
  uint8_t plen = body - HDR_LEN;

  if (type == MSG_HELLO) {
    if (plen < 4) return;
    linkSend(MSG_HELLO_ACK, payload, 4);
    // Give the peer time to get back into RX before we challenge it.
    if (!peerOk || session != peerSession) {
      // Peer restarted (or a stale HELLO): re-verify before accepting anything.
      if (!peerOk || now - sessionAt >= HELLO_RESET_GUARD_MS) {
        peerOk = false;
        helloDueAt = (now + 60) | 1;
      }
    }
    reframeSlots(now);
    return;
  }
  if (type == MSG_HELLO_ACK) {
    if (plen >= 4 && challenge != 0 && getU32(payload) == challenge) {
      peerOk = true;
      peerSession = session;
      peerLastSeq = seq;
      peerWindow = 1;
      sessionAt = now;
      memset(acks, 0, sizeof(acks));
      challenge = 0;
      stats.sessions++;
      stats.lastRxAt = now;
      stats.lastRssi = rssi;
      stats.lastSnr = snr;
      logEvent(EV_SESSION, (int32_t)session);
    }
    return;
  }

  if (!peerOk || session != peerSession) {
    sendHello(now, false);
    return;
  }
  if (!acceptSeq(seq)) {
    // Retransmission of a message we already acked (our ACK was lost)? Re-ACK, don't re-process.
    bool memo = false;
    for (auto &m : acks) memo |= m.valid && m.seq == seq;
    if (memo) {
      resendAck(seq);
    } else {
      stats.replay++;
      logEvent(EV_REPLAY, (int32_t)seq, (int32_t)peerLastSeq);
    }
    return;
  }

  stats.rx++;
  stats.lastRxAt = now;
  stats.lastRssi = rssi;
  stats.lastSnr = snr;

  if (type == MSG_ACK) {
    handleAck(payload, plen);
    return;
  }
  if (rxHandler) {
    RxMsg m = { type, seq, payload, plen, rssi, snr };
    rxHandler(m);
  }
}

void linkPoll(uint32_t now) {
  uint8_t buf[256];
  int16_t rssi;
  float snr;
  size_t n = radioReceive(buf, sizeof(buf), rssi, snr);
  if (n) handleFrame(buf, n, rssi, snr, now);
  if (helloDueAt && (int32_t)(now - helloDueAt) >= 0) {
    helloDueAt = 0;
    if (!peerOk) sendHello(now, true);
  }

  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    PendingSlot &s = slots[i];
    if (!s.active || (int32_t)(now - s.nextAt) < 0) continue;
    if ((int32_t)(now - s.expiresAt) >= 0) {
      s.active = false;
      stats.giveups++;
      logEvent(EV_TX_GIVEUP, s.type, (int32_t)s.seq);
      if (ackHandler) ackHandler((Slot)i, s.type, false, 0);
      continue;
    }
    if (s.attempts > cfg.retries) {
      s.nextAt = s.expiresAt;  // out of resends: wait for a late ACK until the TTL ends
      continue;
    }
    // Its ACK would be dropped until the peer is verified, so sending now only takes airtime from the
    // handshake (at SF12 the retries crowded out the HELLO_ACK for good). Hold it; the TTL keeps running.
    if (!peerOk) continue;
    if (s.attempts > 0) stats.retries++;
    transmit(s.frame, s.frameLen);
    s.attempts++;
    s.nextAt = millis() + retryDelay(s);
    if ((int32_t)(s.nextAt - s.expiresAt) > 0) s.nextAt = s.expiresAt;
  }
}

const LinkStats &linkStats() {
  return stats;
}

bool linkPeerVerified() {
  return peerOk;
}

void linkDebugReplay() {
  if (lastFrameLen) radioSend(lastFrame, lastFrameLen);
}
