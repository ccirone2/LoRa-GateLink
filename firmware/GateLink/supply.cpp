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
  if (!known || !elapsed(now, lastPoll, POLL_MS)) return;
  lastPoll = now;
  int s = readStatus();
  if (s < 0) return;  // a failed read keeps the last reading
  bool g = s & PG_BIT;
  if (g == good) return;
  good = g;
  logEvent(EV_SUPPLY, g, s);
}

bool supplyKnown() { return known; }
bool supplyGood() { return good; }
