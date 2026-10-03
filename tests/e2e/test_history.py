"""Link quality history (hist.get / hist.clear): bucket counters against the status totals, bucket rollover and
paging, link-down time across an outage, and the gate's counters as the house records them from STATUS."""
import time

from gatelink.bench import PROFILE_COMMON
from gatelink.flows import outage

HEARTBEAT_S = PROFILE_COMMON["heartbeat_s"]
LINK_TIMEOUT_S = PROFILE_COMMON["link_timeout_s"]
PERIOD_S = 3600  # firmware default (history.h HIST_PERIOD_S)
PAGE = 12  # history.h HIST_PAGE
PINGS = 10


def _current(board):
    buckets, head = board.history()
    assert buckets and buckets[-1]["idx"] == head["current"], f"{board.name}: no bucket in progress ({head})"
    return buckets[-1]


def _clear(rig, period_s=PERIOD_S):
    for b in (rig.house, rig.gate):
        b.request("hist.clear", period_s=period_s)


def test_history_counts(rig):
    """The bucket in progress counts what the status totals count, with levels and both directions filled in."""
    s0 = {n: rig.board(n).status()["link"] for n in ("house", "gate")}
    _clear(rig)
    m = rig.mark()
    pongs = [p for p in (rig.ping(required=False) for _ in range(PINGS)) if p]
    assert len(pongs) >= PINGS - 2, f"only {len(pongs)}/{PINGS} pongs"
    # A STATUS with the gate's view after the clear (one per heartbeat). Its noise covers the time since the
    # previous one, which can have had no reading kept during the pings: then the next heartbeat brings one.
    rig.wait_for(lambda: _current(rig.house)["peer_noise_avg"] is not None, 2 * HEARTBEAT_S + 5,
                 "a gate STATUS with its noise floor in the house history")
    h, g = _current(rig.house), _current(rig.gate)
    s1 = {n: rig.board(n).status()["link"] for n in ("house", "gate")}

    for name, b in (("house", h), ("gate", g)):
        grown = {k: s1[name][k] - s0[name][k] for k in ("tx", "rx", "retries")}
        assert b["idx"] == 0, f"{name}: hist.clear should restart at bucket 0: {b}"
        # Everything since the clear is in the bucket; the status totals also cover the moments before it.
        assert len(pongs) <= b["rx"] <= grown["rx"], f"{name}: rx {b['rx']}, pongs {len(pongs)}, status +{grown['rx']}"
        assert len(pongs) <= b["tx"] <= grown["tx"], f"{name}: tx {b['tx']}, status +{grown['tx']}"
        assert b["retries"] <= grown["retries"]
        assert b["down_s"] == 0, f"{name}: link counted down on a healthy link: {b}"
        assert b["rssi_min"] is not None and b["rssi_min"] <= b["rssi_avg"] < 0, f"{name}: RSSI {b}"
        assert b["snr_min"] <= b["snr_avg"], f"{name}: SNR {b}"
        assert b["noise_avg"] is not None and -140 < b["noise_avg"] < -40, f"{name}: noise floor {b}"
        # Not compared with the RSSI: while pinging, most readings are dropped (a frame followed them), and a
        # burst from a nearby radio can dominate the few left (the bench house sees peaks above our frames).
        assert b["noise_max"] >= b["noise_avg"] - 0.25

    # The gate's side, as the house records it from STATUS.
    assert h["peer_rssi_min"] <= h["peer_rssi_avg"] < 0, f"house peer RSSI {h}"
    assert h["peer_noise_avg"] is not None and -140 < h["peer_noise_avg"] < -40, f"house peer noise {h}"
    assert g["peer_n"] == 0, f"the gate gets no STATUS, so no peer columns: {g}"

    st = rig.house.status()
    for k in ("retries", "giveups", "crc_err", "noise"):
        assert k in st["remote"], f"house status remote.{k} missing (gate STATUS without the 0.4.0 fields?)"
    for n in ("house", "gate"):
        link = rig.board(n).status()["link"]
        assert isinstance(link["crc_err"], int) and link["noise"] is not None, f"{n} status link: {link}"
    rig.facts["noise floor (house / gate)"] = f"{h['noise_avg']} / {g['noise_avg']} dBm"
    rig.expect_no("house", "link_down", since=m)


def test_history_rollover_and_paging(rig):
    """With a 60 s bucket the first one closes on time, the next starts, and hist.get pages by `from`/`n`."""
    try:
        _clear(rig, 60)
        time.sleep(62)
        buckets, head = rig.house.history()
        assert head["period_s"] == 60 and head["current"] == 1 and head["oldest"] == 0, head
        assert [b["idx"] for b in buckets] == [0, 1]
        first = buckets[0]
        # A whole minute of a healthy link: a STATUS every heartbeat, from the gate and acked by us.
        assert first["peer_n"] >= 60 // HEARTBEAT_S - 2, f"STATUS count in a minute: {first}"
        assert first["rx"] >= first["peer_n"] and first["tx"] >= first["peer_n"] - 2, first
        assert first["down_s"] == 0, first

        one = rig.house.request("hist.get", **{"from": 0, "n": 1})
        assert [r[0] for r in one["rows"]] == [0]
        past = rig.house.request("hist.get", **{"from": 5})
        assert past["rows"] == [] and past["current"] == 1
        page = rig.house.request("hist.get", n=PAGE + 10)  # clamped to a page
        assert len(page["rows"]) <= PAGE
        bad = rig.house.request("hist.clear", period_s=30, check=False)
        assert not bad["ok"], "hist.clear accepted a period under 60 s"
    finally:
        _clear(rig)


def test_history_outage(rig):
    """An outage shows as link-down seconds on both boards, and the gate's lost STATUS frames as its retries on
    the house's record."""
    _clear(rig)
    m = rig.mark()
    with outage(rig):
        down = rig.wait_log("house", "link_down", since=m, timeout=LINK_TIMEOUT_S + HEARTBEAT_S + 5)
        time.sleep(3)
    up = rig.wait_log("house", "link_up", since=down["t"], timeout=HEARTBEAT_S + 10)
    # The STATUS after the outage carries the gate's retries; one more heartbeat makes sure it's in.
    rig.wait_for(lambda: _current(rig.house)["peer_retries"] >= 1, HEARTBEAT_S + 5,
                 "the gate's STATUS retries in the house history")
    h, g = _current(rig.house), _current(rig.gate)
    gap = up["t"] - down["t"]
    assert abs(h["down_s"] - gap) <= 2, f"house down_s {h['down_s']}, link was down {gap:.1f} s"
    assert g["down_s"] >= 1, f"gate down_s {g['down_s']}"
    assert g["retries"] >= 1, f"gate STATUS retries during the outage: {g}"
    # Not compared with the gate's bucket: the house counts from the last STATUS before its clear.
    assert h["peer_retries"] >= 1, f"house peer_retries {h['peer_retries']}, gate retries {g['retries']}"
