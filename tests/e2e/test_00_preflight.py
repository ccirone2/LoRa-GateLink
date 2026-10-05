"""Is the bench fit to test? If anything here fails, the scenario tests are skipped."""
import time


def test_boards_and_link(bench):
    h, g = bench.house.info(), bench.gate.info()
    assert h["role"] == "house" and g["role"] == "gate"
    assert h["fw"] == g["fw"], f"firmware differs: house {h['fw']}, gate {g['fw']}"
    assert h["key_set"] and g["key_set"], "set the link key on both boards"
    bench.wait_for(lambda: bench.house.status()["link"]["verified"] and bench.gate.status()["link"]["verified"],
                   30, "peer verified on both boards")
    bench.facts["firmware"] = h["fw"]
    # From 0.5.0 config lives in the SPI flash chip; "internal" means it's lost on the next upload.
    bench.facts["config store"] = f"house {h.get('cfg_store', 'internal')}, gate {g.get('cfg_store', 'internal')}"


def test_bench_safe_settings(bench):
    for name in ("house", "gate"):
        p = bench.backup[name]
        # Full-power TX with a relay energized crashed USB-powered boards into watchdog resets.
        assert p["tx_power"] <= 5, f"{name} tx_power {p['tx_power']}: keep it at about 5 on USB power"
    # The suite runs with every invert at 0 and puts the saved values back afterwards, so a stray invert would
    # never fail a scenario. Inverted, a dead opto or cut wire reads active (e.g. a closed limit): keep them 0.
    inverted = [f"{name} {k}" for name in ("house", "gate") for k in ("in1_invert", "in2_invert", "in3_invert",
                                                                       "in4_invert") if bench.backup[name].get(k)]
    assert not inverted, f"set back to 0 and save: {', '.join(inverted)} (fix reversed signals in the wiring)"


def test_ping(bench):
    pong = bench.ping("house")
    bench.facts["ping RTT"] = f"{pong['rtt_ms']} ms"
    bench.facts["RSSI/SNR at house"] = f"{pong['rssi']} dBm / {pong['snr']} dB"
    bench.facts["RSSI/SNR at gate"] = f"{pong['peer_rssi']} dBm / {pong['peer_snr']} dB"


def test_simulator(bench):
    st = bench.sim.status()
    # The relays should match the simulated state: power relay on, and the closed relay on only when closed.
    assert st["power"] == "1", "simulator reports no power: send `power on`"
    if st["state"] == "closed":
        assert st["relay_closed"] == "1", "closed but the closed-limit signal is off: check `polarity`"
        time.sleep(0.5)
        gs = bench.gate.status()
        assert gs["io"]["in2"] and gs["io"]["in3"], (
            f"simulator closed and powered but the gate sees in2={gs['io']['in2']} in3={gs['io']['in3']}: "
            "check the opto wiring (closed limit on D3's NC contact) and relay polarity")


def test_controller(bench):
    assert bench.ctrl.state() in ("on", "off"), "controller unavailable in Home Assistant"


# (level mid-pulse, level after) when K1 is pulsed while the controller is on and K1 off
SW_MODES = {
    (True, False): None,  # follows the SW level: stays on while K1 is on, off with it
    (False, True): "toggles on every SW edge",
    (False, False): "toggles on the rising SW edge only (momentary / push-button)",
    (True, True): "ignores the SW input (detached)",
}


def test_controller_follow_mode(bench):
    """The controller's SW input must follow K1's level. Normal-operation tests expect no resync after K1 changes,
    and in the field a controller that toggles on SW edges would turn every sync into a reverse command."""
    b = bench
    b.begin_test("preflight_controller_mode")
    b.baseline()
    pulse_ms = 2000
    try:
        # The house must ignore the controller's edges here: have it read the controller as unpowered.
        b.power.fake(True)
        b.house.config_set(ctrl_power_sense=1)
        b.wait_house(5, ctrl_power=False)
        b.ctrl.on()
        b.wait_ctrl(True, timeout=15)
        time.sleep(1)
        b.relay_test("house", 1, pulse_ms)
        time.sleep(pulse_ms / 2000)
        mid = b.house.status()["ctrl"]
        time.sleep(pulse_ms / 2000 + 2)
        after = b.house.status()["ctrl"]
        problem = SW_MODES[(mid, after)]
        b.facts["controller SW mode"] = problem or "follows the SW level"
        assert problem is None, (
            f"the controller {problem} (K1 pulse with it on: {'on' if mid else 'off'} mid-pulse, "
            f"{'on' if after else 'off'} after). Set its switch input so the relay follows the input level.")
    finally:
        if b.house.status()["ctrl"]:
            b.ctrl.off()  # still read as unpowered: ignored
            b.wait_ctrl(False, timeout=15)
        b.apply_profile("house")
