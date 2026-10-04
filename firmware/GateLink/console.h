#pragma once
#include <Arduino.h>
#include "log.h"

// JSON-lines API used by the web UI, over USB serial and, with uart_console, Serial1 too.
// A reply goes to the port the request came from; events go to both.
// Request:  {"id":1,"cmd":"status", ...}\n
// Response: {"id":1,"ok":true, ...}\n
// Events:   {"event":"log"|"status"|"pong"|"remote_diag"|"remote_set", ...}\n
void consoleBegin();
void consoleConfigure();  // start or stop the Serial1 console to match cfg.uart_console
void consolePoll();
void consoleEmitLog(const LogEntry &e);
void consoleEventStatus();
void consoleEventPong(uint16_t id, uint32_t rttMs, int16_t rssi, float snr, int16_t peerRssi, int8_t peerSnr);
void consoleEventDiag(const uint8_t *p, uint8_t len);
void consoleEventRemoteSet(bool acked, uint8_t result);
