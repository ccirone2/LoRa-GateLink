// Gate node: pulses the CSW24UL OPEN/CLOSE inputs and reports gate position.
//
// The opener's inputs are shared with other controllers (AES Prime Edge, siren
// sensor), so outputs are only ever pulsed and gate state always comes from the
// limit inputs, never from our own last command.
#include "roles.h"
#include "config.h"
#include "log.h"
#include "radio.h"
#include "history.h"

#define STATUS_TTL_MS 10000
// After boot, the first STATUS waits until the inputs have been steady this long (at most BOOT_SETTLE_MAX_MS): the
// opener may be restarting with us (a power blip at the gate) and its limits come back after we do. Reporting them
// early made the house show not-closed for a moment: the contact sensor opening, K1 flicking.
#define BOOT_SETTLE_MS 3000
#define BOOT_SETTLE_MAX_MS 10000

static uint8_t state = GS_UNKNOWN;
static uint8_t cause = CAUSE_NONE;
static uint8_t lastResult = TR_NONE;
static uint8_t target = GS_UNKNOWN;  // state we're waiting to reach after our pulse
static uint8_t leaving = GS_UNKNOWN;  // limit our pulse is moving the gate off (it may be the target after a reversal)
static uint32_t targetSince = 0;
static bool haveCmd = false;
static uint16_t lastCmdId = 0;
static uint8_t lastCmdAck = RES_OK;
static uint32_t lastStatusAt = 0;
static int32_t reportedHeartbeat = 0;  // heartbeat_s in our last STATUS
static uint32_t seenSessions = 0;
static bool settling = true;  // after boot: tracking the inputs silently, see BOOT_SETTLE_MS
static uint32_t bootAt = 0, steadySince = 0;
static bool rebootAfterCmd = false;  // debug: next pulsed command reboots us before its ACK (a power cut)
static uint32_t rebootAt = 0;

// IN3 = AC power (the 24 V supply on mains). The opener has battery backup, so it keeps running without AC.
static bool acPower() {
  return !cfg.power_sense || in3.active();
}

static uint8_t readState() {
  bool open = in1.active(), closed = in2.active();
  if (open && closed) return GS_FAULT;
  if (closed) return GS_CLOSED;
  if (open) return GS_OPEN;
  // Without AC a limit that reads can still be trusted (the opener runs on its battery), but none reading may
  // mean the opener's battery is dead too (its AUX limit relays drop), not a gate between limits: unknown.
  if (!acPower()) return GS_NO_POWER;
  return GS_BETWEEN;
}

static void sendStatus(uint32_t now) {
  if (settling) return;  // the first one goes out once the inputs have settled
  uint8_t p[ST_LEN];
  p[ST_STATE] = state;
  p[ST_INPUTS] = in1.active() | (in2.active() << 1) | (k1.on() << 2) | (k2.on() << 3)
                 | (in3.active() << 4) | (in4.active() << 5) | (!acPower() << 6);
  p[ST_CAUSE] = cause;
  p[ST_RESULT] = lastResult;
  putU16(p + ST_CMD_ID, lastCmdId);
  putU32(p + ST_UPTIME, now / 1000);
  // Every authenticated frame from the house counts, ACKs included (most of what it sends).
  const LinkStats &st = linkStats();
  putU16(p + ST_RSSI, st.lastRxAt ? (uint16_t)st.lastRssi : 0);
  p[ST_SNR] = st.lastRxAt ? (uint8_t)(int8_t)st.lastSnr : 0;
  p[ST_TARGET] = target;
  putU16(p + ST_HEARTBEAT, cfg.heartbeat_s);
  putU16(p + ST_RETRIES, (uint16_t)st.retries);
  putU16(p + ST_GIVEUPS, (uint16_t)st.giveups);
  putU16(p + ST_CRC, (uint16_t)radioCrcErrors());
  int8_t noise, noiseMax;
  histNoiseTake(noise, noiseMax);
  p[ST_NOISE] = (uint8_t)noise;
  p[ST_NOISE_MAX] = (uint8_t)noiseMax;
  reportedHeartbeat = cfg.heartbeat_s;
  // Retries spread over the TTL: cap it so a lost status is retried within a fraction of a second even with a
  // long heartbeat (the next heartbeat supersedes it anyway).
  uint32_t ttl = (uint32_t)cfg.heartbeat_s * 1000;
  linkSendReliable(SLOT_STATUS, MSG_STATUS, p, ST_LEN, ttl < STATUS_TTL_MS ? ttl : STATUS_TTL_MS);
  lastStatusAt = now;
}

// Our pulse should take the gate to `want`: track it, so the movement it causes is attributed to us.
static void setTarget(uint8_t want, uint32_t now) {
  // When reversing before the gate left its limit, the earlier pulse is still what moves it off that limit.
  if (state != want && (state == GS_OPEN || state == GS_CLOSED)) leaving = state;
  target = want;
  targetSince = now;
  lastResult = TR_NONE;
}

static void clearTarget() {
  target = GS_UNKNOWN;
  leaving = GS_UNKNOWN;
}

// Interlock: never both. Releasing one relay and energizing the other in the same instant isn't enough (a contact
// can make before the other has broken: the bench opener saw OPEN and CLOSE together), so wait for the release.
#define INTERLOCK_MS 100

static void interlockedPulse(Relay &r, Relay &other, uint32_t now, uint32_t ms) {
  bool wasOn = other.on() || other.pulsing();
  other.set(false);
  r.pulse(now, ms, wasOn ? INTERLOCK_MS : 0);
}

static void pulse(Relay &r, Relay &other, uint8_t which, uint32_t now) {
  interlockedPulse(r, other, now, cfg.pulse_ms);
  logEvent(EV_PULSE, which, cfg.pulse_ms);
}

void gateBegin() {
  state = readState();
  cause = CAUSE_NONE;
  settling = true;
  bootAt = steadySince = millis();
}

void gateLoop(uint32_t now) {
  if (rebootAt && elapsed(now, rebootAt, 0)) NVIC_SystemReset();
  in1.update(now, cfg.debounce_ms, cfg.in1_invert);
  in2.update(now, cfg.debounce_ms, cfg.in2_invert);
  bool spareChanged = updateSpareInputs(now);  // before readState(): IN3 is the power sense

  uint8_t s = readState();
  if (settling) {
    // Follow the inputs without reporting (no cause: nothing is attributed to a boot), until they hold still.
    if (s != state) {
      state = s;
      steadySince = now;
    }
    if (elapsed(now, steadySince, BOOT_SETTLE_MS) || elapsed(now, bootAt, BOOT_SETTLE_MAX_MS)) {
      settling = false;
      sendStatus(now);
    }
  } else if (s != state) {
    // Into or out of no_power there's no telling a movement from the power changing (on battery the gate may
    // have moved while no limit read; on AC return the limits and IN3 settle in either order): no cause.
    bool power = s == GS_NO_POWER || state == GS_NO_POWER;
    uint8_t prev = state;
    state = s;
    // Ours only while our pulse is still heading somewhere and the gate moves that way: reaching the target,
    // or leaving a limit toward it (or the limit our pulse is moving it off). Anything else (a local button,
    // the other controllers, an override to the far limit) is external.
    bool ours = target != GS_UNKNOWN
                && (s == target || (s == GS_BETWEEN && (prev != target || prev == leaving)));
    cause = power ? CAUSE_NONE : ours ? CAUSE_LORA : CAUSE_EXTERNAL;
    if (s == GS_BETWEEN) leaving = GS_UNKNOWN;
    if (target != GS_UNKNOWN && state == target) {
      lastResult = TR_REACHED;
      clearTarget();
    } else if (target != GS_UNKNOWN && (state == GS_OPEN || state == GS_CLOSED)) {
      // Overridden to the other limit (siren, AES, local button): our command is over, and reporting it as
      // a timeout makes the house resync its controller at once.
      lastResult = TR_TIMEOUT;
      clearTarget();
    }
    logEvent(EV_GATE_STATE, state, cause);
    sendStatus(now);
  } else if (spareChanged) {
    sendStatus(now);
  }

  // New house session (house rebooted): its command ids restart, so forget the last one, and
  // report now so the house restores K1/K2 without waiting for the next heartbeat.
  if (linkStats().sessions != seenSessions) {
    seenSessions = linkStats().sessions;
    haveCmd = false;
    sendStatus(now);
  }

  if (target != GS_UNKNOWN && elapsed(now, targetSince, (uint32_t)cfg.travel_timeout_s * 1000)) {
    // A reversal pulsed before the gate left its limit never sees a state change: it's there already.
    if (state == target) {
      lastResult = TR_REACHED;
    } else {
      logEvent(EV_TRAVEL_TIMEOUT, target);
      lastResult = TR_TIMEOUT;
    }
    clearTarget();
    sendStatus(now);
  }

  // A new heartbeat_s goes out at once: the house sizes its link timeout from it, and waiting for the next
  // (longer) heartbeat would let its old timeout expire first.
  if (elapsed(now, lastStatusAt, (uint32_t)cfg.heartbeat_s * 1000) || cfg.heartbeat_s != reportedHeartbeat) {
    sendStatus(now);
  }
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
  // Still at this limit, but our last pulse is heading for the other one: pulse to reverse it.
  bool reversing = target != GS_UNKNOWN && target != want;
  // Without AC the opener may be on its last battery reserve, and the AES controller sharing the inputs is
  // unpowered: refuse, even with a limit reading.
  if (!acPower()) {
    logEvent(EV_CMD_REFUSED, action, id);
    lastCmdAck = RES_NO_POWER;
  } else if (state == want && !reversing) {
    lastResult = TR_ALREADY;
    lastCmdAck = RES_ALREADY;
  } else {
    if (action == ACT_OPEN) pulse(k1, k2, 1, now);
    else pulse(k2, k1, 2, now);
    setTarget(want, now);
    lastCmdAck = RES_OK;
    if (rebootAfterCmd) {
      // Bench fault injection: the pulse runs, then we reset without ACKing, as a power cut right after it would.
      rebootAfterCmd = false;
      rebootAt = (now + cfg.pulse_ms + 100) | 1;
      return;
    }
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
  if (!p || !(p->flags & P_REMOTE) || !paramSet(p, v)) {
    linkAck(m.seq, RES_BAD);
    return;
  }
  bool saved = configSaveParam(p);  // not configSave(): that would also persist unsaved console edits
  logEvent(EV_CFG_REMOTE, p->id, v);
  linkAck(m.seq, saved ? RES_OK : RES_NOT_SAVED);
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
  o["ac_power"] = acPower();
  o["settling"] = settling;
}

void gateDebugRebootAfterCmd() {
  rebootAfterCmd = true;
}

void gateRelayTest(uint8_t k, uint32_t ms) {
  uint32_t now = millis();
  Relay &r = k == 1 ? k1 : k2;
  Relay &other = k == 1 ? k2 : k1;
  interlockedPulse(r, other, now, ms);
  // Track it like a command so the resulting movement is attributed to us, not external. A gate already at
  // that limit won't move for it, so there's nothing to attribute (a later move off it is someone else's).
  // With no AC and no limit reading the opener may be dead: the pulse is only a wiring check, and the limit read
  // when power returns isn't ours.
  uint8_t want = k == 1 ? GS_OPEN : GS_CLOSED;
  if (state != want && state != GS_NO_POWER) setTarget(want, now);
  logEvent(EV_PULSE, k, ms);
}
