"""Tests for tools/fw_size.py (the firmware size budget CI applies)."""

import fw_size

LOG = """\
Sketch uses 90252 bytes (34%) of program storage space. Maximum is 262144 bytes.
Global variables use 22304 bytes (68%) of dynamic memory, leaving 10464 bytes for local variables. Maximum is 32768 bytes.
"""


def test_parse():
    assert fw_size.parse(LOG) == (90252, 262144, 22304, 32768)


def test_within_budget(tmp_path, capsys):
    log = tmp_path / "build.log"
    log.write_text("Compiling...\n" + LOG, encoding="utf-8")
    assert fw_size.main([str(log)]) == 0
    out = capsys.readouterr().out
    assert "| Flash | 90,252 B (34%) | 163,840 B | 73,588 B | 262,144 B |" in out
    assert "| Static RAM | 22,304 B (68%) | 24,576 B | 2,272 B | 32,768 B |" in out


def test_over_budget_fails(tmp_path, capsys):
    log = tmp_path / "build.log"
    log.write_text(LOG, encoding="utf-8")
    assert fw_size.main([str(log), "--ram-max", "22000"]) == 1
    assert "-304 B **over**" in capsys.readouterr().out


def test_writes_the_step_summary(tmp_path, monkeypatch):
    log = tmp_path / "build.log"
    log.write_text(LOG, encoding="utf-8")
    summary = tmp_path / "summary.md"
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    fw_size.main([str(log)])
    assert summary.read_text(encoding="utf-8").startswith("### Firmware size")


def test_failed_compile_is_an_error(tmp_path, capsys):
    log = tmp_path / "build.log"
    log.write_text("error: 'x' was not declared\n", encoding="utf-8")
    assert fw_size.main([str(log)]) == 2
    assert "no size lines" in capsys.readouterr().err


def test_budget_leaves_room_on_the_chip():
    _, flash_size, _, ram_size = fw_size.parse(LOG)
    assert fw_size.FLASH_MAX <= flash_size - 8 * 1024  # the bootloader
    assert fw_size.RAM_MAX <= ram_size - 8 * 1024  # the stack and heap
