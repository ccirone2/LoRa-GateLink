// Gate node: pulses the CSW24UL OPEN/CLOSE inputs and reports gate position.
//
// The opener's inputs are shared with other controllers (AES Prime Edge, siren
// sensor), so outputs are only ever pulsed and gate state always comes from the
// limit inputs, never from our own last command.
#include "roles.h"
#include "config.h"
#include "log.h"

static uint8_t state = GS_UNKNOWN;
static uint8_t cause = CAUSE_NONE;
static uint8_t lastResult = TR_NONE;
static uint8_t target = GS_UNKNOWN;  // state we're waiting to reach after our pulse
static uint32_t targetSince = 0;
static uint32_t lastPulseAt = 0;
static bool havePulsed = false;
static bool haveCmd = false;
static uint16_t lastCmdId = 0;
static uint8_t lastCmdAck = RES_OK;
static uint32_t lastStatusAt = 0;
static int16_t houseRssi = 0;
static int8_t houseSnr = 0;
static uint32_t seenSessions = 0;

// Signed elapsed-time check: safe when t was stamped slightly after the loop's `now`.
static bool elapsed(uint32_t now, uint32_t t, uint32_t ms) {
  return (int32_t)(now - t) >= (int32_t)ms;
}

static uint8_t readState() {
  // IN3 = opener 24 V present. Without power the AUX limit relays drop, which would read as BETWEEN.
  if (cfg.power_sense && !in3.active()) return GS_NO_POWER;
  bool open = in1.active(), closed = in2.active();
  if (open && closed) return GS_FAULT;
  if (closed) return GS_CLOSED;
  if (open) return GS_OPEN;
  return GS_BETWEEN;
}

static void sendStatus(uint32_t now) {
  uint8_t p[ST_LEN];
  p[ST_STATE] = state;
  p[ST_INPUTS] = in1.active() | (in2.active() << 1) | (k1.on() << 2) | (k2.on() << 3)
                 | (in3.active() << 4) | (in4.active() << 5);
  p[ST_CAUSE] = cause;
  p[ST_RESULT] = lastResult;
  putU16(p + ST_CMD_ID, lastCmdId);
  putU32(p + ST_UPTIME, now / 1000);
  putU16(p + ST_RSSI, (uint16_t)houseRssi);
  p[ST_SNR] = (uint8_t)houseSnr;
  p[ST_TARGET] = target;
  linkSendReliable(SLOT_STATUS, MSG_STATUS, p, ST_LEN, (uint32_t)cfg.heartbeat_s * 1000);
  lastStatusAt = now;
}

static void pulse(Relay &r, Relay &other, uint8_t which, uint32_t now) {
  other.set(false);  // interlock: never both
  r.pulse(now, cfg.pulse_ms);
  lastPulseAt = now;
  havePulsed = true;
  logEvent(EV_PULSE, which, cfg.pulse_ms);
}

void gateBegin() {
  state = readState();
  cause = CAUSE_NONE;
  sendStatus(millis());
}

void gateLoop(uint32_t now) {
  in1.update(now, cfg.debounce_ms, cfg.in1_invert);
  in2.update(now, cfg.debounce_ms, cfg.in2_invert);
  bool spareChanged = updateSpareInputs(now);  // before readState(): IN3 is the power sense

  uint8_t s = readState();
  if (s != state) {
    // Power loss/return isn't a gate movement, so it has no cause.
    bool power = s == GS_NO_POWER || state == GS_NO_POWER;
    state = s;
    bool ours = havePulsed && !elapsed(now, lastPulseAt, (uint32_t)cfg.travel_timeout_s * 1000);
    cause = power ? CAUSE_NONE : ours ? CAUSE_LORA : CAUSE_EXTERNAL;
    if (target != GS_UNKNOWN && state == target) {
      lastResult = TR_REACHED;
      target = GS_UNKNOWN;
    }
    logEvent(EV_GATE_STATE, state, cause);
    sendStatus(now);
  } else if (spareChanged) {
    sendStatus(now);
  }

  // New house session (house rebooted): its command ids restart, so forget the last one.
  if (linkStats().sessions != seenSessions) {
    seenSessions = linkStats().sessions;
    haveCmd = false;
  }

  if (target != GS_UNKNOWN && elapsed(now, targetSince, (uint32_t)cfg.travel_timeout_s * 1000)) {
    logEvent(EV_TRAVEL_TIMEOUT, target);
    lastResult = TR_TIMEOUT;
    target = GS_UNKNOWN;
    sendStatus(now);
  }

  if (elapsed(now, lastStatusAt, (uint32_t)cfg.heartbeat_s * 1000)) sendStatus(now);
}

static void handleCmd(const RxMsg &m, uint32_t now) {
  if (m.len < 3) {
    linkAck(m.seq, RES_BAD);
    return;
  }
  uint16_t id = getU16(m.payload);
  uint8_t action = m.payload[2];
  if (haveCmd && id == lastCmdId) {
    logEvent(EV_CMD_DUP, id);
    linkAck(m.seq, lastCmdAck);
    return;
  }
  if (action != ACT_OPEN && action != ACT_CLOSE) {
    linkAck(m.seq, RES_BAD);
    return;
  }
  logEvent(EV_CMD_RX, action, id);
  haveCmd = true;
  lastCmdId = id;
  uint8_t want = action == ACT_OPEN ? GS_OPEN : GS_CLOSED;
  if (state == GS_NO_POWER) {
    logEvent(EV_CMD_REFUSED, action, id);
    lastCmdAck = RES_NO_POWER;
  } else if (state == want) {
    lastResult = TR_ALREADY;
    lastCmdAck = RES_ALREADY;
  } else {
    if (action == ACT_OPEN) pulse(k1, k2, 1, now);
    else pulse(k2, k1, 2, now);
    target = want;
    targetSince = now;
    lastResult = TR_NONE;
    lastCmdAck = RES_OK;
  }
  linkAck(m.seq, lastCmdAck);
  sendStatus(now);
}

static void handleCfgSet(const RxMsg &m) {
  if (m.len < 5) {
    linkAck(m.seq, RES_BAD);
    return;
  }
  const ParamDef *p = paramById(m.payload[0]);
  int32_t v = (int32_t)getU32(m.payload + 1);
  bool ok = p && (p->flags & P_REMOTE) && paramSet(p, v);
  if (ok) {
    configSave();
    logEvent(EV_CFG_REMOTE, p->id, v);
  }
  linkAck(m.seq, ok ? RES_OK : RES_BAD);
}

static void sendDiag() {
  // fw(3) uptime(4) tx rx macFail replay retries giveups (u16 x6) then (id u8, value i32) per remote param
  uint8_t p[100];
  uint8_t n = 0;
  int maj = 0, min = 0, pat = 0;
  sscanf(FW_VERSION, "%d.%d.%d", &maj, &min, &pat);
  p[n++] = maj;
  p[n++] = min;
  p[n++] = pat;
  putU32(p + n, millis() / 1000);
  n += 4;
  const LinkStats &st = linkStats();
  uint32_t counters[6] = { st.tx, st.rx, st.macFail, st.replay, st.retries, st.giveups };
  for (uint32_t c : counters) {
    putU16(p + n, c > 0xFFFF ? 0xFFFF : c);
    n += 2;
  }
  for (size_t i = 0; i < PARAM_COUNT && n + 5u <= sizeof(p); i++) {
    if (!(PARAMS[i].flags & P_REMOTE)) continue;
    p[n++] = PARAMS[i].id;
    putU32(p + n, (uint32_t)(cfg.*(PARAMS[i].field)));
    n += 4;
  }
  linkSend(MSG_DIAG, p, n);
}

void gateOnRx(const RxMsg &m) {
  uint32_t now = millis();
  houseRssi = m.rssi;
  houseSnr = (int8_t)m.snr;
  switch (m.type) {
    case MSG_CMD: handleCmd(m, now); break;
    case MSG_CFG_SET: handleCfgSet(m); break;
    case MSG_DIAG_REQ: sendDiag(); break;
    default: break;
  }
}

void gateOnAck(Slot, uint8_t, bool, uint8_t) {}

void gateStatus(JsonObject o) {
  o["gate"] = gateStateName(state);
  o["cause"] = causeName(cause);
  o["last_result"] = resultName(lastResult);
  o["target"] = target == GS_UNKNOWN ? "" : gateStateName(target);
  o["last_cmd_id"] = lastCmdId;
  o["power_sense"] = (bool)cfg.power_sense;
}

void gateRelayTest(uint8_t k, uint32_t ms) {
  uint32_t now = millis();
  Relay &r = k == 1 ? k1 : k2;
  Relay &other = k == 1 ? k2 : k1;
  other.set(false);
  r.pulse(now, ms);
  lastPulseAt = now;
  havePulsed = true;
  logEvent(EV_PULSE, k, ms);
}
