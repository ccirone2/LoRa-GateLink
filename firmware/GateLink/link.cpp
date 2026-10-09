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
#define TXQ_LEN 4               // unreliable frames (ACK, HELLO, PING...) waiting for a clear channel
#define RESPONSE_SLACK_MS 40    // how late a response may start (the peer's loop can stall on USB writes)
#define RX_AGE_CAP_MS 0x40000000UL  // ~12.4 days: lastRxAt is kept no older (see linkPoll)
// A HELLO is answered at most this often from the verified session, and from all other sessions together. The
// peer's own retries are further apart (HELLO_INTERVAL_MS plus a few frames), so only replays are dropped.
#define HELLO_ANSWER_GAP_MS 1000

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
  uint32_t busySince;  // listen-before-talk: channel busy since (0 = not waiting)
  uint32_t backoff;    // this frame's gap after the last air activity (0 = not drawn yet)
};

struct QueuedFrame {
  uint8_t frame[MAX_FRAME];
  uint8_t len;
};

struct AckMemo {
  bool valid;
  bool pending;  // accepted, answer still to come (linkAckLater): retransmits are held, not answered
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
static uint32_t challenge;
static uint32_t challengeAt;
static uint32_t lastHelloAt;
static uint32_t helloGap;  // this round's HELLO interval, randomized
static uint8_t helloRound;  // HELLOs since the last verified session: the interval doubles, up to ~8 s
static bool helloSent;
static uint32_t helloDueAt;  // deferred HELLO after answering the peer's HELLO; kept until it is sent
static uint32_t helloKnownAt, helloOtherAt;  // when we last answered a HELLO from the verified session / any other
// The pending CMD is held: a HELLO came from a session we haven't verified, which is the peer restarting (the
// command may have run there already) or a recorded HELLO being replayed. Dropped if that session verifies,
// sent once the verified session proves alive.
static bool cmdHeld;
static bool sessionWeak;  // mySession was drawn while the radio was down (no RSSI noise): drawn again first
static uint32_t muteUntil;  // debug: received frames are ignored until then (0 = not muted)

static PendingSlot slots[SLOT_COUNT];
static AckMemo acks[ACK_MEMO];
static uint8_t ackNext;

static QueuedFrame txq[TXQ_LEN];
static uint8_t txqHead, txqCount;
static uint32_t txqBusySince, txqBackoff;

static uint8_t lastFrame[MAX_FRAME];
static uint8_t lastFrameLen;
static uint8_t bootHello[HDR_LEN + 4 + TAG_LEN];  // debug: our first HELLO since boot (an old session's after a link restart)
static uint8_t bootHelloLen;
static uint32_t lastAirAt;  // end of our last TX or RX
static uint32_t rxDoneSeen;  // radioRxDoneCount() when last looked at
static bool txOnAir;  // our frame is being transmitted

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

// TX is asynchronous: one frame at a time, and the air time counts from its end.
static bool txIdle() {
  if (txOnAir && !radioTxBusy()) {
    txOnAir = false;
    uint32_t end = radioTxEndAt();  // a frame received since then may already have moved lastAirAt on
    if ((int32_t)(end - lastAirAt) > 0) lastAirAt = end;
  }
  return !txOnAir;
}

static bool transmit(const uint8_t *frame, uint8_t len) {
  if (!cfg.key_set) return false;  // never run the link on the default key
  if (sessionWeak) return false;   // radio just came up: linkPoll draws the session again first
  if (!radioSend(frame, len)) return false;
  txOnAir = true;
  memcpy(lastFrame, frame, len);
  lastFrameLen = len;
  stats.tx++;
  return true;
}

static bool isResponse(uint8_t type) {
  return type == MSG_ACK || type == MSG_HELLO_ACK || type == MSG_PONG || type == MSG_DIAG;
}

// Listen-before-talk, with priorities as in Wi-Fi's SIFS/DIFS. The end of a frame lines both nodes up: the
// receiver answers and the sender may have something new due, and frames that start together can't hear
// each other (a preamble takes a few symbols to detect). On the bench every gate heartbeat collided like
// that with a ping. So a response (ACK, PONG...) goes after the short turnaround, while a new frame first
// waits out the response slot plus a random backoff drawn once per frame, by which time a response or the
// other side's new frame is detectable. On a quiet channel neither waits.
// Then hold a frame while the peer's frame is on the air (or one waits unread). A channel that never
// clears (noise read as a signal) must not mute the board: after twice the longest frame it is sent anyway.
static bool clearToSend(uint32_t &busySince, uint32_t &backoff, uint8_t type) {
  if (!txIdle()) return false;
  uint32_t now = millis();
  if ((int32_t)(now - lastAirAt) < TURNAROUND_MS) return false;  // let the peer get back into RX
  if (!isResponse(type)) {
    if (!backoff) {
      uint32_t symUs = (1000000UL << cfg.sf) / (uint32_t)cfg.bw_hz;
      backoff = TURNAROUND_MS + RESPONSE_SLACK_MS + 8 * symUs / 1000 + random(0, 32 * symUs / 1000 + 1);
    }
    if ((int32_t)(now - lastAirAt) < (int32_t)backoff) return false;
  }
  if (!radioChannelBusy()) {
    busySince = 0;
    backoff = 0;
    return true;
  }
  if (!busySince) {
    busySince = now | 1;
    stats.lbtDefers++;
    return false;
  }
  uint32_t waited = now - busySince;
  if ((int32_t)waited < (int32_t)(2 * radioAirtimeMs(MAX_FRAME))) return false;
  busySince = 0;
  backoff = 0;
  stats.lbtForced++;
  logEvent(EV_LBT_FORCED, type, waited);
  return true;
}

static void drainQueue() {
  if (txqCount && clearToSend(txqBusySince, txqBackoff, txq[txqHead].frame[1])) {
    transmit(txq[txqHead].frame, txq[txqHead].len);  // unreliable: dropped if the radio is down
    txqHead = (txqHead + 1) % TXQ_LEN;
    txqCount--;
  }
}

void linkSend(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (!cfg.key_set) return;
  uint8_t frame[MAX_FRAME];
  uint8_t n = buildFrame(type, payload, len, frame, nullptr);
  if (type == MSG_HELLO && !bootHelloLen && n == sizeof(bootHello)) {
    memcpy(bootHello, frame, n);
    bootHelloLen = n;
  }
  if (!txqCount && clearToSend(txqBusySince, txqBackoff, type)) {
    transmit(frame, n);  // the usual case: nothing queued, channel clear
    return;
  }
  // Full: drop the tail. Responses go in at the head and new frames at the tail, so that's the newest new frame
  // (a HELLO or PING, sent again later anyway), or with only responses waiting the oldest response. Never the
  // newest response, an ACK the peer is waiting for.
  if (txqCount == TXQ_LEN) txqCount--;
  uint8_t slot;
  if (isResponse(type)) {  // ahead of any new frame waiting out its backoff
    txqHead = (txqHead + TXQ_LEN - 1) % TXQ_LEN;
    slot = txqHead;
    txqBusySince = txqBackoff = 0;
  } else {
    slot = (txqHead + txqCount) % TXQ_LEN;
  }
  txqCount++;
  QueuedFrame &q = txq[slot];
  memcpy(q.frame, frame, n);
  q.len = n;
}

// False if it's too soon after the last one (unless forced).
static bool sendHello(uint32_t now, bool force) {
  if (!force && helloSent && now - lastHelloAt < helloGap) return false;
  if (challenge == 0 || now - challengeAt > CHALLENGE_LIFE_MS) {
    do { challenge = radioRandom32(); } while (challenge == 0);
    challengeAt = now;
  }
  uint8_t p[4];
  putU32(p, challenge);
  linkSend(MSG_HELLO, p, 4);
  lastHelloAt = now;
  // Random, and at least a few frames long, so two boards retrying at once can't stay in step. Doubling, so
  // a board whose peer is off for hours doesn't keep the channel busy.
  uint32_t base = HELLO_INTERVAL_MS << (helloRound < 3 ? helloRound : 3);
  helloGap = base + 4 * radioAirtimeMs(HDR_LEN + 4 + TAG_LEN) + random(0, base);
  if (helloRound < 3) helloRound++;
  helloSent = true;
  return true;
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

// The peer is verifying our session from the HELLO_ACK just built, and will accept only later seqs from us:
// renumber everything still waiting (the HELLO_ACK, a response, is ahead of it all, see linkSend).
// A renumbered slot no longer matches the peer's ACK memo (keyed by seq), so if the peer had already accepted it
// (and its ACK was lost), the resend is accepted as new. That's safe only because every reliable message is
// idempotent: STATUS and CFG_SET are, and the gate runs a CMD only once per cmd_id (lastCmdId). A new reliable
// type must be too.
static void reframePending(uint32_t now) {
  for (auto &s : slots) {
    if (!s.active) continue;
    s.frameLen = buildFrame(s.type, s.payload, s.len, s.frame, &s.seq);
    s.nextAt = now + random(120, 300);
  }
  for (uint8_t i = 0; i < txqCount; i++) {
    QueuedFrame &q = txq[(txqHead + i) % TXQ_LEN];
    if (q.frame[1] == MSG_HELLO_ACK) continue;
    uint8_t len = q.len - HDR_LEN - TAG_LEN;
    uint8_t payload[MAX_PAYLOAD];
    memcpy(payload, q.frame + HDR_LEN, len);
    q.len = buildFrame(q.frame[1], payload, len, q.frame, nullptr);
  }
}

// The session id must never repeat under the same key: with txSeq starting over, frames recorded in the earlier
// boot would pass both the MAC and the replay window. radioRandom32() is seeded per boot (boot counter, chip
// serial) and samples RSSI noise only while the radio is up. Starting seq at a random offset as well costs nothing.
static void drawSession() {
  sessionWeak = !radioOk();
  do { mySession = radioRandom32(); } while (mySession == 0);
  txSeq = radioRandom32() & 0x7FFFFFFF;  // leaves 2^31 frames before the (unsigned) window would wrap
}

static void releaseCmd() {
  if (!cmdHeld) return;
  cmdHeld = false;
  logEvent(EV_CMD_HOLD, 0, (int32_t)peerSession);
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
  txqCount = 0;
  txqBusySince = txqBackoff = 0;
  txOnAir = false;  // radioBegin() abandoned any TX
  lastAirAt = millis();
  rxDoneSeen = radioRxDoneCount();
  randomSeed(radioRandom32());
  drawSession();
  peerOk = false;
  peerSession = 0;
  challenge = 0;
  helloSent = false;
  helloRound = 0;
  helloKnownAt = helloOtherAt = 0;
  cmdHeld = false;
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
  s.busySince = s.backoff = 0;
  s.ttl = ttlMs;
  s.expiresAt = now + ttlMs;
  // A new command isn't a repeat of one the peer may have run before restarting.
  if (slot == SLOT_CMD) cmdHeld = false;
}

bool linkPending(Slot slot) {
  return slots[slot].active;
}

static void sendAck(uint32_t seq, uint8_t result) {
  uint8_t p[5];
  putU32(p, seq);
  p[4] = result;
  linkSend(MSG_ACK, p, 5);
}

static AckMemo *ackMemo(uint32_t seq) {
  for (auto &m : acks)
    if (m.valid && m.seq == seq) return &m;
  AckMemo *m = &acks[ackNext];
  ackNext = (ackNext + 1) % ACK_MEMO;
  return m;
}

void linkAck(uint32_t seq, uint8_t result) {
  *ackMemo(seq) = { true, false, seq, result };
  sendAck(seq, result);
}

void linkAckLater(uint32_t seq) {
  *ackMemo(seq) = { true, true, seq, 0 };
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

// Re-ACK a retransmission we already accepted (our ACK was lost), or hold it quietly while its answer is still to
// come (linkAckLater). False if it isn't in the memo.
static bool resendAck(uint32_t seq) {
  for (auto &m : acks) {
    if (m.valid && m.seq == seq) {
      if (!m.pending) sendAck(seq, m.result);
      return true;
    }
  }
  return false;
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
    // HELLOs aren't covered by the replay window (a restarted peer's must get through), so a recorded one is acted
    // on too. Answering costs airtime and renumbers our pending frames: do it at most once a second per kind.
    bool known = peerSession != 0 && session == peerSession;
    uint32_t &answeredAt = known ? helloKnownAt : helloOtherAt;
    if (answeredAt && !elapsed(now, answeredAt, HELLO_ANSWER_GAP_MS)) return;
    answeredAt = now | 1;
    // A new session after a verified one: the peer restarted, and with it went its ACK memo and its record of the
    // last command, so a command still waiting for an ACK may already have run there (the ACK lost to the reset).
    // Sending it again would run it twice, late. But it may also be a replayed HELLO, sent to make us drop the
    // command: hold it until we know which (HELLO_ACK below, or a fresh frame from the verified session).
    // Status and config writes are safe to repeat and are renumbered below.
    if (peerSession != 0 && !known && slots[SLOT_CMD].active && !cmdHeld) {
      cmdHeld = true;
      logEvent(EV_CMD_HOLD, 1, (int32_t)session);
    }
    linkSend(MSG_HELLO_ACK, payload, 4);
    reframePending(now);
    // A session we haven't verified: the peer restarted, or an old HELLO is being replayed. Challenge it
    // (once the peer is back in RX), but keep the verified session until the new one answers, so a replayed
    // HELLO can't take the link down.
    if (!peerOk || !known) helloDueAt = (now + 60) | 1;
    return;
  }
  if (type == MSG_HELLO_ACK) {
    // The challenge stays current until answered, which while verified can be long after the last HELLO carrying
    // it: an answer recorded back then (and kept from us) mustn't count later. Sending a HELLO already replaces a
    // challenge over CHALLENGE_LIFE_MS old, and holding a command sends one at once; this covers the rest.
    if (plen < 4 || challenge == 0 || getU32(payload) != challenge) return;
    if (elapsed(now, lastHelloAt, CHALLENGE_LIFE_MS)) return;
    challenge = 0;
    stats.lastRxAt = now;
    stats.lastRssi = rssi;
    stats.lastSnr = snr;
    if (peerSession != 0 && session == peerSession) {
      // Already verified (we challenged it after a stale HELLO): keep its replay window and ACK memo. The peer
      // didn't restart, so a held command can go.
      acceptSeq(seq);
      peerOk = true;
      releaseCmd();
      return;
    }
    // The peer restarted: drop a held command (see MSG_HELLO). The house then resyncs its controller to the gate.
    if (cmdHeld && slots[SLOT_CMD].active) {
      slots[SLOT_CMD].active = false;
      logEvent(EV_CMD_HOLD, 2, (int32_t)session);
      if (ackHandler) ackHandler(SLOT_CMD, slots[SLOT_CMD].type, false, 0);
    }
    cmdHeld = false;
    peerOk = true;
    peerSession = session;
    // Nothing at or before the HELLO_ACK is accepted, so frames recorded earlier (e.g. before our reboot)
    // can't be replayed; the peer renumbered what it still had waiting.
    peerLastSeq = seq;
    peerWindow = 0xFFFFFFFF;
    memset(acks, 0, sizeof(acks));
    stats.sessions++;
    helloRound = 0;
    logEvent(EV_SESSION, (int32_t)session);
    return;
  }

  if (!peerOk || session != peerSession) {
    sendHello(now, false);
    return;
  }
  if (!acceptSeq(seq)) {
    // Retransmission of a message we already acked (our ACK was lost)? Re-ACK, don't re-process.
    if (!resendAck(seq)) {
      stats.replay++;
      logEvent(EV_REPLAY, (int32_t)seq, (int32_t)peerLastSeq);
    }
    return;
  }

  releaseCmd();  // a fresh frame from the verified session: it didn't restart
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
  txIdle();
  size_t n = radioReceive(buf, sizeof(buf), rssi, snr);
  // Any frame heard is air activity, also one the radio drops (bad CRC, oversize) or one we ignore: it restarts the
  // turnaround and the backoff (clearToSend), as an answer to it, or the frame it answered, may follow.
  uint32_t done = radioRxDoneCount();
  if (done != rxDoneSeen) {
    rxDoneSeen = done;
    lastAirAt = millis();
  }
  if (sessionWeak && radioOk()) {
    // Nothing has been sent with the weak session (transmit() refuses): draw it again and renumber what waits.
    drawSession();
    reframePending(now);
  }
  if (muteUntil && elapsed(now, muteUntil, 0)) muteUntil = 0;
  if (n && !muteUntil) handleFrame(buf, n, rssi, snr, now);
  // Ages are signed (elapsed()): after 2^31 ms of silence lastRxAt would read as fresh again and the link as up
  // (house K2 no longer failing open). Keep it at most RX_AGE_CAP_MS old; status age_ms tops out there.
  if (stats.lastRxAt && elapsed(now, stats.lastRxAt, RX_AGE_CAP_MS)) {
    stats.lastRxAt = now - RX_AGE_CAP_MS;
    if (!stats.lastRxAt) stats.lastRxAt = 1;  // 0 means never
  }
  // While verified, the usual HELLO interval applies: that limits what replayed HELLOs can provoke. A HELLO it
  // holds back is sent once the interval is up, not forgotten.
  if (helloDueAt && (int32_t)(now - helloDueAt) >= 0 && sendHello(now, !peerOk)) helloDueAt = 0;
  // A held command waits for the verified session to answer: keep asking (at the HELLO interval).
  if (cmdHeld && slots[SLOT_CMD].active && peerOk && cfg.key_set) sendHello(now, false);
  // Keep challenging until the peer answers. Nothing else may be on the air to provoke a HELLO (slots
  // hold until the peer is verified), so if both first HELLOs were lost the link would never come back.
  if (!peerOk && helloSent && cfg.key_set) sendHello(now, false);
  drainQueue();

  for (uint8_t i = 0; i < SLOT_COUNT; i++) {
    PendingSlot &s = slots[i];
    if (!s.active || (int32_t)(now - s.nextAt) < 0) continue;
    if ((int32_t)(now - s.expiresAt) >= 0) {
      s.active = false;
      if (i == SLOT_CMD) cmdHeld = false;  // nothing left to hold
      stats.giveups++;
      logEvent(EV_TX_GIVEUP, s.type, (int32_t)s.seq);
      if (ackHandler) ackHandler((Slot)i, s.type, false, 0);
      continue;
    }
    if (s.attempts > cfg.retries) {
      s.nextAt = s.expiresAt;  // out of resends: wait for a late ACK until the TTL ends
      continue;
    }
    if (i == SLOT_CMD && cmdHeld) continue;  // the TTL keeps running
    // Its ACK would be dropped until the peer is verified, so sending now only takes airtime from the
    // handshake (at SF12 the retries crowded out the HELLO_ACK for good). Hold it; the TTL keeps running.
    if (!peerOk) continue;
    if (!clearToSend(s.busySince, s.backoff, s.type)) {
      s.nextAt = now + random(10, 60);  // poll again soon; deferring doesn't use up an attempt
      continue;
    }
    if (!transmit(s.frame, s.frameLen)) {  // radio down: doesn't use up an attempt
      s.nextAt = now + 100;
      continue;
    }
    if (s.attempts > 0) stats.retries++;
    s.attempts++;
    s.nextAt = millis() + radioAirtimeMs(s.frameLen) + retryDelay(s);  // gaps count from the frame's end
    if ((int32_t)(s.nextAt - s.expiresAt) > 0) s.nextAt = s.expiresAt;
  }
}

const LinkStats &linkStats() {
  return stats;
}

bool linkPeerVerified() {
  return peerOk;
}

bool linkDebugReplay(bool hello) {
  const uint8_t *f = hello ? bootHello : lastFrame;
  uint8_t len = hello ? bootHelloLen : lastFrameLen;
  if (!len || !txIdle() || !radioSend(f, len)) return false;
  txOnAir = true;
  return true;
}

void linkDebugMute(uint32_t ms) {
  muteUntil = ms ? (millis() + ms) | 1 : 0;
}
