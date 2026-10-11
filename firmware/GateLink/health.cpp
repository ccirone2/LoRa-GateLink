// The board's health and the fault output on D5 (health.h).
#include "health.h"
#include "config.h"
#include "link.h"
#include "log.h"
#include "pins.h"
#include "radio.h"
#include "roles.h"

static uint8_t problems = PROB_STARTING;  // as of the last poll
static bool healthy = false;  // D5's level with fault_out on: LOW from boot until the first pass with no problem
static bool troubled = false;  // a run of problems is going on, since troubleSince
static uint32_t troubleSince = 0;
static int8_t driven = -1;  // D5 as driven: 1 HIGH, 0 LOW, -1 an input (fault_out off)

// elapsed() is signed: a stamp left alone for 2^31 ms (~24.8 days) would read as in the future. A problem that goes
// on that long (a dead link) moves its stamp along instead.
#define STAMP_CAP_MS 0x40000000UL

static uint8_t evaluate(bool linkUp) {
  bool decided = false, ac = true;
  uint8_t gs = GS_UNKNOWN;
  if (activeRole == ROLE_HOUSE) {
    decided = houseDecided();
    ac = !houseGateAcLost();
    gs = houseGateState();
  } else if (activeRole == ROLE_GATE) {
    decided = gateDecided();
    ac = gateAcPresent();
    gs = gateCurrentState();
  }
  uint8_t p = 0;
  if (!decided) p |= PROB_STARTING;
  if (!radioOk()) p |= PROB_RADIO;
  if (!linkUp) p |= PROB_LINK;
  if (!ac) p |= PROB_AC;
  if (gs == GS_NO_POWER) p |= PROB_NO_POWER;
  if (gs == GS_FAULT) p |= PROB_FAULT;
  return p;
}

// D5 follows `healthy` while fault_out is on; with it off, D5 is an input with the pull-down.
static void drive() {
  int8_t want = cfg.fault_out ? healthy : -1;
  if (want == driven) return;
  if (want < 0) {
    pinMode(PIN_FAULT, INPUT_PULLDOWN);
  } else {
    digitalWrite(PIN_FAULT, want ? HIGH : LOW);  // the level first, so turning it into an output doesn't glitch
    if (driven < 0) pinMode(PIN_FAULT, OUTPUT);
  }
  driven = want;
  logEvent(EV_HEALTH, want, problems);
}

void healthBegin() {
  problems = PROB_STARTING;
  healthy = false;
  troubled = true;
  troubleSince = millis();
  driven = -1;
  pinMode(PIN_FAULT, INPUT_PULLDOWN);
  drive();  // fault_out on: LOW until the role has decided
}

void healthPoll(uint32_t now, bool linkUp) {
  problems = evaluate(linkUp);
  if (!problems) {
    troubled = false;
    healthy = true;  // recovery raises D5 at once
  } else {
    if (!troubled) {
      troubled = true;
      troubleSince = now;
    } else if (elapsed(now, troubleSince, STAMP_CAP_MS)) {
      troubleSince = now - STAMP_CAP_MS;
    }
    // Only once the problems have lasted fault_hold_s: a brief blip doesn't trip the alarm.
    if (elapsed(now, troubleSince, (uint32_t)cfg.fault_hold_s * 1000)) healthy = false;
  }
  drive();
}

uint8_t healthProblems() {
  return problems;
}

const char *healthName(uint8_t bit) {
  static const char *const n[] = { "starting", "radio", "link", "ac", "no_power", "fault" };
  static_assert(sizeof(n) / sizeof(n[0]) == PROBLEM_COUNT, "a name per Problem");
  return bit < PROBLEM_COUNT ? n[bit] : "?";
}

int8_t healthFaultOut() {
  return driven;
}
