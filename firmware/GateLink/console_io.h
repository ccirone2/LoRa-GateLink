#pragma once
#include <Arduino.h>

// The console's transport: bytes in from, and lines out to, the USB port and (with uart_console) Serial1.
// Implemented in console_io.cpp on the board, and by the host simulation in tests/native; the protocol on top is
// console.cpp.
enum ConsolePortId : uint8_t { CON_USB = 0, CON_UART, CON_PORTS };

void conIoBegin();
void conIoUart(bool on);       // start or stop the Serial1 console
bool conIoOpen(uint8_t port);  // something may be listening: USB with DTR up, the UART while it runs
int conIoRead(uint8_t port);   // next byte received, -1 if none
void conIoFlush(uint8_t port);
// One line out, streamed in pieces: conIoLineWrite() as often as needed, then conIoLineEnd(), which returns false if
// any of the line was lost (the host stopped taking USB packets).
void conIoLineWrite(uint8_t port, const uint8_t *data, size_t n);
bool conIoLineEnd(uint8_t port);
