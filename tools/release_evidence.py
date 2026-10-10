#!/usr/bin/env python3
"""Release evidence: check a GateLink release against docs/release-criteria.md and record what proves it.

    python tools/release_evidence.py collect vX.Y.Z [--results DIR] [--runs RUN ...] [--dry-run]
    python tools/release_evidence.py check vX.Y.Z [--file F] [--ci]

collect reads the bench runs' results/<run>/summary.json (written by tests/e2e/conftest.py), keeps the runs with
both boards on X.Y.Z (or the ones listed), works out the release kind (patch, minor, major) from the previous
release tag, checks every criterion and writes docs/releases/vX.Y.Z.md: a criteria table, one section per run,
hand-written install notes (kept when the file is collected again) and a machine-readable JSON block. It exits 1
if a criterion the release needs isn't met, after writing the file, so it can be looked at.

check validates a committed evidence file: present, for FW_VERSION (firmware/GateLink/config.h), for the firmware
at HEAD (its git tree is the one the bench runs tested), and every criterion its kind needs met; with --ci also
that CI passed on HEAD. Exit 1 otherwise. The release workflow and /release run it before a release is built.

Stdlib only (Python 3.10+). CI results come from the `gh` CLI when it is installed and logged in.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
RESULTS = Path("tests/e2e/results")
RELEASES = Path("docs/releases")
CONFIG_H = "firmware/GateLink/config.h"
FIRMWARE = "firmware/GateLink"
E2E = "tests/e2e"
CI_WORKFLOW = "ci.yml"
SUMMARY_SCHEMA = 1  # tests/e2e/conftest.py SUMMARY_SCHEMA
EVIDENCE_SCHEMA = 1

# The bench hardware matrix (docs/release-criteria.md): each role's board by the tail of its USB serial number (the
# SAMD21 chip id, printed by `tools/gatelink.py ports`). Change the doc and these together; tests/tools checks the
# doc names them.
BENCH_BOARDS = {"house": "183013", "gate": "0C301C"}
NEVER_BOARDS = {"191117": "weak receiver, CRC errors and lost pongs (docs/bench-testing.md)"}
# Local changes here mean the run didn't test the committed firmware, or tested it with changed tests.
SOURCE_PATHS = ("firmware/", "tests/e2e/", "tools/gatelink_client/", "tools/GateSim/")

SOAK_CYCLES = 20
LONGSOAK_MINUTES = 60
LONGSOAK_MAJOR_MINUTES = 24 * 60
FULL_POWER_DBM = 17
# Skips a group may have and still pass: -m power runs only the tests for the LiPo state it finds.
EXPECTED_SKIPS = {"power": re.compile(r"needs the (gate|house) LiPo")}

KINDS = ("patch", "minor", "major")
REQUIRED_FOR = {"patch": "every release", "minor": "MINOR, MAJOR", "major": "MAJOR"}

VERSION_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")
FW_RE = re.compile(r'^#define FW_VERSION "([^"]+)"', re.M)
BLOCK_RE = re.compile(r"<!-- release-evidence\n(.*?)\n-->", re.S)
NOTES_START, NOTES_END = "<!-- notes:start -->", "<!-- notes:end -->"
TICK_RE = re.compile(r"^\s*[-*] \[[xX]\] `?([a-z0-9_]+)`?:[ \t]*(\S.*)$", re.M)


class UsageError(Exception):
    pass


def parse_version(s):
    m = VERSION_RE.match(s.strip()) if s else None
    return tuple(int(x) for x in m.groups()) if m else None


def vstr(v):
    return ".".join(str(x) for x in v)


def fw_version(config_h):
    m = FW_RE.search(config_h or "")
    return m.group(1) if m else None


def release_kind(version, previous):
    """patch, minor or major, from the version change since the previous release (docs/development.md
    "Versioning"). Without a previous release it's a minor one."""
    if previous is None:
        return "minor"
    if version <= previous:
        raise UsageError(f"v{vstr(version)} isn't newer than the previous release v{vstr(previous)}")
    if version[0] != previous[0]:
        return "major"
    return "minor" if version[1] != previous[1] else "patch"


def previous_release(version, tags):
    older = [v for v in (parse_version(t) for t in tags) if v and v < version]
    return max(older) if older else None


# --- git and gh ---------------------------------------------------------------------------------------------------
class Repo:
    """The git checkout and GitHub (through `gh`). Tests replace it with a fake."""

    def __init__(self, root):
        self.root = Path(root)

    def _run(self, *cmd, timeout=60):
        try:
            r = subprocess.run(cmd, cwd=self.root, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=timeout)
        except (OSError, subprocess.SubprocessError) as e:
            return None, str(e)
        if r.returncode:
            return None, (r.stderr.strip().splitlines() or [f"exit status {r.returncode}"])[-1]
        return r.stdout, None

    def git(self, *args):
        return self._run("git", *args)[0]

    def commit(self, rev):
        out = self.git("rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}")
        return out.strip() if out else None

    def tree(self, rev, path):
        out = self.git("rev-parse", "--verify", "--quiet", f"{rev}:{path}")
        return out.strip() if out else None

    def show(self, rev, path):
        return self.git("show", f"{rev}:{path}")

    def tags(self):
        """Release tags: git's, or GitHub's releases if the clone has no tags."""
        out = self.git("tag", "--list", "v*")
        tags = out.split() if out else []
        if not tags and shutil.which("gh"):
            out, _ = self._run("gh", "release", "list", "--limit", "500", "--json", "tagName", "-q", ".[].tagName")
            tags = out.split() if out else []
        return tags

    def ci(self, sha):
        """The latest CI workflow run on `sha`: {"state": success|failure|in_progress|...|none|unavailable,
        "url", "jobs": [[name, conclusion]], "detail"}."""
        if not shutil.which("gh"):
            return {"state": "unavailable", "detail": "the gh CLI isn't installed"}
        out, err = self._run("gh", "run", "list", "--workflow", CI_WORKFLOW, "--commit", sha, "--limit", "50",
                             "--json", "databaseId,status,conclusion,event,url,createdAt,headSha")
        if out is None:
            return {"state": "unavailable", "detail": f"gh run list: {err}"}
        runs = [r for r in json.loads(out) if r.get("headSha") == sha]
        if not runs:
            return {"state": "none", "detail": f"no {CI_WORKFLOW} run on {sha[:10]}"}
        run = max(runs, key=lambda r: r.get("createdAt") or "")
        state = run.get("conclusion") if run.get("status") == "completed" else run.get("status")
        res = {"state": state or "unknown", "url": run.get("url"), "event": run.get("event"), "jobs": []}
        out, _ = self._run("gh", "run", "view", str(run["databaseId"]), "--json", "jobs")
        if out:
            res["jobs"] = [[j.get("name"), j.get("conclusion") or j.get("status")] for j in json.loads(out)["jobs"]]
        return res


# --- bench runs ---------------------------------------------------------------------------------------------------
class Run:
    """One bench run's summary.json."""

    def __init__(self, path, data):
        self.path = Path(path)
        self.data = data
        self.name = data.get("run") or self.path.name
        self.problems = []  # why it doesn't count at all (eligibility)

    def __repr__(self):
        return f"Run({self.name})"

    @property
    def start(self):
        return self.data.get("start") or ""

    @property
    def order(self):
        """The start as a number, for ordering runs (the ISO strings' offsets change with daylight saving)."""
        try:
            return datetime.fromisoformat(self.start).timestamp()
        except ValueError:
            return 0.0

    def option(self, name):
        return (self.data.get("options") or {}).get(name)

    def tests(self, group=None):
        return [t for t in self.data.get("tests", []) if group is None or t.get("group", "default") == group]

    def test(self, nodeid):
        return next((t for t in self.tests() if t.get("nodeid") == nodeid), None)

    def lipos(self):
        """{"gate": "in"|"out", "house": ...} from the power tests' LiPos fact, e.g. "gate out, house in"."""
        return dict(re.findall(r"(gate|house) (in|out)\b", (self.data.get("facts") or {}).get("LiPos", "")))

    def incomplete(self, group):
        """None if every test of `group` was selected and reported, else why not."""
        sel = self.data.get("selection") or {}
        paths = sel.get("paths") or []
        if not paths or any(p.rstrip("/") != E2E for p in paths):
            return f"run on {' '.join(paths) or 'unknown paths'}, not the whole {E2E}"
        if sel.get("filters"):  # these can leave whole files uncollected, which the group counts can't show
            return f"run with {', '.join(sel['filters'])}"
        g = (sel.get("groups") or {}).get(group) or {}
        if not g.get("selected"):
            return "none of its tests selected"
        if g["selected"] != g.get("collected"):
            return f"{g.get('collected', 0) - g['selected']} of its {g.get('collected', 0)} tests left out"
        reported = len(self.tests(group))
        if reported != g["selected"]:
            return f"{reported} of its {g['selected']} selected tests reported (stopped early?)"
        return None

    def faults(self, group):
        """(failed tests, unexpected skips) in `group`."""
        tests = self.tests(group)
        failed = [t for t in tests if t.get("outcome") == "failed"]
        allowed = EXPECTED_SKIPS.get(group)
        skipped = [t for t in tests if t.get("outcome") == "skipped"
                   and not (allowed and allowed.search(t.get("reason") or ""))]
        return failed, skipped

    def passed(self, group):
        failed, skipped = self.faults(group)
        return not failed and not skipped

    def counts(self, group=None):
        c = {}
        for t in self.tests(group):
            c[t.get("outcome")] = c.get(t.get("outcome"), 0) + 1
        return c


def counts_text(counts):
    order = ("passed", "failed", "skipped", "xfailed", "xpassed")
    parts = [f"{counts[k]} {k}" for k in order if counts.get(k)]
    parts += [f"{n} {k}" for k, n in counts.items() if k not in order]
    return ", ".join(parts) or "no tests"


def load_runs(results, names=None):
    """Runs under `results` (each <run>/summary.json), oldest first; only `names` (run names or directories) if
    given, which must all exist. Unreadable summaries are skipped with a warning unless named."""
    if names:
        dirs = []
        for n in names:
            d = Path(n) if Path(n).is_dir() else results / n
            if not (d / "summary.json").is_file():
                raise UsageError(f"no summary.json in {d}")
            dirs.append(d)
    else:
        dirs = sorted(p.parent for p in results.glob("*/summary.json")) if results.is_dir() else []
    runs = []
    for d in dirs:
        try:
            data = json.loads((d / "summary.json").read_text(encoding="utf-8"))
            if not isinstance(data, dict):
                raise ValueError("not a JSON object")
        except (OSError, ValueError) as e:
            if names:
                raise UsageError(f"{d / 'summary.json'}: {e}") from None
            print(f"warning: skipping {d / 'summary.json'}: {e}", file=sys.stderr)
            continue
        runs.append(Run(d, data))
    return sorted(runs, key=lambda r: (r.order, r.name))


def on_version(run, version):
    boards = run.data.get("boards") or {}
    return all((boards.get(r) or {}).get("fw") == version for r in ("house", "gate"))


def eligibility(run, version, release_tree):
    """Why a run can't count as evidence for `version` at all; [] if it can."""
    p = []
    d = run.data
    if d.get("schema") != SUMMARY_SCHEMA:
        p.append(f"summary.json schema {d.get('schema')!r}, expected {SUMMARY_SCHEMA}")
    if d.get("exitstatus") not in (0, 1):
        p.append(f"pytest exit status {d.get('exitstatus')!r}: interrupted or broken")
    if (d.get("selection") or {}).get("collect_errors"):
        p.append(f"{d['selection']['collect_errors']} test files failed to collect")
    for role in ("house", "gate"):
        b = (d.get("boards") or {}).get(role) or {}
        if b.get("fw") != version:
            p.append(f"{role} board on {b.get('fw') or 'unknown firmware'}, not {version}")
        sn = (b.get("usb_serial") or "").upper()
        bad = next((tail for tail in NEVER_BOARDS if sn.endswith(tail)), None)
        if bad:
            p.append(f"{role} board …{bad} is never used: {NEVER_BOARDS[bad]}")
        elif not sn.endswith(BENCH_BOARDS[role]):
            p.append(f"{role} board {sn or 'with no USB serial'} isn't the bench {role} board …{BENCH_BOARDS[role]}")
    git = d.get("git") or {}
    tree = git.get("firmware_tree")
    if not tree:
        p.append("firmware source unknown (no git tree recorded)")
    elif release_tree and tree != release_tree:
        p.append(f"firmware source (tree {tree[:10]}) isn't the release commit's ({release_tree[:10]})")
    local = [x for x in git.get("dirty_paths") or []
             if any(part.strip('"').startswith(SOURCE_PATHS) for part in x.split(" -> "))]
    if local:
        p.append(f"local changes in {', '.join(local[:3])}{' ...' if len(local) > 3 else ''}")
    return p


# --- criteria -----------------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Criterion:
    id: str
    title: str
    kind: str  # the smallest release kind that needs it
    group: str | None = None  # bench criteria: the test group it's judged on ("default" or an opt-in marker)
    shape: Callable | None = None  # which complete runs of the group are attempts at it
    how: str = ""  # the command that makes such a run
    manual: bool = False  # recorded by hand in the evidence file's install notes


CRITERIA = (
    Criterion("ci", "CI green on the release commit (all jobs)", "patch"),
    Criterion("full_suite", "Full suite: default selection, GATELINK_KEY set, the bench's own tx_power", "patch",
              "default", lambda r: r.option("tx_power") is None, "GATELINK_KEY=<key> pytest tests/e2e -v"),
    Criterion("soak", f"Soak: -m soak, at least {SOAK_CYCLES} cycles", "patch", "soak",
              lambda r: (r.option("cycles") or 0) >= SOAK_CYCLES, f"pytest tests/e2e -m soak --cycles {SOAK_CYCLES}"),
    Criterion("power_lipo_out", "Power: -m power with both LiPos out", "minor", "power",
              lambda r: r.lipos() == {"gate": "out", "house": "out"}, "pytest tests/e2e -m power"),
    Criterion("power_lipo_in", "Power: -m power with both LiPos in", "minor", "power",
              lambda r: r.lipos() == {"gate": "in", "house": "in"}, "pytest tests/e2e -m power"),
    Criterion("longsoak_60m", f"Long soak: -m longsoak, at least {LONGSOAK_MINUTES} min", "minor", "longsoak",
              lambda r: (r.option("soak_minutes") or 0) >= LONGSOAK_MINUTES,
              f"pytest tests/e2e -m longsoak --soak-minutes {LONGSOAK_MINUTES}"),
    Criterion("full_suite_17dbm", f"Full suite at --tx-power {FULL_POWER_DBM} on the install-like supplies", "minor",
              "default", lambda r: r.option("tx_power") == FULL_POWER_DBM,
              f"GATELINK_KEY=<key> pytest tests/e2e -v --tx-power {FULL_POWER_DBM}"),
    Criterion("longsoak_24h", f"Long soak: -m longsoak, at least {LONGSOAK_MAJOR_MINUTES // 60} h", "major",
              "longsoak", lambda r: (r.option("soak_minutes") or 0) >= LONGSOAK_MAJOR_MINUTES,
              f"pytest tests/e2e -m longsoak --soak-minutes {LONGSOAK_MAJOR_MINUTES}"),
    Criterion("install_rf", "Real RF at the install site (TODO.md: Real RF)", "major", manual=True),
    Criterion("install_full_power", "Full power at the install for days (TODO.md: Full power at the install)",
              "major", manual=True),
)


def required(c, kind):
    return KINDS.index(c.kind) <= KINDS.index(kind)


@dataclass
class Result:
    met: bool
    evidence: str
    runs: list = field(default_factory=list)  # the runs it rests on
    rerun: dict | None = None


def names(tests):
    return ", ".join(f"`{t.get('name')}`" for t in tests)


def first_reason(tests):
    return next((t.get("reason") for t in tests if t.get("reason")), None) or "no reason recorded"


def conditions(run, group):
    """What a rerun must share with the run it repeats a scenario of."""
    c = {"tx_power": run.option("tx_power")}
    if group == "power":
        c["lipos"] = run.lipos()
    return c


def find_rerun(c, run, test, runs):
    """The first later run, under the same conditions and with the criterion's options (a soak's --cycles, a long
    soak's minutes), that ran `test` again: (run, its outcome there) or None."""
    for r in runs:
        if r.order <= run.order or r.problems or not c.shape(r) or conditions(r, c.group) != conditions(run, c.group):
            continue
        t = r.test(test.get("nodeid"))
        if t is not None:
            return r, t.get("outcome")
    return None


def judge(c, runs, todo):
    """A bench criterion. The latest complete run of its group with its shape decides. If that run failed exactly one
    scenario, the scenario's one rerun may pass it instead, if TODO.md names it as a flaky scenario."""
    complete = [r for r in runs if not r.problems and r.incomplete(c.group) is None]
    attempts = [r for r in complete if c.shape(r)]
    if not attempts:
        other = ", ".join(r.name + (f" (LiPos {r.data['facts']['LiPos']})" if c.group == "power" and r.lipos() else "")
                          for r in complete)
        return Result(False, f"no run yet: `{c.how}`" + (f"; other runs of its tests: {other}" if other else ""))
    latest = attempts[-1]
    earlier = [r.name for r in attempts[:-1] if not r.passed(c.group)]
    note = f" (earlier attempts that failed: {', '.join(earlier)})" if earlier else ""
    head = f"run {latest.name}: {counts_text(latest.counts(c.group))}"
    failed, skipped = latest.faults(c.group)
    if skipped:
        return Result(False, f"{head}; unexpected skip of {names(skipped)}: {first_reason(skipped)}{note}",
                      [latest.name])
    if not failed:
        return Result(True, head + note, [latest.name])
    if len(failed) > 1:
        return Result(False, f"{head}; {len(failed)} failed: {names(failed)}{note}", [latest.name])
    t = failed[0]
    what = f"{head}; `{t.get('name')}` failed ({t.get('reason') or 'no reason recorded'})"
    found = find_rerun(c, latest, t, runs)
    if found is None:
        return Result(False, f"{what}, no rerun of it yet{note}", [latest.name])
    rr, outcome = found
    if outcome != "passed":
        return Result(False, f"{what} and its rerun {rr.name} did not pass ({outcome}){note}",
                      [latest.name, rr.name])
    base = (t.get("name") or "").split("[")[0]
    rerun = {"run": latest.name, "test": t.get("nodeid"), "reason": t.get("reason"), "rerun": rr.name}
    if not base or base not in todo:
        return Result(False, f"{what}; its rerun {rr.name} passed, but TODO.md doesn't name `{base}` as a flaky "
                             f"scenario{note}", [latest.name, rr.name], rerun)
    return Result(True, f"{what}; passed its one rerun {rr.name}{note}", [latest.name, rr.name], rerun)


def judge_ci(ci):
    state = ci.get("state")
    jobs = ", ".join(f"{n} {s}" for n, s in ci.get("jobs") or [])
    link = f"[{CI_WORKFLOW} run]({ci['url']})" if ci.get("url") else f"{CI_WORKFLOW}"
    if state == "success":
        return Result(True, f"{link}: success" + (f" ({jobs})" if jobs else ""))
    return Result(False, f"{link}: {state}" + (f" ({jobs})" if jobs else "") +
                  (f" - {ci['detail']}" if ci.get("detail") else ""))


def ticked(notes):
    """{criterion id: note} for the ticked install checks in the notes ("- [x] install_rf: what was done")."""
    return {m.group(1): m.group(2).strip() for m in TICK_RE.finditer(notes or "")}


def judge_manual(c, notes):
    note = ticked(notes).get(c.id)
    if note:
        return Result(True, f"by hand: {note}")
    return Result(False, f"not ticked in the install notes (`- [x] {c.id}: <what was done, when, numbers>`)")


def evaluate(runs, ci, todo, notes):
    out = {}
    for c in CRITERIA:
        if c.id == "ci":
            out[c.id] = judge_ci(ci)
        elif c.manual:
            out[c.id] = judge_manual(c, notes)
        else:
            out[c.id] = judge(c, runs, todo)
    return out


# --- the evidence file --------------------------------------------------------------------------------------------
def default_notes(kind):
    lines = ["Hand-written, and kept when the file is collected again: anything a reviewer should know."]
    if kind == "major":
        lines += ["Tick each install check and say what was done, when, and the numbers (docs/release-criteria.md):",
                  "", "- [ ] install_rf: ", "- [ ] install_full_power: "]
    return "\n".join(lines)


def existing_notes(text):
    if not text or NOTES_START not in text or NOTES_END not in text:
        return None
    return text.split(NOTES_START, 1)[1].split(NOTES_END, 1)[0].strip("\n")


def cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def when(start, end):
    try:
        a, b = datetime.fromisoformat(start), datetime.fromisoformat(end)
    except (TypeError, ValueError):
        return f"{start or '?'} to {end or '?'}", None
    minutes = (b - a).total_seconds() / 60
    span = f"{a:%Y-%m-%d %H:%M} to {b:%H:%M}" if a.date() == b.date() else f"{a:%Y-%m-%d %H:%M} to {b:%Y-%m-%d %H:%M}"
    dur = f"{minutes / 60:.1f} h" if minutes >= 120 else f"{minutes:.0f} min"
    return f"{span} {a:%z} ({dur})", round(minutes, 1)


def run_record(run, counts_for):
    d = run.data
    git = d.get("git") or {}
    return {
        "run": run.name, "start": d.get("start"), "end": d.get("end"), "minutes": when(d.get("start"), d.get("end"))[1],
        "command": d.get("command"),
        "selection": {k: (d.get("selection") or {}).get(k) for k in ("markexpr", "keyword", "paths")},
        "commit": git.get("commit"), "firmware_tree": git.get("firmware_tree"),
        "dirty_paths": git.get("dirty_paths") or [], "boards": d.get("boards"), "options": d.get("options"),
        "counts": run.counts(), "counts_for": counts_for,
        "failed": [{"test": t.get("nodeid"), "reason": t.get("reason")} for t in run.tests()
                   if t.get("outcome") == "failed"],
        "skipped": [{"test": t.get("nodeid"), "reason": t.get("reason")} for t in run.tests()
                    if t.get("outcome") == "skipped"],
        "latencies": d.get("latencies") or [], "facts": d.get("facts") or {}, "anomalies": d.get("anomalies") or [],
    }


def board_text(b):
    sn = (b or {}).get("usb_serial") or ""
    return f"{(b or {}).get('fw') or '?'} (…{sn[-6:] or '?'}, {(b or {}).get('port') or '?'})"


def options_text(run):
    """The options that shaped this run: tx_power always, cycles and minutes only for the soaks it ran."""
    opts = run.data.get("options") or {}
    groups = ((run.data.get("selection") or {}).get("groups")) or {}
    tx = opts.get("tx_power")
    parts = [f"tx_power {tx} dBm (--tx-power)" if tx is not None else "the boards' saved tx_power (5 dBm or less)"]
    if (groups.get("soak") or {}).get("selected"):
        parts.append(f"--cycles {opts.get('cycles')}")
    if (groups.get("longsoak") or {}).get("selected"):
        parts.append(f"--soak-minutes {opts.get('soak_minutes')}")
    parts.append("GATELINK_KEY set" if opts.get("gatelink_key") else "no GATELINK_KEY")
    return ", ".join(parts)


def render_run(run, counts_for):
    d = run.data
    git = d.get("git") or {}
    boards = d.get("boards") or {}
    span, _ = when(d.get("start"), d.get("end"))
    lines = [f"### {run.name}", "",
             f"- **When:** {span}",
             f"- **Command:** `{d.get('command')}`",
             f"- **Options:** {options_text(run)}",
             f"- **Commit:** `{(git.get('commit') or '?')[:10]}`, firmware tree `{(git.get('firmware_tree') or '?')[:10]}`"
             + (f"; local changes: {', '.join(git['dirty_paths'][:5])}" if git.get("dirty_paths") else ""),
             f"- **Boards:** house {board_text(boards.get('house'))}, gate {board_text(boards.get('gate'))}",
             f"- **Results:** {counts_text(run.counts())}"]
    for t in run.tests():
        if t.get("outcome") in ("failed", "skipped", "xfailed", "xpassed"):
            lines.append(f"  - {t.get('outcome')}: `{t.get('name')}`" + (f" ({t['reason']})" if t.get("reason") else ""))
    lines.append(f"- **Counts for:** {', '.join(counts_for) or 'nothing'}")
    facts = d.get("facts") or {}
    lines.append("- **Facts:**" + ("" if facts else " none"))
    lines += [f"  - {k}: {v}" for k, v in facts.items()]
    anomalies = d.get("anomalies") or []
    lines.append("- **Anomalies:**" + ("" if anomalies else " none"))
    lines += [f"  - {a}" for a in anomalies]
    return lines


def render(info, results, runs, others, notes):
    kind = info["kind"]
    counts_for = {r.name: [] for r in runs}
    for cid, res in results.items():
        for n in res.runs:
            counts_for.setdefault(n, []).append(cid)
    unmet = [c.id for c in CRITERIA if required(c, kind) and not results[c.id].met]
    prev = f"v{info['previous']}" if info["previous"] else "none"
    lines = [f"# GateLink v{info['version']}: release evidence", "",
             f"**{kind.upper()}** release (previous release: {prev}), commit `{info['commit'][:10]}`, firmware tree "
             f"`{info['firmware_tree'][:10]}`. Written by `python tools/release_evidence.py collect v{info['version']}`"
             f" on {info['collected'][:10]}; the criteria are in [docs/release-criteria.md](../release-criteria.md).",
             "",
             ("**All criteria for this release are met.**" if not unmet else
              f"**Not ready:** {len(unmet)} required criteria not met ({', '.join(unmet)})."),
             "", "## Criteria", "", "| Criterion | Required for | Met | Evidence |", "|---|---|---|---|"]
    for c in CRITERIA:
        res = results[c.id]
        met = "yes" if res.met else "**no**"
        if not required(c, kind):
            met = ("yes" if res.met else "no") + " (not required)"
        lines.append(f"| `{c.id}`: {cell(c.title)} | {REQUIRED_FOR[c.kind]} | {met} | {cell(res.evidence)} |")
    reruns = [r.rerun for r in results.values() if r.rerun]
    if reruns:
        lines += ["", "## Reruns", "", "One failed scenario per criterion may be rerun once (docs/release-criteria.md).",
                  "", "| Run | Scenario | Failure | Rerun |", "|---|---|---|---|"]
        lines += [f"| {x['run']} | `{cell(x['test'])}` | {cell(x['reason'] or '')} | {x['rerun']} |" for x in reruns]
    lines += ["", "## Runs", ""]
    if not runs:
        lines.append(f"No bench run on {info['version']} counts yet.")
    for r in runs:
        lines += render_run(r, counts_for.get(r.name, [])) + [""]
    if others:
        lines += ["", "### Not counted", "", "| Run | Why |", "|---|---|"]
        lines += [f"| {r.name} | {cell('; '.join(r.problems))} |" for r in others]
    lines += ["", "## Install notes", "", NOTES_START, notes, NOTES_END, ""]
    data = {**info, "schema": EVIDENCE_SCHEMA,
            "criteria": [{"id": c.id, "title": c.title, "required_for": c.kind, "required": required(c, kind),
                          "manual": c.manual, "met": results[c.id].met, "evidence": results[c.id].evidence,
                          "runs": results[c.id].runs} for c in CRITERIA],
            "reruns": reruns,
            "runs": [run_record(r, counts_for.get(r.name, [])) for r in runs],
            "not_counted": [{"run": r.name, "why": r.problems} for r in others]}
    # '>' only occurs inside JSON strings, so escaping it keeps "-->" from closing the comment.
    block = json.dumps(data, indent=1, ensure_ascii=False).replace(">", "\\u003e")
    lines += ["<!-- release-evidence", block, "-->", ""]
    return "\n".join(lines), unmet


def read_block(text):
    m = BLOCK_RE.search(text)
    if not m:
        raise ValueError("no release-evidence JSON block")
    data = json.loads(m.group(1))
    if not isinstance(data, dict):
        raise ValueError("the release-evidence block isn't a JSON object")
    return data


# --- commands -----------------------------------------------------------------------------------------------------
def collect(args, repo):
    root = repo.root
    v = parse_version(args.version)
    if not v:
        raise UsageError(f"{args.version!r} isn't a version like v1.2.3")
    version = vstr(v)
    sha = repo.commit(args.commit)
    if not sha:
        raise UsageError(f"can't resolve {args.commit!r} to a commit")
    fw = fw_version(repo.show(sha, CONFIG_H))
    if fw != version:
        raise UsageError(f"{args.commit} ({sha[:10]}) has FW_VERSION {fw}, not {version}: check out the commit "
                         "that carries it (main once its pull request is merged)")
    tree = repo.tree(sha, FIRMWARE)
    if not tree:
        raise UsageError(f"can't read the {FIRMWARE} tree at {sha[:10]}")
    tags = repo.tags()
    if args.previous:
        prev = parse_version(args.previous)
        if not prev:
            raise UsageError(f"--previous {args.previous!r} isn't a version like v1.2.3")
    else:
        prev = previous_release(v, tags)
    kind = release_kind(v, prev)
    if args.kind and KINDS.index(args.kind) > KINDS.index(kind):
        kind = args.kind  # --kind can only raise the bar
    results_dir = Path(args.results) if args.results else root / RESULTS
    runs = load_runs(results_dir, args.runs)
    if not args.runs:
        runs = [r for r in runs if on_version(r, version)]
    for r in runs:
        r.problems = eligibility(r, version, tree)
    counted = [r for r in runs if not r.problems]
    others = [r for r in runs if r.problems]
    out = Path(args.out) if args.out else root / RELEASES / f"v{version}.md"
    old = out.read_text(encoding="utf-8") if out.is_file() else None
    notes = existing_notes(old)
    if notes is None:
        notes = default_notes(kind)
    elif kind == "major":  # kept notes from a smaller kind: add the install checks
        missing = [c.id for c in CRITERIA if c.manual and c.id not in notes]
        notes = "\n".join([notes, ""] + [f"- [ ] {m}: " for m in missing]) if missing else notes
    todo_path = root / "TODO.md"
    todo = todo_path.read_text(encoding="utf-8") if todo_path.is_file() else ""
    results = evaluate(counted, repo.ci(sha), todo, notes)
    info = {"version": version, "kind": kind, "previous": vstr(prev) if prev else None, "commit": sha,
            "firmware_tree": tree, "collected": datetime.now().astimezone().isoformat(timespec="seconds")}
    text, unmet = render(info, results, counted, others, notes)

    print(f"GateLink v{version}: {kind} release (previous {f'v{vstr(prev)}' if prev else 'none'}), "
          f"commit {sha[:10]}, firmware tree {tree[:10]}")
    if f"v{version}" in tags:
        print(f"note: v{version} is already tagged")
    print(f"runs on {version}: {len(counted)} counted, {len(others)} not counted")
    for r in others:
        print(f"  not counted: {r.name}: {'; '.join(r.problems)}")
    for c in CRITERIA:
        res = results[c.id]
        mark = "met" if res.met else ("NOT MET" if required(c, kind) else "not met, not required")
        print(f"  [{mark}] {c.id}: {res.evidence}")
    if args.dry_run:
        print("dry run: nothing written")
    else:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {out}")
    if unmet:
        print(f"{len(unmet)} required criteria not met: {', '.join(unmet)}")
        return 1
    return 0


def check(args, repo):
    """Problems with the committed evidence for args.version; [] if it's complete."""
    root = repo.root
    v = parse_version(args.version)
    if not v:
        raise UsageError(f"{args.version!r} isn't a version like v1.2.3")
    version = vstr(v)
    path = Path(args.file) if args.file else root / RELEASES / f"v{version}.md"
    if not path.is_file():
        return [f"no release evidence: {path} is missing (python tools/release_evidence.py collect v{version}; "
                "see docs/release-criteria.md)"]
    text = path.read_text(encoding="utf-8")
    try:
        data = read_block(text)
    except ValueError as e:
        return [f"{path}: {e}"]
    problems = []
    cfg = root / CONFIG_H
    fw = fw_version(cfg.read_text(encoding="utf-8")) if cfg.is_file() else None
    if fw != version:
        problems.append(f"FW_VERSION in {CONFIG_H} is {fw}, not {version}")
    if data.get("version") != version:
        problems.append(f"{path} is evidence for {data.get('version')}, not {version}")
    kind = data.get("kind")
    if kind not in KINDS:
        return problems + [f"{path}: unknown release kind {kind!r}"]
    prev = previous_release(v, repo.tags())  # older than v, so release_kind can't refuse it
    should = release_kind(v, prev)  # with no earlier release a minor one, as collect has it
    if KINDS.index(should) > KINDS.index(kind):
        after = f"after v{vstr(prev)}" if prev else "with no earlier release tag"
        problems.append(f"collected as a {kind} release, but {after} it's a {should} one: collect it again")
    tree = repo.tree("HEAD", FIRMWARE)
    if not tree:
        problems.append(f"can't read the {FIRMWARE} tree at HEAD (not a git checkout?)")
    elif tree != data.get("firmware_tree"):
        problems.append(f"the bench runs tested firmware tree {str(data.get('firmware_tree'))[:10]}, but HEAD has "
                        f"{tree[:10]}: the firmware changed after them")
    recorded = {c.get("id"): c for c in data.get("criteria") or [] if isinstance(c, dict)}
    notes = existing_notes(text) or ""
    for c in CRITERIA:
        if not required(c, kind):
            continue
        if c.manual:
            res = judge_manual(c, notes)
            if not res.met:
                problems.append(f"{c.id} ({c.title}): {res.evidence}")
            continue
        r = recorded.get(c.id)
        if r is None:
            problems.append(f"{c.id} ({c.title}) missing from the evidence")
        elif r.get("met") is not True:
            problems.append(f"{c.id} ({c.title}) not met: {r.get('evidence')}")
    if args.ci:
        sha = repo.commit("HEAD")
        res = judge_ci(repo.ci(sha) if sha else {"state": "unavailable", "detail": "no HEAD commit"})
        if not res.met:
            problems.append(f"CI on the release commit {(sha or '?')[:10]}: {res.evidence}")
    return problems


def main(argv=None, repo=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=str(ROOT), help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect", help="check the criteria against the bench runs and write the evidence file")
    c.add_argument("version", help="the release, e.g. v0.14.0")
    c.add_argument("--results", help=f"bench results directory (default {RESULTS.as_posix()})")
    c.add_argument("--runs", nargs="+", metavar="RUN", help="use these runs (names or directories) instead of every "
                   "run with both boards on the version")
    c.add_argument("--commit", default="HEAD", help="the commit being released (default HEAD)")
    c.add_argument("--previous", help="the previous release (default: the newest older v* tag)")
    c.add_argument("--kind", choices=KINDS, help="treat the release as at least this kind")
    c.add_argument("--out", help=f"evidence file (default {RELEASES.as_posix()}/vX.Y.Z.md)")
    c.add_argument("--dry-run", action="store_true", help="print the verdict without writing the file")
    k = sub.add_parser("check", help="validate a committed evidence file")
    k.add_argument("version", help="the release, e.g. v0.14.0")
    k.add_argument("--file", help=f"evidence file (default {RELEASES.as_posix()}/vX.Y.Z.md)")
    k.add_argument("--ci", action="store_true", help="also require CI to have passed on HEAD (needs gh)")
    args = ap.parse_args(argv)
    repo = repo or Repo(args.root)
    try:
        if args.cmd == "collect":
            return collect(args, repo)
        problems = check(args, repo)
    except UsageError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if problems:
        print(f"release evidence for {args.version} is not complete:")
        for p in problems:
            print(f"  - {p}")
        print("see docs/release-criteria.md")
        return 1
    print(f"release evidence for {args.version}: complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
