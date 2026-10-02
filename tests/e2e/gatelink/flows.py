"""Multi-step flows shared by several scenarios: a full controller-driven open or close, checked at every hop
and timed for the summary, and a radio outage."""
import contextlib

from .bench import ACT_CLOSE, ACT_OPEN, CAUSE, GS, SIM_TRAVEL_S

SLOW_STATUS_S = 0.5


def open_via_ctrl(b, record=True):
    """Controller ON -> house OPEN -> gate pulses K1 -> opener travels -> open limit -> house shows open."""
    return _move(b, open_=True, record=record)


def close_via_ctrl(b, record=True):
    return _move(b, open_=False, record=record)


def _move(b, open_, record):
    word = "open" if open_ else "close"
    dest = "open" if open_ else "closed"
    # An edge inside a sync window (opened by the last K1 change) counts as sync, not a command.
    b.wait_house(10, "no sync window open", sync_window=False, armed=True)
    m = b.mark()
    t0 = b.timeline.now()
    link0 = b.gate.status()["link"]
    b.ctrl.set(open_)
    sent = b.wait_log("house", "cmd_sent", a=ACT_OPEN if open_ else ACT_CLOSE, since=m, timeout=10)
    pulse = b.wait_log("gate", "pulse", a=1 if open_ else 2, since=m, timeout=5)
    moving = b.wait_sim("state opening" if open_ else "state closing", since=m, timeout=5)
    between = b.wait_log("gate", "gate_state", a=GS["between"], since=m, timeout=5)
    if between["b"] != CAUSE["lora"]:
        b.fail(f"gate left its limit with cause {between['b']}, expected lora")
    b.wait_log("house", "gate_state", a=GS["between"], since=m, timeout=10)
    # Mid-travel: the contact sensor reads open at once; K1 holds the limit the gate left.
    b.wait_house(5, f"house mid-travel outputs ({word})", gate="between", io__k2=False, io__k1=not open_)
    b.wait_sim(f"state {dest}", since=m, timeout=SIM_TRAVEL_S + 5)
    reached = b.wait_log("gate", "gate_state", a=GS[dest], since=m, timeout=5)
    if reached["b"] != CAUSE["lora"]:
        b.fail(f"gate reached {dest} with cause {reached['b']}, expected lora")
    house_seen = b.wait_log("house", "gate_state", a=GS[dest], since=m, timeout=10)
    b.wait_house(5, f"house shows {dest}", gate=dest, cause="lora", io__k1=open_, io__k2=not open_, ctrl=open_)
    b.wait_gate(dest, last_result="reached", timeout=5)
    if record:
        status_s = house_seen["t"] - reached["t"]
        if status_s > SLOW_STATUS_S:
            # Normally ~0.15 s; a slow one is a lost STATUS or ACK and a retry. Watch whether it grows at range.
            link = b.gate.status()["link"]
            retried = link["retries"] - link0["retries"]
            b.anomalies.append(f"{b.test_name}: gate -> house status took {status_s:.3f}s at {reached['t']}s "
                               f"(gate retries +{retried}, giveups +{link['giveups'] - link0['giveups']})")
        b.latency("controller -> house cmd_sent", sent["t"] - t0)
        b.latency("cmd_sent -> gate pulse (LoRa)", pulse["t"] - sent["t"])
        b.latency("gate pulse -> opener moving", moving["t"] - pulse["t"])
        b.latency("gate state -> house state (LoRa)", house_seen["t"] - reached["t"])
        b.latency(f"controller -> house shows {dest} (incl. {SIM_TRAVEL_S}s travel)", house_seen["t"] - t0)
    return m


@contextlib.contextmanager
def outage(b):
    """A total radio outage: the gate moves to another net_id (applied, not saved), so each board drops every frame
    from the other before authentication (link.cpp). Putting the net_id back ends it; the baseline re-applies the
    profile too, so a failed test can't leave the link broken."""
    b.note("radio outage starts")
    b.gate.config_set(net_id=(b.profile["gate"]["net_id"] + 1) % 256)
    try:
        yield
    finally:
        b.gate.config_set(net_id=b.profile["gate"]["net_id"])
        b.note("radio outage ends")
