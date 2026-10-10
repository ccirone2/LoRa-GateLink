"""Radio link failures, injected with flows.outage() (the gate moved to another net_id; see there)."""
import os
import secrets
import time

import pytest

from gatelink.bench import PROFILE_COMMON, SIM_TRAVEL_S
from gatelink.flows import outage
from gatelink_client.wire import ACT_OPEN, GS

LINK_TIMEOUT_S = PROFILE_COMMON["link_timeout_s"]
HEARTBEAT_S = PROFILE_COMMON["heartbeat_s"]
CMD_TTL_S = PROFILE_COMMON["cmd_ttl_s"]


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
        dropped = rig.wait_log("house", "cmd_dropped", a=ACT_OPEN, since=m, timeout=CMD_TTL_S + 5)
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
        time.sleep(0.6)  # well inside the retry budget (retries spread over cmd_ttl_s: ~0.3, 0.9, 2.2, 4.7, 9.7 s)
    rx = rig.wait_log("gate", "cmd_rx", a=ACT_OPEN, since=m, timeout=10)
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=5)
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 8)
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False, cmd_result=0)
    assert len(rig.logs("gate", "cmd_rx", since=m)) == 1
    assert len(rig.logs("gate", "pulse", since=m)) == 1
    assert rig.house.status()["link"]["retries"] > retries0, "expected the command to need a retry"
    rig.latency("cmd_sent -> gate cmd_rx (after short outage)", rx["t"] - sent["t"])


def test_long_outage_command_delivered_within_ttl(rig):
    """A 4 s outage outlasts the old fixed retry budget (~4.1 s on 0.3.1); retries spread over cmd_ttl_s deliver it
    once. 4 s leaves two retries after the outage (~4.4 and ~9 s): a longer one leaves only the last, which a single
    lost frame would turn into a flaky failure."""
    rig.expect_commands(1)
    m = rig.mark()
    with outage(rig):
        rig.ctrl.on()
        sent = rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        time.sleep(max(0.0, 4 - (rig.mark() - sent["t"])))
    rx = rig.wait_log("gate", "cmd_rx", a=ACT_OPEN, since=m, timeout=10)
    assert rx["t"] - sent["t"] < CMD_TTL_S + 0.5, "delivered after cmd_ttl_s"
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 8)
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False, cmd_result=0)
    assert len(rig.logs("gate", "pulse", since=m)) == 1
    rig.expect_no("house", "cmd_dropped", since=m)
    rig.latency("cmd_sent -> gate cmd_rx (after 4 s outage)", rx["t"] - sent["t"])


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
        for _ in range(5):  # a heartbeat or ACK can slip in between; then the replayed frame is a different one
            m = rig.mark()
            # A PING is never ACK-memoed, so its replay must be counted rather than re-ACKed.
            if not rig.ping(sender, timeout=3, required=False):
                continue  # a lost ping or pong (a few % on the bench): try again
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


def _restart_gate_link(rig):
    """Restart the gate's link without a reboot (a radio param change): new session, but its first HELLO since boot
    (debug.replay hello) is kept, and is now an old session's."""
    sessions = rig.house.status()["link"]["sessions"]
    tx = rig.gate.config_get()["tx_power"]
    rig.gate.config_set(tx_power=tx - 1 if tx > 2 else tx + 1)
    rig.gate.config_set(tx_power=tx)
    rig.wait_for(lambda: rig.house.status()["link"]["sessions"] >= sessions + 1 and rig.gate.status()["link"]["verified"],
                 20, "gate's new session verified")
    rig.wait_house(10, link_up=True, gate="closed", armed=True)


def test_replayed_hello_holds_command(rig):
    """A recorded HELLO from an old gate session arrives while a command waits for its ACK. That is also what a
    gate restart looks like, so the house holds the command (rather than resending it, which could pulse twice);
    but as the verified gate session answers the house's challenge, it was a replay: the command goes after all.
    Before 0.13.1 the house dropped it, so a replayed HELLO could cancel any command."""
    rig.expect_commands(1)
    rig.allow_counters("house")
    rig.allow_counters("gate")
    _restart_gate_link(rig)
    sessions = rig.house.status()["link"]["sessions"]
    m = rig.mark()
    rig.gate.request("debug.mute", ms=2500)  # the command's frames go unheard, so it stays pending
    rig.ctrl.on()
    rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=5)
    # debug.replay sends without listening first (not at all if the gate's radio is busy), so the HELLO can be lost,
    # e.g. while the house sends a retry: replay it again until the house holds the command.
    held = None
    for _ in range(4):
        res = rig.gate.request("debug.replay", hello=True)
        if not res.get("sent", True):
            rig.note("debug.replay: gate radio busy, not sent")
        held = rig.timeline.first("house", "log", m, ev="cmd_hold", a=1)
        if not held:
            try:
                held = rig.wait_log("house", "cmd_hold", a=1, since=m, timeout=1.5)
            except AssertionError:
                rig.note("replayed HELLO not answered with a hold: replaying again")
                continue
        break
    if not held:
        rig.fail("house never held the command after 4 replayed HELLOs", since=m)
    sent = rig.wait_log("house", "cmd_hold", a=0, since=m, timeout=CMD_TTL_S)
    rig.latency("replayed HELLO -> held command released", sent["t"] - held["t"])
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=CMD_TTL_S)
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 10)
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False, ctrl=True)
    assert len(rig.logs("gate", "pulse", since=m)) == 1
    rig.expect_no("house", "cmd_dropped", since=m)
    rig.expect_no("house", "cmd_hold", a=2, since=m)
    assert rig.house.status()["link"]["sessions"] == sessions, "the replayed session must never verify"


def test_replayed_hello_flood_answered_once_a_second(rig):
    """HELLOs bypass the replay window, so the house answers replayed ones too, but at most once a second per kind:
    a flood can't use the channel up or keep renumbering (and so postponing) pending messages."""
    rig.expect_commands(0)
    rig.allow_counters("house")
    rig.allow_counters("gate")
    _restart_gate_link(rig)
    tx0 = rig.house.status()["link"]["tx"]
    m = rig.mark()
    for _ in range(15):  # old-session HELLOs about every 100 ms for 1.5 s
        rig.gate.request("debug.replay", hello=True)
        time.sleep(0.1)
    time.sleep(0.5)
    tx = rig.house.status()["link"]["tx"] - tx0
    # 2 HELLO_ACKs (1.5 s at one a second), the house's own challenge, and a heartbeat's ACK or two.
    assert tx <= 6, f"the house sent {tx} frames during the flood: replayed HELLOs aren't rate-limited"
    st = rig.house.status()
    assert st["link_up"] and st["gate"] == "closed"
    rig.expect_no("house", "cmd_hold", since=m)


@pytest.mark.needs_key
def test_wrong_key_rejected(rig):
    """A gate with a different key can't be commanded: frames fail authentication; restored afterwards."""
    key = os.environ.get("GATELINK_KEY", "")
    if len(key) != 32:
        pytest.skip("set GATELINK_KEY (the 32-hex-char key both boards share) to run this")
    rig.expect_commands(1)
    rig.allow_counters("house")
    rig.allow_counters("gate")  # key.set restarts the gate's link, which resets its counters
    # key.set saves only the key (0.3.5), so the unsaved test profile stays unsaved. (Putting the saved timings
    # back first, as older firmware needed, would now stretch the house's link timeout to 2.5 x the saved
    # heartbeat and the link would outlast the wait below.)
    m = rig.mark()
    try:
        rig.gate.request("key.set", key=secrets.token_hex(16))
        rig.wait_log("house", "mac_fail", since=m, timeout=10)
        rig.wait_for(lambda: not rig.gate.status()["link"]["verified"], 10, "gate not verified")
        rig.wait_house(LINK_TIMEOUT_S + 10, link_up=False, io__k2=False)
        rig.ctrl.on()
        rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        rig.wait_log("house", "cmd_dropped", since=m, timeout=CMD_TTL_S + 5)
        rig.expect_no("gate", "cmd_rx", since=m)
        assert rig.sim.status()["state"] == "closed"
    finally:
        rig.gate.request("key.set", key=key)
    rig.wait_for(lambda: rig.house.status()["link_up"] and rig.gate.status()["link"]["verified"], 30,
                 "link back with the shared key (if not, GATELINK_KEY isn't the house's key)")
    rig.wait_ctrl(False, timeout=40)
