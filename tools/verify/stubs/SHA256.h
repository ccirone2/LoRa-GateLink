// Stand-in for the Crypto library's SHA256 for the model checker. Declarations only: CBMC gives a function without a
// body no effect, so a MAC or hash it "computes" is whatever the output buffer held (nondeterministic). The link
// harness (cbmc/link.cpp) defines these itself, as an ideal MAC.
#pragma once
#include <stddef.h>
#include <stdint.h>
class SHA256 {
public:
  void resetHMAC(const void *key, size_t keyLen);
  void update(const void *data, size_t len);
  void finalizeHMAC(const void *key, size_t keyLen, void *hash, size_t hashLen);
};
