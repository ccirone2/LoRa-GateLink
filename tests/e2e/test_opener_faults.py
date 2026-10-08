"""Opener-side failures, injected with the GateSim: power loss, a jammed or deaf opener, bad limit signals."""
import time

import pytest

from gatelink.bench import ACT_CLOSE, ACT_OPEN, CAUSE, GS, PROFILE_COMMON, RES_NO_POWER, SIM_TRAVEL_S

TRAVEL_TIMEOUT_S = PROFILE_COMMON["travel_timeout_s"]


def test_power_loss_at_rest(rig):
    """Opener unpowered: gate no_power, house shows not-closed, commands refused without pulsing."""
    rig.expect_commands(1)
    m = rig.mark()
    rig.sim.power(False)
    rig.wait_log("gate", "gate_state", a=GS["no_power"], b=CAUSE["none"], since=m, timeout=5)
    # Position can't be verified: contact sensor open, K1 energized, controller follows to on.
    rig.wait_house(10, gate="no_power", io__k2=False, io__k1=True)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(10, "sync window closed", sync_window=False, resyncing=False)

    m2 = rig.mark()
    rig.ctrl.off()
    rig.wait_log("house", "cmd_sent", a=ACT_CLOSE, since=m2, timeout=10)
    rig.wait_log("gate", "cmd_refused", a=ACT_CLOSE, since=m2, timeout=5)
    rig.wait_house(5, cmd_result=RES_NO_POWER)
    rig.expect_no("gate", "pulse", seconds=1, since=m)

    m3 = rig.mark()
    rig.sim.power(True)
    rig.wait_log("gate", "gate_state", a=GS["closed"], b=CAUSE["none"], since=m3, timeout=5)
    rig.wait_house(10, gate="closed", io__k2=True)
    rig.wait_ctrl(False, timeout=45)
    rig.wait_house(10, io__k1=False, resyncing=False)


def test_ac_loss_limits_trusted(rig):
    """AC lost, opener on battery: the closed limit is still trusted (house unchanged), commands are refused and
    the controller is put back; AC return changes nothing."""
    rig.expect_commands(1)
    m = rig.mark()
    rig.sim.ac(False)
    rig.wait_gate("closed", ac_power=False, timeout=5)
    rig.wait_house(10, gate="closed", io__k2=True, io__k1=False, remote__ac_power=False)
    rig.expect_no("gate", "gate_state", since=m)

    m2 = rig.mark()
    rig.ctrl.on()
    rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m2, timeout=10)
    rig.wait_log("gate", "cmd_refused", a=ACT_OPEN, since=m2, timeout=5)
    rig.wait_house(5, cmd_result=RES_NO_POWER)
    # Put back at once (0.12.5), not after mismatch_timeout_s.
    rig.wait_log("house", "resync", since=m2, timeout=5)
    rig.wait_ctrl(False, timeout=30)
    rig.expect_no("gate", "pulse", since=m)

    m3 = rig.mark()
    rig.sim.ac(True)
    rig.wait_gate("closed", ac_power=True, timeout=5)
    rig.wait_house(10, gate="closed", io__k2=True, remote__ac_power=True)
    rig.expect_no("gate", "gate_state", seconds=1, since=m)


def test_ac_loss_mid_travel(rig):
    """AC lost while our command opens the gate: it carries on (no limit reads meanwhile: no_power), and reaching
    the open limit is trusted and ends the command."""
    rig.expect_commands(1)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["lora"], since=m, timeout=15)
    m2 = rig.mark()
    rig.sim.ac(False)
    rig.wait_log("gate", "gate_state", a=GS["no_power"], b=CAUSE["none"], since=m2, timeout=5)
    rig.wait_house(10, gate="no_power", io__k1=True, io__k2=False)
    rig.wait_log("gate", "gate_state", a=GS["open"], b=CAUSE["none"], since=m2, timeout=SIM_TRAVEL_S + 5)
    rig.wait_gate("open", last_result="reached", target="", ac_power=False, timeout=5)
    rig.wait_house(10, gate="open", io__k1=True, io__k2=False)
    rig.sim.ac(True)
    rig.wait_gate("open", ac_power=True, timeout=5)
    rig.expect_no("house", "resync", seconds=2, since=m)


@pytest.mark.parametrize("gap_ms", [100, 300])
def test_input_supply_lost_limit_drops_before_ac(rig, gap_ms):
    """The 24 V that wets the gate's inputs fails: the closed limit's opto drops before IN3's, and on its return
    IN3 comes back first. The gate goes closed -> no_power -> closed with no cause and never reports between (the
    firmware holds a move into between for BETWEEN_HOLD_MS, and BOOT_SETTLE_MS out of no_power)."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.sim.relays([(2, "off"), (3, "off")], gap_ms / 1000)
    rig.wait_log("gate", "gate_state", a=GS["no_power"], b=CAUSE["none"], since=m, timeout=5)
    rig.wait_house(10, gate="no_power", io__k2=False, io__k1=True)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(10, "sync window closed", sync_window=False, resyncing=False)

    m2 = rig.mark()
    rig.sim.relays([(3, "auto"), (2, "auto")], gap_ms / 1000)
    rig.wait_log("gate", "gate_state", a=GS["closed"], b=CAUSE["none"], since=m2, timeout=10)
    rig.wait_house(10, gate="closed", io__k2=True)
    rig.wait_ctrl(False, timeout=45)
    rig.wait_house(10, io__k1=False, resyncing=False)
    rig.expect_no("gate", "gate_state", a=GS["between"], since=m)
    rig.expect_no("gate", "pulse", since=m)


def test_limit_lost_with_ac_still_reports_between(rig):
    """A limit that drops while AC stays is a movement: between/external is reported once the hold is over."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.sim.relays([(2, "off")], 0)
    t = rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["external"], since=m, timeout=5)
    assert t["t"] - m >= 0.45, "reported before the hold (BETWEEN_HOLD_MS) was over"
    rig.wait_house(10, gate="between", io__k2=False)
    m2 = rig.mark()
    rig.sim.relays([(2, "auto")], 0)
    rig.wait_log("gate", "gate_state", a=GS["closed"], b=CAUSE["external"], since=m2, timeout=5)
    rig.wait_house(10, gate="closed", io__k2=True, io__k1=False, resyncing=False)
    rig.expect_no("gate", "pulse", since=m)


def test_relay_test_without_power(rig):
    """A gate relay test while the opener is unpowered still pulses (wiring check) but sets no target, so the
    limit read when power returns is neither reached nor a timeout, and the house doesn't resync."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.sim.power(False)
    rig.wait_log("gate", "gate_state", a=GS["no_power"], b=CAUSE["none"], since=m, timeout=5)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(10, "sync window closed", sync_window=False, resyncing=False)

    before = rig.gate.status()["last_result"]
    m2 = rig.mark()
    rig.relay_test("gate", 1, rig.pulse_ms)
    rig.wait_log("gate", "pulse", a=1, b=rig.pulse_ms, since=m2, timeout=5)
    rig.wait_gate("no_power", target="", timeout=5)

    m3 = rig.mark()
    rig.sim.power(True)
    rig.wait_log("gate", "gate_state", a=GS["closed"], b=CAUSE["none"], since=m3, timeout=5)
    rig.wait_gate("closed", target="", timeout=5)
    assert rig.gate.status()["last_result"] == before, "the test pulse had no target to reach or time out"
    rig.wait_house(10, gate="closed", io__k2=True)
    rig.wait_ctrl(False, timeout=45)
    rig.wait_house(10, io__k1=False, resyncing=False)
    rig.expect_no("gate", "travel_timeout", since=m2)


def test_power_loss_mid_travel(rig):
    """Power dies while opening and comes back with the gate stranded part-way."""
    rig.expect_commands(1)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["lora"], since=m, timeout=15)
    time.sleep(3)
    rig.sim.power(False)
    rig.wait_log("gate", "gate_state", a=GS["no_power"], since=m, timeout=5)
    rig.wait_house(10, gate="no_power", io__k1=True, io__k2=False)
    time.sleep(2)
    m2 = rig.mark()
    rig.sim.power(True)
    back = rig.wait_log("gate", "gate_state", a=GS["between"], since=m2, timeout=8)  # held BOOT_SETTLE_MS
    assert back["b"] == CAUSE["none"], "power return isn't a movement"
    assert rig.sim.status()["state"] == "stopped", "the opener doesn't resume after a power cut"
    # The limit it left is no longer trusted after no_power, so the house shows not-closed straight away.
    rig.wait_house(10, gate="between", io__k1=True, io__k2=False, ctrl=True)
    rig.wait_log("gate", "travel_timeout", a=GS["open"], since=m, timeout=TRAVEL_TIMEOUT_S + 5)
    rig.wait_gate("between", last_result="timeout", timeout=5)
    rig.expect_no("house", "resync", seconds=2, since=m)


def test_jammed_gate(rig):
    """The gate leaves closed and jams: travel timeout, house ends showing open (not closed)."""
    rig.sim.fault("stuck")
    rig.expect_commands(1)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_sim("jammed (fault stuck)", since=m, timeout=15)
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["lora"], since=m, timeout=5)
    # K2 opens at once; K1 holds "closed" until the house's own travel timeout, then shows open.
    rig.wait_house(10, gate="between", io__k2=False, io__k1=False)
    t = rig.wait_log("gate", "travel_timeout", a=GS["open"], since=m, timeout=TRAVEL_TIMEOUT_S + 5)
    rig.wait_gate("between", last_result="timeout", timeout=5)
    rig.wait_house(10, gate="between", io__k1=True, io__k2=False, ctrl=True)
    rig.latency("command -> travel timeout reported", t["t"] - rig.logs("house", "cmd_sent", since=m)[0]["t"])


def test_opener_ignores_command(rig):
    """Opener deaf (e.g. the siren input holding it): pulse goes out, nothing moves, controller resynced off."""
    rig.sim.fault("deaf")
    rig.expect_commands(1)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=10)
    rig.wait_sim("ignored (fault deaf)", since=m, timeout=5)
    t = rig.wait_log("gate", "travel_timeout", a=GS["open"], since=m, timeout=TRAVEL_TIMEOUT_S + 5)
    rig.wait_gate("closed", last_result="timeout", timeout=5)
    r = rig.wait_log("house", "resync", a=0, since=m, timeout=10)
    rig.wait_ctrl(False, timeout=15)
    rig.wait_house(10, gate="closed", io__k1=False, io__k2=True, resyncing=False)
    rig.latency("travel timeout -> house resync", r["t"] - t["t"])
    assert not rig.logs("gate", "gate_state", since=m), "the gate never moved"


def test_both_limits_active(rig):
    """Both limit signals at once (wiring fault): gate fault, house not-closed, nothing commanded."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.sim.fault("both")
    rig.wait_log("gate", "gate_state", a=GS["fault"], since=m, timeout=5)
    rig.wait_house(10, gate="fault", io__k2=False, io__k1=True)
    rig.wait_ctrl(True, timeout=30)
    m2 = rig.mark()
    rig.sim.fault("none")
    rig.wait_log("gate", "gate_state", a=GS["closed"], since=m2, timeout=5)
    rig.wait_house(10, gate="closed", io__k2=True, io__k1=False)
    rig.wait_ctrl(False, timeout=30)
    rig.expect_no("gate", "pulse", since=m)


def test_limit_chatter_debounced(rig):
    """Limit contacts chatter on arrival: one clean state change each way, no flapping reports."""
    rig.sim.fault("flicker")
    rig.expect_commands(2)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_sim("state open", since=m, timeout=SIM_TRAVEL_S + 10)
    rig.wait_house(10, gate="open", io__k1=True)
    rig.wait_house(10, "sync window closed", sync_window=False)
    time.sleep(1)  # let the chatter burst finish
    rig.ctrl.off()
    rig.wait_log("house", "cmd_sent", a=ACT_CLOSE, since=m, timeout=10)
    rig.wait_sim("state closed", since=m, timeout=SIM_TRAVEL_S + 10)
    rig.wait_house(10, gate="closed", io__k2=True)
    time.sleep(1)
    states = [e["a"] for e in rig.logs("gate", "gate_state", since=m)]
    assert states == [GS["between"], GS["open"], GS["between"], GS["closed"]], f"gate states {states}"
    house_states = [e["a"] for e in rig.logs("house", "gate_state", since=m)]
    assert house_states == states, f"house saw {house_states}"
    assert len(rig.logs("house", "cmd_sent", a=ACT_OPEN, since=m)) == 1
