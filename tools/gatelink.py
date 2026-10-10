"""Command-line access to GateLink boards over their USB JSON console (docs/console.md).

    python tools/gatelink.py ports                      # every board: port, role, firmware, key, link
    python tools/gatelink.py house status               # one console request; target = house | gate | COMx
    python tools/gatelink.py gate relay.test k=1 ms=500 # arguments as key=value (values parsed as JSON)
    python tools/gatelink.py snapshot                   # save every board's config before flashing
    python tools/gatelink.py restore                    # after flashing: config + key back, reboot, wait for link
    python tools/gatelink.py house hist --csv link.csv  # link quality history (every bucket) as CSV
    python tools/gatelink.py rftest                     # radio preflight for a new board (5 min of pings)

From firmware 0.5.0 the config and key live in the board's SPI flash chip and survive uploads (`ports` shows
`cfg spi`); older firmware, or a board whose chip doesn't answer (`cfg internal`), loses them on every upload.
`snapshot`/`restore` remain the safety net. `snapshot` stores each board's running config by its USB serial number
(default ~/.gatelink_config.json), so `restore` finds the same board even if Windows gave it another COM port
after the upload (snapshots from before keyed by port still work); `restore` applies it, saves it, sets the key
from ~/.gatelink_key and reboots. Take the snapshot from boards in their normal state, not mid-test: an
interrupted e2e run can leave its unsaved test profile running. Close the web console first; only one program
can hold a port.

Uses the console client shared with the e2e suite (tools/gatelink_client), so needs pyserial.
"""
import argparse
import csv
import json
import os
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports

from gatelink_client.board import ARDUINO_VID, Board, BoardError, board_ports, find_boards, open_board
from gatelink_client.timeline import Timeline

DEFAULT_SNAPSHOT = os.path.expanduser("~/.gatelink_config.json")
DEFAULT_KEY_FILE = os.path.expanduser("~/.gatelink_key")


def port_serials():
    """{port: USB serial number} for the Arduino ports. The number is the SAMD21's chip id: it stays with the board
    across uploads, resets and replugs, while the COM port can change."""
    return {p.device: p.serial_number for p in serial.tools.list_ports.comports()
            if p.vid == ARDUINO_VID and p.serial_number}


def open_target(target):
    """A port name, or a role (house/gate) found among the Arduino ports."""
    role = target.lower()
    if role not in ("house", "gate"):
        return open_board(target, Timeline())
    try:
        boards = find_boards(Timeline())
    except BoardError as e:
        sys.exit(str(e))
    b = boards.pop(role, None)
    for other in boards.values():
        other.close()
    if b is None:
        sys.exit(f"no {target} board found (is the web console still connected?)")
    return b


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
            b = open_board(port, Timeline())
        except (serial.SerialException, BoardError) as e:
            print(f"{port:6} not a GateLink board, or busy ({e})")
            continue
        try:
            s = b.status()
            link = s["link"]
            tx_power = b.config_get()["tx_power"]
            print(f"{port:6} {s['role']:5} fw {s['fw']:7} key {'set' if s['key_set'] else 'NOT SET':7} "
                  f"verified {link['verified']!s:5} rssi {link['rssi']:4} tx_power {tx_power:2} "
                  f"cfg {s.get('cfg_store', 'internal')} last reset {s['reset_cause']}")
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
    serials = port_serials()
    for port in board_ports():
        try:
            b = open_board(port, Timeline())
        except (serial.SerialException, BoardError) as e:
            print(f"{port}: skipped ({e})")
            continue
        try:
            info = b.info()
            ser = serials.get(port)
            entry = {"role": info["role"], "fw": info["fw"], "key_set": info["key_set"], "port": port, "serial": ser,
                     "params": b.config_get(), "taken": time.strftime("%Y-%m-%d %H:%M:%S")}
            snap[f"usb:{ser}" if ser else port] = entry
            print(f"{port}: {info['role']} fw {info['fw']}, {len(entry['params'])} params"
                  + (f" (USB serial {ser})" if ser else " (no USB serial number: keyed by port)"))
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
    by_serial = {s: p for p, s in port_serials().items()}
    boards = []
    try:
        for name, saved in snap.items():
            # By USB serial number when the snapshot has one (the COM port may have changed); else by port.
            port = by_serial.get(saved["serial"]) if saved.get("serial") else name
            if port is None:
                print(f"{saved.get('port', name)}: the {saved['role']} board (USB serial {saved['serial']}) isn't "
                      "connected; skipped")
                continue
            if saved.get("port") and port != saved["port"]:
                print(f"{saved['role']} board was on {saved['port']}, now on {port}")
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
        if not boards:
            print("none of the snapshot's boards is connected")
            return 1
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


def cmd_rftest(args):
    """Radio preflight: ping both ways between the two linked boards and judge each board's receiver.

    A board's `crc_err` counts frames it received with a bad CRC, so it points at that board's receiver; lost
    pongs can't be split between the ping and the pong, so loss is judged on both directions together.
    """
    tl = Timeline()
    try:
        found = find_boards(tl)
    except BoardError as e:
        sys.exit(str(e))
    boards = list(found.values())
    usb = port_serials()
    serials = {b.name: (b.port, usb.get(b.port) or "?") for b in boards}
    try:
        if sorted(found) != ["gate", "house"]:
            sys.exit("rftest needs one house and one gate board on USB, both with the key set "
                     f"(found: {sorted(found) or 'none'}; is the web console still connected?)")
        start = {}
        for b in boards:
            s = b.status()
            if not s["link"]["verified"]:
                sys.exit(f"{b.name}: link not verified; wait for it (or `restore`) and retry")
            start[b.name] = s["link"]
        sf, tx_power = boards[0].config_get()["sf"], boards[0].config_get()["tx_power"]
        print(f"pinging both ways for {args.seconds:.0f} s (SF{sf}, tx_power {tx_power}) ...")

        stats = {b.name: {"sent": 0, "lost": 0, "snr": [], "rssi": [], "fei": []} for b in boards}
        end_at = time.monotonic() + args.seconds
        while time.monotonic() < end_at:
            for b in boards:
                st = stats[b.name]
                mark = tl.now()
                b.request("radio.ping")
                st["sent"] += 1
                deadline = time.monotonic() + 3
                pong = None
                while pong is None and time.monotonic() < deadline:
                    time.sleep(0.02)
                    pong = tl.first(b.name, "pong", mark)
                if pong is None:
                    st["lost"] += 1
                else:
                    for k in ("snr", "rssi", "fei"):
                        if pong.get(k) is not None:
                            st[k].append(pong[k])
                time.sleep(0.5)
        end = {b.name: b.status()["link"] for b in boards}
    finally:
        for b in boards:
            b.close()

    def avg(xs):
        return f"{sum(xs) / len(xs):6.2f}" if xs else "     -"

    sent = sum(s["sent"] for s in stats.values())
    lost = sum(s["lost"] for s in stats.values())
    loss = 100 * lost / max(sent, 1)
    ok = loss <= args.max_loss
    print(f"\n{'board':5} {'port':6} {'serial':32} {'pings':>5} {'lost':>4} {'crc':>3} {'snr':>6} {'min':>5} "
          f"{'rssi':>6} {'fei Hz':>7} {'noise':>5}  receiver")
    for name, st in stats.items():
        port, sn = serials[name]
        crc = (end[name]["crc_err"] - start[name]["crc_err"]) & 0xFFFFFFFF
        good = crc <= args.max_crc
        ok = ok and good
        print(f"{name:5} {port:6} {sn:32} {st['sent']:5} {st['lost']:4} {crc:3} {avg(st['snr'])} "
              f"{min(st['snr'], default=float('nan')):5.2f} {avg(st['rssi'])} {avg(st['fei']):>7} "
              f"{end[name]['noise'] if end[name]['noise'] is not None else '-':>5}  {'ok' if good else 'FAIL'}")
    print(f"\npong loss {lost}/{sent} ({loss:.1f} %, limit {args.max_loss:g} %); "
          f"CRC errors limit {args.max_crc} per board")
    print("PASS" if ok else "FAIL: see docs/bench-testing.md (New board preflight) before using this pair")
    return 0 if ok else 1


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
    p = sub.add_parser("rftest", help="radio preflight: ping both ways, check pong loss and CRC errors")
    p.add_argument("--seconds", type=float, default=300, help="test length (default 300)")
    p.add_argument("--max-loss", type=float, default=1.0, help="pong loss limit in %% (default 1)")
    p.add_argument("--max-crc", type=int, default=0, help="CRC errors allowed per board (default 0)")
    p.set_defaults(fn=cmd_rftest)

    # Anything else is `<target> <cmd> [key=value ...]`.
    if len(sys.argv) > 1 and sys.argv[1] not in ("ports", "snapshot", "restore", "rftest", "-h", "--help"):
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
