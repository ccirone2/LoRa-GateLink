#pragma once
#include <Arduino.h>
#include "log.h"

// USB serial JSON-lines API used by the web UI.
// Request:  {"id":1,"cmd":"status", ...}\n
// Response: {"id":1,"ok":true, ...}\n
// Events:   {"event":"log"|"status"|"pong"|"remote_diag"|"remote_set", ...}\n
void consoleBegin();
void consolePoll();
void consoleEmitLog(const LogEntry &e);
void consoleEventStatus();
void consoleEventPong(uint16_t id, uint32_t rttMs, int16_t rssi, float snr, int16_t peerRssi, int8_t peerSnr);
void consoleEventDiag(const uint8_t *p, uint8_t len);
void consoleEventRemoteSet(bool acked, uint8_t result);
