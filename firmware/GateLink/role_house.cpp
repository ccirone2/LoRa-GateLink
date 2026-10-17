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
static uint16_t gateTravel = 0;  // gate travel_timeout_s, from its STATUS (0 = none yet: use ours)
static uint16_t gateRetries = 0, gateGiveups = 0, gateCrc = 0;
static int8_t gateNoise = 0;  // dBm, 0 = no sample
static bool haveStatus = false;
// The last STATUS taken: its gate session and seq (only later ones count), and when (the link's liveness).
static uint32_t statusSession = 0, statusSeq = 0, statusAt = 0;
// The gate's clock (ST_CLOCK), which commands carry their deadline in, as of a STATUS taken from it: the reading and
// when it was taken here (millis() in the handler, so it can be a little later than the loop's `now`: compared
// signed). Only for the gate session it came from (clockSession; 0 = none): the clock starts over when the gate boots.
static uint32_t clockSession = 0, clockAt = 0;
static uint64_t clockGate = 0;
// A STATUS resent (its first frames lost) carries the clock of when it was built, so it reads behind, by seconds. A
// reading replaces the one in use if it puts the gate's clock later, or earlier by no more than the crystals can have
// drifted apart since (CLOCK_DRIFT: 100 ppm), so this board's crystal running a little fast against the gate's is
// followed while a resent STATUS isn't taken. No reading for CLOCK_MAX_AGE_MS (a dead link: the gate reports every
// heartbeat_s, an hour at most) and the clock isn't used: the crystals may have drifted apart by most of the slack.
#define CLOCK_DRIFT 10000  // 1/100 ppm
#define CLOCK_MAX_AGE_MS 10800000UL
static bool clockWait = false;  // the pending command is waiting for the gate's clock (sendCommand)
// The gate's count of departures from closed (ST_LEFT_CLOSED) in its latest STATUS, and what it was when the contact
// sensor last went closed (applyOutputs): a count past that with the sensor still closed is a move the alarm never saw.
static uint8_t gateLeft = 0, leftWhenClosed = 0;
static bool sensorClosed = false;  // as applyOutputs last decided
// A move the house never heard (its reports jammed or lost) shows on the contact sensor afterwards: open this long.
#define MISSED_OPEN_MS 5000
static uint32_t missedUntil = 0;  // the sensor shows a missed move until then (0 = it doesn't)
static uint8_t lastEnd = GS_UNKNOWN;  // last limit reached (OPEN/CLOSED), held while BETWEEN
static uint32_t betweenSince = 0;

static bool linkUp = false;
static bool armed = false;      // user commands accepted
static uint32_t armAt = 0;      // arm once this time passes (0 = not scheduled)
static bool everArmed = false;  // armed at least once since boot (houseDecided; read by health.cpp only)
static bool shellyLevel = false;
static bool syncActive = false;
static uint32_t syncUntil = 0;
static bool syncExpect = false;
static uint32_t settleUntil = 0;  // no early end of the sync window before this (boot, controller power return, resync)
static bool resyncing = false;
static uint32_t resyncUntil = 0;
static uint32_t mismatchSince = 0;
static bool checkSoon = false;  // boot, power return, command refused or overridden: resync at once if out of step
static bool k1WasPulsing = false;
static bool ctrlPower = true;
static bool senseDark = false;   // IN2 off for CTRL_SENSE_DARK_MS while the supply it shares is good (health only)
static uint32_t senseLitAt = 0;  // the last pass that didn't see that
static uint8_t pendingAction = 0;  // user edge waiting out ctrl_confirm_ms (CLOSE only, see below) or the gate's clock
static uint32_t pendingAt = 0;

static uint16_t cmdId = 0;
static uint8_t cmdAction = 0;
static int cmdResult = -1;  // last ACK result, -2 = gave up, -1 = none
static bool cmdUnreported = false;  // the last command ACKed ok, but no STATUS since: its target isn't known here yet

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

uint32_t houseStatusAt() {
  return statusAt;
}

void houseLinkRestarted() {
  statusAt = 0;  // nothing heard on this link yet; the gate reports as soon as it has verified our new session
}

bool houseDecided() {
  return everArmed;
}

uint8_t houseGateState() {
  return haveStatus ? gateState : (uint8_t)GS_UNKNOWN;
}

bool houseGateAcLost() {
  return haveStatus && (gateInputs & STI_AC_LOST);
}

bool houseGateRebootLoop() {
  return haveStatus && (gateInputs & STI_REBOOTS);
}

bool houseCtrlSenseDark() {
  return senseDark;
}

// The gate's clock now, as far as its STATUS tells: false with no reading from its current session (it just restarted,
// or its link did, and hasn't reported yet), or none for CLOCK_MAX_AGE_MS (capStamps).
static bool gateClockNow(uint32_t now, uint64_t &clock) {
  if (!clockSession || clockSession != linkPeerSession()) return false;
  clock = clockGate + (int64_t)(int32_t)(now - clockAt);  // under CLOCK_MAX_AGE_MS old: signed is safe
  return true;
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
  // Contact sensor: closed only when we positively know the gate is closed (and not showing a missed move).
  if (missedUntil && elapsed(now, missedUntil, 0)) missedUntil = 0;
  bool closed = gateState == GS_CLOSED && !missedUntil;
  if (!linkUp && cfg.linkloss_open) closed = false;
  if (closed && !sensorClosed) leftWhenClosed = gateLeft;
  sensorClosed = closed;
  bool k2on = closed != (bool)cfg.sensor_invert;
  if (!k2.pulsing() && k2.on() != k2on) k2.set(k2on);

  if (cfg.ctrl_sync && haveStatus && !resyncing && !k1.pulsing()) {
    driveK1(now, k1Target(now));
  }
}

// False while the command waits for the gate's clock (below): call it again next pass.
static bool sendCommand(uint8_t action, uint32_t now) {
  uint8_t want = action == ACT_OPEN ? GS_OPEN : GS_CLOSED;
  // Don't suppress while an opposite command is queued, or ACKed but not yet reported in a STATUS, or the gate is
  // still heading the other way (switch flipped off and straight back on before the gate left its limit).
  bool opposing = ((linkPending(SLOT_CMD) || cmdUnreported) && cmdAction != action)
                  || (gateTarget != GS_UNKNOWN && gateTarget != want);
  if (linkUp && gateState == want && !opposing) {
    logEvent(EV_CMD_SUPPRESSED, action, gateState);
    return true;
  }
  // Without the gate's clock for its session there is nothing to date the command in, and one it couldn't date could be
  // played back to it later: the gate restarted and hasn't reported yet (BOOT_SETTLE_MS, a few seconds), or a link
  // restarted and hasn't verified the gate again. So it waits, for up to cmd_ttl_s from the switch, and loses that
  // much of its lifetime; never dated by then, it is dropped like one never ACKed, and the controller put back.
  uint32_t ttl = (uint32_t)cfg.cmd_ttl_s * 1000;
  uint64_t clock;
  bool dated = gateClockNow(now, clock);
  if (clockWait || !dated) {
    uint32_t waited = now - pendingAt;
    ttl = waited < ttl ? ttl - waited : 0;
  }
  if (!dated && ttl) {
    if (!clockWait) logEvent(EV_CMD_HOLD, 3, (int32_t)linkPeerSession());
    clockWait = true;
    return false;
  }
  clockWait = false;
  cmdId++;
  mismatchSince = 0;
  if (!ttl) {
    cmdResult = -2;
    checkSoon = true;
    logEvent(EV_CMD_DROPPED, action, cmdId);
    return true;
  }
  cmdAction = action;
  cmdResult = -1;
  cmdUnreported = false;
  uint8_t p[CMD_LEN];
  putU16(p, cmdId);
  p[2] = action;
  putU48(p + CMD_DEADLINE, clock + ttl);  // the gate refuses it after that, as this board gives it up
  linkSendReliable(SLOT_CMD, MSG_CMD, p, CMD_LEN, ttl);
  checkSoon = false;  // a boot's or power return's check is for the old level, not the user's new one
  logEvent(EV_CMD_SENT, action, cmdId);
  return true;
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
  senseDark = false;
  senseLitAt = millis();
  ctrlPower = ctrlPowered();
  checkSoon = true;
  uint32_t now = millis();
  // A shared supply may have just powered up the Shelly too: let it settle to K1 first.
  settleUntil = now;
  openSettleWindow(now);
  applyOutputs(now);
}

// IN2 and the board's supply share the controller's 12 V, so IN2 off long after the supply came back good is a dead
// opto or a cut wire, not a power cut (the opto drops ~1.9 s after the supply in a cut, and is back sooner on its return). It
// only tells health.cpp (a dead IN2 otherwise just reads as a controller without power, its switch ignored, and
// nothing alarms); the controller still counts as unpowered. senseLitAt is restamped every pass it doesn't hold, and
// read only until senseDark is set, so it never ages past elapsed()'s range.
static void updateSenseDark(uint32_t now) {
  bool dark = cfg.ctrl_power_sense && cfg.ctrl_power_pmic && supplyKnown() && supplyGood() && !in2.active();
  if (!dark) {
    senseDark = false;
    senseLitAt = now;
  } else if (!senseDark && elapsed(now, senseLitAt, CTRL_SENSE_DARK_MS)) {
    senseDark = true;
  }
}

static void updateCtrlPower(uint32_t now) {
  updateSenseDark(now);
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
  if (statusAt && elapsed(now, statusAt, STAMP_CAP_MS)) statusAt = (now - STAMP_CAP_MS) | 1;  // else it reads as fresh
  if (clockSession && elapsed(now, clockAt, CLOCK_MAX_AGE_MS)) clockSession = 0;
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
  if (armed) everArmed = true;
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
        clockWait = false;
      }
    }
  }
  // The relay can drop before the IN2 opto does: on the bench the controller's relay dropped ~0.46 s into a 12 V cut
  // and the opto only ~2.1 s in (the rail's capacitors keep it lit); the board's power good dropped ~0.2 s before the
  // relay. A power loss can only drop the relay, which reads as OFF: so a CLOSE is sent only once power has held for
  // ctrl_confirm_ms, while an OPEN (relay on), which no power loss can produce, goes at once.
  if (pendingAction && ((!cfg.ctrl_power_sense && !cfg.ctrl_power_pmic) || pendingAction == ACT_OPEN
                        || elapsed(now, pendingAt, cfg.ctrl_confirm_ms))) {
    if (sendCommand(pendingAction, now)) pendingAction = 0;
  }

  // Resync the Shelly to the real gate if they disagree for too long
  // (e.g. a command was overridden by the siren input, or lost while the link was down).
  if (resyncing) {
    // A K1 test pulse started meanwhile runs its length: setting K1 would cut it short.
    if (elapsed(now, resyncUntil, 0) && !k1.pulsing()) {
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
      // The window covers K1 at !t and its return, so no edge may end it before K1 is back: one at t meanwhile (the
      // user's, still in its debounce, or chatter) would, and the controller then following K1 to !t would be taken
      // as the user.
      if (elapsed(resyncUntil, settleUntil, 0)) settleUntil = resyncUntil;
      mismatchSince = 0;
    }
  }
}

static void handleStatus(const RxMsg &m, uint32_t now) {
  if (m.len < ST_LEN_V4) {  // every gate that can verify our link (frame format 2, 0.16.0 on) sends this much
    linkAck(m.seq, RES_BAD);
    return;
  }
  linkAck(m.seq, RES_OK);
  // Only a STATUS later than the last one taken, in this gate session. The link takes any frame it hasn't seen inside
  // its window, also one kept from us and delivered late (an old "closed", recorded while it was jammed here, and
  // played back once the gate has opened): that one tells nothing new, and it mustn't keep the link up either.
  uint32_t session = linkPeerSession();
  if (session == statusSession && m.seq <= statusSeq) {
    logEvent(EV_REPLAY, (int32_t)m.seq, (int32_t)statusSeq);
    return;
  }
  bool sameSession = session == statusSession;
  statusSession = session;
  statusSeq = m.seq;
  statusAt = now | 1;
  uint64_t clock = getU48(m.payload + ST_CLOCK), held;
  if (!gateClockNow(now, held) || (int64_t)(clock - held) >= -(int64_t)((uint32_t)(now - clockAt) / CLOCK_DRIFT)) {
    clockSession = session;
    clockGate = clock;
    clockAt = now;
  }
  cmdUnreported = false;
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
  gateTravel = getU16(m.payload + ST_TRAVEL);
  PeerReport r = { gateRssi, gateSnr, 0, 0, 0, 0, 0 };
  r.retries = gateRetries = getU16(m.payload + ST_RETRIES);
  r.giveups = gateGiveups = getU16(m.payload + ST_GIVEUPS);
  r.crcErr = gateCrc = getU16(m.payload + ST_CRC);
  r.noiseAvg = gateNoise = (int8_t)m.payload[ST_NOISE];
  r.noiseMax = (int8_t)m.payload[ST_NOISE_MAX];
  histPeer(r);
  // The gate left its closed limit (and came back) since the contact sensor went closed, which it still is: the move's
  // reports were jammed or lost, and the alarm never saw the gate open. Open the sensor for a moment now, so the alarm
  // records the move late rather than never. Not when it was open meanwhile (a link loss): the alarm saw that. (A gate
  // before 0.17.0 doesn't count; a reboot starts the count over, which says nothing about a move.)
  if (m.len > ST_LEFT_CLOSED) {
    uint8_t left = m.payload[ST_LEFT_CLOSED];
    if (!sameSession) {
      leftWhenClosed = left;
    } else if (left != leftWhenClosed && gateState == GS_CLOSED && sensorClosed) {
      logEvent(EV_MISSED_MOVE, (uint8_t)(left - leftWhenClosed));
      missedUntil = (now + MISSED_OPEN_MS) | 1;
    }
    gateLeft = left;
  }
  bool first = !haveStatus;
  haveStatus = true;

  if (gateState != prevState) logEvent(EV_GATE_STATE, gateState, gateCause);
  if (gateState == GS_OPEN || gateState == GS_CLOSED) lastEnd = gateState;
  else if (gateState != GS_BETWEEN) lastEnd = GS_UNKNOWN;  // fault/no power (no AC, no limit): show not-closed
  if (gateState == GS_BETWEEN && prevState != GS_BETWEEN) betweenSince = now;
  // Command overridden (e.g. siren holding the gate open): resync the Shelly right away. A mismatch held off through
  // the travel (holdingTravel()) never started: check it as soon as nothing is in the way.
  if (gateResult == TR_TIMEOUT && prevResult != TR_TIMEOUT) {
    if (mismatchSince) mismatchSince = (now - (uint32_t)cfg.mismatch_timeout_s * 1000) | 1;
    else checkSoon = true;
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
    // Refused without a pulse (no AC at the gate, or past its deadline there): the Shelly shows a move that won't
    // happen. Put it back as soon as nothing else is in the way, as for an overridden command, instead of after
    // mismatch_timeout_s.
    if (acked && (result == RES_NO_POWER || result == RES_LATE)) checkSoon = true;
    // Pulsed: until its STATUS says where the gate is heading, an opposite command must not be suppressed.
    if (acked && result == RES_OK) cmdUnreported = true;
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
  g["open_limit"] = (bool)(gateInputs & STI_IN1);
  g["close_limit"] = (bool)(gateInputs & STI_IN2);
  g["k1"] = (bool)(gateInputs & STI_K1);
  g["k2"] = (bool)(gateInputs & STI_K2);
  g["in3"] = (bool)(gateInputs & STI_IN3);
  g["in4"] = (bool)(gateInputs & STI_IN4);
  g["ac_power"] = !(gateInputs & STI_AC_LOST);
  g["reboot_loop"] = (bool)(gateInputs & STI_REBOOTS);
  if (haveStatus) {
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
