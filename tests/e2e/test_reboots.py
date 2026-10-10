"""Board resets (power blips, watchdog): nothing may be commanded by a reboot, and state recovers."""
import time

from gatelink.bench import PROFILE_COMMON, SIM_TRAVEL_S
from gatelink.flows import open_via_ctrl
from gatelink_client.wire import ACT_OPEN, CAUSE, GS


def test_gate_reboot_at_rest(rig):
    """The gate resets: new session, the house keeps its state, no relay moves."""
    rig.expect_commands(0)
    m = rig.mark()
    sessions = rig.house.status()["link"]["sessions"]
    rig.reboot("gate")
    rig.wait_for(lambda: rig.house.status()["link"]["sessions"] > sessions and rig.gate.status()["link"]["verified"],
                 20, "new gate session verified")
    rig.latency("gate reboot -> session re-established", rig.mark() - m)
    rig.wait_house(10, gate="closed", link_up=True, io__k2=True, io__k1=False)
    # Back well inside link_timeout_s: the house never declared the link down or changed the sensor.
    rig.expect_no("house", "link_down", since=m)
    rig.expect_no("house", "gate_state", since=m)
    rig.expect_no("gate", "pulse", since=m)


def test_gate_reboot_mid_travel(rig):
    """The gate resets while its OPEN is travelling: it reports the real position and doesn't claim the move."""
    rig.expect_commands(1)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "gate_state", a=GS["between"], b=CAUSE["lora"], since=m, timeout=15)
    time.sleep(1)  # let the 500 ms pulse finish so the reset doesn't cut it short
    rig.reboot("gate")
    t_boot = rig.mark()
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 10)
    st = rig.gate.status()
    assert st["cause"] != "lora", "after a reset the gate has no command in flight, so the move isn't ours"
    assert st["target"] == ""
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False, ctrl=True)
    assert len(rig.logs("gate", "pulse", since=m)) == 1, "exactly the one pulse before the reset"
    rig.expect_no("gate", "cmd_rx", since=t_boot)


def test_gate_reset_before_ack_no_second_pulse(rig):
    """The gate pulses for a command and resets before its ACK reaches the house (a power cut or crash right after
    the pulse; the gate's debug.reboot_after_cmd). The rebooted gate has forgotten the command, so the house must
    not send it again: the opener sees exactly one OPEN press, and the house drops the command."""
    rig.expect_commands(1)
    rig.allow_reboot("gate")
    rig.gate.request("debug.reboot_after_cmd")
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=15)
    rig.wait_log("house", "session", since=m, timeout=25)  # the rebooted gate's new session verified
    # The house's retries span cmd_ttl_s: give every one of them the chance to land.
    time.sleep(PROFILE_COMMON["cmd_ttl_s"] + 2)
    presses = [e for e in rig.timeline.select(src="sim", kind="evt", since=m) if e["line"].startswith("pulse open")]
    assert len(presses) == 1, f"the opener saw {len(presses)} OPEN presses: the command ran again after the reset"
    rig.wait_log("house", "cmd_dropped", a=ACT_OPEN, since=m, timeout=1)
    # The one pulse still opens the gate, and the house follows it.
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 10)
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False, ctrl=True)


def test_gate_boot_waits_for_opener_limits(rig):
    """Gate and opener restart together (a power blip at the gate): the gate boots while the opener's limits are
    still off. Its first report must wait for them, or the house would flash not-closed (the contact sensor
    opening, K1 flicking) before the limits come back."""
    rig.expect_commands(0)
    rig.allow_reboot("gate")
    m = rig.mark()
    rig.gate.request_reboot()
    time.sleep(0.2)  # the gate is in its bootloader (~0.5 s): the opener restarts under it
    rig.sim.restart(2500)
    rig.wait_log("house", "session", since=m, timeout=25)
    rig.wait_gate("closed", timeout=15)
    time.sleep(2)
    blips = [e for e in rig.timeline.select(src="house", kind="status", since=m) if e["gate"] != "closed" or not e["k2"]]
    assert not blips, f"the house showed the gate as {blips[0]['gate']} (K2 {blips[0]['k2']}) while it restarted"
    rig.expect_no("house", "gate_state", since=m)


def test_house_reboot_controller_on_gate_closed(rig):
    """House resets while the controller is on but the gate is closed: it must not open the gate."""
    rig.expect_commands(0)
    # Get the controller on without a command: report the controller unpowered while switching it on.
    rig.power.fake(True)
    rig.house.config_set(ctrl_power_sense=1)
    rig.wait_house(5, ctrl_power=False)
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("house", "ctrl", a=1, b=1, since=m, timeout=10)
    rig.reboot("house")  # also re-applies the profile: ctrl_power_sense back off
    t_boot = rig.mark()
    rig.wait_house(30, link_up=True, gate="closed", armed=True)
    # The controller is wrong (on, gate closed); the house fixes the controller, never the gate. It does so as
    # soon as the boot settle window (ctrl_settle_ms) closes, not after mismatch_timeout_s.
    rig.wait_log("house", "resync", a=0, since=t_boot, timeout=18)  # mismatch_timeout_s alone would be ~25 s
    rig.wait_ctrl(False, timeout=15)
    rig.wait_house(10, io__k1=False, io__k2=True, resyncing=False)
    rig.expect_no("gate", "cmd_rx", since=m)
    rig.expect_no("gate", "pulse", since=m)
    rig.latency("house reboot -> controller resynced", rig.mark() - t_boot)


def test_house_reboot_gate_open(rig):
    """House resets with the gate open: relays drop and come back, the controller ends on, nothing commanded."""
    open_via_ctrl(rig, record=False)
    rig.expect_commands(1)
    m = rig.mark()
    rig.reboot("house")
    rig.wait_house(30, link_up=True, gate="open", io__k1=True, io__k2=False)
    # K1 dropping during the reset can switch the controller off; it must come back on without a command.
    rig.wait_ctrl(True, timeout=45)
    time.sleep(2)
    rig.expect_no("gate", "cmd_rx", since=m)
    assert rig.gate.status()["gate"] == "open"
