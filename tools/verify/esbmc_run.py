#!/usr/bin/env python3
"""Runs one ESBMC harness and judges the verdict.

    python3 tools/verify/esbmc_run.py [--esbmc PATH] [--define NAME[=V]]... [--expect fail] HARNESS.cpp

Every property must hold (VERIFICATION SUCCESSFUL), unless --expect fail: then ESBMC must find a violation. The
Makefile runs each harness three ways: the proof (inductive step, every property), its base case (-DBOOT), and a
reachability check (-DREACH, an assertion that fails if the end of the harness can be reached at all), which must
fail, so a harness whose assumptions exclude every run can't pass by proving nothing. Prints the failed properties and
the counterexample's last steps on a failure. Exits 0 when the verdict is the expected one.
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "tools/verify"


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("harness")
    ap.add_argument("--esbmc", default=os.environ.get("ESBMC", "esbmc"))
    ap.add_argument("--define", "-D", action="append", default=[])
    ap.add_argument("--expect", choices=["pass", "fail"], default="pass")
    ap.add_argument("--timeout", type=int, default=3000)
    ap.add_argument("--expect-msg", help="with --expect fail: the violated property's message must contain this")
    ap.add_argument("--all", action="store_true", help="a verdict on every property, not just the first violation")
    ap.add_argument("--fw", default=str(ROOT / "firmware/GateLink"), help="firmware directory (mutate.py's copy)")
    ap.add_argument("--build", default=str(VERIFY / "build"), help="where params.h is (the Makefile's BUILD)")
    ap.add_argument("--log", help="also write ESBMC's whole output (the full counterexample) to this file")
    args = ap.parse_args(argv)
    harness = Path(args.harness)
    cmd = [args.esbmc, "--std", "c++17", "-DHOST_BUILD", *[f"-D{d}" for d in args.define],
           "-I", args.build, "-I", str(VERIFY / "stubs"), "-I", str(ROOT / "tests/native/stubs"),
           "-I", args.fw, "-I", str(VERIFY / "esbmc"), str(harness),
           "--unwind", "130", "--boolector"]
    if args.all:
        cmd.append("--multi-property")  # a verdict per property (slower: a query each)
    label = f"{harness.stem}{''.join(' -D' + d for d in args.define)}"
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print(f"{label}: timed out after {args.timeout} s")
        return 1
    out = p.stdout + p.stderr
    if args.log:
        Path(args.log).write_text(out)
    ok = "VERIFICATION SUCCESSFUL" in out
    failed = "VERIFICATION FAILED" in out
    if not ok and not failed:
        print(f"{label}: ESBMC gave no verdict (exit {p.returncode}):\n{out[-3000:]}")
        return 1
    summary = re.search(r"\*\* (\d+ of \d+ properties failed.*)", out)
    print(f"{label}: {'holds' if ok else 'violated'}" + (f" ({summary.group(1)})" if summary else ""))
    good = ok if args.expect == "pass" else failed
    if good and failed and args.expect_msg:
        vio = out[out.find("Violated property:"):]
        head = vio.splitlines()[:4]
        good = any(args.expect_msg in line for line in head)
        if not good:
            print(f"{label}: violated, but not {args.expect_msg!r}: " + " / ".join(x.strip() for x in head))
    if failed and args.expect == "pass":
        for m in re.finditer(r"^\s+FAILED\s+\[[^\]]+\]\s+(.*)$", out, re.M):
            print(f"  FAILED {m.group(1).strip()}")
        trace = out[out.find("[Counterexample]"):] if "[Counterexample]" in out else ""
        vio = trace[trace.find("Violated property:"):] if "Violated property:" in trace else ""
        if vio:
            print("  " + "\n  ".join(vio.splitlines()[:6]))
    if not good:
        print(f"{label}: UNEXPECTED (expected {'every property to hold' if args.expect == 'pass' else 'a violation'})")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
