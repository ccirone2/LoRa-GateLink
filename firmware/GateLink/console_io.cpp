// Console transport on the board: USB (Serial) and the optional UART console (Serial1). The protocol is console.cpp.
#include "console_io.h"
#include "link.h"

// Second console on Serial1 (uart_console), for bench power tests: a USB-to-UART adapter stays on the PC when the
// board loses power, so it sees the boot right away. Serial1 writes block once its 256-byte buffer is full (a ~5 KB
// config.get reply takes ~200 ms at 250 kbaud). Not faster: at 1 Mbaud ~4 % of requests arrived garbled on the
// bench (bad json), whatever the interrupt priority; at 250 kbaud none did with the board idle.
#define UART_BAUD 250000

static bool uartOn = false;

static Stream &stream(uint8_t port) {
  return port == CON_USB ? (Stream &)Serial : (Stream &)Serial1;
}

void conIoBegin() {
  Serial.begin(115200);
}

void conIoUart(bool on) {
  if (on == uartOn) return;
  uartOn = on;
  if (!on) {
    Serial1.end();
    return;
  }
  Serial1.begin(UART_BAUD);
  // Pull RX up, so an unplugged adapter reads as an idle line rather than noise.
  const PinDescription &rx = g_APinDescription[PIN_SERIAL1_RX];
  PORT->Group[rx.ulPort].PINCFG[rx.ulPin].bit.PULLEN = 1;
  PORT->Group[rx.ulPort].OUTSET.reg = 1ul << rx.ulPin;
}

bool conIoOpen(uint8_t port) {
  return port == CON_USB ? Serial.dtr() : uartOn;  // not Serial's bool operator: it delays 10 ms
}

int conIoRead(uint8_t port) {
  Stream &s = stream(port);
  return s.available() ? s.read() : -1;
}

void conIoFlush(uint8_t port) {
  stream(port).flush();
}

// How long a USB packet may wait for the host to take the previous one, as in the SAMD core.
#define USB_TX_TIMEOUT_MS 70

// The USB data IN endpoint (the bulk IN one). Its BK1RDY bit is set while a packet waits for the host.
static UsbDeviceEndpoint *usbInEndpoint() {
  for (int ep = 1; ep < 8; ep++)
    if (USB->DEVICE.DeviceEndpoint[ep].EPCFG.bit.EPTYPE1 == 3) return &USB->DEVICE.DeviceEndpoint[ep];
  return nullptr;
}

// Lines go out in 64-byte pieces, and to USB each only once the host has taken the previous one. Serialized
// straight to Serial, every character was its own USB transfer, and single bytes went missing on the bench (lines
// like `{"event":"lo",...`). Handed a longer write, the core's USBDevice.send() waits between packets for the
// endpoint's transfer-complete flag, which its USB interrupt also clears (the CDC IN endpoint has no handler, so
// the interrupt acks all its flags; it runs at every 1 ms start of frame): when the interrupt got there first,
// send() waited out its 70 ms and dropped the rest of the line. A single packet onto an idle endpoint never waits.
// BK1RDY is cleared only by the hardware.
struct LineOut {
  uint8_t buf[64];
  size_t len;
  bool lost;  // part of this line was dropped
};
static LineOut lines[CON_PORTS];

static bool usbPacket(const uint8_t *buf, size_t len) {
  // Once the host has left a packet for USB_TX_TIMEOUT_MS, later ones don't wait until it takes that one (as in
  // the core), or every line would block the loop that long.
  static bool stalled = false;
  UsbDeviceEndpoint *ep = usbInEndpoint();
  if (!ep) return false;
  uint32_t t0 = millis();
  while (ep->EPSTATUS.bit.BK1RDY) {
    if (stalled || elapsed(millis(), t0, USB_TX_TIMEOUT_MS)) {
      stalled = true;
      return false;
    }
  }
  stalled = false;
  // On a failed send the core returns -1, which Serial.write passes on as a huge count.
  return Serial.write(buf, len) == len;
}

static void push(uint8_t port) {
  LineOut &l = lines[port];
  if (!l.lost && l.len) l.lost = !(port == CON_USB ? usbPacket(l.buf, l.len) : Serial1.write(l.buf, l.len) == l.len);
  l.len = 0;
}

void conIoLineWrite(uint8_t port, const uint8_t *data, size_t n) {
  LineOut &l = lines[port];
  for (size_t i = 0; i < n; i++) {
    l.buf[l.len++] = data[i];
    if (l.len == sizeof(l.buf)) push(port);
  }
}

bool conIoLineEnd(uint8_t port) {
  push(port);
  bool ok = !lines[port].lost;
  lines[port].lost = false;
  return ok;
}
