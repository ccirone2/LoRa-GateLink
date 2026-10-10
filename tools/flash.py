#!/usr/bin/env python3
"""Upload a GateLink .bin to boards over their USB bootloader, one at a time, and check each came back on it.

    python tools/flash.py build/GateLink.ino.bin house gate     # by role (the boards' running firmware answers)
    python tools/flash.py build/GateLink.ino.bin COM5           # by port
    python tools/flash.py build/GateLink.ino.bin COM19          # a board already in its bootloader (PID 0x0059)

What `arduino-cli upload` does, with every wait bounded: the 1200-baud touch (DTR off) that resets the board into
its SAM-BA bootloader, waiting for the bootloader's own port (it often gets a new COM number), bossac with a timeout,
then waiting for the board to come back with the same USB serial number and asking it its firmware version, which
must be the one in the .bin (its GATELINK_FW= marker). arduino-cli's upload has hung in bossac on the bench and left
a board wedged in its bootloader; here a hang ends in a clear message instead.

Config and key survive uploads from 0.5.0 (SPI flash); still take `tools/gatelink.py snapshot` first (the /flash
skill). Close the web console first: only one program can hold a port.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports

from gatelink_client.board import ARDUINO_VID, BoardError, find_boards, open_board
from gatelink_client.timeline import Timeline

APP_PID = 0x8059  # MKR WAN 1310 running a sketch
BOOT_PID = 0x0059  # its SAM-BA bootloader
MARKER = b"GATELINK_FW="


class FlashError(Exception):
    pass


def bin_version(data):
    """The firmware version in a .bin's GATELINK_FW= marker (config.h FW_MARKER_PREFIX), or None."""
    at = data.find(MARKER)
    if at < 0:
        return None
    m = re.match(rb"\d+\.\d+\.\d+", data[at + len(MARKER):at + len(MARKER) + 16])
    return m.group(0).decode() if m else None


def find_bossac():
    """The newest bossac the Arduino SAMD core installed, or one on PATH."""
    base = Path(os.environ.get("LOCALAPPDATA", "")) / "Arduino15" if os.name == "nt" else Path.home() / ".arduino15"
    exe = "bossac.exe" if os.name == "nt" else "bossac"
    found = sorted(glob.glob(str(base / "packages/arduino/tools/bossac/*" / exe)))
    if found:
        return found[-1]
    for d in os.environ.get("PATH", "").split(os.pathsep):
        if (Path(d) / exe).exists():
            return str(Path(d) / exe)
    raise FlashError("bossac not found (install the arduino:samd core: arduino-cli core install arduino:samd)")


def arduino_ports():
    """{port: (pid, USB serial number or '')} for the Arduino-VID ports."""
    return {p.device: (p.pid, p.serial_number or "") for p in serial.tools.list_ports.comports()
            if p.vid == ARDUINO_VID}


def wait_for(cond, timeout, step=0.25):
    deadline = time.monotonic() + timeout
    while True:
        got = cond()
        if got or time.monotonic() > deadline:
            return got
        time.sleep(step)


def touch_1200(port):
    """Open at 1200 baud and close with DTR off: the SAMD core's request to reset into the bootloader."""
    s = serial.Serial()
    s.port, s.baudrate, s.dtr = port, 1200, False
    try:
        s.open()
        time.sleep(0.1)
    finally:
        s.close()


def run_bossac(bossac, port, image, timeout):
    """Erase, write, verify and reset. Raises FlashError unless bossac reports a successful verify."""
    cmd = [bossac, "-i", "-d", f"--port={port}", "-U", "true", "-i", "-e", "-w", "-v", str(image), "-R"]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        raise FlashError(f"bossac didn't finish within {timeout} s on {port}: the board is likely still in its "
                         "bootloader. Replug its USB cable (or double-tap its reset button), then run this again "
                         "with the bootloader's port") from e
    out = p.stdout + p.stderr
    if "Verify successful" not in out:
        tail = "\n".join(out.strip().splitlines()[-5:])
        raise FlashError(f"bossac on {port} failed (exit {p.returncode}):\n{tail}")


def flash_one(target, image, version, bossac, timeout, log=print):
    """Flash one board (a role, an app port or a bootloader port); returns its info after the upload."""
    ports = arduino_ports()
    serial_no, old_fw = "", None
    if target.lower() in ("house", "gate"):
        try:
            boards = find_boards(Timeline())
        except BoardError as e:
            raise FlashError(str(e)) from e
        b = boards.pop(target.lower(), None)
        for other in boards.values():
            other.close()
        if b is None:
            raise FlashError(f"no {target} board answers (is the web console still connected?)")
        port, old_fw = b.port, b.info().get("fw")
        b.close()
    else:
        port = target
        if port not in ports:
            raise FlashError(f"{port} isn't an Arduino port (ports: {', '.join(ports) or 'none'})")
    pid, serial_no = ports.get(port, (None, ""))

    if pid == BOOT_PID:
        boot = port
        log(f"{port}: already in the bootloader")
    else:
        before = {p for p, (pid, _) in ports.items() if pid == BOOT_PID}
        log(f"{target} {port}{f' (fw {old_fw})' if old_fw else ''}: resetting into the bootloader")
        touch_1200(port)
        boot = wait_for(lambda: next((p for p, (pid, _) in arduino_ports().items()
                                      if pid == BOOT_PID and p not in before), None), 10)
        if not boot:
            raise FlashError(f"{port} didn't come back as a bootloader port within 10 s; double-tap its reset "
                             "button and run this again with the port it shows")
        log(f"bootloader on {boot}; writing {image.name} ({version})")
    run_bossac(bossac, boot, image, timeout)

    # Back as an app port: the same USB serial number if we knew it, else any new one.
    def back():
        now = arduino_ports()
        for p, (pid, sn) in now.items():
            if pid == APP_PID and ((serial_no and sn == serial_no) or (not serial_no and p not in ports)):
                return p
        return None

    app = wait_for(back, 30)
    if not app:
        raise FlashError("the board didn't come back on USB within 30 s after the upload (bossac verified it); "
                         "replug it, then check with `python tools/gatelink.py ports`")
    info = wait_for(lambda: _info(app), 15, step=1)
    if not info:
        raise FlashError(f"{app} doesn't answer `info` after the upload")
    if version and info.get("fw") != version:
        raise FlashError(f"{app} reports fw {info.get('fw')}, not {version} from the .bin")
    log(f"{info.get('role')} {app}: fw {info.get('fw')}, cfg {info.get('cfg_store', '?')}, "
        f"key {'set' if info.get('key_set') else 'NOT SET'}")
    return info


def _info(port):
    try:
        b = open_board(port, Timeline(), timeout=2)
    except (serial.SerialException, BoardError):
        return None
    try:
        return b.info()
    except BoardError:
        return None
    finally:
        b.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image", help="the GateLink .bin (arduino-cli compile --output-dir)")
    ap.add_argument("targets", nargs="+", help="house, gate, or a port (an app port or a bootloader port)")
    ap.add_argument("--bossac", help="bossac to use (default: the arduino:samd core's)")
    ap.add_argument("--timeout", type=int, default=120, help="seconds bossac may take (default 120)")
    ap.add_argument("--force", action="store_true", help="flash a .bin without the GATELINK_FW marker")
    args = ap.parse_args(argv)

    image = Path(args.image)
    try:
        data = image.read_bytes()
    except OSError as e:
        sys.exit(f"can't read {image}: {e}")
    version = bin_version(data)
    if not version and not args.force:
        sys.exit(f"{image} has no GATELINK_FW= marker: not a GateLink build (--force to flash it anyway)")
    try:
        bossac = args.bossac or find_bossac()
        for target in args.targets:  # one at a time: two boards in their bootloaders at once are hard to tell apart
            flash_one(target, image, version, bossac, args.timeout)
    except FlashError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
