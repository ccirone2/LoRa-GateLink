"""The site survey: tools/gatelink_client/survey.py against the test vectors web/js/survey.js is checked against too
(fixtures/survey-vectors.json, tests/web/unit/survey.test.js), so the CLI and the web console judge a link alike;
and `gatelink.py survey` against a fake board and a fake clock."""

import json
import math
from pathlib import Path

import pytest

from gatelink_client import survey
from gatelink_client.timeline import Timeline

ROOT = Path(__file__).resolve().parents[2]
VECTORS = json.loads((Path(__file__).parent / "fixtures/survey-vectors.json").read_text(encoding="utf-8"))
FIRMWARE = json.loads((ROOT / "tests/web/fixtures/firmware.json").read_text(encoding="utf-8"))


def close(actual, expected, path="value"):
    """Equal, numbers to 1e-9 (both languages compute in IEEE doubles, but the vectors are JSON text)."""
    if isinstance(expected, bool) or expected is None or isinstance(expected, str):
        assert actual == expected, path
    elif isinstance(expected, (int, float)):
        assert survey.is_num(actual) and math.isclose(actual, expected, abs_tol=1e-9), f"{path}: {actual} != {expected}"
    elif isinstance(expected, dict):
        assert sorted(actual) == sorted(expected), path
        for k, v in expected.items():
            close(actual[k], v, f"{path}.{k}")
    else:
        assert len(actual) == len(expected), path
        for i, (a, e) in enumerate(zip(actual, expected, strict=True)):
            close(a, e, f"{path}[{i}]")


# ---- the shared vectors ----------------------------------------------------------------------------------------


def test_snr_floors():
    close({str(k): v for k, v in survey.SNR_FLOOR.items()}, VECTORS["floors"], "floors")


@pytest.mark.parametrize("v", VECTORS["fmt1"], ids=lambda v: str(v["x"]))
def test_fmt1(v):
    assert survey.fmt1(v["x"]) == v["out"]


@pytest.mark.parametrize("v", VECTORS["margins"], ids=lambda v: f"sf{v['sf']}-snr{v['snr']}-noise{v['noise']}")
def test_sample_margin(v):
    close(survey.sample_margin(v["sf"], v["snr"], v["rssi"], v["noise"]), v["margin"])


@pytest.mark.parametrize("v", VECTORS["quantiles"], ids=lambda v: f"{v['values']}@{v['q']}")
def test_quantile(v):
    close(survey.quantile(v["values"], v["q"]), v["out"])


@pytest.mark.parametrize("case", VECTORS["cases"], ids=lambda c: c["name"])
def test_case(case):
    r = survey.analyze(case["settings"], case["samples"], case["role"])
    close(r["summary"], case["summary"], "summary")
    assert r["verdict"] == case["verdict"]
    assert r["headline"] == case["headline"]
    assert r["advice"] == case["advice"]
    # The command line prints them: plain ASCII only.
    for text in [r["headline"], *r["advice"]]:
        assert text.isascii(), text


def test_limits_are_the_firmwares():
    meta = {m["name"]: m for m in FIRMWARE["meta"]}
    assert (survey.TX_POWER_MIN, survey.TX_POWER_MAX) == (meta["tx_power"]["min"], meta["tx_power"]["max"])
    assert survey.SF_MAX == meta["sf"]["max"]
    assert all(survey.snr_floor(sf) is not None for sf in range(meta["sf"]["min"], meta["sf"]["max"] + 1))


def test_samples_from_pongs_and_status():
    house = {"role": "house", "link": {"noise": -118}, "remote": {"noise": -117}}
    assert survey.noise_from_status(house) == {"noise": -118, "peer_noise": -117}
    # The gate doesn't know the house's noise floor; null before the first reading.
    assert survey.noise_from_status({"role": "gate", "link": {"noise": None}, "remote": {"noise": -117}}) == \
        {"noise": None, "peer_noise": None}
    pong = {"ping_id": 4, "rtt_ms": 142, "rssi": -61, "snr": 9.25, "peer_rssi": -60, "peer_snr": 9, "fei": -1450}
    assert survey.sample_from_pong(pong, 2.0, {"noise": -118, "peer_noise": None}) == {
        "t": 2.0, "lost": False, "rtt_ms": 142, "rssi": -61, "snr": 9.25, "peer_rssi": -60, "peer_snr": 9,
        "fei": -1450, "noise": -118, "peer_noise": None}
    assert survey.sample_from_pong(None, 4.0) == {"t": 4.0, "lost": True, "noise": None, "peer_noise": None}


def test_text_report_is_ascii_and_has_the_table():
    case = VECTORS["cases"][0]
    r = survey.analyze(case["settings"], case["samples"], case["role"])
    text = survey.format_text({"time": "2026-10-10T14:03:00+02:00", "board": {"role": "house", "fw": "0.14.0",
                               "port": "COM5"}, "settings": case["settings"], "elapsed_s": 50.2, "stopped": None, **r})
    assert text.isascii()
    lines = text.splitlines()
    assert "Board: house, firmware 0.14.0, COM5" in lines
    assert "Radio: SF9, 500 kHz, tx_power 17 dBm on this board" in lines
    assert "Ran 0:50: 25 pings, 0 unanswered (0.0 % loss)" in lines
    assert any(ln.startswith("at the house") and "67.5 / 67.5 dB" in ln for ln in lines)
    assert any(ln.startswith("at the gate") for ln in lines)
    assert r["headline"] in lines
    assert f"- {r['advice'][0]}" in lines


# ---- gatelink.py survey ---------------------------------------------------------------------------------------

gatelink = pytest.importorskip("gatelink")  # needs pyserial, as the tools do


class Clock:
    def __init__(self):
        self.t = 1000.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += max(s, 0)


class FakeBoard:
    """Answers the console like a linked board. `answer(i)` gives the pong for ping i (from 0) as a dict of its
    levels, or None for no answer; a pong is recorded on the timeline as Board does, after 0.15 s of fake time.
    `late(i)`, if given, gives the levels of the previous ping's pong landing late, just before ping i reaches the
    board (it only makes sense after an unanswered ping: the firmware reports a ping's pong at most once)."""

    def __init__(self, clock, role="house", answer=None, verified=True, noise=-118, peer_noise=-117, sf=9,
                 tx_power=17, late=None):
        self.clock, self.name, self.port, self.timeline = clock, role, "COM99", Timeline()
        self.answer = answer or (lambda i: {"rssi": -61, "snr": 9.5, "peer_rssi": -60, "peer_snr": 9})
        self.late = late or (lambda i: None)
        self.verified, self.noise, self.peer_noise = verified, noise, peer_noise
        self.params = {"role": 1 if role == "house" else 2, "sf": sf, "bw_hz": 500000, "tx_power": tx_power}
        self.pings = 0
        self.requests = []
        self.closed = False

    def info(self):
        return {"role": self.name, "fw": "0.14.0", "key_set": True}

    def status(self):
        self.requests.append("status")
        return {"role": self.name, "link": {"verified": self.verified, "noise": self.noise},
                "remote": {"noise": self.peer_noise}}

    def config_get(self):
        return dict(self.params)

    def request(self, cmd, timeout=3.0, check=True, **kw):
        self.requests.append(cmd)
        if cmd == "radio.ping":
            late = self.late(self.pings)
            if late is not None:  # ping ids count from 1: the previous ping's is the count so far
                self.timeline.add(self.name, "pong", ping_id=self.pings, rtt_ms=8100, fei=-1450, **late)
            levels = self.answer(self.pings)
            self.pings += 1
            if levels is not None:
                self.clock.sleep(0.15)
                self.timeline.add(self.name, "pong", ping_id=self.pings, rtt_ms=150, fei=-1450, **levels)
        return {"ok": True}

    def close(self):
        self.closed = True


def test_run_survey_paces_pings_and_times_out_lost_ones():
    clock = Clock()
    b = FakeBoard(clock, answer=lambda i: None if i == 3 else {"rssi": -70 - i, "snr": 9.0, "peer_rssi": -71,
                                                                   "peer_snr": 8})
    samples, elapsed, stopped = gatelink.run_survey(b, 30, 2, clock=clock.now, sleep=clock.sleep,
                                                    progress=lambda s: None)
    assert stopped is None
    # 0, 2, 4, 6 (lost: given up at 14), then 14, 16, ... 28: one ping at a time (to the 20 ms polling step).
    assert [round(s["t"]) for s in samples] == [0, 2, 4, 6, 14, 16, 18, 20, 22, 24, 26, 28]
    assert [s["lost"] for s in samples].count(True) == 1 and samples[3]["lost"]
    assert abs(samples[4]["t"] - samples[3]["t"] - gatelink.SURVEY_PING_TIMEOUT_S) < 0.05
    assert samples[1]["rssi"] == -71 and samples[0]["noise"] == -118 and samples[0]["peer_noise"] == -117
    assert b.pings == 12
    # The noise floors are re-read every 10 s, not before every ping.
    assert b.requests.count("status") == 3  # at 0, 14 and 24
    assert 28 < elapsed < 29


def test_run_survey_stops_on_ctrl_c_and_keeps_what_it_has():
    clock = Clock()

    def answer(i):
        if i == 5:
            raise KeyboardInterrupt
        return {"rssi": -70, "snr": 9.0, "peer_rssi": -71, "peer_snr": 8}

    b = FakeBoard(clock, answer=answer)
    samples, _, stopped = gatelink.run_survey(b, 60, 2, clock=clock.now, sleep=clock.sleep, progress=lambda s: None)
    assert stopped == "stopped early"
    assert len(samples) == 5


def test_run_survey_skips_the_previous_pings_late_pong():
    # Pings 1 and 3 go unanswered, and each one's pong lands late (weak, -120 dBm), just before the next ping reaches
    # the board. Neither is the next ping's answer: ping 2 takes its own pong, and ping 4, unanswered, is lost.
    clock = Clock()
    weak = {"rssi": -120, "snr": -5.0, "peer_rssi": -121, "peer_snr": -6}
    b = FakeBoard(clock, answer=lambda i: None if i in (1, 3, 4) else {"rssi": -70, "snr": 9.0, "peer_rssi": -71,
                                                                       "peer_snr": 8},
                  late=lambda i: weak if i in (2, 4) else None)
    samples, _, _ = gatelink.run_survey(b, 40, 2, clock=clock.now, sleep=clock.sleep, progress=lambda s: None)
    assert [s["lost"] for s in samples[:6]] == [False, True, False, True, True, False]
    assert all(s.get("rssi") != -120 for s in samples)


def test_run_survey_keeps_what_it_has_when_the_board_fails():
    clock = Clock()

    def answer(i):
        if i == 4:
            raise gatelink.BoardError("house: no reply to 'radio.ping' within 3.0s")
        return {"rssi": -70, "snr": 9.0, "peer_rssi": -71, "peer_snr": 8}

    b = FakeBoard(clock, answer=answer)
    samples, _, stopped = gatelink.run_survey(b, 60, 2, clock=clock.now, sleep=clock.sleep, progress=lambda s: None)
    assert stopped == "failed: house: no reply to 'radio.ping' within 3.0s"
    assert len(samples) == 4


@pytest.fixture
def cli(monkeypatch, tmp_path):
    """Runs `gatelink.py survey ...` against a fake board on a fake clock; returns (exit code, board, report)."""

    def run(argv, board_kw=None, found=None):
        clock = Clock()
        board = FakeBoard(clock, **(board_kw or {}))
        real = gatelink.run_survey
        monkeypatch.setattr(gatelink, "run_survey",
                            lambda b, s, i: real(b, s, i, clock=clock.now, sleep=clock.sleep, progress=lambda m: None))
        monkeypatch.setattr(gatelink, "port_serials", lambda: {"COM99": "ABC183013"})
        opened = []
        monkeypatch.setattr(gatelink, "open_target", lambda target: opened.append(target) or board)
        if found is not None:
            monkeypatch.setattr(gatelink, "find_boards", lambda tl: found(board))
        out = tmp_path / "survey.json"
        code = gatelink.main(["survey", *argv, "--json", str(out)])
        return code, board, json.loads(out.read_text(encoding="utf-8")), opened

    return run


def test_cli_good_link_exits_0_and_writes_the_report(cli, capsys):
    code, board, report, opened = cli(["house", "--seconds", "60", "--interval", "2"])
    assert code == 0 and opened == ["house"] and board.closed
    assert report["verdict"] == "good"
    assert report["settings"] == {"sf": 9, "bw_hz": 500000, "tx_power": 17}
    assert report["board"] == {"role": "house", "fw": "0.14.0", "port": "COM99", "usb_serial": "ABC183013"}
    assert len(report["samples"]) == 30 and report["summary"]["sent"] == 30
    assert report["seconds"] == 60 and report["interval_s"] == 2 and report["stopped"] is None
    assert report["time"][:2] == "20" and "T" in report["time"]
    out = capsys.readouterr().out
    assert "site survey from the house board on COM99 (SF9, 500 kHz, tx_power 17 dBm)" in out
    assert report["headline"] in out
    assert out.isascii()


def test_cli_marginal_link_exits_1(cli):
    code, _, report, _ = cli(["gate", "--seconds", "20"], board_kw={
        "role": "gate", "sf": 12, "tx_power": 20, "noise": -125,
        "answer": lambda i: {"rssi": -132, "snr": -13.0, "peer_rssi": -133, "peer_snr": -14}})
    assert code == 1
    assert report["verdict"] == "marginal"
    assert "tx_power and sf are at their limits" in " ".join(report["advice"])


def test_cli_fair_link_exits_0(cli):
    # SF9, SNR 0 both ways: 12.5 dB in hand.
    code, _, report, _ = cli(["house", "--seconds", "20"], board_kw={
        "answer": lambda i: {"rssi": -110, "snr": 0.0, "peer_rssi": -111, "peer_snr": 0}})
    assert code == 0
    assert report["verdict"] == "fair"


def test_cli_board_failure_reports_what_it_has_and_exits_1(cli, capsys):
    def answer(i):
        if i == 10:
            raise gatelink.BoardError("house: no reply to 'radio.ping' within 3.0s")
        return {"rssi": -61, "snr": 9.5, "peer_rssi": -60, "peer_snr": 9}

    code, _, report, _ = cli(["house", "--seconds", "60"], board_kw={"answer": answer})
    assert code == 1
    assert report["verdict"] == "good" and report["summary"]["sent"] == 10
    assert report["stopped"].startswith("failed: ")
    assert "(failed: house: no reply" in capsys.readouterr().out


def test_cli_lossy_link_is_poor(cli):
    code, _, report, _ = cli(["house", "--seconds", "100"], board_kw={
        "answer": lambda i: None if i % 10 == 9 else {"rssi": -61, "snr": 9.5, "peer_rssi": -60, "peer_snr": 9}})
    assert code == 1
    assert report["verdict"] == "poor" and report["summary"]["lost"] > 0


def test_cli_needs_a_verified_link(cli):
    with pytest.raises(SystemExit, match="link not verified"):
        cli(["house"], board_kw={"verified": False})


def test_cli_without_a_target_takes_the_house_when_both_are_on_usb(cli, monkeypatch):
    others = []

    class Other:
        name = "gate"

        def close(self):
            others.append("closed")

    code, board, report, opened = cli(["--seconds", "10"], found=lambda b: {"gate": Other(), "house": b})
    assert code == 0 and opened == [] and report["board"]["role"] == "house" and others == ["closed"]
