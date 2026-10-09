// House node: bridges the Shelly Wave 1 (Alarm.com) to the gate over LoRa.
//
//  IN1 = Shelly relay contact ("switch state"). User edges become OPEN/CLOSE commands.
//  K1  = Shelly SW input. Energized while the gate is not closed, so the Shelly (and
//        the Alarm.com switch) follows the real gate even when another controller
//        moved it. While the gate travels (BETWEEN) K1 holds its level until the far
//        limit is reached, falling back to open only if it stays BETWEEN longer than
//        the gate's travel_timeout_s (from its STATUS). Edges on IN1 caused by K1 fall inside a sync window and are ignored.
//  K2  = wireless contact sensor. Energized (closed loop) only when the gate is closed.
//  IN2 = Shelly supply, via a PNP opto (ctrl_power_sense). The Shelly's relay drops when it loses
//        power and comes back at the K1 level when it boots; neither edge may become a command.
//        The opto is slow (~2.1 s into a 12 V cut, never on a 300 ms dip), so the board's own supply,
//        which shares the 12 V through its buck, counts too (ctrl_power_pmic, supply.h): it drops first.
#include "roles.h"
#include "config.h"
#include "log.h"
#include "console.h"
#include "radio.h"
#include "history.h"
#include "app.h"
#include "supply.h"

static uint8_t gateState = GS_UNKNOWN;
static uint8_t gateInputs = 0;
static uint8_t gateCause = CAUSE_NONE;
static uint8_t gateResult = TR_NONE;
static uint8_t gateTarget = GS_UNKNOWN;
static uint32_t gateUptime = 0;
static int16_t gateRssi = 0;  // RSSI measured at the gate
static int8_t gateSnr = 0;
static uint16_t gateHeartbeat = 0;  // gate heartbeat_s, from its STATUS
static uint16_t gateTravel = 0;  // gate travel_timeout_s, from its STATUS (0 = gate before 0.13.0: use ours)
static bool gateExt = false;  // its STATUS carries the link counters and noise (0.4.0 on)
static uint16_t gateRetries = 0, gateGiveups = 0, gateCrc = 0;
static int8_t gateNoise = 0;  // dBm, 0 = no sample
static bool haveStatus = false;
static uint8_t lastEnd = GS_UNKNOWN;  // last limit reached (OPEN/CLOSED), held while BETWEEN
static uint32_t betweenSince = 0;

static bool linkUp = false;
static bool armed = false;      // user commands accepted
static uint32_t armAt = 0;      // arm once this time passes (0 = not scheduled)
static bool shellyLevel = false;
static bool syncActive = false;
static uint32_t syncUntil = 0;
static bool syncExpect = false;
static uint32_t settleUntil = 0;  // boot / controller power return: no early end of the sync window before this
static bool resyncing = false;
static uint32_t resyncUntil = 0;
static uint32_t mismatchSince = 0;
static bool checkSoon = false;  // boot, controller power return, command refused: resync at once if out of step
static bool k1WasPulsing = false;
static bool ctrlPower = true;
static uint8_t pendingAction = 0;  // user edge waiting out ctrl_confirm_ms (CLOSE only, see below)
static uint32_t pendingAt = 0;

static uint16_t cmdId = 0;
static uint8_t cmdAction = 0;
static int cmdResult = -1;  // last ACK result, -2 = gave up, -1 = none

// The gate's travel_timeout_s, which only it can have set right (it's the remote-writable one): a hold shorter
// than the gate's flips the controller to not-closed in the middle of a slow but normal travel.
static uint32_t travelTimeoutMs() {
  return (uint32_t)(gateTravel ? gateTravel : cfg.travel_timeout_s) * 1000;
}

// Gate in travel from a known limit: K1 keeps showing where it started.
static bool holdingTravel(uint32_t now) {
  return gateState == GS_BETWEEN && lastEnd != GS_UNKNOWN && !elapsed(now, betweenSince, travelTimeoutMs());
}

// The gate only reports every heartbeat_s, so the link timeout must cover a few of them whatever this board's
// link_timeout_s says (only the gate's heartbeat_s matters, and it can be changed remotely).
uint32_t houseLinkTimeoutMs() {
  uint32_t ms = (uint32_t)cfg.link_timeout_s * 1000;
  uint32_t hb = (uint32_t)gateHeartbeat * 2500;
  return hb > ms ? hb : ms;
}

static bool k1Target(uint32_t now) {
  if (holdingTravel(now)) return lastEnd != GS_CLOSED;
  return gateState != GS_CLOSED;
}

// Never shortens an open window (e.g. the settle window after the Shelly powers up).
static void openSyncWindow(uint32_t now, bool expect, uint32_t extraMs) {
  uint32_t until = now + extraMs + cfg.sync_window_ms;
  if (!syncActive || (int32_t)(until - syncUntil) > 0) syncUntil = until;
  syncActive = true;
  syncExpect = expect;
}

// After boot or controller power return the Shelly may restore its own state or chatter, so the window
// lasts at least ctrl_settle_ms even if it shows the K1 level early.
static void openSettleWindow(uint32_t now) {
  openSyncWindow(now, k1.on(), cfg.ctrl_settle_ms);
  uint32_t until = now + cfg.ctrl_settle_ms;
  if (elapsed(until, settleUntil, 0)) settleUntil = until;
}

// Every K1 change opens a sync window, even when the Shelly should already be at that level:
// a Shelly misconfigured to toggle on SW edges flips anyway, and that flip must not become a command.
static void driveK1(uint32_t now, bool level) {
  if (k1.on() == level) return;
  k1.set(level);
  openSyncWindow(now, level, 0);
}

static void applyOutputs(uint32_t now) {
  // Contact sensor: closed only when we positively know the gate is closed.
  bool closed = gateState == GS_CLOSED;
  if (!linkUp && cfg.linkloss_open) closed = false;
  bool k2on = closed != (bool)cfg.sensor_invert;
  if (!k2.pulsing() && k2.on() != k2on) k2.set(k2on);

  if (cfg.ctrl_sync && haveStatus && !resyncing && !k1.pulsing()) {
    driveK1(now, k1Target(now));
  }
}

static void sendCommand(uint8_t action) {
  uint8_t want = action == ACT_OPEN ? GS_OPEN : GS_CLOSED;
  // Don't suppress while an opposite command is queued or the gate is still heading the other way
  // (switch flipped off and straight back on before the gate left its limit).
  bool opposing = (linkPending(SLOT_CMD) && cmdAction != action) || (gateTarget != GS_UNKNOWN && gateTarget != want);
  if (linkUp && gateState == want && !opposing) {
    logEvent(EV_CMD_SUPPRESSED, action, gateState);
    return;
  }
  cmdId++;
  cmdAction = action;
  cmdResult = -1;
  uint8_t p[3];
  putU16(p, cmdId);
  p[2] = action;
  linkSendReliable(SLOT_CMD, MSG_CMD, p, 3, (uint32_t)cfg.cmd_ttl_s * 1000);
  mismatchSince = 0;
  logEvent(EV_CMD_SENT, action, cmdId);
}

// Controller powered: IN2 on (ctrl_power_sense) and the board's supply good (ctrl_power_pmic).
static bool ctrlPowered() {
  return (!cfg.ctrl_power_sense || in2.active()) && (!cfg.ctrl_power_pmic || supplyGood());
}

void houseBegin() {
  // Random start so the gate's duplicate-command check can't match an id from before a reboot.
  cmdId = (uint16_t)radioRandom32();
  shellyLevel = in1.active();
  armed = false;
  armAt = 0;
  ctrlPower = ctrlPowered();
  checkSoon = true;
  uint32_t now = millis();
  // A shared supply may have just powered up the Shelly too: let it settle to K1 first.
  settleUntil = now;
  openSettleWindow(now);
  applyOutputs(now);
}

static void updateCtrlPower(uint32_t now) {
  bool p = ctrlPowered();
  if (p == ctrlPower) return;
  ctrlPower = p;
  logEvent(EV_CTRL_POWER, p, p ? 0 : pendingAction);
  if (!p) pendingAction = 0;  // the edge was the relay dropping with the supply
  else {
    openSettleWindow(now);
    checkSoon = true;
  }
}

// elapsed() is signed, so a time stamp left alone for 2^31 ms (~24.8 days) reads as in the future. Keep the ones
// that can sit unused that long (no controller power changes, a dead link, a gate left between) from getting there.
#define STAMP_CAP_MS 0x40000000UL

static void capStamps(uint32_t now) {
  if (elapsed(now, settleUntil, 0)) settleUntil = now;  // passed: no longer holds a sync window open
  if (mismatchSince && elapsed(now, mismatchSince, STAMP_CAP_MS)) mismatchSince = (now - STAMP_CAP_MS) | 1;
  if (elapsed(now, betweenSince, STAMP_CAP_MS)) betweenSince = now - STAMP_CAP_MS;
}

void houseLoop(uint32_t now) {
  capStamps(now);
  bool up = appLinkUp(now);
  if (up != linkUp) {
    linkUp = up;
    logEvent(up ? EV_LINK_UP : EV_LINK_DOWN);
    applyOutputs(now);
  }

  if (!armed && armAt && elapsed(now, armAt, 0)) armed = true;
  // The end of a K1 test pulse moves the Shelly too (the pulse may outlast arming): cover it with a window.
  bool k1Pulsing = k1.pulsing();
  if (k1WasPulsing && !k1Pulsing) openSyncWindow(now, k1.on(), 0);
  k1WasPulsing = k1Pulsing;
  applyOutputs(now);  // also restores K1/K2 after a relay test pulse
  if (syncActive && elapsed(now, syncUntil, 0)) syncActive = false;

  updateSpareInputs(now);
  // IN2 is the Shelly power sense; with ctrl_power_sense off it's a spare, only logged.
  if (in2.update(now, cfg.debounce_ms, cfg.in2_invert) && !cfg.ctrl_power_sense) logEvent(EV_INPUT, 2, in2.active());
  updateCtrlPower(now);

  if (in1.update(now, cfg.debounce_ms, cfg.in1_invert)) {
    shellyLevel = in1.active();
    // Any edge inside a sync window is attributed to K1 (e.g. a Shelly that toggles on every SW edge,
    // or one still booting); if the Shelly ends up wrong, the mismatch/resync logic below corrects it.
    if (syncActive) {
      if (shellyLevel == syncExpect && elapsed(now, settleUntil, 0)) syncActive = false;
      logEvent(EV_SYNC, shellyLevel);
    } else if (!ctrlPower) {
      logEvent(EV_CTRL, shellyLevel, 1);
    } else {
      logEvent(EV_CTRL, shellyLevel);
      if (armed) {
        pendingAction = shellyLevel ? ACT_OPEN : ACT_CLOSE;
        pendingAt = now;
      }
    }
  }
  // The relay can drop before the IN2 opto does: on the bench the controller's relay dropped ~0.46 s into a 12 V cut
  // and the opto only ~2.1 s in (the rail's capacitors keep it lit); the board's power good dropped ~0.2 s before the
  // relay. A power loss can only drop the relay, which reads as OFF: so a CLOSE is sent only once power has held for
  // ctrl_confirm_ms, while an OPEN (relay on), which no power loss can produce, goes at once.
  if (pendingAction && ((!cfg.ctrl_power_sense && !cfg.ctrl_power_pmic) || pendingAction == ACT_OPEN
                        || elapsed(now, pendingAt, cfg.ctrl_confirm_ms))) {
    sendCommand(pendingAction);
    pendingAction = 0;
  }

  // Resync the Shelly to the real gate if they disagree for too long
  // (e.g. a command was overridden by the siren input, or lost while the link was down).
  if (resyncing) {
    if (elapsed(now, resyncUntil, 0)) {
      resyncing = false;
      driveK1(now, k1Target(now));
    }
  } else if (!ctrlPower) {
    mismatchSince = 0;  // an unpowered Shelly can't follow K1; start over once it's back
  } else if (cfg.ctrl_sync && armed && linkUp && haveStatus && !linkPending(SLOT_CMD)) {
    bool t = k1Target(now);
    // Mid-travel the Shelly may already show a user's new command while K1 holds the old limit.
    if (shellyLevel == t || holdingTravel(now)) {
      mismatchSince = 0;
      if (!syncActive) checkSoon = false;
    } else if (checkSoon && !syncActive && !pendingAction) {
      // Settled after a boot or power return and still wrong: K1 alone won't move it (the Shelly only
      // follows SW edges), so resync now instead of waiting out mismatch_timeout_s.
      mismatchSince = (now - (uint32_t)cfg.mismatch_timeout_s * 1000) | 1;  // 0 means "not started"
      checkSoon = false;
    } else if (mismatchSince == 0) {
      mismatchSince = now | 1;
    } else if (elapsed(now, mismatchSince, (uint32_t)cfg.mismatch_timeout_s * 1000)) {
      // Shelly only reacts to SW transitions: drive K1 to the Shelly's current level, then to the target.
      logEvent(EV_RESYNC, t);
      resyncing = true;
      resyncUntil = now + cfg.resync_ms;
      k1.set(!t);
      openSyncWindow(now, t, cfg.resync_ms);
      mismatchSince = 0;
    }
  }
}

static void handleStatus(const RxMsg &m, uint32_t now) {
  if (m.len < ST_LEN_V1) {
    linkAck(m.seq, RES_BAD);
    return;
  }
  linkAck(m.seq, RES_OK);
  uint8_t prevState = gateState;
  uint8_t prevResult = gateResult;
  gateState = m.payload[ST_STATE];
  gateInputs = m.payload[ST_INPUTS];
  gateCause = m.payload[ST_CAUSE];
  gateResult = m.payload[ST_RESULT];
  gateUptime = getU32(m.payload + ST_UPTIME);
  gateRssi = (int16_t)getU16(m.payload + ST_RSSI);
  gateSnr = (int8_t)m.payload[ST_SNR];
  gateTarget = m.payload[ST_TARGET];
  gateHeartbeat = getU16(m.payload + ST_HEARTBEAT);
  gateTravel = m.len >= ST_LEN ? getU16(m.payload + ST_TRAVEL) : 0;
  PeerReport r = { gateRssi, gateSnr, m.len >= ST_LEN_V2, 0, 0, 0, 0, 0 };
  gateExt = r.ext;
  if (r.ext) {
    r.retries = gateRetries = getU16(m.payload + ST_RETRIES);
    r.giveups = gateGiveups = getU16(m.payload + ST_GIVEUPS);
    r.crcErr = gateCrc = getU16(m.payload + ST_CRC);
    r.noiseAvg = gateNoise = (int8_t)m.payload[ST_NOISE];
    r.noiseMax = (int8_t)m.payload[ST_NOISE_MAX];
  }
  histPeer(r);
  bool first = !haveStatus;
  haveStatus = true;

  if (gateState != prevState) logEvent(EV_GATE_STATE, gateState, gateCause);
  if (gateState == GS_OPEN || gateState == GS_CLOSED) lastEnd = gateState;
  else if (gateState != GS_BETWEEN) lastEnd = GS_UNKNOWN;  // fault/no power (no AC, no limit): show not-closed
  if (gateState == GS_BETWEEN && prevState != GS_BETWEEN) betweenSince = now;
  // Command overridden (e.g. siren holding the gate open): resync the Shelly right away.
  if (gateResult == TR_TIMEOUT && prevResult != TR_TIMEOUT && mismatchSince) {
    mismatchSince = (now - (uint32_t)cfg.mismatch_timeout_s * 1000) | 1;
  }
  applyOutputs(now);
  // Accept user commands once the initial sync has settled.
  if (first) armAt = (now + cfg.sync_window_ms) | 1;
  consoleEventStatus();
}

void houseOnRx(const RxMsg &m) {
  switch (m.type) {
    case MSG_STATUS: handleStatus(m, millis()); break;
    case MSG_DIAG: consoleEventDiag(m.payload, m.len); break;
    default: break;
  }
}

void houseOnAck(Slot slot, uint8_t, bool acked, uint8_t result) {
  if (slot == SLOT_CMD) {
    cmdResult = acked ? result : -2;
    if (!acked) logEvent(EV_CMD_DROPPED, cmdAction, cmdId);
    // Refused without a pulse (no AC at the gate): the Shelly shows a move that won't happen. Put it back as soon
    // as nothing else is in the way, as for an overridden command, instead of after mismatch_timeout_s.
    if (acked && result == RES_NO_POWER) checkSoon = true;
  } else if (slot == SLOT_CFG) {
    consoleEventRemoteSet(acked, result);
  }
}

void houseStatus(JsonObject o) {
  o["gate"] = haveStatus ? gateStateName(gateState) : "unknown";
  o["cause"] = causeName(gateCause);
  o["last_result"] = resultName(gateResult);
  o["target"] = gateTarget == GS_UNKNOWN ? "" : gateStateName(gateTarget);
  o["link_up"] = linkUp;
  o["link_timeout_eff_s"] = houseLinkTimeoutMs() / 1000;
  o["armed"] = armed;
  o["ctrl"] = shellyLevel;
  o["ctrl_power"] = ctrlPower;
  o["sync_window"] = syncActive;
  o["resyncing"] = resyncing;
  o["cmd_id"] = cmdId;
  o["cmd_pending"] = linkPending(SLOT_CMD);
  o["cmd_result"] = cmdResult;
  JsonObject g = o["remote"].to<JsonObject>();
  g["uptime_s"] = gateUptime;
  g["rssi"] = gateRssi;
  g["snr"] = gateSnr;
  g["heartbeat_s"] = gateHeartbeat;
  if (gateTravel) g["travel_timeout_s"] = gateTravel;
  g["open_limit"] = (bool)(gateInputs & 1);
  g["close_limit"] = (bool)(gateInputs & 2);
  g["k1"] = (bool)(gateInputs & 4);
  g["k2"] = (bool)(gateInputs & 8);
  g["in3"] = (bool)(gateInputs & 16);
  g["in4"] = (bool)(gateInputs & 32);
  g["ac_power"] = !(gateInputs & 64);
  if (gateExt) {
    g["retries"] = gateRetries;
    g["giveups"] = gateGiveups;
    g["crc_err"] = gateCrc;
    if (gateNoise) g["noise"] = gateNoise;
    else g["noise"] = nullptr;
  }
}

void houseRelayTest(uint8_t k, uint32_t ms) {
  uint32_t now = millis();
  // A K1 test toggles the Shelly; don't turn the resulting edges into gate commands.
  if (k == 1 && haveStatus) {
    armed = false;
    armAt = (now + ms + cfg.sync_window_ms) | 1;
  }
  (k == 1 ? k1 : k2).pulse(now, ms);
  logEvent(EV_PULSE, k, ms);
}

void houseRemoteDiag() {
  linkSend(MSG_DIAG_REQ, nullptr, 0);
}

void houseRemoteSet(uint8_t id, int32_t value) {
  uint8_t p[5];
  p[0] = id;
  putU32(p + 1, (uint32_t)value);
  linkSendReliable(SLOT_CFG, MSG_CFG_SET, p, 5, 10000);
}
