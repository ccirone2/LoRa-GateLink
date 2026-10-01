"""Radio link failures.

An outage is injected by moving the gate to another net_id (applied, not saved): each board then drops every
frame from the other before authentication (link.cpp), which looks the same as a total RF outage. Putting the
net_id back ends it. The baseline re-applies the profile, so a failed test can't leave the link broken.
"""
import contextlib
import os
import secrets
import time

import pytest

from gatelink.bench import ACT_OPEN, GS, SIM_TRAVEL_S

LINK_TIMEOUT_S = 15  # PROFILE_COMMON link_timeout_s
HEARTBEAT_S = 5


def start_outage(b):
    b.note("radio outage starts")
    b.gate.config_set(net_id=(b.profile["gate"]["net_id"] + 1) % 256)


def end_outage(b):
    b.gate.config_set(net_id=b.profile["gate"]["net_id"])
    b.note("radio outage ends")


@contextlib.contextmanager
def outage(b):
    start_outage(b)
    try:
        yield
    finally:
        end_outage(b)


def test_link_loss_at_rest(rig):
    """No frames: house declares the link down and fails the contact sensor open; recovers when frames return."""
    rig.expect_commands(0)
    m = rig.mark()
    with outage(rig):
        down = rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + HEARTBEAT_S + 5)
        rig.wait_house(5, link_up=False, io__k2=False, io__k1=False)
        rig.latency("outage -> house link_down", down["t"] - m)
        t_restore = rig.mark()
    up = rig.wait_log("house", "link_up", since=t_restore, timeout=HEARTBEAT_S + 10)
    rig.wait_house(10, link_up=True, gate="closed", io__k2=True, io__k1=False)
    rig.latency("link restored -> house link_up", up["t"] - t_restore)


def test_command_during_outage_expires(rig):
    """A command sent into a dead link is dropped, never fires late, and the controller is resynced."""
    rig.expect_commands(1)
    m = rig.mark()
    with outage(rig):
        rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + HEARTBEAT_S + 5)
        rig.ctrl.on()
        sent = rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        dropped = rig.wait_log("house", "cmd_dropped", a=ACT_OPEN, since=m, timeout=15)
        rig.wait_house(5, cmd_result=-2, cmd_pending=False)
        rig.latency("cmd_sent -> cmd_dropped (dead link)", dropped["t"] - sent["t"])
    rig.wait_house(HEARTBEAT_S + 10, link_up=True, gate="closed")
    rig.expect_no("gate", "cmd_rx", seconds=3, since=m)
    # Controller says on, gate is closed: the mismatch path brings the controller back to off.
    rig.wait_log("house", "resync", a=0, since=m, timeout=30)
    rig.wait_ctrl(False, timeout=15)
    rig.wait_house(10, io__k1=False, io__k2=True, resyncing=False)


def test_short_outage_command_delivered_by_retry(rig):
    """The first attempts are lost, a retry gets through: delivered exactly once, one pulse."""
    rig.expect_commands(1)
    m = rig.mark()
    retries0 = rig.house.status()["link"]["retries"]
    with outage(rig):
        rig.ctrl.on()
        sent = rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        time.sleep(0.6)  # well inside the retry budget (cfg retries=5, backoff grows to ~1 s)
    rx = rig.wait_log("gate", "cmd_rx", a=ACT_OPEN, since=m, timeout=10)
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=5)
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 8)
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False, cmd_result=0)
    assert len(rig.logs("gate", "cmd_rx", since=m)) == 1
    assert len(rig.logs("gate", "pulse", since=m)) == 1
    assert rig.house.status()["link"]["retries"] > retries0, "expected the command to need a retry"
    rig.latency("cmd_sent -> gate cmd_rx (after short outage)", rx["t"] - sent["t"])


def test_gate_moved_during_outage(rig):
    """The gate moves while nothing gets through: the house catches up when frames return, no command."""
    rig.expect_commands(0)
    m = rig.mark()
    with outage(rig):
        rig.sim.open_gate()
        rig.wait_gate("open", timeout=SIM_TRAVEL_S + 5)
        assert rig.house.status()["gate"] == "closed", "the house can't have heard about it yet"
        t_restore = rig.mark()
    seen = rig.wait_log("house", "gate_state", a=GS["open"], since=t_restore, timeout=HEARTBEAT_S + 15)
    rig.wait_house(5, gate="open", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.expect_no("gate", "pulse", since=m)
    rig.latency("link restored -> house learns missed move", seen["t"] - t_restore)


def test_replayed_frames_rejected(rig):
    """A recorded frame sent again is rejected by the peer (replay counter) and changes nothing."""
    rig.expect_commands(0)
    rig.allow_counters("house")
    rig.allow_counters("gate")
    for sender, peer in (("house", "gate"), ("gate", "house")):
        before = rig.board(peer).status()["link"]["replay"]
        m = rig.mark()
        for _ in range(3):  # a heartbeat or ACK can slip in between; then the replayed frame is a different one
            m = rig.mark()
            # A PING is never ACK-memoed, so its replay must be counted rather than re-ACKed.
            rig.board(sender).request("radio.ping")
            rig.wait_for(lambda: rig.timeline.first(sender, "pong", m), 5, "pong", poll=0.05)
            rig.board(sender).request("debug.replay")
            time.sleep(1)
            if rig.board(peer).status()["link"]["replay"] > before:
                break
        else:
            rig.fail(f"{peer} never counted a replay from {sender}")
        rig.wait_log(peer, "replay", since=m, timeout=1)
    rig.expect_no("gate", "pulse")
    st = rig.house.status()
    assert st["gate"] == "closed" and st["link_up"], "replays must not disturb the link or the state"


@pytest.mark.needs_key
def test_wrong_key_rejected(rig):
    """A gate with a different key can't be commanded: frames fail authentication; restored afterwards."""
    key = os.environ.get("GATELINK_KEY", "")
    if len(key) != 32:
        pytest.skip("set GATELINK_KEY (the 32-hex-char key both boards share) to run this")
    rig.expect_commands(1)
    rig.allow_counters("house")
    rig.allow_counters("gate")  # key.set restarts the gate's link, which resets its counters
    # key.set saves the whole config, so put the gate's saved values back first; the profile goes on after.
    rig.gate.config_set(**{k: rig.backup["gate"][k] for k in rig.profile["gate"]})
    m = rig.mark()
    try:
        rig.gate.request("key.set", key=secrets.token_hex(16))
        rig.wait_log("house", "mac_fail", since=m, timeout=10)
        rig.wait_for(lambda: not rig.gate.status()["link"]["verified"], 10, "gate not verified")
        rig.wait_house(LINK_TIMEOUT_S + 10, link_up=False, io__k2=False)
        rig.ctrl.on()
        rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        rig.wait_log("house", "cmd_dropped", since=m, timeout=15)
        rig.expect_no("gate", "cmd_rx", since=m)
        assert rig.sim.status()["state"] == "closed"
    finally:
        rig.gate.request("key.set", key=key)
        rig.gate.config_set(**rig.profile["gate"])
    rig.wait_for(lambda: rig.house.status()["link_up"] and rig.gate.status()["link"]["verified"], 30,
                 "link back with the shared key (if not, GATELINK_KEY isn't the house's key)")
    rig.wait_ctrl(False, timeout=40)
