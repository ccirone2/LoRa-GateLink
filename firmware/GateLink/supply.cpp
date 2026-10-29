#include "supply.h"
#include <Wire.h>
#include "link.h"
#include "log.h"

static const uint8_t PMIC_ADDR = 0x6B;
static const uint8_t REG_STATUS = 0x08;  // bit 2 PG_STAT: input power good
static const uint8_t PG_BIT = 0x04;
static const uint32_t POLL_MS = 5;

static bool known = false;
static bool good = true;
static uint32_t lastPoll;
static int dipped = -1;  // REG08 as a blocking wait read it with power good low (-1: none since the last pass)

static int readStatus() {
  Wire.beginTransmission(PMIC_ADDR);
  Wire.write(REG_STATUS);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(PMIC_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

void supplyBegin() {
  Wire.begin();
  Wire.setClock(400000);
  int s = readStatus();
  known = s >= 0;
  good = !known || (s & PG_BIT);
  lastPoll = millis();
  logEvent(EV_SUPPLY, known ? good : -1, s);
}

void supplyPoll(uint32_t now) {
  if (!known) return;
  if (dipped >= 0) {
    // Power good went low inside a stall of the loop and may be back already: this pass sees it low all the same,
    // and the next poll sees what it reads then.
    int s = dipped;
    dipped = -1;
    if (good) {
      good = false;
      lastPoll = now;
      logEvent(EV_SUPPLY, 0, s);
      return;
    }
  }
  if (!elapsed(now, lastPoll, POLL_MS)) return;
  lastPoll = now;
  int s = readStatus();
  if (s < 0) return;  // a failed read keeps the last reading
  bool g = s & PG_BIT;
  if (g == good) return;
  good = g;
  logEvent(EV_SUPPLY, g, s);
}

// A loop stall (a flash save with the radio restart, ~0.5 s) could otherwise hide a dip from supplyPoll: the house
// would see the controller's relay drop with the power reading good, and send a CLOSE nobody asked for. The stall's
// waits call this (delay() through yield(), the flash's busy wait), so power good is still read every POLL_MS, and a
// low reading is kept for the next pass. Nothing else: this may run inside a flash write, so it doesn't log.
void supplyWait() {
  if (!known) return;
  uint32_t now = millis();
  if (!elapsed(now, lastPoll, POLL_MS)) return;
  lastPoll = now;
  int s = readStatus();
  if (s >= 0 && !(s & PG_BIT) && dipped < 0) dipped = s;
}

// The core's delay() calls yield() while it waits (a weak, empty hook otherwise).
extern "C" void yield(void) {
  supplyWait();
}

bool supplyKnown() { return known; }
bool supplyGood() { return good; }
