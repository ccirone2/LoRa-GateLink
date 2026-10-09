#include "io.h"

void Input::begin(uint8_t pin, bool invert) {
  _pin = pin;
  pinMode(_pin, INPUT_PULLDOWN);
  delayMicroseconds(50);  // let the pull-down settle before the first read
  _raw = _candidate = (digitalRead(_pin) == HIGH) != invert;
  _state = _raw;
  _since = millis();
}

bool Input::update(uint32_t now, uint32_t debounceMs, bool invert) {
  _raw = (digitalRead(_pin) == HIGH) != invert;
  if (_raw != _candidate) {
    _candidate = _raw;
    _since = now;
  }
  if (_candidate != _state && now - _since >= debounceMs) {
    _state = _candidate;
    return true;
  }
  return false;
}

void Relay::begin(uint8_t pin) {
  _pin = pin;
  digitalWrite(_pin, LOW);
  pinMode(_pin, OUTPUT);
  _on = false;
  _pulseUntil = 0;
}

// Longer than any gap asked of gapLeft(); clearing _offAt then keeps the signed comparison from wrapping.
#define RELEASE_MEMO_MS 1000

void Relay::set(bool on) {
  if (_on && !on) _offAt = millis() | 1;
  _startAt = 0;
  _pulseUntil = 0;
  _on = on;
  digitalWrite(_pin, on ? HIGH : LOW);
}

void Relay::pulse(uint32_t now, uint32_t ms, uint32_t delayMs) {
  if (delayMs) {
    _startAt = (now + delayMs) | 1;
  } else {
    _startAt = 0;
    _on = true;
    digitalWrite(_pin, HIGH);
  }
  _pulseUntil = (now + delayMs + ms) | 1;
}

void Relay::update(uint32_t now) {
  if (_startAt && (int32_t)(now - _startAt) >= 0) {
    _startAt = 0;
    _on = true;
    digitalWrite(_pin, HIGH);
  }
  if (_pulseUntil && !_startAt && (int32_t)(now - _pulseUntil) >= 0) set(false);
  if (_offAt && (int32_t)(now - _offAt) >= RELEASE_MEMO_MS) _offAt = 0;
}

uint32_t Relay::gapLeft(uint32_t now, uint32_t gap) const {
  if (!_offAt) return 0;
  int32_t since = (int32_t)(now - _offAt);
  if (since < 0) since = 0;  // stamped with millis(), which can be later than the caller's `now`
  return (uint32_t)since < gap ? gap - since : 0;
}
