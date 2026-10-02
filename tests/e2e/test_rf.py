"""Real RF: the full open/close loop over a marginal link (minimum TX power, SF12).

The rest of the suite simulates outages with net_id; this one puts real RF margin under test. Opt-in, and meant to
be run with something in the path: at the bench with an attenuator in line or the antennas off, and at the install
site (see README "End-to-end tests"):
    pytest tests/e2e -m rf [--rf-cycles 5]
The radio params are applied unsaved and put back afterwards.
"""
import statistics
import time

import pytest

from gatelink.flows import close_via_ctrl, open_via_ctrl

MARGINAL = {"tx_power": 2, "sf": 12}
PINGS = 20
MIN_PONGS = 18  # 90 %: before listen-before-talk (0.3.2) this link managed about 11/20


def _verified(rig):
    return rig.house.status()["link_up"] and rig.house.status()["link"]["verified"] and \
        rig.gate.status()["link"]["verified"]


def _set_radio(rig, params):
    # Gate first, then house: the link is down in between, and each change restarts that board's link.
    for n in ("gate", "house"):
        rig.board(n).config_set(**params[n])
    rig.wait_for(lambda: _verified(rig), 60, f"link verified with {params['house']}")


@pytest.mark.rf
def test_marginal_link(rig, request):
    cycles = request.config.getoption("--rf-cycles")
    rig.expect_commands(2 * cycles)
    for n in ("house", "gate"):
        rig.allow_counters(n)  # a radio change restarts the link, which resets its counters
    before = {n: {k: rig.board(n).config_get()[k] for k in MARGINAL} for n in ("house", "gate")}
    try:
        _set_radio(rig, {"house": MARGINAL, "gate": MARGINAL})
        pongs = []
        for _ in range(PINGS):
            m = rig.mark()
            rig.house.request("radio.ping")
            try:
                pongs.append(rig.wait_for(lambda: rig.timeline.first("house", "pong", m), 4, "pong", poll=0.05))
            except AssertionError:
                pass
            time.sleep(0.5)
        assert len(pongs) >= MIN_PONGS, f"only {len(pongs)}/{PINGS} pongs on the marginal link"
        rig.facts["marginal link"] = f"tx_power {MARGINAL['tx_power']} dBm, SF{MARGINAL['sf']}"
        rig.facts["marginal pings"] = f"{len(pongs)}/{PINGS}, RTT median {statistics.median(p['rtt_ms'] for p in pongs)} ms"
        rig.facts["marginal RSSI/SNR at house"] = (f"{statistics.median(p['rssi'] for p in pongs)} dBm / "
                                                   f"{statistics.median(p['snr'] for p in pongs)} dB")
        rig.facts["marginal RSSI/SNR at gate"] = (f"{statistics.median(p['peer_rssi'] for p in pongs)} dBm / "
                                                  f"{statistics.median(p['peer_snr'] for p in pongs)} dB")
        for i in range(cycles):
            rig.note(f"marginal-link cycle {i + 1}/{cycles}")
            open_via_ctrl(rig, record=False)
            close_via_ctrl(rig, record=False)
        for n in ("house", "gate"):
            link = rig.board(n).status()["link"]
            rig.facts[f"marginal {n} tx/retries/giveups/lbt held/lbt forced"] = (
                f"{link['tx']}/{link['retries']}/{link['giveups']}/{link['lbt_defers']}/{link['lbt_forced']}")
    finally:
        _set_radio(rig, before)
