"""Multi-step flows shared by several scenarios: a full controller-driven open or close, checked at every hop
and timed for the summary."""
from .bench import ACT_CLOSE, ACT_OPEN, CAUSE, GS, SIM_TRAVEL_S


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
        b.latency("controller -> house cmd_sent", sent["t"] - t0)
        b.latency("cmd_sent -> gate pulse (LoRa)", pulse["t"] - sent["t"])
        b.latency("gate pulse -> opener moving", moving["t"] - pulse["t"])
        b.latency("gate state -> house state (LoRa)", house_seen["t"] - reached["t"])
        b.latency(f"controller -> house shows {dest} (incl. {SIM_TRAVEL_S}s travel)", house_seen["t"] - t0)
    return m
