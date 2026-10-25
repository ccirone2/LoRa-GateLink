// Stand-in for the Crypto library's AES for the model checker (declarations only, as SHA256.h). The payload cipher is
// below what the proofs look at: they take an authentic frame's payload as the sender built it.
#pragma once
#include <stddef.h>
#include <stdint.h>
class AESTiny128 {
public:
  bool setKey(const uint8_t *key, size_t len);
  void encryptBlock(uint8_t *output, const uint8_t *input);
};
