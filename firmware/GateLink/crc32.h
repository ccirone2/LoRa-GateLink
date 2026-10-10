#pragma once
#include <stddef.h>
#include <stdint.h>

// CRC-32 (IEEE 802.3, as zlib's crc32), four bits at a time. Chain it by passing the previous result as crc. Used
// by the config record (config.cpp), the history log's records (histlog.cpp) and the console's request check
// (console.cpp).
inline uint32_t crc32(const void *data, size_t n, uint32_t crc = 0) {
  static const uint32_t T[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  const uint8_t *p = (const uint8_t *)data;
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    crc = (crc >> 4) ^ T[crc & 15];
    crc = (crc >> 4) ^ T[crc & 15];
  }
  return ~crc;
}
