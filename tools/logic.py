#!/usr/bin/env python3
"""Capture board outputs with the USB logic analyzer and report their edges and glitches.

    python tools/logic.py capture -c D5=CH5 [-c K1=CH1 ...] [--time 10] [--at "2:house reboot"] [--out run.vcd]
    python tools/logic.py report run.vcd [-c D5=CH5 ...]

The bench analyzer is an 8-channel fx2lafw clone read with sigrok-cli (docs/bench-testing.md, "Logic analyzer").
`-c NAME=CHn` names a channel by its case label (CH1-CH8); `NAME=Dn` uses sigrok's numbering (D0-D7, = CHn-1).
`--at SECONDS:ARGS` runs `tools/gatelink.py ARGS` that long after the capture starts (repeatable), e.g. a reboot to
watch the outputs through a reset. The report gives each channel's levels and edges, times from the capture start,
and flags any pulse shorter than `--glitch-ms` (both edges inside the capture). Exits 1 if one is flagged, 2 if
the capture fails. An unconnected input reads HIGH. Only one program can hold the analyzer: close PulseView first.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SIGROK_PATHS = [r"C:\Program Files\sigrok\sigrok-cli\sigrok-cli.exe",
                r"C:\Program Files (x86)\sigrok\sigrok-cli\sigrok-cli.exe"]
UNITS = {"s": 1.0, "ms": 1e-3, "us": 1e-6, "ns": 1e-9, "ps": 1e-12, "fs": 1e-15}
GLITCH_MS = 5.0


def parse_channel(spec):
    """'D5=CH5' -> ('D5', 'D4'); 'K1=D0' -> ('K1', 'D0')."""
    name, sep, ch = spec.partition("=")
    m = re.fullmatch(r"(CH|D)([0-9])", ch.strip().upper())
    if not sep or not name.strip() or not m:
        raise ValueError(f"channel {spec!r}: expected NAME=CH1..CH8 or NAME=D0..D7")
    n = int(m.group(2)) - (1 if m.group(1) == "CH" else 0)
    if not 0 <= n <= 7:
        raise ValueError(f"channel {spec!r}: the analyzer has CH1-CH8 (D0-D7)")
    return name.strip(), f"D{n}"


def parse_at(spec):
    """'2.5:house reboot' -> (2.5, ['house', 'reboot']). Split on whitespace only, so JSON values keep their quotes."""
    t, sep, rest = spec.partition(":")
    try:
        delay = float(t)
    except ValueError:
        delay = -1
    args = rest.split()
    if not sep or delay < 0 or not args:
        raise ValueError(f"--at {spec!r}: expected SECONDS:gatelink.py arguments, e.g. '2:house reboot'")
    return delay, args


def parse_vcd(text):
    """(changes, end): changes maps each signal name to [(seconds, level)], first entry at the capture start; end is
    the capture's length in seconds. Reads sigrok's VCD (one bit per wire)."""
    m = re.search(r"\$timescale\s+(\d+)\s*(\w+)\s+\$end", text)
    if not m or m.group(2) not in UNITS:
        raise ValueError("no $timescale in the VCD")
    scale = int(m.group(1)) * UNITS[m.group(2)]
    ids = dict((i, n) for i, n in re.findall(r"\$var\s+wire\s+1\s+(\S+)\s+(\S+)\s+\$end", text))
    if not ids:
        raise ValueError("no signals in the VCD")
    body = text.split("$enddefinitions", 1)[-1].split("$end", 1)[-1]
    body = re.sub(r"FRAME-(BEGIN|END)", " ", body)  # sigrok glues these to the value before them: "0!FRAME-END"
    changes = {n: [] for n in ids.values()}
    t = end = 0.0
    for tok in body.split():
        if tok.startswith("#") and tok[1:].isdigit():
            t = end = int(tok[1:]) * scale
        elif tok[:1] in "01" and tok[1:] in ids:
            ch = changes[ids[tok[1:]]]
            level = int(tok[0])
            if not ch or ch[-1][1] != level:
                ch.append((t, level))
    return changes, end


def analyze(changes, end, glitch_ms=GLITCH_MS):
    """Per channel: first and last level, edges [(seconds, new level)], time HIGH, and glitches [(start seconds,
    width seconds, level)]: pulses narrower than glitch_ms with both edges inside the capture."""
    out = {}
    for name, ch in changes.items():
        if not ch:
            continue
        bounds = [t for t, _ in ch[1:]]
        spans = list(zip([0.0] + bounds, bounds + [end], [lv for _, lv in ch], strict=True))
        out[name] = {
            "first": ch[0][1],
            "last": ch[-1][1],
            "edges": [(t, lv) for t, lv in ch[1:]],
            "high_s": sum(b - a for a, b, lv in spans if lv),
            "glitches": [(a, b - a, lv) for a, b, lv in spans[1:-1] if (b - a) * 1000 < glitch_ms],
        }
    return out


def report(result, end, names=None, max_edges=40):
    """Plain-text report; names maps sigrok channels (D4) to the names given on the command line (D5)."""
    names = names or {}
    lines = [f"capture {end:.3f} s"]
    for ch, r in sorted(result.items()):
        label = f"{names[ch]} ({ch})" if ch in names else ch
        lv = "HIGH" if r["first"] else "LOW"
        lines.append(f"{label}: starts {lv}, {len(r['edges'])} edge(s), HIGH {r['high_s']:.3f} s, "
                     f"{len(r['glitches'])} glitch(es)")
        prev = 0.0
        for t, level in r["edges"][:max_edges]:
            arrow = "rises" if level else "falls"
            lines.append(f"  {t:10.6f} s  {arrow}  (after {1000 * (t - prev):.3f} ms)")
            prev = t
        if len(r["edges"]) > max_edges:
            lines.append(f"  ... {len(r['edges']) - max_edges} more")
        for t, w, level in r["glitches"]:
            lines.append(f"  glitch: {'HIGH' if level else 'LOW'} for {1000 * w:.3f} ms at {t:.6f} s")
    return "\n".join(lines)


def find_sigrok():
    exe = shutil.which("sigrok-cli")
    if exe:
        return exe
    for p in SIGROK_PATHS:
        if os.path.exists(p):
            return p
    raise FileNotFoundError("sigrok-cli not found (install it from sigrok.org, docs/bench-testing.md)")


def run_at(actions, t0, done, log):
    """Run each (delay, gatelink.py args) at its time after t0, in order, until the capture is done; log
    (start seconds, seconds taken, args, exit code, output); exit code None for one the capture ended before."""
    for delay, args in sorted(actions, key=lambda a: a[0]):
        if done.wait(max(0.0, t0 + delay - time.monotonic())):
            log.append((delay, 0.0, args, None, "not run: the capture had ended"))
            continue
        start = time.monotonic() - t0
        r = subprocess.run([sys.executable, os.path.join(HERE, "gatelink.py"), *args],
                           capture_output=True, text=True)
        log.append((start, time.monotonic() - t0 - start, args, r.returncode, (r.stdout + r.stderr).strip()))


def capture(sigrok, chans, ms, rate, out, actions):
    """Run sigrok-cli for ms milliseconds into the VCD out, running the actions meanwhile; returns their log.
    Times count from sigrok-cli's start, which is a little before the first sample."""
    # The scan loads the analyzer's firmware, so an open that would fail does so here, before any action runs.
    scan = subprocess.run([sigrok, "-d", "fx2lafw", "--scan"], capture_output=True, text=True)
    if "fx2lafw" not in scan.stdout:
        hint = " (is PulseView holding it?)" if "LIBUSB_ERROR_ACCESS" in scan.stderr else ""
        raise RuntimeError(f"no analyzer found{hint}: {scan.stderr.strip()}")
    cmd = [sigrok, "-d", "fx2lafw", "-C", ",".join(chans), "-c", f"samplerate={rate}", "--time", str(ms),
           "-O", "vcd", "-o", out]
    log, done = [], threading.Event()
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    th = threading.Thread(target=run_at, args=(actions, t0, done, log), daemon=True)
    th.start()
    _, err = proc.communicate()
    done.set()
    th.join()
    if proc.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"sigrok-cli failed: {err.strip()}")
    return log


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    cp = sub.add_parser("capture", help="capture, then report")
    cp.add_argument("-c", "--channel", action="append", required=True, help="NAME=CHn (case label) or NAME=Dn")
    cp.add_argument("--time", type=float, default=10, help="seconds to capture (default 10)")
    cp.add_argument("--rate", default="100k", help="sample rate, e.g. 100k, 1M (default 100k: 10 us steps)")
    cp.add_argument("--at", action="append", default=[], help="SECONDS:gatelink.py args, run during the capture")
    cp.add_argument("--out", default="capture.vcd", help="VCD file to write (default capture.vcd); opens in PulseView")
    rp = sub.add_parser("report", help="report a VCD captured earlier")
    rp.add_argument("vcd")
    rp.add_argument("-c", "--channel", action="append", default=[], help="NAME=CHn or NAME=Dn, to name channels")
    for p in (cp, rp):
        p.add_argument("--glitch-ms", type=float, default=GLITCH_MS,
                       help=f"flag pulses shorter than this (default {GLITCH_MS:g} ms)")
        p.add_argument("--json", action="store_true", help="print the report as JSON")
    args = ap.parse_args(argv)
    try:
        names = {sig: name for name, sig in map(parse_channel, args.channel)}
        log = []
        if args.cmd == "capture":
            actions = [parse_at(a) for a in args.at]
            log = capture(find_sigrok(), list(names), round(args.time * 1000), args.rate, args.out, actions)
            path = args.out
        else:
            path = args.vcd
        with open(path, encoding="utf-8") as f:
            changes, end = parse_vcd(f.read())
    except (ValueError, OSError, RuntimeError) as e:
        print(f"logic: {e}", file=sys.stderr)
        return 2
    result = analyze(changes, end, args.glitch_ms)
    if args.json:
        print(json.dumps({"end_s": end, "channels": {names.get(k, k): v for k, v in result.items()},
                          "actions": [{"t_s": t, "took_s": d, "args": a, "rc": rc} for t, d, a, rc, _ in log]},
                         indent=2))
    else:
        for t, d, a, rc, text in log:
            print(f"at {t:.2f} s (took {d:.2f} s): gatelink.py {' '.join(a)} -> rc {rc}"
                  + (f": {text}" if rc != 0 else ""))
        print(report(result, end, names))
    return 1 if any(r["glitches"] for r in result.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
