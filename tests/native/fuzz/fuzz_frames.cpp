// fuzz_frames: one board (house or gate) with a verified link, and the harness as its peer sending it whatever the
// input says: authenticated frames of any type and content (STATUS, CMD, CFG_SET, DIAG, ACKs...), unauthenticated
// bytes, replays, peer restarts, input changes, console commands and time. Drives handleStatus, handleCmd,
// handleCfgSet, DIAG and the replay window with arbitrary content, under ASan/UBSan and the relay monitors (hal.cpp).
//
// Input: byte 0, then records until the input ends.
//   byte 0: bit 0 role (0 house, 1 gate); bit 1 cold boot (else warm: verified, settled, house armed);
//           bit 2 clock starts ~30 s before millis() wraps; bits 3..6 inputs IN1..IN4 at a cold boot
//   record: op (u8 % OP_COUNT), then its arguments:
//     AUTH     type, flags, [delta i8 | seq u32], [session u32], [rssi u8, snr u8], len (u8 % 109), payload
//              flags: bits 0-1 seq (0 next, 1 the last again, 2 next + delta, 3 seq given), 2 session given,
//              3 bad tag, 4 src/dst swapped, 5 rssi/snr given, 6 don't wait for the board (the next frame may
//              overwrite it in the FIFO)
//     RAW      flags, len, bytes (unauthenticated; flags bit 0: with a valid header, so it fails at the MAC)
//     TIME     ms u16
//     PINS     bits 0-3 IN1..IN4, bit 4 board supply lost (charger power good off)
//     CONSOLE  flags (bit 0 UART, bit 1 with crc, bits 2-7 template), template arguments (see consoleTemplate)
//     ACK      which (bits 0-6: frame type, bit 7: seq offset follows), [offset i8], result
//     RESTART  session u32 (0: ours + 1), seq u32, challenge u32: the peer restarts and sends a HELLO
//     ANSWER   flags (bit 0: the previous challenge, bit 1: first wait up to 3 s for a new HELLO): HELLO_ACK
//     REPLAY   which (bit 7: one of the board's own frames back to it, else one of ours), verbatim
//     CRCERR   a frame heard with a bad CRC
//     RADIO    flags (bit 0 module absent, bit 1 channel jammed, bit 2 noise u8 follows, bit 3 the radio resets now)
#include <stdio.h>
#include <string>
#include "harness.h"
#include "config.h"
#include "link.h"
#include "log.h"
#include "radio.h"
#include "roles.h"

enum Op : uint8_t {
  OP_AUTH,
  OP_RAW,
  OP_TIME,
  OP_PINS,
  OP_CONSOLE,
  OP_ACK,
  OP_RESTART,
  OP_ANSWER,
  OP_REPLAY,
  OP_CRCERR,
  OP_RADIO,
  OP_COUNT
};

static void remember(const Bytes &f) {
  peer.sent.push_back(f);
  if (peer.sent.size() > 32) peer.sent.erase(peer.sent.begin());
}

static bool opAuth(Reader &r) {
  uint8_t type = r.u8(), flags = r.u8();
  uint32_t seq;
  switch (flags & 3) {
    case 0: seq = peer.seq + 1; break;
    case 1: seq = peer.seq; break;
    case 2: seq = peer.seq + 1 + (int32_t)(int8_t)r.u8(); break;
    default: seq = r.u32(); break;
  }
  uint32_t session = (flags & 4) ? r.u32() : peer.session;
  int16_t rssi = -60;
  float snr = 9.0f;
  if (flags & 0x20) {
    rssi = (int16_t)(r.u8() - 157);  // SX1276 packet RSSI, HF port
    snr = (int8_t)r.u8() * 0.25f;
  }
  Bytes payload = r.bytes(r.u8() % 109);  // MAX_PAYLOAD 100: a little past it is dropped unread
  Bytes f = peerFrame(type, payload.data(), payload.size(), session, seq);
  if (flags & 8) f.back() ^= 0x5A;
  if (flags & 0x10) std::swap(f[3], f[4]);
  if (session == peer.session && (int32_t)(seq - peer.seq) > 0) peer.seq = seq;
  remember(f);
  deliver(f, rssi, snr);
  return !(flags & 0x40);
}

static bool opRaw(Reader &r) {
  uint8_t flags = r.u8();
  Bytes f = r.bytes(r.u8());
  if ((flags & 1) && f.size() >= 13) {
    f[0] = 1;
    f[2] = (uint8_t)cfg.net_id;
    f[3] = peerNodeId();
    f[4] = myNodeId();
    putU32(&f[5], peer.session);
    putU32(&f[9], peer.seq + 1);
  }
  deliver(f, -80, 2.0f);
  return !(flags & 0x40);
}

// Console requests the frames target sends; the console target sends raw bytes instead.
static std::string consoleTemplate(Reader &r, unsigned which, int id) {
  char b[256];
  const ParamDef *p = &PARAMS[r.u8() % PARAM_COUNT];
  switch (which) {
    case 0: {
      Bytes raw = r.bytes(r.u8());
      return std::string(raw.begin(), raw.end());
    }
    case 1: {
      unsigned k = r.u8() % 3;
      snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"relay.test\",\"k\":%u,\"ms\":%u}", id, k, r.u16());
      break;
    }
    case 2: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"remote.set\",\"name\":\"%s\",\"value\":%d}", id, p->name, (int)r.u32()); break;
    case 3: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"remote.diag\"}", id); break;
    case 4: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"radio.ping\"}", id); break;
    case 5: {
      int32_t v = (int32_t)r.u32();
      if (r.u8() & 1) v = p->minV + (int32_t)((uint32_t)v % (uint32_t)(p->maxV - p->minV + 1));  // in range
      snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"config.set\",\"params\":{\"%s\":%d}}", id, p->name, (int)v);
      break;
    }
    case 6: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"config.save\"}", id); break;
    case 7: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"debug.mute\",\"ms\":%u}", id, r.u16()); break;
    case 8: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"debug.replay\",\"hello\":%s}", id, r.u8() & 1 ? "true" : "false"); break;
    case 9: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"debug.reboot_after_cmd\"}", id); break;
    case 10: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"status\"}", id); break;
    case 11: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"config.get\"}", id); break;
    case 12: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"log.get\"}", id); break;
    case 13: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"hist.get\",\"from\":%d,\"n\":%d}", id, (int16_t)r.u16(), (int8_t)r.u8()); break;
    case 14: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"hist.clear\",\"period_s\":%u}", id, r.u16()); break;
    case 15: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"identify\",\"ms\":%u}", id, r.u16()); break;
    case 16: {
      uint8_t key[16];
      if (r.u8() & 1) memcpy(key, FUZZ_KEY, 16);
      else for (uint8_t &x : key) x = r.u8();
      int n = snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"key.set\",\"key\":\"", id);
      for (uint8_t x : key) n += snprintf(b + n, sizeof(b) - n, "%02x", x);
      snprintf(b + n, sizeof(b) - n, "\"}");
      break;
    }
    case 17: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"config.reset\"}", id); break;
    case 18: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"reboot\"}", id); break;
    default: snprintf(b, sizeof(b), "{\"id\":%d,\"cmd\":\"info\"}", id); break;
  }
  return b;
}
#define TEMPLATES 20

static bool opConsole(Reader &r, int &id) {
  uint8_t flags = r.u8();
  std::string line = consoleTemplate(r, (flags >> 2) % TEMPLATES, id++);
  if (flags & 2) line = withCrc(line);
  consoleLine(flags & 1, line);
  return true;
}

static bool opAck(Reader &r) {
  static const uint8_t types[] = { MSG_CMD, MSG_STATUS, MSG_CFG_SET, MSG_PING, MSG_HELLO, MSG_DIAG, MSG_ACK, MSG_PONG };
  uint8_t which = r.u8();
  uint8_t type = types[(which & 0x7F) % sizeof(types)];
  uint32_t seq = peer.lastSeq[type];
  if (which & 0x80) seq += (int32_t)(int8_t)r.u8();
  uint8_t p[5];
  putU32(p, seq);
  p[4] = r.u8();
  sendAuth(MSG_ACK, p, 5);
  return true;
}

static bool opRestart(Reader &r) {
  uint32_t s = r.u32(), q = r.u32(), c = r.u32();
  peer.session = s ? s : peer.session + 1;
  peer.seq = q;
  uint8_t p[4];
  putU32(p, c ? c : 0xC0FFEE01);
  sendAuth(MSG_HELLO, p, 4);
  return true;
}

static bool opAnswer(Reader &r) {
  uint8_t flags = r.u8();
  if (flags & 2) {
    uint32_t c = peer.challenge, end = hal.ms + 3000;
    bool had = peer.haveChallenge;
    while ((int32_t)(hal.ms - end) < 0 && (!peer.haveChallenge || (had && peer.challenge == c)) && pass(1)) {
    }
  }
  uint8_t p[4];
  putU32(p, (flags & 1) ? peer.prevChallenge : peer.challenge);
  sendAuth(MSG_HELLO_ACK, p, 4);
  return true;
}

static bool opReplay(Reader &r) {
  uint8_t which = r.u8();
  const std::vector<Bytes> &from = (which & 0x80) ? peer.heard : peer.sent;
  if (from.empty()) return false;
  deliver(from[(which & 0x7F) % from.size()]);
  return true;
}

static bool opCrcErr() {
  if (hal.radioUp && !hal.radioHeld) {
    hal.rxq.clear();
    hal.rxq.push_back({ Bytes(), 0, 0 });
  }
  return true;
}

static bool opRadio(Reader &r) {
  uint8_t flags = r.u8();
  hal.radioPresent = !(flags & 1);
  hal.jammed = flags & 2;
  if (flags & 4) hal.noise = (int16_t)(r.u8() - 157);
  if (flags & 8) halRadioFault();  // radio.cpp's fault(): a TX that never finished, or a reset seen in RX
  return true;
}

static int runInput(const uint8_t *data, size_t size) {
  Reader r = { data, size };
  uint8_t head = r.u8();
  FuzzRole role = (head & 1) ? FR_GATE : FR_HOUSE;
  bool cold = head & 2, wrap = head & 4;
  try {
    if (cold) startCold(role, wrap, (head >> 3) & 0x0F);
    else startWarm(role, wrap);
    int id = 1;
    for (int records = 0; !r.empty() && budgetLeft() && records < 400; records++) {
      bool wait = true;
      switch (r.u8() % OP_COUNT) {
        case OP_AUTH: wait = opAuth(r); break;
        case OP_RAW: wait = opRaw(r); break;
        case OP_TIME: advance(r.u16()); break;
        case OP_PINS: {
          uint8_t v = r.u8();
          setPins(v & 0x0F);
          hal.supplyGood = !(v & 0x10);
          break;
        }
        case OP_CONSOLE: wait = opConsole(r, id); break;
        case OP_ACK: wait = opAck(r); break;
        case OP_RESTART: wait = opRestart(r); break;
        case OP_ANSWER: wait = opAnswer(r); break;
        case OP_REPLAY: wait = opReplay(r); break;
        case OP_CRCERR: wait = opCrcErr(); break;
        default: wait = opRadio(r); break;
      }
      if (wait && !settle()) break;
    }
    finish();
  } catch (const BoardReset &) {
    // A reboot (console, debug.reboot_after_cmd): the input ends here.
  }
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return runInput(data, size);
}

extern "C" int LLVMFuzzerInitialize(int *, char ***) {
  // Self-test samples. A: the gate takes a CMD OPEN and pulses. B: a console edit of pulse_ms, then a CMD.
  Bytes cmd = { OP_AUTH, MSG_CMD, 0, 3, 0x34, 0x12, 1 };
  Bytes a = { 1 };
  a.insert(a.end(), cmd.begin(), cmd.end());
  a.insert(a.end(), { OP_TIME, 0xE8, 0x03 });
  Bytes b = { 1, OP_CONSOLE, 5 << 2 };
  b.push_back(15);  // PARAMS[15]: pulse_ms
  b.insert(b.end(), { 0xD2, 0x04, 0, 0, 0 });  // 1234
  b.insert(b.end(), cmd.begin(), cmd.end());
  harnessSetup();
  harnessSelfTest(runInput, a, b);
  return 0;
}
