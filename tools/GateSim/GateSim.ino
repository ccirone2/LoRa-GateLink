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
// Power rig (optional, all on NC so a released coil means powered/connected, as during a Uno reset):
//   D7 relay NC  -> 24 V into the gate board's buck   (gate board supply)
//   D8 relay NC  -> gate board LiPo + lead
//   D9 relay NC  -> house 12 V rail (controller, IN2 opto, house board's buck)
//   D10 relay NC -> house board LiPo + lead
//
// Model: OPEN heads for open, CLOSE heads for closed, reversing mid-travel from the current position
// (dedicated open/close inputs, no single-button stop). Limits drop as soon as the gate leaves them.
// The opener runs on AC or its battery: without AC (IN3 off) it carries on; with neither it's dead: limits
// drop, motion freezes and pulses are ignored. The gate board's supply follows the opener's accessory output
// (alive) or the AC supply, per `supply`, or only explicit cuts (`supply none`, the default).
//
// Serial 115200, one command per line (send "help"). Replies and "evt ..." lines are plain text:
// "evt pulse open|close|both" on each debounced press ("both" whenever one input closes while the other
// is held), "evt release open|close <ms>" with the press length, "evt state <name>", "evt cmd ...".
// "evt ac on|off", "evt rail gate|house on|off" and "evt lipo gate|house on|off" on every change.
// Opening the port resets the Uno (DTR), which restarts the simulation closed and powered.
#include <EEPROM.h>

const uint8_t PIN_R_OPEN = 2, PIN_R_CLOSED = 3, PIN_R_POWER = 4;
const uint8_t PIN_R_GATE_RAIL = 7, PIN_R_GATE_LIPO = 8, PIN_R_HOUSE_RAIL = 9, PIN_R_HOUSE_LIPO = 10;
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
bool ac = true;        // AC mains: the 24 V supply IN3 watches
bool battery = true;   // the opener's own battery backup
enum Supply : uint8_t { SUP_NONE, SUP_ACC, SUP_PSU };
const char *const SUPPLY_NAMES[] = {"none", "acc", "psu"};
Supply supply = SUP_NONE;  // what the gate board's buck is fed from (not saved)
// Rails 0 = gate, 1 = house: -1 follow the model, 0/1 forced; a timed cut forces 0 until cutUntil.
const char *const SITE_NAMES[] = {"gate", "house"};
int8_t railForce[2] = {-1, -1};
uint32_t cutUntil[2] = {0, 0};
bool lipo[2] = {true, true};  // battery connected (NC: connected while the coil is released)
bool lastRail[2] = {true, true}, lastLipo[2] = {true, true}, lastAc = true;
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

bool alive() { return ac || battery; }

// Whether a site's supply rail is on: the model (gate: its supply source), or forced, or cut for a while.
bool railOn(uint8_t i) {
  if (railForce[i] >= 0) return railForce[i];
  if (i == 0 && supply == SUP_ACC) return alive();
  if (i == 0 && supply == SUP_PSU) return ac;
  return true;
}

const char *stateName() {
  if (!alive()) return "no_power";
  if (dir > 0) return "opening";
  if (dir < 0) return "closing";
  if (pos == 0) return "closed";
  if (pos >= travelMs()) return "open";
  return "stopped";
}

bool atOpen() { return alive() && dir == 0 && pos >= travelMs(); }
bool atClosed() { return alive() && dir == 0 && pos == 0; }

void printSite(const __FlashStringHelper *what, uint8_t i, bool on) {
  Serial.print(what);
  Serial.print(SITE_NAMES[i]);
  Serial.println(on ? F(" on") : F(" off"));
}

void applyRelays(uint32_t now) {
  bool o = atOpen(), c = atClosed();
  if (fault == F_BOTH && alive()) o = c = true;
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
  bool in3 = override[2] < 0 ? ac : override[2];
  writeRelay(PIN_R_POWER, in3);
  if (in3 != lastAc) {
    lastAc = in3;
    Serial.println(in3 ? F("evt ac on") : F("evt ac off"));
  }
  const uint8_t railPins[2] = {PIN_R_GATE_RAIL, PIN_R_HOUSE_RAIL}, lipoPins[2] = {PIN_R_GATE_LIPO, PIN_R_HOUSE_LIPO};
  for (uint8_t i = 0; i < 2; i++) {
    if (cutUntil[i] && (int32_t)(now - cutUntil[i]) >= 0) {
      cutUntil[i] = 0;
      railForce[i] = -1;
    }
    bool r = railOn(i);
    writeRelay(railPins[i], !r);  // NC: energized = cut
    writeRelay(lipoPins[i], !lipo[i]);
    if (r != lastRail[i]) {
      lastRail[i] = r;
      printSite(F("evt rail "), i, r);
    }
    if (lipo[i] != lastLipo[i]) {
      lastLipo[i] = lipo[i];
      printSite(F("evt lipo "), i, lipo[i]);
    }
  }
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
  if (!alive()) {
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
  if (!alive() || dir == 0) return;
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
  Serial.print(alive());
  Serial.print(F(" relays open="));
  Serial.print(atOpen() || (fault == F_BOTH && alive()));
  Serial.print(F(" closed="));
  Serial.print(atClosed() || (fault == F_BOTH && alive()));
  Serial.print(F(" power="));
  Serial.print(override[2] < 0 ? ac : override[2]);
  Serial.print(F(" ac="));
  Serial.print(ac);
  Serial.print(F(" battery="));
  Serial.print(battery);
  Serial.print(F(" supply="));
  Serial.print(SUPPLY_NAMES[supply]);
  Serial.print(F(" rail_gate="));
  Serial.print(railOn(0));
  Serial.print(F(" rail_house="));
  Serial.print(railOn(1));
  Serial.print(F(" lipo_gate="));
  Serial.print(lipo[0]);
  Serial.print(F(" lipo_house="));
  Serial.print(lipo[1]);
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
  Serial.println(F("commands: status | open | close | stop | travel <1-300 s> | help"));
  Serial.println(F("  power on|off (AC and opener battery) | ac on|off | battery on|off"));
  Serial.println(F("  supply none|acc|psu  gate board fed by: explicit cuts only / accessory output / AC supply"));
  Serial.println(F("  rail gate|house on|off|auto | rail gate|house cut <ms> | lipo gate|house on|off"));
  Serial.println(F("  fault none|stuck|both|flicker|deaf | polarity low|high"));
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
  } else if ((!strcmp(cmd, "power") || !strcmp(cmd, "ac") || !strcmp(cmd, "battery")) && arg
             && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
    bool on = !strcmp(arg, "on");
    if (cmd[0] != 'b') ac = on;
    if (cmd[0] != 'a') battery = on;
    if (!alive()) dir = 0;  // opener stops dead; it doesn't resume on power return
    Serial.println(F("ok"));
  } else if (!strcmp(cmd, "supply") && arg) {
    for (uint8_t i = 0; i < 3; i++) {
      if (!strcmp(arg, SUPPLY_NAMES[i])) {
        supply = (Supply)i;
        Serial.println(F("ok"));
        return;
      }
    }
    Serial.println(F("err supply none|acc|psu"));
  } else if ((!strcmp(cmd, "rail") || !strcmp(cmd, "lipo")) && arg
             && (!strcmp(arg, "gate") || !strcmp(arg, "house"))) {
    uint8_t i = !strcmp(arg, "house");
    char *mode = strtok(nullptr, " \t");
    char *ms = strtok(nullptr, " \t");
    bool rail = cmd[0] == 'r';
    if (mode && (!strcmp(mode, "on") || !strcmp(mode, "off"))) {
      if (rail) railForce[i] = !strcmp(mode, "on");
      else lipo[i] = !strcmp(mode, "on");
      if (rail) cutUntil[i] = 0;
    } else if (rail && mode && !strcmp(mode, "auto")) {
      railForce[i] = -1;
      cutUntil[i] = 0;
    } else if (rail && mode && !strcmp(mode, "cut") && ms && atol(ms) >= 1 && atol(ms) <= 600000L) {
      railForce[i] = 0;
      cutUntil[i] = (millis() + (uint32_t)atol(ms)) | 1;
    } else {
      Serial.println(rail ? F("err rail gate|house on|off|auto|cut <ms>") : F("err lipo gate|house on|off"));
      return;
    }
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
  const uint8_t relays[] = {PIN_R_OPEN, PIN_R_CLOSED, PIN_R_POWER, PIN_R_GATE_RAIL, PIN_R_GATE_LIPO,
                            PIN_R_HOUSE_RAIL, PIN_R_HOUSE_LIPO};
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
