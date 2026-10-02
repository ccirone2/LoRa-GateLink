"""Long soak: hours of open/close cycles with outages and opener faults mixed in.

Watches for what short runs can't show: watchdog resets, radio faults, MAC failures or replays, and counter drift.
Both boards' counters are appended to results/<run>/soak_counters.csv after every scenario. Opt-in:
    pytest tests/e2e -m longsoak --soak-minutes 120
"""
import csv
import time

import pytest

from gatelink.bench import ACT_OPEN, GS, SIM_TRAVEL_S
from gatelink.flows import close_via_ctrl, open_via_ctrl, outage

LINK_TIMEOUT_S = 15  # PROFILE_COMMON
HEARTBEAT_S = 5
TRAVEL_TIMEOUT_S = 15
COUNTERS = ("tx", "rx", "retries", "giveups", "mac_fail", "replay", "sessions")


# Each scenario starts from the baseline and returns how many commands the house should have sent.
def cycle(rig):
    open_via_ctrl(rig)
    close_via_ctrl(rig)
    return 2


def outage_at_rest(rig):
    m = rig.mark()
    with outage(rig):
        rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + HEARTBEAT_S + 5)
    rig.wait_house(HEARTBEAT_S + 15, link_up=True, gate="closed", io__k2=True)
    return 0


def command_through_outage(rig):
    """A command sent into a 3 s outage is delivered by a later retry, inside cmd_ttl_s."""
    m = rig.mark()
    with outage(rig):
        rig.ctrl.on()
        rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        time.sleep(3)
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 15)
    rig.wait_house(15, gate="open", io__k1=True, ctrl=True, cmd_result=0)
    close_via_ctrl(rig, record=False)
    return 2


def opener_power_blip(rig):
    m = rig.mark()
    rig.sim.power(False)
    rig.wait_log("gate", "gate_state", a=GS["no_power"], since=m, timeout=5)
    rig.wait_house(10, gate="no_power", io__k1=True, io__k2=False)
    time.sleep(3)
    rig.sim.power(True)
    rig.wait_house(10, gate="closed", io__k2=True)
    rig.wait_ctrl(False, timeout=45)
    return 0


def external_moves(rig):
    rig.sim.open_gate()
    rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k1=True)
    rig.wait_ctrl(True, timeout=30)
    rig.sim.close_gate()
    rig.wait_house(SIM_TRAVEL_S + 10, gate="closed", io__k1=False)
    rig.wait_ctrl(False, timeout=30)
    return 0


def jam(rig):
    """The gate jams on the way open; the next baseline closes it from the simulator (an external move)."""
    rig.sim.fault("stuck")
    m = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "travel_timeout", since=m, timeout=TRAVEL_TIMEOUT_S + 15)
    rig.wait_house(10, gate="between", io__k1=True, ctrl=True)
    rig.sim.fault("none")
    return 1


ROTATION = [cycle, cycle, outage_at_rest, cycle, opener_power_blip, cycle, external_moves, command_through_outage,
            cycle, jam]


@pytest.mark.longsoak
def test_long_soak(rig, request):
    minutes = request.config.getoption("--soak-minutes")
    deadline = time.monotonic() + minutes * 60
    path = rig.run_dir / "soak_counters.csv"
    cols = ["t_s", "scenario"] + [f"{n}_{c}" for n in ("house", "gate")
                                  for c in ("uptime_s", "reset_cause", "radio_faults") + COUNTERS]
    expected = 0
    last_uptime = {}
    t0 = rig.mark()
    i = 0
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols)
        while time.monotonic() < deadline:
            scenario = ROTATION[i % len(ROTATION)]
            if i:
                rig.baseline()
            rig.note(f"long soak {i + 1}: {scenario.__name__}")
            expected += scenario(rig)
            row = [round(rig.mark() - t0, 1), scenario.__name__]
            for n in ("house", "gate"):
                st = rig.board(n).status()
                if st["uptime_ms"] < last_uptime.get(n, 0):
                    rig.fail(f"{n} reset during {scenario.__name__} (reset_cause {st['reset_cause']})")
                last_uptime[n] = st["uptime_ms"]
                if st["radio_faults"]:
                    rig.fail(f"{n} radio_faults {st['radio_faults']} after {scenario.__name__}")
                row += [st["uptime_ms"] // 1000, st["reset_cause"], st["radio_faults"]]
                row += [st["link"][c] for c in COUNTERS]
            w.writerow(row)
            f.flush()
            i += 1
    rig.facts["long soak"] = f"{i} scenarios in {minutes:g} min, counters in {path.name}"
    rig.expect_commands(expected)
