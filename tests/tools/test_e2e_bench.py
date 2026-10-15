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

    def status(self):
        return self.st


@pytest.mark.parametrize("remote_up, gate_up_ms, link_up, verified, ok", [
    (14, 14_300, True, True, True),     # a STATUS from the gate's current boot
    (2, 14_300, True, True, True),      # an older one from this boot: still the current session
    (900, 14_300, True, True, False),   # the old boot's last STATUS after a gate reboot (20261014-231116)
    (14, 14_300, False, True, False),
    (14, 14_300, True, False, False),
])
def test_baseline_link_is_on_a_status_from_the_gate_s_current_boot(remote_up, gate_up_ms, link_up, verified, ok):
    b = bench_mod.Bench.__new__(bench_mod.Bench)
    b.house = StatusBoard({"link_up": link_up, "remote": {"uptime_s": remote_up}})
    b.gate = StatusBoard({"uptime_ms": gate_up_ms, "link": {"verified": verified}})
    assert bool(b._link_current()) is ok
