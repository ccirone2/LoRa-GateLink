#!/usr/bin/env python3
"""Check that the firmware, the e2e suite and docs/console.md agree on the names and numbers they share.

CLAUDE.md asks for console.cpp, log.cpp, roles.h/link.h, tests/e2e/gatelink and docs/console.md to change
together; this catches the drift that slips through. Run from anywhere (CI runs it): exits 1 with a list of
mismatches, 0 if all agree.

Checked:
- roles.h / link.h enums (GateState, Cause, Action, AckResult) against the wire values in gatelink/bench.py;
- log event names (log.cpp NAMES) against the log table in docs/console.md;
- every log or event name the suite waits for or checks against the firmware's names;
- console commands (console.cpp `strcmp(cmd, ...)`) against the commands table in docs/console.md.
"""
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "firmware" / "GateLink"
DOC = ROOT / "docs" / "console.md"
BENCH = ROOT / "tests" / "e2e" / "gatelink" / "bench.py"
TESTS = ROOT / "tests" / "e2e"

problems = []


def read(p):
    return p.read_text(encoding="utf-8")


def enum(src, name):
    """{member: value} of `enum name ... { ... };`, implicit values counted up as in C."""
    m = re.search(r"enum\s+" + name + r"\b[^{]*\{(.*?)\};", src, re.S)
    if not m:
        problems.append(f"enum {name} not found")
        return {}
    out, nxt = {}, 0
    body = re.sub(r"//[^\n]*", "", m.group(1))
    for item in filter(None, (s.strip() for s in body.split(","))):
        k, _, v = (p.strip() for p in item.partition("="))
        nxt = int(v, 0) if v else nxt
        out[k] = nxt
        nxt += 1
    return out


def bench_values():
    """Module-level assignments in bench.py: GS/CAUSE dicts and the ACT_/RES_ constants."""
    vals = {}
    for node in ast.parse(read(BENCH)).body:
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name):
                try:
                    vals[target.id] = ast.literal_eval(node.value)
                except ValueError:
                    pass
            elif isinstance(target, ast.Tuple) and isinstance(node.value, ast.Tuple):
                for t, v in zip(target.elts, node.value.elts, strict=True):
                    if isinstance(t, ast.Name):
                        vals[t.id] = ast.literal_eval(v)
    return vals


def compare(what, fw, py):
    for k in sorted(set(fw) | set(py)):
        if k not in py:
            problems.append(f"{what}: {k} = {fw[k]} in the firmware, missing in {BENCH.name}")
        elif k not in fw:
            problems.append(f"{what}: {k} = {py[k]} in {BENCH.name}, not in the firmware")
        elif fw[k] != py[k]:
            problems.append(f"{what}: {k} is {fw[k]} in the firmware, {py[k]} in {BENCH.name}")


def doc_table(header):
    """Backticked names in the first column of the docs/console.md table under `header` (its header row)."""
    lines = read(DOC).splitlines()
    try:
        i = next(n for n, line in enumerate(lines) if line.strip().startswith(header))
    except StopIteration:
        problems.append(f"{DOC.name}: no table starting {header!r}")
        return set()
    names = set()
    for line in lines[i + 2:]:
        if not line.startswith("|"):
            break
        names |= set(re.findall(r"`([^`]+)`", line.split("|")[1]))
    return names


def main():
    roles, link, bench = read(FW / "roles.h"), read(FW / "link.h"), bench_values()

    # Wire values (roles.h, link.h) <-> bench.py
    gs = {k[3:].lower(): v for k, v in enum(roles, "GateState").items()}
    compare("GateState", gs, bench.get("GS", {}))
    compare("Cause", {k[6:].lower(): v for k, v in enum(roles, "Cause").items()}, bench.get("CAUSE", {}))
    compare("Action", enum(roles, "Action"), {k: v for k, v in bench.items() if k.startswith("ACT_")})
    compare("AckResult", enum(link, "AckResult"), {k: v for k, v in bench.items() if k.startswith("RES_")})

    # Log event names: log.cpp <-> docs
    m = re.search(r"NAMES\[\]\s*=\s*\{(.*?)\};", read(FW / "log.cpp"), re.S)
    log_names = set(re.findall(r'"([a-z_0-9]+)"', m.group(1))) if m else set()
    if not log_names:
        problems.append("log.cpp: NAMES[] not found")
    doc_log = doc_table("| Event | a | b |")
    for n in sorted(log_names - doc_log):
        problems.append(f"log event `{n}` (log.cpp) isn't in the log table in {DOC.name}")
    for n in sorted(doc_log - log_names):
        problems.append(f"log event `{n}` in {DOC.name} isn't in log.cpp")

    # Names the suite waits for or checks: log events, or console events (docs events table).
    events = doc_table("| Event | Fields | When |")
    known = log_names | events
    call = re.compile(r'\b(?:wait_log|expect_no|logs|first)\(\s*(?:"(?:house|gate)"|\w+)\s*,\s*"([a-z_0-9]+)"')
    for f in sorted(TESTS.rglob("*.py")):
        for n, line in enumerate(read(f).splitlines(), 1):
            for name in call.findall(line):
                if name not in known:
                    problems.append(f"{f.relative_to(ROOT)}:{n}: waits for `{name}`, which the firmware never logs")

    # Console commands: console.cpp <-> docs
    cmds = set(re.findall(r'strcmp\(cmd,\s*"([^"]+)"\)', read(FW / "console.cpp")))
    doc_cmds = doc_table("| Command | Arguments |")
    for c in sorted(cmds - doc_cmds):
        problems.append(f"console command `{c}` (console.cpp) isn't in the commands table in {DOC.name}")
    for c in sorted(doc_cmds - cmds):
        problems.append(f"console command `{c}` in {DOC.name} isn't in console.cpp")

    if problems:
        print("Contract mismatches:\n" + "\n".join(f"- {p}" for p in problems))
        return 1
    print(f"Contract OK: {len(gs)} gate states, {len(log_names)} log events, {len(cmds)} console commands.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
