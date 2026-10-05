"""Controller-side failures.

Controller power goes through `bench.power` (gatelink.controller.CtrlPower). With GATELINK_HA_POWER_ENTITY set,
an HA smart plug really cuts the controller's supply and house IN2 is wired to it, so the controller's relay
drops by itself. Otherwise in2_invert simulates the loss (IN2 unwired, or wired to the live supply, which can't be cut
because the house board shares it); the relay doesn't drop, and where the real one would, the test switches it off
itself.
"""
import threading
import time

import pytest

from gatelink.bench import ACT_OPEN, SIM_TRAVEL_S
from gatelink.flows import open_via_ctrl


@pytest.fixture
def powered(rig):
    """ctrl_power_sense on, controller powered. Set IN2 up first so enabling the sense doesn't see a power cut."""
    rig.power.fake(False)
    time.sleep(0.3)
    rig.house.config_set(ctrl_power_sense=1)
    rig.wait_house(5, ctrl_power=True, sync_window=False)
    return rig


def test_toggle_while_unpowered_ignored(powered):
    """Edges while the controller is unpowered are logged, never sent; after power returns they count as sync."""
    rig = powered
    rig.expect_commands(0)
    m = rig.mark()
    rig.power.fake(True)  # the house reads it unpowered, but it keeps its supply so it can still be switched
    rig.wait_log("house", "ctrl_power", a=0, b=0, since=m, timeout=5)
    rig.ctrl.on()
    rig.wait_log("house", "ctrl", a=1, b=1, since=m, timeout=10)
    rig.expect_no("house", "cmd_sent", seconds=3, since=m)
    rig.expect_no("house", "resync", since=m)  # resync pauses while unpowered
    m2 = rig.mark()
    rig.power.fake(False)
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
    if rig.power.real:
        _real_supply_cut(rig, m)
        return
    # The relay dropping with the supply... (the HA call returns only after Z-Wave confirms, often after the
    # relay has already moved, so switch from a thread and react to the edge itself)
    threading.Thread(target=rig.ctrl.off, daemon=True).start()
    edge = rig.wait_log("house", "ctrl", a=0, b=0, since=m, timeout=10)
    rig.power.set(False)  # ...and the power sense following inside ctrl_confirm_ms
    lost = rig.wait_log("house", "ctrl_power", a=0, since=m, timeout=2)
    assert lost["b"] == 2, f"pending CLOSE should be discarded (ctrl_power b=2), got b={lost['b']}"
    rig.latency("controller edge -> power sense drop (must be < ctrl_confirm_ms)", lost["t"] - edge["t"])
    rig.expect_no("house", "cmd_sent", seconds=2, since=m)
    assert rig.gate.status()["gate"] == "open"
    # Power back: the controller is out of step (off, gate open) and gets resynced on.
    m2 = rig.mark()
    rig.power.set(True)
    rig.wait_log("house", "resync", a=1, since=m2, timeout=45)
    rig.wait_ctrl(True, timeout=15)
    rig.expect_no("gate", "cmd_rx", since=m)


def _real_supply_cut(rig, m):
    """Plug backend: cut the real supply. Whichever drops first (relay or opto), nothing may be commanded."""
    rig.power.set(False)
    lost = rig.wait_log("house", "ctrl_power", a=0, since=m, timeout=10)
    edges = rig.logs("house", "ctrl", a=0, since=m)
    if lost["b"] == 2:
        order = "relay first, pending CLOSE discarded"
        rig.latency("controller edge -> power sense drop (must be < ctrl_confirm_ms)", lost["t"] - edges[0]["t"])
    else:
        order = "opto first" + (", relay edge ignored" if rig.logs("house", "ctrl", a=0, b=1, since=m) else "")
    rig.note(f"real supply cut: {order}")
    rig.facts["controller supply cut"] = order
    rig.expect_no("house", "cmd_sent", seconds=2, since=m)
    assert rig.gate.status()["gate"] == "open"
    m2 = rig.mark()
    rig.power.set(True)
    rig.wait_log("house", "ctrl_power", a=1, since=m2, timeout=10)
    # It boots at whatever level it restores; if that isn't on, the house resyncs it (never commands the gate).
    rig.wait_ctrl(True, timeout=45)
    rig.wait_house(15, io__k1=True, resyncing=False)
    rig.expect_no("gate", "cmd_rx", since=m)


def test_controller_returns_at_wrong_level(powered):
    """After a power cut the controller boots at some level: inside the settle window that's sync, not a command,
    and the mismatch path then brings it back to the gate's state."""
    rig = powered
    rig.expect_commands(0)
    rig.power.set(False)
    rig.wait_house(10, ctrl_power=False)
    m = rig.mark()
    rig.power.set(True)
    rig.wait_house(10, ctrl_power=True, sync_window=True)
    # Comes back on although the gate is closed (a real one may still be booting: retry the switch).
    rig.wait_for(lambda: rig.ctrl.on() or True, rig.power.BOOT_S + 5, "controller switchable after power-up")
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
