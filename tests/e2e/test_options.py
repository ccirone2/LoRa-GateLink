"""Gate relay tests and the settings the profile otherwise pins: power_sense, linkloss_open, sensor_invert and
ctrl_sync. Each test puts its setting back; the next baseline re-applies the profile anyway."""
import time

from gatelink.bench import CAUSE, GS, PROFILE_COMMON, SIM_TRAVEL_S
from gatelink.flows import outage

LINK_TIMEOUT_S = PROFILE_COMMON["link_timeout_s"]
HEARTBEAT_S = PROFILE_COMMON["heartbeat_s"]


def test_gate_relay_test_opens_closed_gate(rig):
    """relay.test K1 on the gate: the opener moves, the move is ours (a relay test sets a target), the house follows
    without a command."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.relay_test("gate", 1, rig.pulse_ms)
    between = rig.wait_log("gate", "gate_state", a=GS["between"], since=m, timeout=5)
    assert between["b"] == CAUSE["lora"], f"relay-test move left closed with cause {between['b']}, expected lora"
    opened = rig.wait_log("gate", "gate_state", a=GS["open"], since=m, timeout=SIM_TRAVEL_S + 5)
    assert opened["b"] == CAUSE["lora"], f"relay-test move reached open with cause {opened['b']}, expected lora"
    rig.wait_house(10, gate="open", cause="lora", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(10, "sync window closed", sync_window=False)
    m2 = rig.mark()
    rig.sim.close_gate()
    closed = rig.wait_log("gate", "gate_state", a=GS["closed"], since=m2, timeout=SIM_TRAVEL_S + 5)
    assert closed["b"] == CAUSE["external"]
    rig.wait_house(10, gate="closed", io__k1=False, io__k2=True)
    rig.wait_ctrl(False, timeout=30)


def test_gate_reversal_mid_pulse_never_overlaps(rig):
    """A CLOSE pulse requested while the OPEN pulse is still on (a reversal landing mid-pulse): K1 must have
    released before K2 closes, or the opener sees OPEN and CLOSE together for a moment (relay make faster than
    break). The invariant check flags any overlap the simulator saw."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.relay_test("gate", 1, 1000)
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=5)
    time.sleep(0.3)
    rig.relay_test("gate", 2, rig.pulse_ms)
    rig.wait_log("gate", "pulse", a=2, since=m, timeout=5)
    time.sleep(1.5)
    both = [e for e in rig.timeline.select(src="sim", kind="evt", since=m) if e["line"] == "pulse both"]
    assert not both, "the opener saw OPEN and CLOSE closed together"
    rig.wait_gate("closed", timeout=SIM_TRAVEL_S + 5)


def test_gate_relay_test_at_open_limit_then_external_close(rig):
    """relay.test K1 with the gate already open sets no target, so a local close straight after is external."""
    rig.expect_commands(0)
    rig.sim.open_gate()
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 5)
    rig.wait_house(10, gate="open", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(10, "sync window closed", sync_window=False)
    m = rig.mark()
    rig.relay_test("gate", 1, rig.pulse_ms)
    rig.wait_sim("cmd open from gate (already there)", since=m, timeout=5)
    time.sleep(rig.pulse_ms / 1000 + 0.2)
    assert rig.gate.status()["target"] == "", "the gate is already open: a K1 test there must not set a target"
    rig.sim.close_gate()  # well inside travel_timeout_s of the test pulse
    between = rig.wait_log("gate", "gate_state", a=GS["between"], since=m, timeout=5)
    assert between["b"] == CAUSE["external"], f"local close after a K1 test at open: cause {between['b']}"
    closed = rig.wait_log("gate", "gate_state", a=GS["closed"], since=m, timeout=SIM_TRAVEL_S + 5)
    assert closed["b"] == CAUSE["external"]
    rig.wait_house(10, gate="closed", cause="external", io__k1=False, io__k2=True)
    rig.wait_ctrl(False, timeout=30)


def test_power_sense_off_ignores_in3(rig):
    """Gate power_sense 0: IN3 is ignored, so an unpowered opener reads as its limits do (here: between), never
    no_power."""
    rig.expect_commands(0)
    try:
        rig.gate.config_set(power_sense=0)
        rig.wait_gate("closed", timeout=5, power_sense=False)
        m = rig.mark()
        rig.sim.power(False)
        rig.wait_gate("between", timeout=5, io__in3=False)
        assert rig.logs("gate", "gate_state", since=m)[0]["b"] == CAUSE["external"]
        # Gone from closed for under travel_timeout_s: K1 holds "closed", K2 reads open.
        rig.wait_house(10, gate="between", io__k1=False, io__k2=False)
        m2 = rig.mark()
        rig.sim.power(True)
        rig.wait_gate("closed", timeout=5)
        rig.expect_no("gate", "gate_state", since=m, a=GS["no_power"])
        rig.expect_no("gate", "cmd_refused", since=m)
        rig.wait_log("house", "gate_state", a=GS["closed"], since=m2, timeout=10)
    finally:
        rig.gate.config_set(power_sense=1)
    rig.wait_house(10, gate="closed", io__k1=False, io__k2=True)


def test_house_holds_travel_for_gate_travel_timeout(rig):
    """The house holds K1 through a travel for the gate's travel_timeout_s (in STATUS since 0.13.0), not its own:
    with the gate at 35 s and the house at the profile's 15 s, K1 still holds "closed" at 20 s and lets go by ~35 s."""
    rig.expect_commands(0)
    gate_s = 35
    house_s = PROFILE_COMMON["travel_timeout_s"]
    try:
        rig.gate.config_set(travel_timeout_s=gate_s, power_sense=0)
        rig.wait_house(HEARTBEAT_S + 5, "house has the gate's travel_timeout_s", remote__travel_timeout_s=gate_s)
        assert rig.house.config_get()["travel_timeout_s"] == house_s
        m = rig.mark()
        rig.sim.power(False)  # with power_sense off the gate reads between, as in a stuck travel
        t0 = rig.wait_log("gate", "gate_state", a=GS["between"], since=m, timeout=5)["t"]
        rig.wait_house(10, gate="between", io__k1=False, io__k2=False)
        time.sleep(max(0.0, t0 + house_s + 5 - rig.mark()))
        st = rig.house.status()
        assert st["gate"] == "between" and not st["io"]["k1"], \
            f"K1 let go after the house's own {house_s} s, not the gate's {gate_s} s: {st['io']}"
        rig.wait_house(gate_s - house_s + 5, "K1 not-closed after the gate's travel timeout", io__k1=True)
        assert rig.mark() - t0 >= gate_s - 1, f"K1 let go after {rig.mark() - t0:.1f} s, before the gate's {gate_s} s"
        rig.sim.power(True)
        rig.wait_gate("closed", timeout=5)
    finally:
        rig.sim.power(True)
        rig.gate.config_set(travel_timeout_s=house_s, power_sense=1)
    rig.wait_house(10, gate="closed", io__k1=False, io__k2=True)
    rig.wait_ctrl(False, timeout=30)


def test_linkloss_open_off_keeps_sensor(rig):
    """House linkloss_open 0: on link loss K2 stays as it was (gate closed: closed). Turning it back on during the
    outage fails K2 open at once."""
    rig.expect_commands(0)
    m = rig.mark()
    try:
        rig.house.config_set(linkloss_open=0)
        with outage(rig):
            rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + HEARTBEAT_S + 5)
            rig.wait_house(5, link_up=False, gate="closed", io__k2=True, io__k1=False)
            time.sleep(2)
            st = rig.house.status()
            assert st["io"]["k2"] and not st["link_up"], "K2 must hold closed through the outage"
            rig.house.config_set(linkloss_open=1)
            rig.wait_house(5, "K2 fails open once linkloss_open is back on", link_up=False, io__k2=False)
    finally:
        rig.house.config_set(linkloss_open=1)
    rig.wait_house(HEARTBEAT_S + 10, link_up=True, gate="closed", io__k2=True)


def test_sensor_invert(rig):
    """House sensor_invert 1: K2 is energized when the gate is NOT closed (normally-closed sensor wiring)."""
    rig.expect_commands(0)
    rig.allow_k2_inverted()
    try:
        rig.house.config_set(sensor_invert=1)
        rig.wait_house(5, gate="closed", io__k2=False)
        rig.sim.open_gate()
        rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k2=True, io__k1=True)
        rig.wait_ctrl(True, timeout=30)
    finally:
        rig.house.config_set(sensor_invert=0)
    rig.wait_house(5, gate="open", io__k2=False)


def test_ctrl_sync_off(rig):
    """House ctrl_sync 0: K1 doesn't follow the gate, so the controller stays where it is (and isn't resynced).
    Back on, K1 catches up and the controller follows as sync, not a command."""
    rig.expect_commands(0)
    m = rig.mark()
    try:
        rig.house.config_set(ctrl_sync=0)
        rig.sim.open_gate()
        rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k2=False)
        time.sleep(3)
        st = rig.house.status()
        assert not st["io"]["k1"] and not st["ctrl"], f"ctrl_sync 0 moved K1/the controller: {st['io']}, {st['ctrl']}"
        rig.expect_no("house", "sync", since=m)
        rig.expect_no("house", "resync", since=m)
        m2 = rig.mark()
        rig.house.config_set(ctrl_sync=1)
        rig.wait_house(5, io__k1=True)
        rig.wait_log("house", "sync", a=1, since=m2, timeout=15)
        rig.wait_ctrl(True, timeout=15)
    finally:
        rig.house.config_set(ctrl_sync=1)
