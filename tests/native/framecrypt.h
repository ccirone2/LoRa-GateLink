// A frame's payload encryption as docs/protocol.md specifies it, written apart from link.cpp's cryptPayload so the
// tests hold the firmware to the protocol rather than to itself: AES-128 in counter mode, keyed with the first 16
// bytes of HMAC-SHA256(link key, "GateLink payload v1"); the counter block is the frame's bytes 3..12 (src, dst,
// session, seq), six zero bytes, and the block number in the last. The same call encrypts and decrypts (an XOR).
#pragma once
#include <AES.h>
#include <SHA256.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#define FRAME_VER 2       // link.cpp PROTO_VER
#define FRAME_PAYLOAD 13  // link.cpp HDR_LEN: where the payload starts
#define FRAME_TAG 8       // link.cpp TAG_LEN

inline void frameCrypt(const uint8_t key[16], uint8_t *frame, size_t payloadLen) {
  static const char label[] = "GateLink payload v1";
  uint8_t k[32];
  SHA256 h;
  h.resetHMAC(key, 16);
  h.update(label, sizeof(label) - 1);
  h.finalizeHMAC(key, 16, k, sizeof(k));
  AES128 aes;
  aes.setKey(k, 16);
  uint8_t block[16] = {}, stream[16];
  memcpy(block, frame + 3, 10);
  for (size_t off = 0; off < payloadLen; off += 16) {
    block[15] = (uint8_t)(off / 16);
    aes.encryptBlock(stream, block);
    for (size_t i = 0; i < 16 && off + i < payloadLen; i++) frame[FRAME_PAYLOAD + off + i] ^= stream[i];
  }
}

// The plain payload of a whole frame (header, encrypted payload, tag) sent under `key`.
inline std::vector<uint8_t> framePlain(const uint8_t key[16], const std::vector<uint8_t> &frame) {
  if (frame.size() < FRAME_PAYLOAD + FRAME_TAG) return {};
  std::vector<uint8_t> f = frame;
  size_t len = f.size() - FRAME_PAYLOAD - FRAME_TAG;
  frameCrypt(key, f.data(), len);
  return std::vector<uint8_t>(f.begin() + FRAME_PAYLOAD, f.begin() + FRAME_PAYLOAD + len);
}

// A whole frame from its header fields and plain payload: encrypted, then tagged (encrypt-then-MAC).
inline std::vector<uint8_t> frameSeal(const uint8_t key[16], uint8_t type, uint8_t netId, uint8_t src, uint8_t dst,
                                      uint32_t session, uint32_t seq, const uint8_t *payload, size_t len) {
  std::vector<uint8_t> f(FRAME_PAYLOAD + len + FRAME_TAG);
  f[0] = FRAME_VER;
  f[1] = type;
  f[2] = netId;
  f[3] = src;
  f[4] = dst;
  for (int i = 0; i < 4; i++) f[5 + i] = (uint8_t)(session >> (8 * i)), f[9 + i] = (uint8_t)(seq >> (8 * i));
  if (len) memcpy(&f[FRAME_PAYLOAD], payload, len);
  frameCrypt(key, f.data(), len);
  SHA256 h;
  h.resetHMAC(key, 16);
  h.update(f.data(), FRAME_PAYLOAD + len);
  h.finalizeHMAC(key, 16, &f[FRAME_PAYLOAD + len], FRAME_TAG);
  return f;
}
