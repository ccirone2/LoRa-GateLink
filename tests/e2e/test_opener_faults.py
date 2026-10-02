"""Opener-side failures, injected with the GateSim: power loss, a jammed or deaf opener, bad limit signals."""
import time

from gatelink.bench import ACT_CLOSE, ACT_OPEN, CAUSE, GS, RES_NO_POWER, SIM_TRAVEL_S

TRAVEL_TIMEOUT_S = 15  # PROFILE_COMMON travel_timeout_s


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
    back = rig.wait_log("gate", "gate_state", a=GS["between"], since=m2, timeout=5)
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
