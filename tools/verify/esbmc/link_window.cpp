// ESBMC harness: the link's replay window (link.cpp handleFrame, acceptSeq, linkRefuse), proved for runs of any length.
//
// An inductive proof over one symbolic sequence number S of the verified peer session:
//
//   once        a frame with seq S reaches the role (the RxHandler) at most once per verified peer session, unless the
//               role refused it (linkRefuse) after it last did
//   after-hello no frame at or before the HELLO_ACK that verified the session reaches the role
//
// handleFrame runs on any frame bytes, of any length; the MAC check and the challenge comparison are left to chance
// (the stand-in SHA256 computes nothing: a tag or a challenge matches or not, nondeterministically), so these hold
// even against frames the attacker could forge: the window alone keeps them. The protocol model (tools/verify/tla)
// takes them as given for authentic frames.
#include "env.h"
#include "link.cpp"
#include "cfgmodel.h"

#define FRAME_MAX (HDR_LEN + CHALLENGE_LEN + TAG_LEN)
static uint32_t S;          // the sequence number the proof follows
static uint32_t epochSess;  // the verified peer session the ghosts below belong to
static uint32_t epochBase;  // the seq of the HELLO_ACK that verified it
static bool accS;           // S reached the role in this session and wasn't refused since
static bool justS;          // the frame being handled is S (the RxHandler may refuse it)

// The transmitter stays busy: every frame the link sends waits in its queue. Nothing in the window reads the sending
// side, and with a frame sent or queued by chance the queue's frames had lengths the checker couldn't bound, so
// reframePending's cipher loops unrolled to the full --unwind bound (it ran out of memory).
bool radioTxBusy() { return true; }
int digitalRead(uint32_t) { return LOW; }
void digitalWrite(uint32_t, uint32_t) {}
void pinMode(uint32_t, uint32_t) {}
long random(long a, long b) {
  long v = (long)nondet_u32();
  ASSUME(v >= a && v < b);
  return v;
}
long random(long b) { return random(0, b); }
void randomSeed(unsigned long) {}

static void rx(const RxMsg &m) {
  if (m.seq == S) {
    CHECK(!accS, "once: a frame reached the role twice in one session");
    CHECK(S > epochBase, "after-hello: a frame from before the handshake reached the role");
    accS = true;
  }
  if (nondet_bool()) {  // the role refuses it (role_gate.cpp handleCfgSet, a full queue)
    linkRefuse(m.seq);
    if (m.seq == S) accS = false;
  }
}
static void ack(Slot, uint8_t, bool, uint8_t) {}

static bool checking;
#define INV(c, msg) do { if (checking) CHECK(c, "invariant: " msg); else ASSUME(c); } while (0)
// Seen: S won't be accepted again (acceptSeq): at or below the newest, and out of the window or marked in it.
static bool windowHas(uint32_t s) {
  uint32_t d = peerLastSeq - s;
  return s <= peerLastSeq && (d >= 32 || (peerWindow & (1UL << d)));
}

static void inv() {
  if (!peerSession) return;  // nothing verified: nothing reaches the role (handleFrame drops it)
  INV(epochSess == peerSession, "the ghosts follow the verified session");
  INV(epochBase <= peerLastSeq, "the handshake is at or below the newest seq");
  INV(!accS || windowHas(S), "a seq that reached the role is in the window");
  INV(S > epochBase || windowHas(S), "a seq at or before the handshake is in the window");
}

// After the frame: the ghosts follow a new session, and the invariant must hold again.
static int after(uint32_t sess0) {
  // A HELLO_ACK that verified a new session (or verified one again from none): the ghosts start over with it.
  if (peerSession != sess0 || (peerSession && !sess0)) {
    epochSess = peerSession;
    epochBase = peerLastSeq;
    accS = false;
    CHECK(peerWindow == 0xFFFFFFFF, "after-hello: a new session's window starts full");
  }
  checking = true;
  inv();
#ifdef REACH
  CHECK(false, "reach: the end of the harness is reachable (this run must fail)");
#endif
  return 0;
}

int main() {
  cfg.key_set = 1;
  cfg.net_id = nondet_u8();
  activeRole = nondet_bool() ? ROLE_HOUSE : ROLE_GATE;
  txOnAir = true;
  rxHandler = rx;
  ackHandler = ack;
  S = nondet_u32();
  peerOk = nondet_bool();
  peerSession = nondet_u32();
  peerLastSeq = nondet_u32();
  peerWindow = nondet_u32();
  challengeSet = nondet_bool();
  epochSess = nondet_u32();
  epochBase = nondet_u32();
  accS = nondet_bool();
  checking = false;
  inv();
  // One frame, any bytes, up to a HELLO's length (the window doesn't look past the header; longer payloads only cost
  // the checker time in the cipher's loops).
  uint8_t buf[FRAME_MAX];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = nondet_u8();
  uint8_t n = nondet_u8();
  ASSUME(n <= FRAME_MAX);
  uint32_t sess0 = peerSession;
  // One call per length, each with a constant one: with the length symbolic the checker unrolls the cipher's loops
  // to the full --unwind bound (and ran out of memory); with it constant they unroll exactly. Each call ends the run
  // (after): carried on, the state each call leaves (the frames it queued) merged into the next call's, and the
  // checker unrolled the queue's re-framing (reframePending) to the --unwind bound per frame, out of memory again.
  for (uint8_t k = 0; k <= FRAME_MAX; k++)
    if (k == n) {
      handleFrame(buf, k, 0, 0, clk);
      return after(sess0);
    }
  return 0;  // unreachable: n <= FRAME_MAX
}
