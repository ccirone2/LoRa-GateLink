"""Tests for tools/logic.py (logic analyzer captures through sigrok-cli; no analyzer needed)."""

import json

import pytest

import logic

# As sigrok-cli 0.8 writes it: FRAME-END glued to the last value change, times in 10 us steps at 100 kHz.
VCD = """\
$date Sun Oct 11 22:21:11 2026 $end
$version libsigrok 0.6.0-git-883c2ac $end
$comment
  Acquisition with 2/8 channels at 100 kHz
$end
$timescale 10 us $end
$scope module libsigrok $end
$var wire 1 ! D0 $end
$var wire 1 " D4 $end
$upscope $end
$enddefinitions $end

#0  0! 1"
#100000  1!
#100002  0!
#150000  0"
#350000  1"FRAME-END
FRAME-BEGIN

#500000
"""


def test_channel_specs():
    assert logic.parse_channel("D5=CH5") == ("D5", "D4")
    assert logic.parse_channel("K1=d0") == ("K1", "D0")
    for bad in ("D5", "D5=CH0", "D5=CH9", "D5=D8", "=CH1", "D5=X1"):
        with pytest.raises(ValueError):
            logic.parse_channel(bad)


def test_at_specs():
    assert logic.parse_at("2.5:house reboot") == (2.5, ["house", "reboot"])
    assert logic.parse_at('1:house config.set params={"x":1}') == (1.0, ["house", "config.set", 'params={"x":1}'])
    for bad in ("house reboot", "x:house", "-1:house reboot", "2:"):
        with pytest.raises(ValueError):
            logic.parse_at(bad)


def test_parse_vcd():
    changes, end = logic.parse_vcd(VCD)
    assert end == pytest.approx(5.0)
    assert changes["D0"] == [(0.0, 0), (pytest.approx(1.0), 1), (pytest.approx(1.00002), 0)]
    # The rise glued to FRAME-END counts.
    assert changes["D4"] == [(0.0, 1), (pytest.approx(1.5), 0), (pytest.approx(3.5), 1)]


def test_parse_vcd_rejects_other_files():
    with pytest.raises(ValueError):
        logic.parse_vcd("not a vcd")
    with pytest.raises(ValueError):
        logic.parse_vcd("$timescale 10 us $end\n$enddefinitions $end\n#0\n")


def test_analyze_flags_short_pulses_only_inside_the_capture():
    changes, end = logic.parse_vcd(VCD)
    r = logic.analyze(changes, end, glitch_ms=5)
    d0 = r["D0"]
    assert (d0["first"], d0["last"], len(d0["edges"])) == (0, 0, 2)
    assert d0["high_s"] == pytest.approx(20e-6)
    [(t, w, level)] = d0["glitches"]
    assert (t, w, level) == (pytest.approx(1.0), pytest.approx(20e-6), 1)
    assert r["D4"]["glitches"] == []
    assert r["D4"]["high_s"] == pytest.approx(1.5 + 1.5)
    # A short first or last span is the capture's edge, not a pulse.
    changes = {"D1": [(0.0, 1), (0.001, 0), (0.999, 1)]}
    assert logic.analyze(changes, 1.0)["D1"]["glitches"] == []


def test_report_names_channels(tmp_path, capsys):
    vcd = tmp_path / "run.vcd"
    vcd.write_text(VCD, encoding="utf-8")
    assert logic.main(["report", str(vcd), "-c", "K1=CH1", "-c", "D5=CH5"]) == 1  # a glitch on K1
    out = capsys.readouterr().out
    assert "K1 (D0): starts LOW, 2 edge(s)" in out
    assert "glitch: HIGH for 0.020 ms at 1.000000 s" in out
    assert "D5 (D4): starts HIGH, 2 edge(s), HIGH 3.000 s, 0 glitch(es)" in out
    assert "  3.500000 s  rises  (after 2000.000 ms)" in out


def test_report_json(tmp_path, capsys):
    vcd = tmp_path / "run.vcd"
    vcd.write_text(VCD, encoding="utf-8")
    assert logic.main(["report", str(vcd), "-c", "D5=CH5", "--glitch-ms", "0.01", "--json"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["end_s"] == pytest.approx(5.0)
    assert set(out["channels"]) == {"D0", "D5"}
    assert out["channels"]["D5"]["edges"] == [[pytest.approx(1.5), 0], [pytest.approx(3.5), 1]]


def test_bad_input_is_an_error(tmp_path, capsys):
    assert logic.main(["report", str(tmp_path / "missing.vcd")]) == 2
    assert logic.main(["capture", "-c", "D5=CH9"]) == 2
    assert "CH1-CH8" in capsys.readouterr().err


class Done:
    def __init__(self, stdout="", stderr="", returncode=0):
        self.stdout, self.stderr, self.returncode = stdout, stderr, returncode


SCAN = "fx2lafw:conn=2.10 - Saleae Logic [S/N: Saleae Logic] with 8 channels: D0 D1 D2 D3 D4 D5 D6 D7\n"


def fake_sigrok(monkeypatch, out, scan=SCAN, scan_err=""):
    """Popen writes VCD to out; run answers the scan and the gatelink.py actions. Returns (sigrok cmds, run cmds)."""
    calls, ran = [], []

    class Proc:
        returncode = 0

        def __init__(self, cmd, **kw):
            calls.append(cmd)
            out.write_text(VCD, encoding="utf-8")

        def communicate(self):
            return "", ""

    def run(cmd, **kw):
        ran.append(cmd)
        return Done(scan, scan_err) if "--scan" in cmd else Done('{"ok": true}')

    monkeypatch.setattr(logic.subprocess, "Popen", Proc)
    monkeypatch.setattr(logic.subprocess, "run", run)
    return calls, ran


def test_capture_runs_sigrok_and_the_actions(tmp_path, monkeypatch, capsys):
    out = tmp_path / "run.vcd"
    monkeypatch.setattr(logic, "find_sigrok", lambda: "sigrok-cli")
    calls, ran = fake_sigrok(monkeypatch, out)
    rc = logic.main(["capture", "-c", "D5=CH5", "--time", "2.5", "--at", "0:house reboot", "--out", str(out)])
    assert rc == 1  # the VCD's K1 glitch
    assert calls == [["sigrok-cli", "-d", "fx2lafw", "-C", "D4", "-c", "samplerate=100k", "--time", "2500",
                      "-O", "vcd", "-o", str(out)]]
    assert ran[0] == ["sigrok-cli", "-d", "fx2lafw", "--scan"]  # firmware loaded before anything runs
    assert ran[1][-2:] == ["house", "reboot"] and ran[1][1].endswith("gatelink.py")
    assert "): gatelink.py house reboot -> rc 0" in capsys.readouterr().out


def test_an_action_after_the_capture_is_skipped(tmp_path, monkeypatch):
    out = tmp_path / "run.vcd"
    _, ran = fake_sigrok(monkeypatch, out)
    log = logic.capture("sigrok-cli", ["D4"], 1000, "100k", str(out), [(60.0, ["house", "reboot"])])
    assert log == [(60.0, 0.0, ["house", "reboot"], None, "not run: the capture had ended")]
    assert all("--scan" in cmd for cmd in ran)


def test_a_busy_analyzer_runs_nothing(tmp_path, monkeypatch):
    out = tmp_path / "run.vcd"
    err = "sr: fx2lafw: Failed to open potential device with VID:PID 0925:3881: LIBUSB_ERROR_ACCESS."
    calls, ran = fake_sigrok(monkeypatch, out, scan="demo - Demo device\n", scan_err=err)
    with pytest.raises(RuntimeError, match="PulseView"):
        logic.capture("sigrok-cli", ["D4"], 1000, "100k", str(out), [(0.0, ["house", "reboot"])])
    assert calls == [] and len(ran) == 1
