"""Controller-side failures.

House IN2 (controller power sense) isn't wired on the bench, so controller power is simulated with in2_invert:
1 = powered (the open input reads active), 0 = unpowered. That's only safe because nothing is connected to IN2.
The controller's own relay does not drop here; where the real one would, the test switches it off itself.
"""
import threading
import time

import pytest

from gatelink.bench import ACT_OPEN, SIM_TRAVEL_S
from gatelink.flows import open_via_ctrl


def set_power(b, on):
    b.house.config_set(in2_invert=1 if on else 0)


@pytest.fixture
def powered(rig):
    """ctrl_power_sense on, controller powered. Invert first so enabling the sense doesn't see a power cut."""
    set_power(rig, True)
    time.sleep(0.3)
    rig.house.config_set(ctrl_power_sense=1)
    rig.wait_house(5, ctrl_power=True, sync_window=False)
    return rig


def test_toggle_while_unpowered_ignored(powered):
    """Edges while the controller is unpowered are logged, never sent; after power returns they count as sync."""
    rig = powered
    rig.expect_commands(0)
    m = rig.mark()
    set_power(rig, False)
    rig.wait_log("house", "ctrl_power", a=0, b=0, since=m, timeout=5)
    rig.ctrl.on()
    rig.wait_log("house", "ctrl", a=1, b=1, since=m, timeout=10)
    rig.expect_no("house", "cmd_sent", seconds=3, since=m)
    rig.expect_no("house", "resync", since=m)  # resync pauses while unpowered
    m2 = rig.mark()
    set_power(rig, True)
    rig.wait_log("house", "ctrl_power", a=1, since=m2, timeout=5)
    rig.wait_house(5, sync_window=True)  # settle window (ctrl_settle_ms)
    rig.ctrl.off()
    rig.wait_log("house", "sync", a=0, since=m2, timeout=10)
    rig.expect_no("gate", "cmd_rx", seconds=1, since=m)


def test_relay_drops_before_power_sense(powered):
    """Supply cut: the relay drops first, the opto later. The edge waits ctrl_confirm_ms and is discarded."""
    rig = powered
    rig.expect_commands(1)
    open_via_ctrl(rig, record=False)
    rig.wait_house(10, "sync window closed", sync_window=False)
    m = rig.mark()
    # The relay dropping with the supply... (the HA call returns only after Z-Wave confirms, often after the
    # relay has already moved, so switch from a thread and react to the edge itself)
    threading.Thread(target=rig.ctrl.off, daemon=True).start()
    edge = rig.wait_log("house", "ctrl", a=0, b=0, since=m, timeout=10)
    set_power(rig, False)  # ...and the power sense following inside ctrl_confirm_ms
    lost = rig.wait_log("house", "ctrl_power", a=0, since=m, timeout=2)
    assert lost["b"] == 2, f"pending CLOSE should be discarded (ctrl_power b=2), got b={lost['b']}"
    rig.latency("controller edge -> power sense drop (must be < ctrl_confirm_ms)", lost["t"] - edge["t"])
    rig.expect_no("house", "cmd_sent", seconds=2, since=m)
    assert rig.gate.status()["gate"] == "open"
    # Power back: the controller is out of step (off, gate open) and gets resynced on.
    m2 = rig.mark()
    set_power(rig, True)
    rig.wait_log("house", "resync", a=1, since=m2, timeout=45)
    rig.wait_ctrl(True, timeout=15)
    rig.expect_no("gate", "cmd_rx", since=m)


def test_controller_returns_at_wrong_level(powered):
    """After a power cut the controller boots at some level: inside the settle window that's sync, not a command,
    and the mismatch path then brings it back to the gate's state."""
    rig = powered
    rig.expect_commands(0)
    set_power(rig, False)
    rig.wait_house(5, ctrl_power=False)
    m = rig.mark()
    set_power(rig, True)
    rig.wait_house(5, ctrl_power=True, sync_window=True)
    rig.ctrl.on()  # comes back on although the gate is closed
    rig.wait_log("house", "sync", a=1, since=m, timeout=10)
    rig.wait_log("house", "resync", a=0, since=m, timeout=45)
    rig.wait_ctrl(False, timeout=15)
    rig.expect_no("gate", "cmd_rx", since=m)


def test_rapid_toggling(rig):
    """ON/OFF/ON as fast as the controller allows: bounded commands, no held or overlapping relays, ends open."""
    rig.expect_commands(1, 3)
    m = rig.mark()
    rig.ctrl.on()
    rig.ctrl.off()
    rig.ctrl.on()
    rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
    rig.wait_gate("open", timeout=SIM_TRAVEL_S * 2 + 10)
    rig.wait_house(20, gate="open", io__k1=True, io__k2=False, ctrl=True, cmd_pending=False)
    time.sleep(2)
    assert rig.gate.status()["gate"] == "open"
    pulses = rig.logs("gate", "pulse", since=m)
    assert 1 <= len(pulses) <= 3, f"{len(pulses)} pulses"
