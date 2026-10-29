#pragma once
#include <Arduino.h>

// The I2C bus, for supply.cpp: every read answers, with the charger's REG08 as the harness's site has it then
// (wireStatus). A bus that fails is not modelled (docs/architecture.md, "Verification").
uint8_t wireStatus();
struct TwoWire {
  void begin() {}
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission(bool = true) { return 0; }
  uint8_t requestFrom(uint8_t, uint8_t n) { return n; }
  int read() { return wireStatus(); }
};
static TwoWire Wire;
