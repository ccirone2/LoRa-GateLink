"""Command-line access to GateLink boards over their USB JSON console (docs/console.md).

    python tools/gatelink.py ports                      # every board: port, role, firmware, key, link
    python tools/gatelink.py house status               # one console request; target = house | gate | COMx
    python tools/gatelink.py gate relay.test k=1 ms=500 # arguments as key=value (values parsed as JSON)
    python tools/gatelink.py snapshot                   # save every board's config before flashing
    python tools/gatelink.py restore                    # after flashing: config + key back, reboot, wait for link
    python tools/gatelink.py house hist --csv link.csv  # link quality history (every bucket) as CSV
    python tools/gatelink.py rftest                     # radio preflight for a new board (5 min of pings)
    python tools/gatelink.py key gen --backup DIR       # new link key: printed once, plus an encrypted backup
    python tools/gatelink.py key backup FILE            # ~/.gatelink_key as an encrypted backup (GLKB v1)
    python tools/gatelink.py key restore FILE [--out P] # a backup's key, printed or written to P
    python tools/gatelink.py key id                     # the id of the key in ~/.gatelink_key
    python tools/gatelink.py survey [house|gate|COMx]   # site survey from one board: link margin both ways

From firmware 0.5.0 the config and key live in the board's SPI flash chip and survive uploads (`ports` shows
`cfg spi`); older firmware, or a board whose chip doesn't answer (`cfg internal`), loses them on every upload.
`snapshot`/`restore` remain the safety net. `snapshot` stores each board's running config by its USB serial number
(default ~/.gatelink_config.json), so `restore` finds the same board even if Windows gave it another COM port
after the upload (snapshots from before keyed by port still work); `restore` applies it, saves it, sets the key
from ~/.gatelink_key and reboots. Take the snapshot from boards in their normal state, not mid-test: an
interrupted e2e run can leave its unsaved test profile running. Close the web console first; only one program
can hold a port.

`key` handles the link key off the board (docs/key-management.md): the key id each board reports (`ports`
shows it) and encrypted backups in the web console's format. Passphrases are asked for (getpass), never taken from
the command line; the plaintext key is only printed, or written where --out says.

Uses the console client shared with the e2e suite (tools/gatelink_client), so needs pyserial; key backups also
need the cryptography package (both in tests/e2e/requirements.txt).
"""
import argparse
import csv
import getpass
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
import serial.tools.list_ports

from gatelink_client import keybackup as kb
from gatelink_client import survey
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
    serials = port_serials()
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
            print(f"{port:6} {s['role']:5} fw {s['fw']:7} key {board_key(b.info()):8} "
                  f"verified {link['verified']!s:5} rssi {link['rssi']:4} tx_power {tx_power:2} "
                  f"cfg {s.get('cfg_store', 'internal')} last reset {s['reset_cause']} "
                  f"usb ...{serials.get(port, '?')[-6:]}")
        finally:
            b.close()


def board_key(info):
    """The key a board holds, as `ports` shows it: its id (firmware 0.13.8 on), `set`, or `NOT SET`."""
    if not info.get("key_set"):
        return "NOT SET"
    return info.get("key_id") or "set"


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
    """Every history bucket as CSV, with each bucket's start in local time (counted back from now as if without a
    break: one recorded before a reset, a lower `boot`, started earlier than its time says)."""
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
            entry = {"role": info["role"], "fw": info["fw"], "key_set": info["key_set"], "key_id": info.get("key_id"),
                     "port": port, "serial": ser, "params": b.config_get(),
                     "taken": time.strftime("%Y-%m-%d %H:%M:%S")}
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
    return key.lower()


def cmd_restore(args):
    try:
        snap = json.loads(Path(args.file).read_text())
    except OSError as e:
        sys.exit(f"no snapshot: {e} (take one with `snapshot` before flashing)")
    key = read_key(args.key_file)
    kid = kb.key_id(bytes.fromhex(key))
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
            if saved.get("key_id") and saved["key_id"] != kid:
                print(f"warning: {args.key_file} holds key {kid}, but the {saved['role']} board had key "
                      f"{saved['key_id']} when the snapshot was taken")
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
                if info.get("key_id") not in (None, kid):
                    print(f"warning: {port} holds key {info['key_id']}, not {args.key_file}'s key {kid} "
                          "(--force writes the file's key)")
                continue
            b.config_set(**params)
            b.request("config.save")
            b.request("key.set", key=key)
            b.reboot()
            info = b.info()
            if info.get("key_id") not in (None, kid):
                print(f"warning: {port} reports key {info['key_id']} after key.set of key {kid}")
            print(f"{port}: restored as {info['role']} ({len(params)} params, key {info.get('key_id') or 'set'}"
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


def ask_passphrase(prompt):
    """getpass; Ctrl-C or the end of input at the prompt ends the command without a traceback."""
    try:
        return getpass.getpass(prompt)
    except (KeyboardInterrupt, EOFError):
        print(file=sys.stderr)
        sys.exit("cancelled; nothing written")


def ask_new_passphrase():
    """A backup passphrase, typed twice (never from argv, so it stays out of shell history)."""
    print(f"The passphrase encrypts the backup: at least {kb.MIN_PASSPHRASE} characters. Nothing can recover it if "
          "it is lost: store it in a password manager, apart from the backup file.", file=sys.stderr)
    for _ in range(3):
        p = ask_passphrase("Backup passphrase: ")
        if kb.passphrase_length(p) < kb.MIN_PASSPHRASE:
            print(f"Too short ({kb.passphrase_length(p)} characters).", file=sys.stderr)
        elif ask_passphrase("Same passphrase again: ") != p:
            print("The two passphrases differ.", file=sys.stderr)
        else:
            return p
    sys.exit("no passphrase; nothing written")


def backup_path(target, kid):
    """Where a backup of key `kid` goes: `target`, or the standard name inside it if it is a directory. Never
    overwrites a file."""
    path = Path(target).expanduser()
    if path.is_dir():
        path = path / kb.backup_file_name({"key_id": kid})
    if path.exists():
        sys.exit(f"{path} already exists; nothing written (move it away or choose another name)")
    if not path.parent.is_dir():
        sys.exit(f"{path.parent} isn't a directory; nothing written")
    return path


def check_out(target, key, force):
    """Where --out writes the plaintext key: a file may be replaced only if it holds the same key, or with --force."""
    path = Path(target).expanduser()
    if path.is_dir():
        sys.exit(f"{path} is a directory; nothing written (--out names the key file)")
    if not path.parent.is_dir():
        sys.exit(f"{path.parent} isn't a directory; nothing written")
    if path.exists() and not force:
        try:
            old = kb.parse_key_hex(path.read_text(errors="replace").strip())
        except OSError as e:
            sys.exit(f"can't read {path}: {e}; nothing written")
        if old != key:
            held = f"key {kb.key_id(old)}" if old else "something else"
            sys.exit(f"{path} already holds {held}; nothing written (--force replaces it)")
    return path


def write_secret(path, key):
    """Writes the plaintext key, readable only by this user where the OS allows it."""
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(key.hex() + "\n")
    except OSError as e:
        sys.exit(f"can't write the key to {path}: {e}")


def encrypt_new(key, note):
    """The key's backup, under a passphrase asked for twice."""
    try:
        return kb.encrypt_backup(key, ask_new_passphrase(), note=note)
    except kb.BackupError as e:
        sys.exit(str(e))


def write_backup(path, backup):
    try:
        path.write_text(kb.dumps(backup), encoding="utf-8", newline="\n")
    except OSError as e:
        sys.exit(f"can't write the backup to {path}: {e}")
    print(f"encrypted backup of key {backup['key_id']} written to {path}")


def cmd_key_gen(args):
    key = kb.generate_key()
    kid = kb.key_id(key)
    # Whatever can fail or be cancelled (the paths, the passphrase) comes before the key is shown or written.
    path = backup_path(args.backup, kid) if args.backup else None
    out = check_out(args.out, key, args.force) if args.out else None
    backup = encrypt_new(key, args.note) if path else None
    print(f"key    {key.hex()}\nkey id {kid}")
    if out:
        write_secret(out, key)
        print(f"key written to {out}")
    elif not backup:
        print("The key is shown only this once: keep it in a password manager, or use --backup.", file=sys.stderr)
    if backup:
        write_backup(path, backup)
    return 0


def cmd_key_backup(args):
    key = bytes.fromhex(read_key(args.key_file))
    kid = kb.key_id(key)
    weak = kb.weak_key_reason(key)
    if weak:
        print(f"warning: this key is weak ({weak}); boards refuse it from firmware 0.13.8. Generate a new one.",
              file=sys.stderr)
    path = backup_path(args.file, kid)
    print(f"backing up key {kid} from {args.key_file}", file=sys.stderr)
    write_backup(path, encrypt_new(key, args.note))
    return 0


def cmd_key_restore(args):
    try:
        backup = kb.parse_backup(Path(args.file).expanduser().read_bytes())
    except OSError as e:
        sys.exit(f"can't read the backup: {e}")
    except kb.BackupError as e:
        sys.exit(f"{args.file}: {e}")
    print(f"{args.file}: key {backup['key_id']}, made {backup.get('created', '?')}"
          + (f" ({backup['note']})" if backup.get("note") else ""), file=sys.stderr)
    key = None
    for tries in range(3):
        try:
            key = kb.decrypt_backup(backup, ask_passphrase("Backup passphrase: "))
            break
        except kb.BackupError as e:
            if tries == 2 or "passphrase" not in str(e):
                sys.exit(str(e))
            print(f"{e}; try again", file=sys.stderr)
    if args.out:
        out = check_out(args.out, key, args.force)
        write_secret(out, key)
        print(f"key {kb.key_id(key)} written to {out}")
    else:
        print(f"key    {key.hex()}\nkey id {kb.key_id(key)}")
    return 0


def cmd_key_id(args):
    print(kb.key_id(bytes.fromhex(read_key(args.key_file))))
    return 0


def cmd_key_set(args):
    """key.set on one board from the key file: the key never goes on a command line."""
    key = read_key(args.key_file)
    kid = kb.key_id(bytes.fromhex(key))
    b = open_target(args.target)
    try:
        res = b.request("key.set", check=False, key=key)
        if not res.get("ok"):
            print(f"{args.target}: key.set refused: {res.get('error', res)}", file=sys.stderr)
            return 1
        reported = b.info().get("key_id")
    finally:
        b.close()
    if reported not in (None, kid):
        print(f"warning: {args.target} reports key {reported} after key.set of key {kid}", file=sys.stderr)
        return 1
    print(f"{args.target}: key {kid} set" + ("" if reported else " (this firmware doesn't report key ids)"))
    return 0


# A survey ping unanswered this long counts as lost (the web console's ping card waits as long; SF12 takes seconds).
SURVEY_PING_TIMEOUT_S = 8.0
# The noise floors the samples are judged against are re-read from status this often.
SURVEY_NOISE_EVERY_S = 10.0


def run_survey(board, seconds, interval, clock=None, sleep=None, progress=print):
    """Ping from `board` for `seconds`: one ping outstanding at a time, a new one every `interval` s (or as soon as
    the last is answered or given up, if that takes longer). The firmware reports a pong only for its latest ping.

    Returns (samples, elapsed_s, stopped): survey.sample_from_pong samples; stopped says why if Ctrl+C ("stopped
    early") or a board error ("failed: ...") cut the survey short (the ping then in flight isn't counted), else
    None."""
    clock = clock or time.monotonic
    sleep = sleep or time.sleep
    tl = board.timeline
    samples, noise, noise_at, stopped = [], {}, None, None
    last_id = None  # the previous ping's ping_id, once a pong has shown the board's numbering
    t0 = last_progress = clock()
    try:
        while clock() - t0 < seconds:
            start = clock()
            if noise_at is None or start - noise_at >= SURVEY_NOISE_EVERY_S:
                noise, noise_at = survey.noise_from_status(board.status()), start
            seen = len(tl.select(src=board.name, kind="pong"))
            board.request("radio.ping")
            deadline, pong = clock() + SURVEY_PING_TIMEOUT_S, None
            while pong is None:
                # The previous ping's pong can still land after its 8 s, just before this ping reaches the board.
                # It isn't this ping's answer, so it is skipped by its id.
                pong = next((p for p in tl.select(src=board.name, kind="pong")[seen:]
                             if last_id is None or p.get("ping_id") != last_id), None)
                if pong is None:
                    if clock() >= deadline:
                        break
                    sleep(0.02)
            # This ping's id: its pong's, else one past the previous ping's (the board counts every radio.ping).
            last_id = (pong["ping_id"] if pong and isinstance(pong.get("ping_id"), int)
                       else None if last_id is None else (last_id + 1) & 0xFFFF)
            samples.append(survey.sample_from_pong(pong, round(start - t0, 3), noise))
            if clock() - last_progress >= 10:
                last_progress = clock()
                lost = sum(1 for x in samples if x["lost"])
                progress(f"  {last_progress - t0:4.0f} s  {len(samples)} pings, {lost} unanswered")
            next_at = start + interval
            if next_at - t0 >= seconds:
                break
            if next_at > clock():
                sleep(next_at - clock())
    except KeyboardInterrupt:
        stopped = "stopped early"
    except BoardError as e:
        stopped = f"failed: {e}"
    return samples, round(clock() - t0, 3), stopped


def survey_board(target):
    """The board to survey from: the one named; else the only board on USB; else the house, which also knows the
    gate's noise floor."""
    if target:
        return open_target(target)
    try:
        found = find_boards(Timeline())
    except BoardError as e:
        sys.exit(str(e))
    if not found:
        sys.exit("no GateLink board found (is the web console still connected?)")
    b = found.pop("house" if "house" in found else sorted(found)[0])
    for other in found.values():
        other.close()
    return b


def cmd_survey(args):
    """Site survey from one board: pings the other for a while and judges the link margin both ways
    (gatelink_client/survey.py; docs/install.md). Exit 0 for a good or fair link, 1 for marginal or poor, or if a
    board error cut the survey short (what it had is still reported)."""
    b = survey_board(args.target)
    try:
        info, status = b.info(), b.status()
        if not status["link"]["verified"]:
            sys.exit(f"{info['role']} board on {b.port}: link not verified. The survey needs the other board "
                     "powered, with the same key and radio settings.")
        params = b.config_get()
        settings = {k: params.get(k) for k in ("sf", "bw_hz", "tx_power")}
        bw = f"{settings['bw_hz'] / 1000:g}" if settings["bw_hz"] else "?"
        print(f"site survey from the {info['role']} board on {b.port} (SF{settings['sf']}, {bw} kHz, tx_power "
              f"{settings['tx_power']} dBm): pinging for {args.seconds:g} s, every {args.interval:g} s; "
              "Ctrl+C stops early")
        when = datetime.now().astimezone().isoformat(timespec="seconds")
        samples, elapsed, stopped = run_survey(b, args.seconds, args.interval)
    except BoardError as e:
        sys.exit(f"survey failed: {e}")
    finally:
        b.close()
    result = survey.analyze(settings, samples, info["role"])
    report = {"tool": "gatelink.py survey", "time": when,
              "board": {"role": info["role"], "fw": info["fw"], "port": b.port,
                        "usb_serial": port_serials().get(b.port)},
              "settings": settings, "seconds": args.seconds, "interval_s": args.interval,
              "ping_timeout_s": SURVEY_PING_TIMEOUT_S, "elapsed_s": elapsed, "stopped": stopped, **result,
              "samples": samples}
    print()
    print(survey.format_text(report), end="")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
        print(f"\nreport written to {args.json}")
    return 0 if result["verdict"] in ("good", "fair") and not (stopped or "").startswith("failed") else 1


def positive(text):
    v = float(text)
    if not v > 0:
        raise argparse.ArgumentTypeError(f"must be above 0, not {text}")
    return v


def main(argv=None):
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
    key = sub.add_parser("key", help="the link key: generate, back up, restore, id, set (docs/key-management.md)")
    ksub = key.add_subparsers(dest="key_action", required=True)
    p = ksub.add_parser("gen", help="generate a key; print it once, optionally back it up encrypted")
    p.add_argument("--backup", metavar="FILE", help="write an encrypted backup (a directory: its standard name there)")
    p.add_argument("--out", metavar="PATH", help="also write the plaintext key to PATH (e.g. ~/.gatelink_key)")
    p.add_argument("--note", default="", help="note stored (unencrypted) in the backup")
    p.add_argument("--force", action="store_true", help="let --out replace a file holding another key")
    p.set_defaults(fn=cmd_key_gen)
    p = ksub.add_parser("backup", help="write an encrypted backup of the key in the key file")
    p.add_argument("file", metavar="FILE", help="the backup to write (a directory: its standard name there)")
    p.add_argument("--key-file", default=DEFAULT_KEY_FILE)
    p.add_argument("--note", default="", help="note stored (unencrypted) in the backup")
    p.set_defaults(fn=cmd_key_backup)
    p = ksub.add_parser("restore", help="decrypt a backup; print the key, or write it with --out")
    p.add_argument("file", metavar="FILE")
    p.add_argument("--out", metavar="PATH", help="write the plaintext key to PATH instead of printing it")
    p.add_argument("--force", action="store_true", help="let --out replace a file holding another key")
    p.set_defaults(fn=cmd_key_restore)
    p = ksub.add_parser("id", help="print the id of the key in the key file")
    p.add_argument("--key-file", default=DEFAULT_KEY_FILE)
    p.set_defaults(fn=cmd_key_id)
    p = ksub.add_parser("set", help="write the key file's key to a board (house, gate or a port) and check its id")
    p.add_argument("target", help="house, gate or a port (COMx)")
    p.add_argument("--key-file", default=DEFAULT_KEY_FILE)
    p.set_defaults(fn=cmd_key_set)
    p = sub.add_parser("survey", help="site survey: ping from one board, judge the link margin both ways")
    p.add_argument("target", nargs="?", help="house, gate or a port (default: the only board, else the house)")
    p.add_argument("--seconds", type=positive, default=120, help="survey length (default 120)")
    p.add_argument("--interval", type=positive, default=2, help="seconds between pings (default 2)")
    p.add_argument("--json", metavar="FILE", help="also write the full report (settings, samples, verdict) here")
    p.set_defaults(fn=cmd_survey)

    argv = sys.argv[1:] if argv is None else argv
    # Anything else is `<target> <cmd> [key=value ...]`.
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] not in ("ports", "snapshot", "restore", "rftest", "survey", "key", "-h", "--help"):
        rp = argparse.ArgumentParser(prog="gatelink.py <target>")
        rp.add_argument("target", help="house, gate or a port (COMx)")
        rp.add_argument("cmd", help="console command, e.g. status, log.get, config.set; or hist (history as CSV)")
        rp.add_argument("args", nargs="*", help="key=value arguments; values are JSON (params={\"sf\":9})")
        rp.add_argument("--timeout", type=float, default=3.0)
        rp.add_argument("--csv", help="with `hist`: write the CSV to this file instead of stdout")
        args = rp.parse_args(argv)
        if args.cmd == "hist":
            return cmd_hist(args)
        return cmd_request(args)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
