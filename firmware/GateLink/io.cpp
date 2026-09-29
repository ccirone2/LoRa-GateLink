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

void Relay::set(bool on) {
  _pulseUntil = 0;
  _on = on;
  digitalWrite(_pin, on ? HIGH : LOW);
}

void Relay::pulse(uint32_t now, uint32_t ms) {
  _on = true;
  digitalWrite(_pin, HIGH);
  _pulseUntil = now + ms;
  if (_pulseUntil == 0) _pulseUntil = 1;
}

void Relay::update(uint32_t now) {
  if (_pulseUntil && (int32_t)(now - _pulseUntil) >= 0) set(false);
}
