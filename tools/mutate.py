#!/usr/bin/env python3
"""Mutation testing for the host tests: does each invariant's test fail when the invariant breaks?

    python3 tools/mutate.py [--only NAME,...] [--invariant TAG] [--jobs N] [--libs DIR] [--list]

Each mutation in tests/native/mutations.json breaks one rule in a scratch copy of firmware/GateLink (exact text
edits: every `find` must occur once), rebuilds the host tests against it (tests/native/Makefile, `FW=`) and runs
them. A mutation is *killed* when some test fails, and *survives* when every test still passes: then the rule it
breaks has no test that would notice, which is a gap to close. Exits 1 if any mutation survives or doesn't apply.

Needs Linux with g++ (the systests load each board's firmware as a shared object); on Windows, run it in the
gatelink-dev container (tests/native/README.md). A base build is made once; each mutation copies it and recompiles
only the files its edits touch. tests/native/README.md describes the catalog.
"""

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NATIVE = ROOT / "tests/native"
FW = ROOT / "firmware/GateLink"
CATALOG = NATIVE / "mutations.json"
PROGRAMS = ("tests", "systests")


class CatalogError(Exception):
    pass


def load_catalog(path):
    """The mutations: [{name, invariant, why, edits: [{file, find, replace}]}], checked for shape and unique names."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise CatalogError(f"{path}: {e}") from e
    seen = set()
    for i, m in enumerate(data):
        where = f"{path} entry {i}"
        for key in ("name", "invariant", "edits"):
            if key not in m:
                raise CatalogError(f"{where}: no {key!r}")
        if not re.fullmatch(r"[a-z0-9_]+", m["name"]):
            raise CatalogError(f"{where}: name {m['name']!r} isn't lower_snake_case")
        if m["name"] in seen:
            raise CatalogError(f"{where}: duplicate name {m['name']!r}")
        seen.add(m["name"])
        if not m["edits"]:
            raise CatalogError(f"{where} ({m['name']}): no edits")
        for e in m["edits"]:
            if set(e) != {"file", "find", "replace"} or e["find"] == e["replace"]:
                raise CatalogError(f"{where} ({m['name']}): an edit needs file, find and a different replace")
    return data


def apply_edits(fw_dir, mutation):
    """Applies a mutation's edits to the firmware copy in `fw_dir`; the files it changed."""
    changed = []
    for e in mutation["edits"]:
        path = Path(fw_dir) / e["file"]
        if not path.exists():
            raise CatalogError(f"{mutation['name']}: no file {e['file']}")
        text = path.read_text(encoding="utf-8")
        n = text.count(e["find"])
        if n != 1:
            raise CatalogError(f"{mutation['name']}: {e['file']} has {n} matches of {e['find']!r}, needs exactly 1")
        path.write_text(text.replace(e["find"], e["replace"]), encoding="utf-8")
        changed.append(path)
    return changed


def failed_tests(output):
    """Names of the tests a runner's output reports as failed (FAIL, or XPASS: an expected failure that passed)."""
    return [m.group(2) for m in re.finditer(r"^(FAIL|XPASS) +([\w.-]+)", output, flags=re.M)]


def make(build, fw, libs, targets, timeout=1800):
    cmd = ["make", "-s", "-C", str(NATIVE), f"BUILD={build}", f"FW={fw}", f"LIBS={libs}", *targets]
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def run_tests(build, fw, libs, timeout, names=None, jobs=None):
    """With `names`, those system tests; else both programs, the system tests in parallel shards (`make run-tests
    run-systests`: not the fuzz replay, which builds the unmutated firmware). Returns (failed test names, crashed: a
    program that died without its summary line)."""
    if names:
        norand = ["setarch", platform.machine(), "-R"] if shutil.which("setarch") else []
        cmd = [*norand, str(Path(build) / "systests"), "--exact", *names]
    else:
        cmd = ["make", "-s", "-C", str(NATIVE), f"BUILD={build}", f"FW={fw}", f"LIBS={libs}", "run-tests",
               "run-systests"]
        if jobs:
            cmd.append(f"JOBS={jobs}")
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, cwd=build)
    except subprocess.TimeoutExpired:
        return [], [f"timed out after {timeout} s"]
    crashed = []
    if names and not re.search(r"^\d+ tests, \d+ failed$", p.stdout, flags=re.M):
        crashed.append(f"systests exit {p.returncode}")
    if not names:
        if len(re.findall(r"^\d+ tests, \d+ failed$", p.stdout, flags=re.M)) < 2:  # tests, and at least one shard
            crashed.append(f"make run exit {p.returncode}")
        shards = re.search(r"^systests: \d+ tests, \d+ failed in (\d+) shards$", p.stdout, flags=re.M)
        if not shards or len(re.findall(r"^\d+ tests, \d+ failed$", p.stdout, flags=re.M)) != int(shards.group(1)) + 1:
            crashed.append("a system test shard died")
    return failed_tests(p.stdout), crashed


def run_one(m, base_build, base_fw, libs, work, timeout, jobs=None):
    """(name, outcome, detail, failed tests): outcome is killed, survived or invalid."""
    d = Path(work) / m["name"]
    build, fw = d / "build", d / "fw"
    shutil.copytree(base_build, build, symlinks=True)
    shutil.copytree(base_fw, fw)  # copy2 keeps the timestamps, so only edited files rebuild
    try:
        changed = apply_edits(fw, m)
    except CatalogError as e:
        return m["name"], "invalid", str(e), []
    later = time.time() + 2
    for f in changed:
        os.utime(f, (later, later))
    r = make(build, fw, libs, [str(build / p) for p in PROGRAMS] + [str(build / "node.so")])
    if r.returncode:
        return m["name"], "invalid", "doesn't build:\n" + (r.stdout + r.stderr)[-1500:], []
    # The tests that killed it when it was written go first: most mutations die there in seconds, and only one that
    # survives them pays for the whole suite.
    failed, crashed = run_tests(build, fw, libs, timeout, m.get("killed_by")) if m.get("killed_by") else ([], [])
    if not (failed or crashed):
        failed, crashed = run_tests(build, fw, libs, timeout, jobs=jobs)
    shutil.rmtree(d, ignore_errors=True)
    if failed or crashed:
        return m["name"], "killed", ", ".join(failed + crashed), failed
    return m["name"], "survived", "every test passed", []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else None)
    ap.add_argument("--catalog", default=str(CATALOG))
    ap.add_argument("--only", help="comma-separated mutation names")
    ap.add_argument("--invariant", help="only mutations with this invariant tag")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument("--libs", default=os.environ.get("LIBS", str(Path.home() / "Arduino/libraries")))
    ap.add_argument("--timeout", type=int, default=1200, help="seconds per test program")
    ap.add_argument("--shard", metavar="I/N", help="every Nth of the selected mutations from the Ith (CI matrix)")
    ap.add_argument("--list", action="store_true", help="list the catalog and exit")
    ap.add_argument("--update", action="store_true",
                    help="record in the catalog the tests that killed each mutation (its killed_by, tried first)")
    args = ap.parse_args(argv)

    try:
        catalog = load_catalog(args.catalog)
    except CatalogError as e:
        print(e, file=sys.stderr)
        return 2
    picked = [m for m in catalog
              if (not args.only or m["name"] in args.only.split(","))
              and (not args.invariant or m["invariant"] == args.invariant)]
    if args.shard:
        i, n = (int(x) for x in args.shard.split("/"))
        picked = picked[i::n]
    if args.list:
        for m in picked:
            print(f"{m['name']:40} {m['invariant']:28} {m.get('why', '')}")
        return 0
    if not picked:
        print("no mutations selected", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="gatelink-mutate-") as work:
        base_fw, base_build = Path(work) / "base-fw", Path(work) / "base-build"
        shutil.copytree(FW, base_fw)
        print(f"base build ({len(picked)} mutations to run)...", flush=True)
        r = make(base_build, base_fw, args.libs, [str(base_build / p) for p in PROGRAMS] + [str(base_build / "node.so")])
        if r.returncode:
            print("the unmutated firmware doesn't build:\n" + (r.stdout + r.stderr)[-3000:], file=sys.stderr)
            return 2
        failed, crashed = run_tests(base_build, base_fw, args.libs, args.timeout)
        if failed or crashed:
            print(f"the unmutated firmware already fails: {', '.join(failed + crashed)}", file=sys.stderr)
            return 2

        results, killers = [], {}
        # A full run of a mutation that outlives its killers shares the CPUs with the other jobs.
        shard_jobs = max(1, (os.cpu_count() or 2) // args.jobs)
        with ThreadPoolExecutor(args.jobs) as ex:
            futures = [ex.submit(run_one, m, base_build, base_fw, args.libs, work, args.timeout, shard_jobs)
                       for m in picked]
            for f in futures:
                name, outcome, detail, failed_names = f.result()
                results.append((name, outcome))
                if outcome == "killed" and failed_names:
                    killers[name] = failed_names
                print(f"{outcome:9} {name}: {detail}", flush=True)

    if args.update and killers:
        for m in catalog:
            if m["name"] in killers:
                m["killed_by"] = sorted(set(killers[m["name"]]))
        Path(args.catalog).write_text(json.dumps(catalog, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"recorded the killing tests of {len(killers)} mutations in {args.catalog}")

    survived = [n for n, o in results if o == "survived"]
    invalid = [n for n, o in results if o == "invalid"]
    print(f"\n{len(results)} mutations: {len(results) - len(survived) - len(invalid)} killed, "
          f"{len(survived)} survived, {len(invalid)} invalid")
    if survived:
        print("survived (a rule no test checks): " + ", ".join(survived))
    return 1 if survived or invalid else 0


if __name__ == "__main__":
    sys.exit(main())
