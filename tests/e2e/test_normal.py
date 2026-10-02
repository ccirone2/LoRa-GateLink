"""Normal operation: the controller opens and closes the gate, other controllers move it, the house follows."""
import time

import pytest

from gatelink.bench import ACT_CLOSE, ACT_OPEN, CAUSE, GS, SIM_TRAVEL_S
from gatelink.flows import close_via_ctrl, open_via_ctrl


def test_open_via_controller(rig):
    rig.expect_commands(1)
    m = open_via_ctrl(rig)
    # The house's K1 change at the open limit must not bounce the controller back (that would need a resync).
    rig.expect_no("house", "resync", seconds=4, since=m)
    rig.wait_ctrl(True, timeout=1)


def test_close_via_controller(rig):
    rig.expect_commands(2)
    open_via_ctrl(rig, record=False)
    m = close_via_ctrl(rig)
    rig.expect_no("house", "resync", seconds=4, since=m)
    rig.wait_ctrl(False, timeout=1)


def test_reverse_mid_travel(rig):
    """Controller OFF a few seconds into an opening: the gate pulses CLOSE and the opener turns back."""
    rig.expect_commands(2)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["lora"], since=m, timeout=15)
    time.sleep(3)
    rig.ctrl.off()
    rig.wait_log("house", "cmd_sent", a=ACT_CLOSE, since=m, timeout=10)
    rig.wait_log("gate", "pulse", a=2, since=m, timeout=5)
    rig.wait_sim("state closing", since=m, timeout=5)
    closed = rig.wait_log("gate", "gate_state", a=GS["closed"], since=m, timeout=SIM_TRAVEL_S + 5)
    assert closed["b"] == CAUSE["lora"], "the reversal we commanded should be attributed to lora"
    rig.wait_house(10, gate="closed", io__k1=False, io__k2=True, ctrl=False)


def test_flip_back_before_gate_leaves_limit(rig):
    """ON then straight back OFF: the opposing CLOSE is still sent (not suppressed) and pulsed to reverse."""
    rig.expect_commands(2)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
    rig.ctrl.off()
    rig.wait_log("house", "cmd_sent", a=ACT_CLOSE, since=m, timeout=10)
    rig.wait_for(lambda: len(rig.logs("gate", "pulse", since=m)) >= 2, 5, "gate pulses OPEN then CLOSE")
    assert [e["a"] for e in rig.logs("gate", "pulse", since=m)] == [1, 2]
    rig.wait_gate("closed", timeout=SIM_TRAVEL_S + 5)
    rig.wait_house(15, gate="closed", io__k1=False, io__k2=True, ctrl=False)


def test_external_moves_followed_without_commands(rig):
    """The opener moved by something else (AES, siren, keypad): reported as external, controller follows."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.sim.open_gate()
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["external"], since=m, timeout=5)
    rig.wait_log("gate", "gate_state", a=GS["open"], b=CAUSE["external"], since=m, timeout=SIM_TRAVEL_S + 5)
    rig.wait_house(10, gate="open", cause="external", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    assert rig.logs("house", "sync", since=m), "the controller's edge should be logged as sync"
    m2 = rig.mark()
    rig.sim.close_gate()
    rig.wait_log("gate", "gate_state", a=GS["closed"], b=CAUSE["external"], since=m2, timeout=SIM_TRAVEL_S + 5)
    rig.wait_house(10, gate="closed", io__k1=False, io__k2=True)
    rig.wait_ctrl(False, timeout=30)
    rig.expect_no("gate", "pulse", since=m)


def test_external_move_right_after_our_command(rig):
    """A local CLOSE while our OPEN is travelling: external, even though our command was just sent."""
    rig.expect_commands(1)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["lora"], since=m, timeout=15)
    time.sleep(2)
    rig.sim.close_gate()
    closed = rig.wait_log("gate", "gate_state", a=GS["closed"], since=m, timeout=SIM_TRAVEL_S + 5)
    assert closed["b"] == CAUSE["external"], "a move away from our target must be external"
    rig.wait_house(10, gate="closed", io__k2=True)
    # The controller is synced back to off (directly, or by resync once the gate reports timeout).
    rig.wait_ctrl(False, timeout=45)
    rig.wait_house(10, io__k1=False, resyncing=False)


@pytest.mark.soak
def test_soak_open_close_cycles(rig, request):
    cycles = request.config.getoption("--cycles")
    rig.expect_commands(2 * cycles)
    for i in range(cycles):
        rig.note(f"soak cycle {i + 1}/{cycles}")
        open_via_ctrl(rig)
        close_via_ctrl(rig)
