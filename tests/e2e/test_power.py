"""Real power cuts through the bench power rig (GateSim `rail ... cut`; tools/GateSim/README.md, tools/bench-wiring).

Needs rig CH1 (the gate buck's feed) and CH3 (the house 12 V rail: controller, IN2 opto and house buck) wired. The
boards' LiPos are plugged in or out by hand; the session finds out which with a 1.5 s cut
per board (the `lipo` fixture) and runs the tests for that state, skipping the rest:
- LiPo out: a cut takes the board down (the gate holds ~0.6 s on its buck, the house ~0.1 s): boots, recovery.
- LiPo in: the board rides through on its battery: what the site does while its supply is gone.
The opener (GateSim) keeps its own supply. Select with -m power.

After every test the usual invariants are checked, including "the opener saw no press the gate didn't log": that
is what catches relay chatter while a board powers down or up.
"""
import random
import threading
import time

import pytest

from gatelink.bench import ACT_OPEN, CAUSE, PROFILE_COMMON, SIM_TRAVEL_S
from gatelink.board import BoardError
from gatelink.flows import outage

pytestmark = pytest.mark.power

LINK_TIMEOUT_S = PROFILE_COMMON["link_timeout_s"]
RCAUSE_POWER = 0x07  # PM->RCAUSE: POR, BOD12, BOD33 (the boot event's a)


@pytest.fixture(scope="module")
def lipo(bench):
    """Which boards have their LiPo plugged in: cut each one's supply for 1.5 s (well past its buck's hold-up) and
    see whether it reboots. {"gate": bool, "house": bool}."""
    bench.begin_test("power_lipo_check")
    bench.baseline()
    found = {}
    for site in ("gate", "house"):
        m = bench.mark()
        bench.power_cut(site, 1500)
        time.sleep(5)
        found[site] = not bench.logs(site, "boot", since=m)
        if not found[site]:
            bench.wait_power_return(site, m)
    bench.facts["LiPos"] = ", ".join(f"{s} {'in' if v else 'out'}" for s, v in found.items())
    return found


def require(lipo, **want):
    """Skip unless the LiPos are as the test needs, e.g. require(lipo, gate=False)."""
    for site, fitted in want.items():
        if lipo[site] != fitted:
            pytest.skip(f"needs the {site} LiPo {'plugged in' if fitted else 'out'}")


def sim_lines(rig, since, prefix):
    return [e for e in rig.timeline.select(src="sim", kind="evt", since=since) if e["line"].startswith(prefix)]


def test_gate_power_cut_at_rest(rig, lipo):
    """The gate board loses power for 3 s, less than the house's link timeout: a clean power-on boot, no relay
    chatter at the opener, and the house never notices (the gate's first report waits for steady inputs)."""
    require(lipo, gate=False)
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


def test_gate_power_cut_beyond_link_timeout(rig, lipo):
    """A gate outage longer than the house's link timeout: the house fails the contact sensor open, and everything
    comes back by itself when the gate does. Nothing is commanded."""
    require(lipo, gate=False)
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", (LINK_TIMEOUT_S + 6) * 1000)
    rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + 10)
    rig.wait_house(5, link_up=False, io__k2=False)
    rig.wait_power_return("gate", m, timeout=LINK_TIMEOUT_S + 30)
    rig.wait_log("house", "link_up", since=m, timeout=30)
    rig.wait_house(15, gate="closed", link_up=True, io__k2=True)


def test_gate_power_cut_mid_pulse(rig, lipo):
    """The gate's supply goes while its OPEN relay is on (a 3 s relay test): the relay drops with the power, so the
    opener sees one short press, which still opens the gate. After the reboot nothing pulses again, and the house
    follows the gate open."""
    require(lipo, gate=False)
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


def test_gate_power_bounce(rig, lipo):
    """A flickering supply: five cuts of 0.3-1.2 s, 1-2.5 s apart. Whichever the board rides through and whichever
    reset it, it ends up running on its saved config (from the SPI flash, nothing dropped), with no stray press at
    the opener and nothing commanded."""
    require(lipo, gate=False)
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
def test_gate_power_cut_during_config_save(rig, lipo, lead_ms):
    """A remote config write reaches the gate while its supply is going: the cut starts `lead_ms` before the
    house sends it, and the board goes down ~0.6 s into the cut, so somewhere in the save (~0.5 s, the radio held
    in reset). The saved config afterwards must be the old value or the new one, loaded from the SPI flash with
    nothing dropped: the two alternating sectors keep the old record until the new one is written and verified."""
    require(lipo, gate=False)
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


@pytest.mark.parametrize("cut_ms", [5000, (LINK_TIMEOUT_S + 5) * 1000], ids=["back_mid_travel", "beyond_link_timeout"])
def test_gate_unpowered_opener_moved_by_hand(rig, lipo, cut_ms):
    """The gate board is down while the opener is opened locally (a button, the AES controller): back while it's
    still travelling, or after the house declared the link lost. The board reads the real position once its inputs
    settle and claims nothing (cause none: nothing is attributed to a boot); the house follows the gate open and
    puts the controller on, without a command."""
    require(lipo, gate=False)
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", cut_ms)
    time.sleep(1.0)  # the board holds ~0.6 s on its buck
    rig.sim.open_gate()
    boot = rig.wait_power_return("gate", m, timeout=cut_ms / 1000 + 30)
    assert boot["a"] & RCAUSE_POWER, f"expected a power-on/brownout reset, got RCAUSE {boot['a']:#x}"
    rig.wait_gate("open", timeout=SIM_TRAVEL_S + 15, settling=False)
    st = rig.gate.status()
    assert st["cause"] == "none" and st["target"] == "", f"after the boot: cause {st['cause']}, target {st['target']!r}"
    for e in rig.logs("gate", "gate_state", since=boot["t"]):
        assert e["b"] != CAUSE["lora"], f"gate_state {e['a']} at {e['t']}s claimed as ours"
    rig.expect_no("gate", "pulse", since=m)
    if cut_ms > LINK_TIMEOUT_S * 1000:
        rig.wait_log("house", "link_down", since=m, timeout=1)
    rig.wait_house(20, gate="open", link_up=True, io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)


def test_ac_loss_board_on_ac_supply_no_lipo(rig, lipo):
    """The gate board fed from the AC 24 V supply (`supply psu`) with no LiPo, and AC goes for longer than the
    house's link timeout: the board dies with it while the opener carries on on its battery. The house fails the
    contact sensor open on link loss; when AC returns the board boots, reads AC and the closed limit, and everything
    is back without a command or a press."""
    require(lipo, gate=False)
    rig.expect_commands(0)
    m = rig.mark()
    rig.allow_reboot("gate")
    rig.sim.supply("psu")
    rig.sim.ac(False)
    rig.wait_sim("rail gate off", since=m, timeout=3)
    rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + 10)
    rig.wait_house(5, link_up=False, io__k2=False)
    assert rig.sim.status()["state"] == "closed", "the opener should ride through on its battery"
    rig.expect_no("gate", "boot", since=m)  # no LiPo, no AC: the board must still be down
    m2 = rig.mark()
    rig.sim.ac(True)
    boot = rig.wait_power_return("gate", m2, timeout=30)
    assert boot["a"] & RCAUSE_POWER, f"expected a power-on/brownout reset, got RCAUSE {boot['a']:#x}"
    rig.wait_gate("closed", ac_power=True, settling=False, timeout=15)
    rig.wait_house(20, gate="closed", link_up=True, io__k2=True, io__k1=False, remote__ac_power=True)
    rig.expect_no("gate", "pulse", since=m)


def test_house_power_cut_with_command_pending(rig, lipo):
    """The house loses power between a controller edge and the gate's ACK: the command is pending (the radio is
    out, so it can't be delivered) when the 12 V rail goes, taking the house board and the controller down. After
    the reboot the house has forgotten it (commands live in RAM) and must not send anything from the controller's
    level at boot; the gate never pulses, and the house puts the controller back to the closed gate."""
    require(lipo, house=False)
    rig.expect_commands(1)  # the one before the cut, never delivered
    m = rig.mark()
    with outage(rig):
        rig.ctrl.on()
        rig.wait_log("house", "cmd_sent", a=ACT_OPEN, since=m, timeout=10)
        time.sleep(1.0)  # a retry or two on the air
        rig.wait_house(2, cmd_pending=True)
        cut = rig.mark()
        rig.power_cut("house", 5000)
        time.sleep(1.0)  # the house is down after ~0.1 s: the radio can come back without it
    rig.wait_power_return("house", cut, timeout=40)
    rig.house.config_set(ctrl_power_sense=1)  # the opto is real: keep it on (the profile turns it off)
    rig.wait_house(45, "settled after power return", gate="closed", io__k1=False, io__k2=True, armed=True,
                   cmd_pending=False, sync_window=False, resyncing=False)
    rig.wait_ctrl(False, timeout=45)
    time.sleep(PROFILE_COMMON["cmd_ttl_s"])  # anything still pending anywhere would have landed by now
    rig.expect_no("gate", "cmd_rx", since=m)
    rig.expect_no("gate", "pulse", since=m)
    assert len(rig.logs("house", "cmd_sent", since=m)) == 1, "the house sent a command after its reboot"


@pytest.mark.parametrize("save_lead_ms", [-40, 0, 40, 80, 150, 300])
def test_house_power_cut_during_config_save(rig, lipo, save_lead_ms):
    """A local config.save on the house while its 12 V rail goes: the save is sent `save_lead_ms` before the cut
    (negative: after it), and the board goes down ~0.1 s into the cut, so somewhere in the erase, program or
    read-back. After the boot the config must be the old record or the new one, loaded from the SPI flash with
    nothing dropped: never defaults, never a mix."""
    require(lipo, house=False)
    rig.expect_commands(0)
    rig.resave("house")  # the saved record is now exactly the backup: the "old" config
    old = rig.backup["house"]
    p = "pulse_ms"  # unused by the house: harmless whichever value survives
    rig.house.config_set(**{p: old[p] + 100 if old[p] <= 4900 else old[p] - 100})
    new = rig.house.config_get()  # what the save writes: the test profile plus the changed param
    m = rig.mark()

    def save():
        try:
            rig.house.request("config.save", timeout=3, check=False)
        except BoardError:
            pass  # the board went down before it answered

    try:
        saver = threading.Thread(target=save, daemon=True)
        if save_lead_ms >= 0:
            saver.start()
            time.sleep(save_lead_ms / 1000)
            rig.power_cut("house", 3000)
        else:
            rig.power_cut("house", 3000)
            time.sleep(-save_lead_ms / 1000)
            saver.start()
        saver.join(5)
        rig.wait_power_return("house", m, timeout=40, profile=False)
        cfg = rig.logs("house", "cfg", since=m)[-1]
        assert cfg["a"] == 1 and cfg["b"] == 0, f"config at boot from source {cfg['a']}, {cfg['b']} dropped"
        got = rig.house.config_get()
        if got == old:
            rig.note(f"save lead {save_lead_ms} ms: the old config survived")
        elif got == new:
            rig.note(f"save lead {save_lead_ms} ms: the new config survived")
        else:
            diff_old = {k: (got.get(k), v) for k, v in old.items() if got.get(k) != v}
            diff_new = {k: (got.get(k), v) for k, v in new.items() if got.get(k) != v}
            pytest.fail(f"loaded config is neither the old nor the new record: vs old {diff_old}, vs new {diff_new}")
    finally:
        rig.resave("house")  # put the backup's saved config back, and the profile on top
    rig.wait_house(45, gate="closed", link_up=True, io__k2=True, armed=True)
    rig.wait_ctrl(False, timeout=45)


def test_house_power_cut_gate_open(rig, lipo):
    """The house 12 V rail goes for 5 s with the gate open: the house board and the controller die together and
    come back together. The house must not command the gate from whatever level the controller boots with, and
    ends showing the open gate with the controller on."""
    require(lipo, house=False)
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


def test_house_supply_dips(rig, lipo):
    """Short dips of the house 12 V rail (30-80 ms) with the gate open and the controller on: the controller's
    relay and the IN2 opto may glitch, and the house board may or may not ride them out on its buck; none of it
    may become a command."""
    require(lipo, house=False)
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


def test_both_sites_power_cut(rig, lipo):
    """Both sites lose power at once for 5 s (a neighbourhood outage, LiPos out): each board comes back on its own,
    the link re-forms, and nothing is commanded; the gate is still closed and the sensor reads closed."""
    require(lipo, gate=False, house=False)
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", 5000)
    rig.power_cut("house", 5000)
    rig.wait_power_return("gate", m, timeout=40)
    rig.wait_power_return("house", m, timeout=40)
    rig.wait_house(30, gate="closed", link_up=True, io__k2=True, armed=True)
    rig.wait_ctrl(False, timeout=30)


# --- LiPo in: the boards ride through -------------------------------------------------------------------------------

def no_reboot(rig, site, since):
    boots = rig.logs(site, "boot", since=since)
    assert not boots, f"the {site} board rebooted at {boots[0]['t']}s: its LiPo didn't carry it"


def test_gate_supply_cut_rides_through(rig, lipo):
    """The gate board's supply goes for 10 s while the opener keeps running: the board stays up on its LiPo, keeps
    reading its limits, and the house sees nothing."""
    require(lipo, gate=True)
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", 10000)
    time.sleep(11)
    no_reboot(rig, "gate", m)
    rig.expect_no("gate", "gate_state", since=m)
    rig.expect_no("house", "gate_state", since=m)
    rig.wait_house(5, gate="closed", link_up=True, io__k2=True)


def test_ac_loss_board_on_ac_supply(rig, lipo):
    """The gate board fed from the AC 24 V supply (GateSim `supply psu`) and AC goes: the board rides on its LiPo,
    the opener on its battery. The closed limit is still trusted (the house shows closed), commands are refused, and
    the controller is put back; when AC returns the board's supply does too and nothing moves."""
    require(lipo, gate=True)
    rig.expect_commands(1)
    m = rig.mark()
    rig.sim.supply("psu")
    rig.sim.ac(False)
    rig.wait_gate("closed", ac_power=False, timeout=10)
    rig.wait_house(10, gate="closed", io__k2=True, remote__ac_power=False)
    m2 = rig.mark()
    rig.ctrl.on()
    rig.wait_log("gate", "cmd_refused", since=m2, timeout=15)
    rig.wait_ctrl(False, timeout=PROFILE_COMMON["cmd_ttl_s"] + 45)
    rig.sim.ac(True)
    rig.wait_gate("closed", ac_power=True, timeout=10)
    no_reboot(rig, "gate", m)
    rig.expect_no("gate", "pulse", since=m)


def test_opener_dead_board_on_accessory_supply(rig, lipo):
    """The gate board fed from the opener's accessory output (`supply acc`) and the opener dies (AC and its battery):
    the board rides on its LiPo and reads no_power, the house shows not-closed and nothing is commanded; when the
    opener comes back everything follows the limits again."""
    require(lipo, gate=True)
    rig.expect_commands(0)
    m = rig.mark()
    rig.sim.supply("acc")
    rig.sim.power(False)
    rig.wait_gate("no_power", timeout=10)
    rig.wait_house(10, gate="no_power", io__k2=False, io__k1=True)
    time.sleep(5)
    rig.sim.power(True)
    rig.wait_gate("closed", timeout=15)
    rig.wait_house(15, gate="closed", io__k2=True)
    rig.wait_ctrl(False, timeout=45)
    no_reboot(rig, "gate", m)


def test_house_supply_cut_rides_through(rig, lipo):
    """The house 12 V rail goes for 10 s with the gate open, the house board on its LiPo: the controller's relay
    drops with its supply, the IN2 opto with it (in either order). That edge must not become a command; when power
    returns the controller is put back on."""
    require(lipo, house=True)
    rig.expect_commands(0)
    rig.house.config_set(ctrl_power_sense=1)
    rig.sim.open_gate()
    rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(15, "sync settled", sync_window=False, resyncing=False, ctrl_power=True)
    m = rig.mark()
    rig.power_cut("house", 10000)
    lost = rig.wait_log("house", "ctrl_power", a=0, since=m, timeout=5)
    rig.note(f"ctrl_power 0 with b={lost['b']} (2 = an edge was pending: the relay dropped first)")
    time.sleep(11)
    rig.wait_log("house", "ctrl_power", a=1, since=m, timeout=10)
    no_reboot(rig, "house", m)
    rig.wait_house(45, "settled after power return", gate="open", io__k1=True, io__k2=False,
                   sync_window=False, resyncing=False)
    rig.wait_ctrl(True, timeout=45)


@pytest.mark.xfail(strict=False, reason="TODO.md: a ~300 ms dip reboots the controller without IN2 noticing, so "
                                       "CLOSE then OPEN is sent")
def test_house_supply_dips_ride_through(rig, lipo):
    """Dips of the house 12 V rail from 30 to 300 ms with the house on its LiPo and the gate open: the controller's
    relay and the IN2 opto glitch in every order; the house never reboots and nothing is commanded."""
    require(lipo, house=True)
    rig.expect_commands(0)
    rig.house.config_set(ctrl_power_sense=1)
    rig.sim.open_gate()
    rig.wait_house(SIM_TRAVEL_S + 10, gate="open", io__k1=True, io__k2=False)
    rig.wait_ctrl(True, timeout=30)
    rig.wait_house(15, "sync settled", sync_window=False, resyncing=False)
    m = rig.mark()
    for ms in (30, 80, 150, 300, 50, 200):
        rig.power_cut("house", ms)
        time.sleep(2)
    time.sleep(3)
    no_reboot(rig, "house", m)
    rig.wait_house(45, "settled", gate="open", io__k1=True, io__k2=False, sync_window=False, resyncing=False)
    rig.wait_ctrl(True, timeout=45)


def test_both_supplies_cut_ride_through(rig, lipo):
    """Both boards' supplies go for 10 s, both on their LiPos (the opener keeps its own): both stay up, the link
    holds, nothing is commanded, and the gate still reads closed."""
    require(lipo, gate=True, house=True)
    rig.expect_commands(0)
    m = rig.mark()
    rig.power_cut("gate", 10000)
    rig.power_cut("house", 10000)
    time.sleep(12)
    no_reboot(rig, "gate", m)
    no_reboot(rig, "house", m)
    rig.wait_house(30, gate="closed", link_up=True, io__k2=True)
