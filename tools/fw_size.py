#!/usr/bin/env python3
"""Check a firmware build against its size budget.

    python tools/fw_size.py build.log [--flash-max BYTES] [--ram-max BYTES]

Reads arduino-cli's compile output ("Sketch uses ... Global variables use ..."), prints flash and static RAM use
against the budget (also to $GITHUB_STEP_SUMMARY in CI) and exits 1 if either is over. The budget lives here, so a
change that needs more is a visible decision in its pull request (docs/development.md, "Size budget").
"""

import argparse
import os
import re
import sys

# The MKR WAN 1310 (SAMD21G18A): 256 KB flash (the bootloader takes 8 KB), 32 KB RAM.
# Static RAM (globals) is capped so the stack and heap keep 8 KB: a config.get reply alone takes ~5 KB of heap.
FLASH_MAX = 160 * 1024
RAM_MAX = 24 * 1024

FLASH_RE = re.compile(r"Sketch uses (\d+) bytes .*?Maximum is (\d+) bytes")
RAM_RE = re.compile(r"Global variables use (\d+) bytes .*?Maximum is (\d+) bytes")


def parse(text):
    """(flash used, flash size, RAM used, RAM size) from arduino-cli's output."""
    f, r = FLASH_RE.search(text), RAM_RE.search(text)
    if not f or not r:
        raise ValueError("no size lines (\"Sketch uses ...\", \"Global variables use ...\") in the compile output")
    return int(f.group(1)), int(f.group(2)), int(r.group(1)), int(r.group(2))


def report(text, flash_max=FLASH_MAX, ram_max=RAM_MAX):
    """(over budget?, Markdown table)."""
    flash, flash_size, ram, ram_size = parse(text)
    rows = [("Flash", flash, flash_max, flash_size), ("Static RAM", ram, ram_max, ram_size)]
    lines = ["| | Used | Budget | Left in budget | Chip |", "|---|---:|---:|---:|---:|"]
    over = False
    for name, used, budget, size in rows:
        left = budget - used
        over |= left < 0
        mark = " **over**" if left < 0 else ""
        lines.append(f"| {name} | {used:,} B ({100 * used / size:.0f}%) | {budget:,} B | {left:,} B{mark} | "
                     f"{size:,} B |")
    return over, "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="arduino-cli compile output")
    ap.add_argument("--flash-max", type=int, default=FLASH_MAX)
    ap.add_argument("--ram-max", type=int, default=RAM_MAX)
    args = ap.parse_args(argv)
    with open(args.log, encoding="utf-8", errors="replace") as f:
        text = f.read()
    try:
        over, table = report(text, args.flash_max, args.ram_max)
    except ValueError as e:
        print(f"fw_size: {e}", file=sys.stderr)
        return 2
    summary = "### Firmware size\n\n" + table + "\n"
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write(summary)
    if over:
        print("Over the size budget: shrink the change, or raise the budget in tools/fw_size.py and say why in the "
              "pull request.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
