#pragma once
#include <Arduino.h>

// Input with internal pull-down; "active" = driven to 3.3 V (unless inverted). Suits PNP
// (sourcing) opto outputs and contacts to 3.3 V: a dead opto or cut wire reads inactive.
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

// Relay output with optional non-blocking pulse, which can start after a delay (the coil stays off until then).
class Relay {
public:
  void begin(uint8_t pin);
  void set(bool on);
  void pulse(uint32_t now, uint32_t ms, uint32_t delayMs = 0);
  void update(uint32_t now);
  bool on() const { return _on; }
  bool pulsing() const { return _pulseUntil != 0; }
  // How much of a `gap` ms wait after the coil last released is still left (0 if it has passed).
  uint32_t gapLeft(uint32_t now, uint32_t gap) const;
private:
  uint8_t _pin = 0;
  bool _on = false;
  uint32_t _startAt = 0;  // delayed pulse: when the coil goes on (0 = not waiting)
  uint32_t _pulseUntil = 0;
  uint32_t _offAt = 0;  // when the coil last released (0 = not recently; cleared after RELEASE_MEMO_MS)
};
