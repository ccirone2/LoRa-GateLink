"""Real power cuts through the bench power rig (GateSim `rail ... cut`; tools/GateSim/README.md, tools/bench-wiring).

Needs rig CH1 (the gate buck's feed) and CH3 (the house 12 V rail: controller, IN2 opto and house buck) wired, and
the boards' LiPos out, so a cut takes the board down: the gate rides through ~0.5 s on its buck and goes down at
~0.6 s, the house at ~0.1 s. The opener (GateSim) keeps its own supply. Select with -m power.

After every test the usual invariants are checked, including "the opener saw no press the gate didn't log": that
is what catches relay chatter while a board powers down or up.
"""
import random
import time

import pytest

from gatelink.bench import PROFILE_COMMON, SIM_TRAVEL_S

pytestmark = pytest.mark.power

LINK_TIMEOUT_S = PROFILE_COMMON["link_timeout_s"]
RCAUSE_POWER = 0x07  # PM->RCAUSE: POR, BOD12, BOD33 (the boot event's a)


def sim_lines(rig, since, prefix):
    return [e for e in rig.timeline.select(src="sim", kind="evt", since=since) if e["line"].startswith(prefix)]


def test_gate_power_cut_at_rest(rig):
    """The gate board loses power for 3 s, less than the house's link timeout: a clean power-on boot, no relay
    chatter at the opener, and the house never notices (the gate's first report waits for steady inputs)."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", 3000)
    boot = rig.wait_power_return("gate", m)
    assert boot["a"] & RCAUSE_POWER, f"expected a power-on/brownout reset, got RCAUSE {boot['a']:#x}"
    rig.wait_gate("closed", timeout=15)
    time.sleep(2)
    rig.expect_no("house", "gate_state", since=m)
    rig.expect_no("house", "link_down", since=m)
    rig.wait_house(5, gate="closed", link_up=True, io__k2=True, io__k1=False)


def test_gate_power_cut_beyond_link_timeout(rig):
    """A gate outage longer than the house's link timeout: the house fails the contact sensor open, and everything
    comes back by itself when the gate does. Nothing is commanded."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", (LINK_TIMEOUT_S + 6) * 1000)
    rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + 10)
    rig.wait_house(5, link_up=False, io__k2=False)
    rig.wait_power_return("gate", m, timeout=LINK_TIMEOUT_S + 30)
    rig.wait_log("house", "link_up", since=m, timeout=30)
    rig.wait_house(15, gate="closed", link_up=True, io__k2=True)


def test_gate_power_cut_mid_pulse(rig):
    """The gate's supply goes while its OPEN relay is on (a 3 s relay test): the relay drops with the power, so the
    opener sees one short press, which still opens the gate. After the reboot nothing pulses again, and the house
    follows the gate open."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.relay_test("gate", 1, 3000)
    rig.wait_log("gate", "pulse", a=1, since=m, timeout=5)
    rig.power_cut("gate", 3000)
    rig.wait_power_return("gate", m)
    presses = sim_lines(rig, m, "pulse open")
    assert len(presses) == 1, f"the opener saw {len(presses)} OPEN presses"
    held = int(sim_lines(rig, m, "release open")[0]["line"].split()[2])
    assert held < 2500, f"the OPEN press lasted {held} ms: the power cut should have ended it"
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 10)
    rig.wait_house(15, gate="open", io__k1=True, io__k2=False)


def test_gate_power_bounce(rig):
    """A flickering supply: five cuts of 0.3-1.2 s, 1-2.5 s apart. Whichever the board rides through and whichever
    reset it, it ends up running on its saved config (from the SPI flash, nothing dropped), with no stray press at
    the opener and nothing commanded."""
    rig.expect_commands(0)
    m = rig.mark()
    rng = random.Random(7)
    for _ in range(5):
        ms = rng.randint(300, 1200)
        rig.power_cut("gate", ms)
        time.sleep(ms / 1000 + rng.uniform(1.0, 2.5))
    boots = rig.logs("gate", "boot", since=m)
    assert boots, "no reset in five cuts up to 1.2 s: is the gate's LiPo still fitted?"
    rig.wait_power_return("gate", boots[-1]["t"] - 0.1)
    for e in rig.logs("gate", "cfg", since=m):
        assert e["a"] == 1 and e["b"] == 0, f"config at boot from source {e['a']} with {e['b']} dropped (1, 0 = SPI)"
    rig.note(f"{len(boots)} resets in 5 cuts")
    rig.wait_gate("closed", timeout=15)
    rig.wait_house(15, gate="closed", io__k2=True)


@pytest.mark.parametrize("lead_ms", [450, 400, 350, 300, 150, 0])
def test_gate_power_cut_during_config_save(rig, lead_ms):
    """A remote config write reaches the gate while its supply is going: the cut starts `lead_ms` before the
    house sends it, and the board goes down ~0.6 s into the cut, so somewhere in the save (~0.5 s, the radio held
    in reset). The saved config afterwards must be the old value or the new one, loaded from the SPI flash with
    nothing dropped: the two alternating sectors keep the old record until the new one is written and verified."""
    rig.expect_commands(0)
    old = rig.backup["gate"]["travel_timeout_s"]  # what the gate has saved
    new = old + 1 if old < 300 else old - 1
    m = rig.mark()
    try:
        rig.power_cut("gate", 3000)
        time.sleep(lead_ms / 1000)
        rig.house.request("remote.set", name="travel_timeout_s", value=new)
        rig.wait_power_return("gate", m, profile=False)
        cfg = rig.logs("gate", "cfg", since=m)[-1]
        assert cfg["a"] == 1 and cfg["b"] == 0, f"config at boot from source {cfg['a']}, {cfg['b']} dropped"
        saved = rig.gate.config_get()["travel_timeout_s"]
        assert saved in (old, new), f"saved travel_timeout_s is {saved}: neither the old {old} nor the new {new}"
        rig.note(f"lead {lead_ms} ms: the {'new' if saved == new else 'old'} value survived")
    finally:
        rig.resave("gate")  # put the backup's saved config back, and the profile on top


def test_house_power_cut_gate_open(rig):
    """The house 12 V rail goes for 5 s with the gate open: the house board and the controller die together and
    come back together. The house must not command the gate from whatever level the controller boots with, and
    ends showing the open gate with the controller on."""
    rig.expect_commands(0)
    rig.sim.open_gate()
    rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(15, "sync settled", sync_window=False, resyncing=False)
    m = rig.mark()
    rig.power_cut("house", 5000)
    rig.wait_power_return("house", m, timeout=40)
    rig.house.config_set(ctrl_power_sense=1)  # the opto is real: keep it on (the profile turns it off)
    rig.wait_house(45, "settled after power return", gate="open", io__k1=True, io__k2=False, armed=True,
                   sync_window=False, resyncing=False)
    rig.wait_ctrl(True, timeout=45)


def test_house_supply_dips(rig):
    """Short dips of the house 12 V rail (30-80 ms) with the gate open and the controller on: the controller's
    relay and the IN2 opto may glitch, and the house board may or may not ride them out on its buck; none of it
    may become a command."""
    rig.expect_commands(0)
    rig.house.config_set(ctrl_power_sense=1)
    rig.sim.open_gate()
    rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(15, "sync settled", sync_window=False, resyncing=False)
    m = rig.mark()
    for ms in (30, 50, 80, 30, 60):
        rig.power_cut("house", ms)
        time.sleep(1.5)
    time.sleep(3)
    if rig.logs("house", "boot", since=m):
        rig.wait_power_return("house", rig.logs("house", "boot", since=m)[-1]["t"] - 0.1, timeout=40)
        rig.house.config_set(ctrl_power_sense=1)
    rig.wait_house(45, "settled", gate="open", io__k1=True, io__k2=False, sync_window=False, resyncing=False)
    rig.wait_ctrl(True, timeout=45)


def test_both_sites_power_cut(rig):
    """Both sites lose power at once for 5 s (a neighbourhood outage, LiPos out): each board comes back on its own,
    the link re-forms, and nothing is commanded; the gate is still closed and the sensor reads closed."""
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", 5000)
    rig.power_cut("house", 5000)
    rig.wait_power_return("gate", m, timeout=40)
    rig.wait_power_return("house", m, timeout=40)
    rig.wait_house(30, gate="closed", link_up=True, io__k2=True, armed=True)
    rig.wait_ctrl(False, timeout=30)
