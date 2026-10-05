// GateSim: bench-only CSW24UL opener simulator for the GateLink gate board. Not for the install.
// Arduino Uno + relay module. Relay contacts are wetted with 24 V (COM) and feed the gate board's opto
// channels, exactly like the opener's AUX relays and accessory output would.
//
//   D2 relay NO -> opto ch1 -> gate IN1  "open limit"   (on only while powered and fully open)
//   D3 relay NC -> opto ch2 -> gate IN2  "closed limit" (on only while powered and fully closed). The CSW24UL's
//                                        closed-limit AUX relay energizes when NOT at the close limit, so it's
//                                        wired on NC and D3's coil is driven inverted, as the real relay is.
//   D4 relay NO -> opto ch3 -> gate IN3  AC power
//   D5 <- gate K1 NO (OPEN pulse),  gate K1 COM -> Uno GND   (dry contacts, INPUT_PULLUP)
//   D6 <- gate K2 NO (CLOSE pulse), gate K2 COM -> Uno GND
//
// Model: OPEN heads for open, CLOSE heads for closed, reversing mid-travel from the current position
// (dedicated open/close inputs, no single-button stop). Limits drop as soon as the gate leaves them.
// Without power the limits drop, motion freezes and pulses are ignored.
//
// Serial 115200, one command per line (send "help"). Replies and "evt ..." lines are plain text:
// "evt pulse open|close|both" on each debounced press ("both" whenever one input closes while the other
// is held), "evt release open|close <ms>" with the press length, "evt state <name>", "evt cmd ...".
// Opening the port resets the Uno (DTR), which restarts the simulation closed and powered.
#include <EEPROM.h>

const uint8_t PIN_R_OPEN = 2, PIN_R_CLOSED = 3, PIN_R_POWER = 4;
const uint8_t PIN_S_OPEN = 5, PIN_S_CLOSE = 6;
const uint32_t SENSE_DEBOUNCE_MS = 20;
const uint8_t FLICKER_TOGGLES = 6;     // relay changes per chatter burst
const uint32_t FLICKER_STEP_MS = 30;

// Persisted settings.
struct Settings {
  uint16_t magic;
  uint8_t activeLow;   // relay module input polarity: 1 = relay on when pin LOW, 0 = on when HIGH
  uint16_t travelS;
};
const uint16_t MAGIC = 0x6753;
Settings st;

enum Fault : uint8_t { F_NONE, F_STUCK, F_BOTH, F_FLICKER, F_DEAF };
const char *const FAULT_NAMES[] = {"none", "stuck", "both", "flicker", "deaf"};

// Position in ms of travel: 0 = closed, travelMs() = open.
uint32_t pos = 0;
int8_t dir = 0;  // +1 opening, -1 closing, 0 stopped
bool powered = true;
Fault fault = F_NONE;
uint8_t flickerLeft = 0;
uint32_t flickerAt = 0;
bool flickerPhase = false;
uint32_t lastTick = 0;
const char *lastState = "";
// Wiring-test overrides for relays 1-3 (D2-D4): -1 = follow the simulation, else forced off/on. Not saved.
int8_t override[3] = {-1, -1, -1};

struct Sense {
  uint8_t pin;
  bool raw, stable;
  uint32_t changedAt, pressedAt;
  // Debounced edge: +1 pressed (contact closed pulls the pin LOW), -1 released, 0 none.
  int8_t update(uint32_t now) {
    bool r = digitalRead(pin) == LOW;
    if (r != raw) {
      raw = r;
      changedAt = now;
    }
    if (raw == stable || now - changedAt < SENSE_DEBOUNCE_MS) return 0;
    stable = raw;
    if (stable) pressedAt = changedAt;
    return stable ? 1 : -1;
  }
  // Raw edge to raw edge, so the debounce delays both ends alike and cancels out.
  uint32_t heldMs() const { return changedAt - pressedAt; }
};
Sense sOpen = {PIN_S_OPEN, false, false, 0, 0}, sClose = {PIN_S_CLOSE, false, false, 0, 0};

uint32_t travelMs() { return (uint32_t)st.travelS * 1000; }

void writeRelay(uint8_t pin, bool on) { digitalWrite(pin, on == !st.activeLow ? HIGH : LOW); }

void loadSettings() {
  EEPROM.get(0, st);
  if (st.magic != MAGIC || st.activeLow > 1 || st.travelS < 1 || st.travelS > 300) {
    st.magic = MAGIC;
    st.activeLow = 0;  // the bench relay module switches on when the pin is HIGH
    st.travelS = 15;
    EEPROM.put(0, st);
  }
}

const char *stateName() {
  if (!powered) return "no_power";
  if (dir > 0) return "opening";
  if (dir < 0) return "closing";
  if (pos == 0) return "closed";
  if (pos >= travelMs()) return "open";
  return "stopped";
}

bool atOpen() { return powered && dir == 0 && pos >= travelMs(); }
bool atClosed() { return powered && dir == 0 && pos == 0; }

void applyRelays(uint32_t now) {
  bool o = atOpen(), c = atClosed();
  if (fault == F_BOTH && powered) o = c = true;
  if (flickerLeft && now - flickerAt >= FLICKER_STEP_MS) {
    flickerAt = now;
    flickerPhase = !flickerPhase;
    flickerLeft--;
  }
  if (flickerLeft && flickerPhase) {
    if (o) o = false;
    if (c) c = false;
  }
  writeRelay(PIN_R_OPEN, override[0] < 0 ? o : override[0]);
  // NC contact: the coil is off while the closed-limit signal is on. Unpowered, the real opener has no 24 V to
  // wet its contacts at all; here the coil stays energized instead, so IN2 reads off just the same.
  writeRelay(PIN_R_CLOSED, !(override[1] < 0 ? c : override[1]));
  writeRelay(PIN_R_POWER, override[2] < 0 ? powered : override[2]);
}

void reportState() {
  const char *s = stateName();
  if (strcmp(s, lastState) == 0) return;
  lastState = s;
  Serial.print(F("evt state "));
  Serial.println(s);
}

// Start moving toward open (+1) or closed (-1). src names who asked, for the event line.
void command(int8_t want, const __FlashStringHelper *src) {
  Serial.print(F("evt cmd "));
  Serial.print(want > 0 ? F("open") : F("close"));
  Serial.print(F(" from "));
  Serial.print(src);
  if (!powered) {
    Serial.println(F(" ignored (no power)"));
    return;
  }
  if ((want > 0 && pos >= travelMs()) || (want < 0 && pos == 0)) {
    dir = 0;
    Serial.println(F(" (already there)"));
    return;
  }
  Serial.println();
  dir = want;
  if (fault == F_STUCK) {
    // Leave the limit, then jam: the gate board sees between forever.
    pos = want > 0 ? (pos == 0 ? 1 : pos) : (pos >= travelMs() ? travelMs() - 1 : pos);
    dir = 0;
    Serial.println(F("evt jammed (fault stuck)"));
  }
}

void tick(uint32_t now) {
  uint32_t dt = now - lastTick;
  lastTick = now;
  if (!powered || dir == 0) return;
  if (dir > 0) {
    pos = pos + dt >= travelMs() ? travelMs() : pos + dt;
    if (pos >= travelMs()) dir = 0;
  } else {
    pos = pos <= dt ? 0 : pos - dt;
    if (pos == 0) dir = 0;
  }
  if (dir == 0 && fault == F_FLICKER) {
    flickerLeft = FLICKER_TOGGLES;
    flickerAt = now;
    flickerPhase = false;
  }
}

void printStatus() {
  Serial.print(F("state="));
  Serial.print(stateName());
  Serial.print(F(" pos="));
  Serial.print(travelMs() ? (uint8_t)(pos * 100 / travelMs()) : 0);
  Serial.print(F("% power="));
  Serial.print(powered);
  Serial.print(F(" relays open="));
  Serial.print(atOpen() || (fault == F_BOTH && powered));
  Serial.print(F(" closed="));
  Serial.print(atClosed() || (fault == F_BOTH && powered));
  Serial.print(F(" power="));
  Serial.print(powered);
  Serial.print(F(" travel="));
  Serial.print(st.travelS);
  Serial.print(F("s fault="));
  Serial.print(FAULT_NAMES[fault]);
  Serial.print(F(" polarity="));
  Serial.println(st.activeLow ? F("low") : F("high"));
}

// How long an input was held, so the suite can check pulse lengths: "evt release open 502".
void printRelease(const __FlashStringHelper *which, uint32_t ms) {
  Serial.print(F("evt release "));
  Serial.print(which);
  Serial.print(' ');
  Serial.println(ms);
}

void printHelp() {
  Serial.println(F("commands: status | open | close | stop | power on|off | travel <1-300 s>"));
  Serial.println(F("  fault none|stuck|both|flicker|deaf | polarity low|high | help"));
  Serial.println(F("  relay <1-3> on|off|auto  force the signal to gate IN1/IN2/IN3 (D2/D3/D4; D3 inverted, NC)"));
  Serial.println(F("  open/close act like a local button (not the gate board); stop halts mid-travel"));
}

void handleLine(char *line) {
  char *cmd = strtok(line, " \t");
  char *arg = strtok(nullptr, " \t");
  if (!cmd) return;
  if (!strcmp(cmd, "status")) {
    printStatus();
  } else if (!strcmp(cmd, "open")) {
    command(+1, F("serial"));
  } else if (!strcmp(cmd, "close")) {
    command(-1, F("serial"));
  } else if (!strcmp(cmd, "stop")) {
    dir = 0;
    Serial.println(F("ok"));
  } else if (!strcmp(cmd, "power") && arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
    powered = !strcmp(arg, "on");
    if (!powered) dir = 0;  // opener stops dead; it doesn't resume on power return
    Serial.println(F("ok"));
  } else if (!strcmp(cmd, "travel") && arg && atoi(arg) >= 1 && atoi(arg) <= 300) {
    // Keep the same fraction of travel so a moving gate doesn't jump.
    uint32_t oldMs = travelMs();
    st.travelS = atoi(arg);
    pos = oldMs ? (uint32_t)((uint64_t)pos * travelMs() / oldMs) : 0;
    EEPROM.put(0, st);
    Serial.println(F("ok"));
  } else if (!strcmp(cmd, "fault") && arg) {
    for (uint8_t i = 0; i < sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]); i++) {
      if (!strcmp(arg, FAULT_NAMES[i])) {
        fault = (Fault)i;
        flickerLeft = 0;
        Serial.println(F("ok"));
        return;
      }
    }
    Serial.println(F("err bad fault"));
  } else if (!strcmp(cmd, "polarity") && arg && (!strcmp(arg, "low") || !strcmp(arg, "high"))) {
    st.activeLow = !strcmp(arg, "low");
    EEPROM.put(0, st);
    Serial.println(F("ok"));
  } else if (!strcmp(cmd, "relay") && arg && arg[0] >= '1' && arg[0] <= '3' && !arg[1]) {
    char *mode = strtok(nullptr, " \t");
    int8_t v = !mode ? -2 : !strcmp(mode, "on") ? 1 : !strcmp(mode, "off") ? 0 : !strcmp(mode, "auto") ? -1 : -2;
    if (v == -2) {
      Serial.println(F("err relay <1-3> on|off|auto"));
      return;
    }
    override[arg[0] - '1'] = v;
    Serial.println(F("ok"));
  } else if (!strcmp(cmd, "help")) {
    printHelp();
  } else {
    Serial.println(F("err unknown command (try help)"));
  }
}

void readSerial() {
  static char buf[48];
  static uint8_t len = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buf[len] = 0;
      len = 0;
      handleLine(buf);
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

void setup() {
  loadSettings();
  // Drive the "off" level before enabling the outputs so the relays don't click at boot (the simulation then
  // starts closed: D3's coil stays off, its NC contact giving the closed limit).
  const uint8_t relays[] = {PIN_R_OPEN, PIN_R_CLOSED, PIN_R_POWER};
  for (uint8_t p : relays) {
    writeRelay(p, false);
    pinMode(p, OUTPUT);
  }
  pinMode(PIN_S_OPEN, INPUT_PULLUP);
  pinMode(PIN_S_CLOSE, INPUT_PULLUP);
  Serial.begin(115200);
  lastTick = millis();
  applyRelays(lastTick);
  Serial.println(F("GateSim ready (bench simulator, send help)"));
  printStatus();
  reportState();
}

void loop() {
  uint32_t now = millis();
  readSerial();
  int8_t eo = sOpen.update(now), ec = sClose.update(now);
  if (eo > 0 || ec > 0) {
    // Overlap whenever one input closes while the other is still held, not only when both land together.
    bool both = sOpen.stable && sClose.stable;
    Serial.print(F("evt pulse "));
    Serial.println(both ? F("both") : eo > 0 ? F("open") : F("close"));
    if (fault == F_DEAF) {
      Serial.println(F("evt ignored (fault deaf)"));
    } else if (both) {
      dir = 0;  // the gate board interlocks K1/K2, so this is a wiring or firmware fault
    } else {
      command(eo > 0 ? +1 : -1, F("gate"));
    }
  }
  if (eo < 0) printRelease(F("open"), sOpen.heldMs());
  if (ec < 0) printRelease(F("close"), sClose.heldMs());
  tick(now);
  applyRelays(now);
  reportState();
}
