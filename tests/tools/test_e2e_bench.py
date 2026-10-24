"""The bench suite's own checks (tests/e2e/gatelink) without the bench: fake boards and a real timeline."""
import importlib
import importlib.util
import sys
from pathlib import Path

import pytest
from gatelink_client.timeline import Timeline

# The suite's package is `gatelink`, as is tools/gatelink.py on this path: load it under another name.
_PKG = Path(__file__).resolve().parents[1] / "e2e" / "gatelink"
_spec = importlib.util.spec_from_file_location("e2e_gatelink", _PKG / "__init__.py",
                                               submodule_search_locations=[str(_PKG)])
sys.modules["e2e_gatelink"] = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sys.modules["e2e_gatelink"])
bench_mod = importlib.import_module("e2e_gatelink.bench")


class FakeBoard:
    def __init__(self, name, tapped):
        self.name = name
        self.uart = object() if tapped else None


def fake_bench(tapped, rebooted=()):
    """A Bench whose power cuts reboot the boards in `rebooted`; their boot reaches the timeline only via a tap."""
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.timeline = Timeline()
    b.house, b.gate = FakeBoard("house", "house" in tapped), FakeBoard("gate", "gate" in tapped)
    b.facts = {}
    b.cuts = []
    b.baseline = lambda: None
    b.wait_power_return = lambda name, since: None

    def power_cut(site, ms):
        b.cuts.append(site)
        if site in rebooted and b.board(site).uart is not None:
            b.timeline.add(site, "log", ev="boot", a=1, b=0)["t"] += 1.0  # heard a second after the cut
    b.power_cut = power_cut
    return b


@pytest.fixture(autouse=True)
def no_sleep(monkeypatch):
    monkeypatch.setattr(bench_mod.time, "sleep", lambda s: None)


def test_find_lipos_from_the_boot_on_each_tap():
    b = fake_bench(tapped=("house", "gate"), rebooted=("gate",))
    assert b.find_lipos() == {"gate": False, "house": True}
    assert b.facts["LiPos"] == "gate out, house in"


@pytest.mark.parametrize("tapped", [(), ("house",), ("gate",)])
def test_find_lipos_refuses_an_untapped_board(tapped):
    """Without a tap a board's boot is never heard: a LiPo-out board would read as LiPo in, and the ride-through
    tests (no_reboot) would pass even when it rebooted. Refuse before cutting anything."""
    b = fake_bench(tapped=tapped, rebooted=("house", "gate"))
    with pytest.raises(bench_mod.NoUartTap, match="UART tap"):
        b.find_lipos()
    assert b.cuts == []


class StatusBoard:
    def __init__(self, st):
        self.st = st
        self.asked = 0

    def status(self):
        self.asked += 1
        return self.st


@pytest.mark.parametrize("link_up, verified, pong, ok", [
    (True, True, True, True),
    (True, True, False, False),   # link_up left from the gate's old session (20261014-231116): no pong
    (False, True, True, False),
    (True, False, True, False),
])
def test_baseline_link_is_up_in_the_gate_s_current_session(link_up, verified, pong, ok):
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.house = StatusBoard({"link_up": link_up})
    b.gate = StatusBoard({"link": {"verified": verified}})
    pings = []
    b.ping = lambda required=True: pings.append(required) or ({"event": "pong"} if pong else None)
    assert bool(b._link_current()) is ok
    assert b.gate.asked == link_up, "the gate is asked only once the house has the link up"
    assert pings == ([False] if link_up and verified else [])


class UptimeBoard:
    """A board whose status reports `uptime(host_t)` at the bench's host clock."""

    def __init__(self, clock, uptime):
        self.clock, self.uptime = clock, uptime
        self.dropped = 0

    def status(self):
        return {"uptime_ms": int(self.uptime(self.clock[0])), "link": {"mac_fail": 0, "replay": 0}, "usb_cut": 0,
                "reset_cause": 1}


def reset_bench(gate_uptime):
    clock = [100.0]
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.timeline = Timeline()
    b.timeline.now = lambda: clock[0]
    b.house = UptimeBoard(clock, lambda t: 600_000 + t * 1000)
    b.gate = UptimeBoard(clock, gate_uptime)
    b.pulse_ms = 500
    b.anomalies = []
    b.begin_test("t")
    return b, clock


@pytest.mark.parametrize("gate_uptime, reset", [
    (lambda t: (t - 95) * 1000, False),         # up 5 s at the start, no reset
    (lambda t: (t - 110) * 1000 if t >= 110 else (t - 95) * 1000, True),  # reset 10 s in: still past the 5 s at start
    (lambda t: (t - 165) * 1000 if t >= 165 else 900_000, True),          # reset 5 s before the end
])
def test_invariant_check_catches_a_reset_whose_uptime_outgrew_the_start(gate_uptime, reset):
    """A board reset during a test whose USB port didn't drop (requests over the UART) is caught by its uptime alone:
    it must be compared with the uptime the board would have had, not only the one at the start, or a board that had
    just booted then (after a reboot or power test) and reset early in the test reads as never reset."""
    b, clock = reset_bench(gate_uptime)
    since = b.mark()
    snap = b.snapshot()
    clock[0] += 70
    if reset:
        with pytest.raises(AssertionError, match="gate reset during the test"):
            b.check_invariants(since, snap)
    else:
        b.check_invariants(since, snap)


def k1_bench(events):
    """A Bench whose timeline holds `events` (host s, src, kind, fields), for the K1 check alone."""
    clock = [0.0]
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.timeline = Timeline()
    b.timeline.now = lambda: clock[0]
    b.profile = {"house": {"resync_ms": 1000}}
    b.begin_test("t")
    for t, src, kind, fields in events:
        clock[0] = t
        b.timeline.add(src, kind, **fields)
    problems = []
    b._check_k1(0.0, problems)
    return problems


def st(gate, k1):
    return "house", "status", {"gate": gate, "k1": k1, "k2": gate == "closed", "cause": None, "ctrl": k1}


@pytest.mark.parametrize("events, bad", [
    ([(1, *st("closed", False)), (2, *st("open", True)), (3, *st("no_power", True)), (4, *st("fault", True))], 0),
    ([(1, *st("open", False))], 1),  # K1 left off with the gate open
    ([(1, *st("closed", True))], 1),  # K1 on with the gate closed
    ([(1, *st("between", False)), (2, *st("between", True)), (3, *st("unknown", True))], 0),  # the travel hold
    ([(1, "house", "log", {"ev": "resync", "a": 1, "b": 0}), (1.5, *st("open", False)), (3, *st("open", False))], 1),
    ([(1, "test", "action", {"relay_test": "house", "k": 1, "ms": 500}), (1.2, *st("closed", True))], 0),
    ([(1, "test", "action", {"relay_test": "house", "k": 2, "ms": 500}), (1.2, *st("closed", True))], 1),
])
def test_k1_check_follows_the_gate_outside_travel_resync_and_relay_tests(events, bad):
    assert len(k1_bench(events)) == bad


def test_k1_check_skips_a_test_with_ctrl_sync_off():
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.timeline = Timeline()
    b.profile = {"house": {"resync_ms": 1000}}
    b.begin_test("t")
    b.allow_k1_free()
    b.timeline.add(*st("open", False)[:2], **st("open", False)[2])
    problems = []
    b._check_k1(0.0, problems)
    assert problems == []


@pytest.mark.parametrize("gap_ms, bad", [(100, 0), (65, 0), (40, 1), (0, 1)])
def test_interlock_gap_check_flags_a_press_soon_after_the_other_release(gap_ms, bad):
    """OPEN released at 10 s (held 500 ms); CLOSE pressed `gap_ms` later, its release reported 300 ms after that."""
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.facts = {}
    releases = {1: [(10.0, 500)], 2: [(10.0 + gap_ms / 1000 + 0.3, 300)]}
    problems = []
    b._check_interlock_gap(releases, problems)
    assert len(problems) == bad
    assert b.facts["closest relay interlock gap (ms)"] == gap_ms


def test_interlock_gap_check_takes_a_press_from_its_sooner_line():
    """Run 20261024-170528: CLOSE's `evt pulse` came at 161.000 s (held 240 ms) but its release line late, at 161.341 s,
    in one read with OPEN's `evt pulse` (held 496 ms, released at 161.838 s). From the release lines alone the gap reads
    1 ms; from CLOSE's pulse line it is the 101 ms the interlock left."""
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.facts = {}
    releases = {1: [(161.838, 496)], 2: [(161.341, 240)]}
    problems = []
    b._check_interlock_gap(releases, problems, {1: [161.341], 2: [161.000]})
    assert problems == []
    assert b.facts["closest relay interlock gap (ms)"] == 101
