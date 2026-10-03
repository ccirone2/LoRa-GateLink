"""Command-line access to GateLink boards over their USB JSON console (docs/console.md).

    python tools/gatelink.py ports                      # every board: port, role, firmware, key, link
    python tools/gatelink.py house status               # one console request; target = house | gate | COMx
    python tools/gatelink.py gate relay.test k=1 ms=500 # arguments as key=value (values parsed as JSON)
    python tools/gatelink.py snapshot                   # save every board's config before flashing
    python tools/gatelink.py restore                    # after flashing: config + key back, reboot, wait for link
    python tools/gatelink.py house hist --csv link.csv  # link quality history (every bucket) as CSV

Uploading firmware erases the saved config and key. `snapshot` stores each board's running config by port
(default ~/.gatelink_config.json); `restore` applies it to the board on the same port, saves it, sets the key
from ~/.gatelink_key and reboots. Take the snapshot from boards in their normal state, not mid-test: an
interrupted e2e run can leave its unsaved test profile running. Close the web console first; only one program
can hold a port.

Uses the e2e suite's console client (tests/e2e/gatelink/board.py), so needs pyserial.
"""
import argparse
import csv
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tests" / "e2e"))
from gatelink.board import ARDUINO_VID, Board, BoardError  # noqa: E402
from gatelink.timeline import Timeline  # noqa: E402

import serial  # noqa: E402
import serial.tools.list_ports  # noqa: E402

DEFAULT_SNAPSHOT = os.path.expanduser("~/.gatelink_config.json")
DEFAULT_KEY_FILE = os.path.expanduser("~/.gatelink_key")


def board_ports():
    return sorted(p.device for p in serial.tools.list_ports.comports() if p.vid == ARDUINO_VID)


def open_board(port):
    b = Board(port, Timeline())
    b.open()
    try:
        b.name = b.info()["role"]
    except BaseException:
        b.close()
        raise
    return b


def open_target(target):
    """A port name, or a role (house/gate) found among the Arduino ports."""
    if target.lower() not in ("house", "gate"):
        return open_board(target)
    for port in board_ports():
        try:
            b = open_board(port)
        except (serial.SerialException, BoardError):
            continue
        if b.name == target.lower():
            return b
        b.close()
    sys.exit(f"no {target} board found (is the web console still connected?)")


def parse_args(pairs):
    out = {}
    for kv in pairs:
        k, sep, v = kv.partition("=")
        if not sep:
            sys.exit(f"argument {kv!r} isn't key=value")
        try:
            out[k] = json.loads(v)
        except ValueError:
            out[k] = v
    return out


def cmd_ports(_):
    ports = board_ports()
    if not ports:
        print("no Arduino boards found")
    for port in ports:
        try:
            b = open_board(port)
        except (serial.SerialException, BoardError) as e:
            print(f"{port:6} not a GateLink board, or busy ({e})")
            continue
        try:
            s = b.status()
            link = s["link"]
            tx_power = b.config_get()["tx_power"]
            print(f"{port:6} {s['role']:5} fw {s['fw']:7} key {'set' if s['key_set'] else 'NOT SET':7} "
                  f"verified {link['verified']!s:5} rssi {link['rssi']:4} tx_power {tx_power:2} "
                  f"last reset {s['reset_cause']}")
        finally:
            b.close()


def cmd_request(args):
    b = open_target(args.target)
    try:
        res = b.request(args.cmd, timeout=args.timeout, check=False, **parse_args(args.args))
    finally:
        b.close()
    res.pop("id", None)
    print(json.dumps(res, indent=2))
    return 0 if res.get("ok") else 1


def cmd_hist(args):
    """Every history bucket as CSV, with each bucket's start in local time (the board counts from its boot)."""
    b = open_target(args.target)
    try:
        buckets, head = b.history()
    finally:
        b.close()
    fetched = time.time()
    out = open(args.csv, "w", newline="") if args.csv else sys.stdout
    try:
        w = csv.writer(out)
        fields = list(buckets[0]) if buckets else []
        w.writerow(["start"] + fields)
        for bk in buckets:
            start = fetched - (head["now_s"] - bk["idx"] * head["period_s"])
            w.writerow([time.strftime("%Y-%m-%d %H:%M", time.localtime(start))]
                       + ["" if bk[f] is None else bk[f] for f in fields])
    finally:
        if args.csv:
            out.close()
    if args.csv:
        print(f"{len(buckets)} buckets of {head['period_s']} s to {args.csv}")
    return 0


def cmd_snapshot(args):
    snap = {}
    for port in board_ports():
        try:
            b = open_board(port)
        except (serial.SerialException, BoardError) as e:
            print(f"{port}: skipped ({e})")
            continue
        try:
            info = b.info()
            snap[port] = {"role": info["role"], "fw": info["fw"], "key_set": info["key_set"],
                          "params": b.config_get(), "taken": time.strftime("%Y-%m-%d %H:%M:%S")}
            print(f"{port}: {info['role']} fw {info['fw']}, {len(snap[port]['params'])} params")
        finally:
            b.close()
    if not snap:
        sys.exit("no boards answered; nothing saved")
    Path(args.file).write_text(json.dumps(snap, indent=2))
    print(f"saved to {args.file}")


def read_key(path):
    try:
        key = Path(path).read_text().strip()
    except OSError as e:
        sys.exit(f"can't read the key file: {e}")
    if len(key) != 32 or any(c not in "0123456789abcdefABCDEF" for c in key):
        sys.exit(f"{path} must hold the 32-hex-char link key")
    return key


def cmd_restore(args):
    try:
        snap = json.loads(Path(args.file).read_text())
    except OSError as e:
        sys.exit(f"no snapshot: {e} (take one with `snapshot` before flashing)")
    key = read_key(args.key_file)
    boards = []
    try:
        for port, saved in snap.items():
            b = Board(port, Timeline(), name=saved["role"])
            b.open()
            boards.append(b)
            current = b.config_get()
            # Only params this firmware knows; ones it added keep their defaults.
            params = {k: v for k, v in saved["params"].items() if k in current}
            skipped = sorted(set(saved["params"]) - set(current))
            info = b.info()
            if (not args.force and info["key_set"] and info["role"] == saved["role"]
                    and all(current[k] == v for k, v in params.items())):
                print(f"{port}: {saved['role']} already matches the snapshot; left alone")
                continue
            b.config_set(**params)
            b.request("config.save")
            b.request("key.set", key=key)
            b.reboot()
            print(f"{port}: restored as {b.info()['role']} ({len(params)} params, key set"
                  + (f"; not on this firmware: {', '.join(skipped)}" if skipped else "") + ")")
        deadline = time.monotonic() + args.wait
        while True:
            states = {b.name: b.status()["link"]["verified"] for b in boards}
            if all(states.values()):
                print("link verified on " + ", ".join(states))
                return 0
            if time.monotonic() > deadline:
                print(f"link not verified within {args.wait} s: {states}")
                return 1
            time.sleep(0.5)
    finally:
        for b in boards:
            b.close()


def main():
    ap = argparse.ArgumentParser(description="GateLink boards over their USB JSON console (docs/console.md); "
                                             "or `gatelink.py <house|gate|COMx> <cmd> [key=value ...]`")
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("ports", help="list boards").set_defaults(fn=cmd_ports)
    p = sub.add_parser("snapshot", help="save every board's config by port")
    p.add_argument("--file", default=DEFAULT_SNAPSHOT)
    p.set_defaults(fn=cmd_snapshot)
    p = sub.add_parser("restore", help="re-apply a snapshot and the key after flashing")
    p.add_argument("--file", default=DEFAULT_SNAPSHOT)
    p.add_argument("--key-file", default=DEFAULT_KEY_FILE)
    p.add_argument("--force", action="store_true", help="restore even boards that already match")
    p.add_argument("--wait", type=float, default=30, help="seconds to wait for the link")
    p.set_defaults(fn=cmd_restore)

    # Anything else is `<target> <cmd> [key=value ...]`.
    if len(sys.argv) > 1 and sys.argv[1] not in ("ports", "snapshot", "restore", "-h", "--help"):
        rp = argparse.ArgumentParser(prog="gatelink.py <target>")
        rp.add_argument("target", help="house, gate or a port (COMx)")
        rp.add_argument("cmd", help="console command, e.g. status, log.get, config.set; or hist (history as CSV)")
        rp.add_argument("args", nargs="*", help="key=value arguments; values are JSON (params={\"sf\":9})")
        rp.add_argument("--timeout", type=float, default=3.0)
        rp.add_argument("--csv", help="with `hist`: write the CSV to this file instead of stdout")
        args = rp.parse_args()
        if args.cmd == "hist":
            return cmd_hist(args)
        return cmd_request(args)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
