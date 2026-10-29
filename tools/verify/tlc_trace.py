#!/usr/bin/env python3
"""Runs TLC on a model and prints its verdict, a counterexample as the steps that changed something.

    python3 tools/verify/tlc_trace.py [--expect-violation INV] [--jar tla2tools.jar] [--workers N] MODEL.cfg

The model is the .tla next to the .cfg named in its comment `\\* model: Name` (default GateLink). Exits 0 when the run
ends as expected: no violation, or, with --expect-violation or a `\\* expect: Inv violated` comment in the .cfg, a
violation of that invariant (the firmware as it was before a fix, kept to show the model sees the bug; or a residual
the model documents). Anything else exits 1.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

STATE = re.compile(r"^State (\d+): <(.*?)>\s*$")
VAR = re.compile(r"^/\\ (\w+) = (.*)$")


def parse_trace(out):
    """[(step, action, {var: value})] from TLC's error trace."""
    steps, cur, last = [], None, None
    for line in out.splitlines():
        m = STATE.match(line)
        if m:
            cur = (int(m.group(1)), m.group(2), {})
            steps.append(cur)
            continue
        if cur is None:
            continue
        m = VAR.match(line)
        if m:
            cur[2][m.group(1)] = m.group(2).strip()
            last = m.group(1)
        elif line.startswith("  ") and last in cur[2]:
            cur[2][last] += " " + line.strip()
        elif not line.strip():
            cur = None
    return steps


def action_name(text, tla_lines):
    """TLC names the disjunct by its position: print the line it points at, the action's name."""
    m = re.search(r"line (\d+), col (\d+)", text)
    if not m:
        return text
    line = tla_lines[int(m.group(1)) - 1]
    inner = re.search(r"\\E .*?: (\w+)\(", line)  # \E f \in air : DeliverCmd(f)
    if inner:
        return inner.group(1)
    for i in range(int(m.group(1)) - 1, -1, -1):  # the operator whose body it is
        d = re.match(r"^(\w+)(\(.*?\))? ==", tla_lines[i])
        if d:
            return d.group(1)
    return text


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("cfg")
    ap.add_argument("--expect-violation", metavar="INV")
    ap.add_argument("--jar", default=os.environ.get("TLA2TOOLS", "/opt/tla2tools.jar"))
    ap.add_argument("--workers", default="auto")
    args = ap.parse_args(argv)
    cfg = Path(args.cfg)
    text = cfg.read_text(encoding="utf-8")
    model = re.search(r"^\\\* model: (\w+)", text, re.M)
    expect = re.search(r"^\\\* expect: (\w+) violated", text, re.M)
    if expect and not args.expect_violation:
        args.expect_violation = expect.group(1)
    tla = cfg.parent / f"{model.group(1) if model else 'GateLink'}.tla"
    # TLC's states on disk (gigabytes for the larger configurations) go in a temporary directory, gone after the run.
    # Java's temporary files too: TLC unpacks its standard modules (Integers.tla...) there, and runs started together
    # (make -j) sharing /tmp read each other's half-written copies and failed to parse.
    with tempfile.TemporaryDirectory(prefix=f"tlc-{cfg.stem}-") as states:
        cmd = ["java", "-XX:+UseParallelGC", f"-Djava.io.tmpdir={states}", "-cp", args.jar, "tlc2.TLC",
               "-workers", args.workers, "-deadlock", "-metadir", str(Path(states) / "states"), "-config", cfg.name,
               tla.name]
        p = subprocess.run(cmd, cwd=cfg.parent, capture_output=True, text=True)
    out = p.stdout + p.stderr
    violated = re.search(r"Error: Invariant (\w+) is violated", out)
    stats = re.findall(r"^(\d+ states generated.*|The depth of the complete state graph search is \d+\.)$", out, re.M)
    print(f"{cfg.name}: {', '.join(stats) or 'no statistics'}")
    if violated:
        print(f"invariant {violated.group(1)} violated; counterexample:")
        lines = tla.read_text(encoding="utf-8").splitlines()
        prev = {}
        for n, act, state in parse_trace(out):
            changed = {k: v for k, v in state.items() if prev.get(k) != v}
            label = "initial state" if n == 1 else action_name(act, lines)
            print(f"  {n:2}. {label}")
            for k in sorted(changed):
                if n > 1 or k in ("pos", "now"):
                    print(f"        {k} = {changed[k]}")
            prev = state
    ok_run = "Model checking completed. No error has been found." in out
    if args.expect_violation:
        good = bool(violated) and violated.group(1) == args.expect_violation
    else:
        good = ok_run and not violated
    if not good and not violated:
        print(out[-3000:])
    print(("OK" if good else "UNEXPECTED") + (f" (expected {args.expect_violation} violated)" if args.expect_violation
                                               else " (no violation)" if good else ""))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
