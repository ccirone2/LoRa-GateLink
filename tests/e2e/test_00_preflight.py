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


def test_bench_safe_settings(bench):
    for name in ("house", "gate"):
        p = bench.backup[name]
        # Full-power TX with a relay energized crashed USB-powered boards into watchdog resets.
        assert p["tx_power"] <= 5, f"{name} tx_power {p['tx_power']}: keep it at about 5 on USB power"
    assert bench.backup["house"]["in1_invert"] == 0, "house IN1 is the real controller relay: leave in1_invert 0"


def test_ping(bench):
    m = bench.mark()
    bench.house.request("radio.ping")
    pong = bench.wait_for(lambda: bench.timeline.first("house", "pong", m), 5, "pong from the gate", poll=0.05)
    bench.facts["ping RTT"] = f"{pong['rtt_ms']} ms"
    bench.facts["RSSI/SNR at house"] = f"{pong['rssi']} dBm / {pong['snr']} dB"
    bench.facts["RSSI/SNR at gate"] = f"{pong['peer_rssi']} dBm / {pong['peer_snr']} dB"


def test_simulator(bench):
    st = bench.sim.status()
    # The relays should match the simulated state: power relay on, and the closed relay on only when closed.
    assert st["power"] == "1", "simulator reports no power: send `power on`"
    if st["state"] == "closed":
        assert st["relay_closed"] == "1", "closed but the closed-limit relay is off: check `polarity`"
        time.sleep(0.5)
        gs = bench.gate.status()
        assert gs["io"]["in2"] and gs["io"]["in3"], (
            f"simulator closed and powered but the gate sees in2={gs['io']['in2']} in3={gs['io']['in3']}: "
            "check the opto wiring and relay polarity")


def test_controller(bench):
    assert bench.ctrl.state() in ("on", "off"), "controller unavailable in Home Assistant"
