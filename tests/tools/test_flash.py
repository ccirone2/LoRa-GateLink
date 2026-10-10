"""Tests for tools/flash.py without boards: the .bin marker, finding bossac, and the upload sequence against fake
ports and a fake bossac (the real thing is checked on the bench with the /flash skill)."""

from types import SimpleNamespace

import pytest

flash = pytest.importorskip("flash")  # needs pyserial, as the tools do


def test_bin_version():
    assert flash.bin_version(b"\x00junk GATELINK_FW=0.13.7\x00more") == "0.13.7"
    assert flash.bin_version(b"GATELINK_FW=") is None
    assert flash.bin_version(b"no marker at all") is None


def test_find_bossac_takes_the_newest_core_tool(tmp_path, monkeypatch):
    exe = "bossac.exe" if flash.os.name == "nt" else "bossac"
    base = tmp_path / "Arduino15" if flash.os.name == "nt" else tmp_path / ".arduino15"
    for v in ("1.7.0-arduino3", "1.9.1-arduino5"):
        d = base / "packages/arduino/tools/bossac" / v
        d.mkdir(parents=True)
        (d / exe).write_text("")
    monkeypatch.setenv("LOCALAPPDATA", str(tmp_path))
    monkeypatch.setattr(flash.Path, "home", lambda: tmp_path)
    assert flash.find_bossac().endswith(f"1.9.1-arduino5{flash.os.sep}{exe}")


class FakeUsb:
    """Arduino ports that change as the board resets: app port -> bootloader port -> app port again."""

    def __init__(self, app="COM5", boot="COM19", serial_no="ABC183013", other=None):
        self.app, self.boot, self.serial_no = app, boot, serial_no
        self.state = "app"
        self.other = other or {}

    def ports(self):
        found = dict(self.other)
        if self.state == "app":
            found[self.app] = (flash.APP_PID, self.serial_no)
        elif self.state == "boot":
            found[self.boot] = (flash.BOOT_PID, "")
        return found


@pytest.fixture
def usb(monkeypatch):
    u = FakeUsb()
    monkeypatch.setattr(flash, "arduino_ports", u.ports)
    monkeypatch.setattr(flash.time, "sleep", lambda s: None)
    return u


def test_flash_by_port_resets_writes_and_checks_the_version(usb, monkeypatch, tmp_path):
    calls = []

    def touch(port):
        calls.append(("touch", port))
        usb.state = "boot"

    def bossac(path, port, image, timeout):
        calls.append(("bossac", port))
        usb.state = "app"

    monkeypatch.setattr(flash, "touch_1200", touch)
    monkeypatch.setattr(flash, "run_bossac", bossac)
    monkeypatch.setattr(flash, "_info", lambda port: {"role": "house", "fw": "0.13.7", "cfg_store": "spi",
                                                      "key_set": True})
    lines = []
    info = flash.flash_one("COM5", tmp_path / "GateLink.ino.bin", "0.13.7", "bossac", 60, log=lines.append)
    assert calls == [("touch", "COM5"), ("bossac", "COM19")]
    assert info["fw"] == "0.13.7"
    assert lines[-1] == "house COM5: fw 0.13.7, cfg spi, key set"


def test_a_board_that_comes_back_on_another_port_is_found_by_serial(usb, monkeypatch, tmp_path):
    def touch(port):
        usb.state = "boot"

    def bossac(*a):
        usb.app = "COM22"  # Windows gave it a new number
        usb.state = "app"

    monkeypatch.setattr(flash, "touch_1200", touch)
    monkeypatch.setattr(flash, "run_bossac", bossac)
    seen = []
    monkeypatch.setattr(flash, "_info", lambda port: seen.append(port) or {"role": "gate", "fw": "0.13.7"})
    flash.flash_one("COM5", tmp_path / "x.bin", "0.13.7", "bossac", 60, log=lambda s: None)
    assert seen == ["COM22"]


def test_wrong_version_after_upload_is_an_error(usb, monkeypatch, tmp_path):
    monkeypatch.setattr(flash, "touch_1200", lambda port: setattr(usb, "state", "boot"))
    monkeypatch.setattr(flash, "run_bossac", lambda *a: setattr(usb, "state", "app"))
    monkeypatch.setattr(flash, "_info", lambda port: {"role": "house", "fw": "0.13.6"})
    with pytest.raises(flash.FlashError, match="reports fw 0.13.6, not 0.13.7"):
        flash.flash_one("COM5", tmp_path / "x.bin", "0.13.7", "bossac", 60, log=lambda s: None)


def test_no_bootloader_port_is_an_error(usb, monkeypatch, tmp_path):
    monkeypatch.setattr(flash, "touch_1200", lambda port: None)  # the board ignores the touch
    monkeypatch.setattr(flash, "wait_for", lambda cond, timeout, step=0.25: cond())
    with pytest.raises(flash.FlashError, match="didn't come back as a bootloader port"):
        flash.flash_one("COM5", tmp_path / "x.bin", "0.13.7", "bossac", 60, log=lambda s: None)


def test_a_board_already_in_its_bootloader_is_written_directly(usb, monkeypatch, tmp_path):
    usb.state = "boot"
    calls = []
    monkeypatch.setattr(flash, "touch_1200", lambda port: calls.append("touch"))

    def bossac(path, port, image, timeout):
        calls.append(port)
        usb.app, usb.state = "COM23", "app"

    monkeypatch.setattr(flash, "run_bossac", bossac)
    monkeypatch.setattr(flash, "_info", lambda port: {"role": "house", "fw": "0.13.7"})
    flash.flash_one("COM19", tmp_path / "x.bin", "0.13.7", "bossac", 60, log=lambda s: None)
    assert calls == ["COM19"]


def test_bossac_timeout_says_what_to_do(monkeypatch, tmp_path):
    def hang(cmd, **kw):
        raise flash.subprocess.TimeoutExpired(cmd, kw["timeout"])

    monkeypatch.setattr(flash.subprocess, "run", hang)
    with pytest.raises(flash.FlashError, match="Replug its USB cable"):
        flash.run_bossac("bossac", "COM19", tmp_path / "x.bin", 5)


def test_bossac_without_a_verify_is_an_error(monkeypatch, tmp_path):
    monkeypatch.setattr(flash.subprocess, "run",
                        lambda cmd, **kw: SimpleNamespace(returncode=1, stdout="No device found on COM19\n", stderr=""))
    with pytest.raises(flash.FlashError, match="No device found"):
        flash.run_bossac("bossac", "COM19", tmp_path / "x.bin", 5)


def test_main_refuses_a_bin_without_the_marker(tmp_path):
    image = tmp_path / "other.bin"
    image.write_bytes(b"\x00" * 64)
    with pytest.raises(SystemExit, match="no GATELINK_FW= marker"):
        flash.main([str(image), "COM5"])
