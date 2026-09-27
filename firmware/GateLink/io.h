#pragma once
#include <Arduino.h>

// Dry-contact input with pull-up; "active" = contact closed (unless inverted).
class Input {
public:
  void begin(uint8_t pin, bool invert);
  // Returns true when the debounced state changed on this call.
  bool update(uint32_t now, uint32_t debounceMs, bool invert);
  bool active() const { return _state; }
  bool raw() const { return _raw; }
private:
  uint8_t _pin = 0;
  bool _state = false;
  bool _raw = false;
  bool _candidate = false;
  uint32_t _since = 0;
};

// Relay output with optional non-blocking pulse.
class Relay {
public:
  void begin(uint8_t pin);
  void set(bool on);
  void pulse(uint32_t now, uint32_t ms);
  void update(uint32_t now);
  bool on() const { return _on; }
  bool pulsing() const { return _pulseUntil != 0; }
private:
  uint8_t _pin = 0;
  bool _on = false;
  uint32_t _pulseUntil = 0;
};
