#!/usr/bin/env python3
"""Claude Code hooks for GateLink, run by .claude/settings.json through tools/agent/hook.sh.

    hooks.py session-start   remember what the branch already lacks; in a cloud session, install the toolchain
    hooks.py post-edit       fast checks for the file just written (syntax, lint, the console contract, links)
    hooks.py stop            before finishing, ask once about the things that change together (FW_VERSION, docs,
                             tests, TODO.md/ROADMAP.md, the skills that describe a changed file)
    hooks.py check [--base REF] [--only ITEM]   print the stop findings for this branch (no state; for people and
                             CI, which fails a pull request that changes firmware without bumping FW_VERSION)
    hooks.py paths FILE...   repo paths named in these Markdown files that don't exist

Each hook reads its event as JSON on stdin. A hook never breaks the session: an error of its own is reported on
stderr and ignored. docs/agent-tooling.md describes them; tests/tools/test_agent_tooling.py tests them.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

CHECK_TIMEOUT_S = 25  # per external check; settings.json gives each hook 60 s
STATE_MAX_AGE_S = 14 * 24 * 3600  # stop-hook state files older than this are removed
FW = "firmware/GateLink/"

# The console contract: tools/check_contract.py compares these.
CONTRACT_FILES = (
    FW + "console.cpp",
    FW + "log.cpp",
    FW + "log.h",
    FW + "roles.h",
    "docs/console.md",
    "tools/check_contract.py",
)
CONTRACT_DIRS = ("tools/gatelink_client/", "tests/e2e/gatelink/")
# What tools/docgen.py reads besides the docs themselves.
DOCGEN_SOURCES = (FW, "web/js/", "tests/e2e/", "tools/release_evidence.py", "tools/bench-wiring/wiring.json",
                  "tools/docgen.py")


# ---- helpers ------------------------------------------------------------------------------------------------


def run(cmd, cwd, timeout=CHECK_TIMEOUT_S):
    """(exit code, stdout + stderr), or None if the program isn't installed."""
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                           timeout=timeout)
    except FileNotFoundError:
        return None
    except subprocess.TimeoutExpired:
        return 124, f"timed out after {timeout} s"
    return p.returncode, (p.stdout + p.stderr).strip()


def git(root, *args):
    """git's stdout, or None if it failed."""
    try:
        p = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=CHECK_TIMEOUT_S)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    return p.stdout if p.returncode == 0 else None


def repo_root(path):
    """The work tree holding `path` (a file or directory), or None outside a git repo."""
    p = Path(path)
    d = p if p.is_dir() else p.parent
    while not d.exists() and d != d.parent:  # a deleted file's directory may be gone too
        d = d.parent
    top = git(d, "rev-parse", "--show-toplevel")
    return Path(top.strip()) if top else None


def tail(text, lines=30):
    out = text.strip().splitlines()
    return "\n".join(out[-lines:] if len(out) > lines else out)


def read_event():
    try:
        data = sys.stdin.read()
        return json.loads(data) if data.strip() else {}
    except (OSError, ValueError):
        return {}


# ---- checks shared by post-edit and the tests ---------------------------------------------------------------

# A repo path in prose or a command: starts at a top-level directory of this repo.
PATH_RE = re.compile(r"(?<![\w./~-])((?:firmware/GateLink|web|tools|tests|docs|\.claude|\.github)/[\w./-]*[\w/])")
# What comes right after a placeholder path: `results/<run>/`, `vX.Y.Z.md`, `js/*.js`, `${x}`.
PLACEHOLDER_NEXT = ("<", "{", "*", "$", "[")


def stale_paths(text, root):
    """Repo paths named in `text` that don't exist (placeholders and gitignored paths are skipped)."""
    missing = []
    for m in PATH_RE.finditer(text):
        path = m.group(1).rstrip(".")
        nxt = text[m.end():m.end() + 1]
        if nxt in PLACEHOLDER_NEXT or "X.Y.Z" in path or "x.y.z" in path:
            continue
        if (root / path).exists() or path in missing:
            continue
        ignored = git(root, "check-ignore", "-q", "--no-index", path)
        if ignored is not None:  # exit 0: ignored, so it may not exist in a fresh clone
            continue
        missing.append(path)
    return missing


MD_LINK_RE = re.compile(r"\]\(([^)\s]+)\)")


def broken_links(md_file):
    """Relative links in a Markdown file whose target doesn't exist."""
    text = Path(md_file).read_text(encoding="utf-8", errors="replace")
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    bad = []
    for target in MD_LINK_RE.findall(text):
        if re.match(r"[a-z]+:", target) or target.startswith("#"):
            continue
        path = target.split("#", 1)[0]
        if not path:
            continue
        resolved = (Path(md_file).parent / path).resolve()
        if not resolved.exists():
            bad.append(target)
    return bad


FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n", re.S)
# `/name` used as a skill: after a space, backtick or bracket, and not part of a path (`/c/Users`, `/tmp/x`).
SKILL_REF_RE = re.compile(r"(?:^|(?<=[\s`(]))/([a-z][a-z0-9-]*)(?![\w/-]|\.\w)", re.M)


def frontmatter(text):
    """The `key: value` lines of a Markdown file's front matter, or None if it has none."""
    m = FRONTMATTER_RE.match(text.replace("\r\n", "\n"))
    if not m:
        return None
    fields = {}
    for line in m.group(1).splitlines():
        if ":" in line and not line.startswith((" ", "\t")):
            k, v = line.split(":", 1)
            fields[k.strip()] = v.strip()
    return fields


def skill_problems(path, root):
    """What's wrong with a .claude/skills/<name>/SKILL.md."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    fm = frontmatter(text)
    if fm is None:
        return ["no front matter (--- name/description ---)"]
    problems = []
    if fm.get("name") != Path(path).parent.name:
        problems.append(f"name {fm.get('name')!r} isn't the directory name {Path(path).parent.name!r}")
    if len(fm.get("description", "")) < 40:
        problems.append("description missing or too short to say when to use the skill")
    problems += [f"names {p}, which doesn't exist" for p in stale_paths(text, root)]
    return problems


def routine_problems(path, root):
    """What's wrong with a .claude/routines/<name>.md."""
    text = Path(path).read_text(encoding="utf-8", errors="replace")
    fm = frontmatter(text)
    if fm is None:
        return ["no front matter (--- name/schedule/description ---)"]
    problems = []
    if fm.get("name") != Path(path).stem:
        problems.append(f"name {fm.get('name')!r} isn't the file name {Path(path).stem!r}")
    if not re.fullmatch(r"\S+ \S+ \S+ \S+ \S+", fm.get("schedule", "")):
        problems.append("schedule isn't a 5-field cron expression")
    if not fm.get("description"):
        problems.append("no description")
    for skill in SKILL_REF_RE.findall(text):
        if not (root / ".claude/skills" / skill / "SKILL.md").exists():
            problems.append(f"uses /{skill}, which isn't a skill in .claude/skills")
    problems += [f"names {p}, which doesn't exist" for p in stale_paths(text, root)]
    return problems


def settings_problems(path, root):
    """What's wrong with .claude/settings.json: unreadable, or a hook naming a script that isn't there."""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except ValueError as e:
        return [f"not valid JSON: {e}"]
    problems = []
    for event, groups in data.get("hooks", {}).items():
        for group in groups:
            for hook in group.get("hooks", []):
                for script in re.findall(r"tools/agent/[\w.-]+", hook.get("command", "")):
                    if not (root / script).exists():
                        problems.append(f"{event} hook runs {script}, which doesn't exist")
    return problems


# ---- post-edit ----------------------------------------------------------------------------------------------


def check_node_syntax(root, rel):
    if not shutil.which("node"):
        return None
    r = run(["node", "--check", rel], root)
    return None if r is None or r[0] == 0 else f"node --check {rel}:\n{tail(r[1])}"


def check_eslint(root, rel):
    eslint = root / "node_modules/eslint/bin/eslint.js"
    if not eslint.exists() or not shutil.which("node"):
        return None  # npm ci not run here; CI lints
    r = run(["node", str(eslint), "--no-warn-ignored", rel], root)
    return None if r is None or r[0] == 0 else f"eslint {rel} (eslint.config.js):\n{tail(r[1])}"


def check_py_syntax(root, rel):
    try:
        compile((root / rel).read_text(encoding="utf-8"), rel, "exec")
    except SyntaxError as e:
        return f"{rel}:{e.lineno}: {e.msg}"
    return None


def check_ruff(root, rel):
    if not (rel.startswith(("tools/", "tests/"))):
        return None
    cmd = ["ruff"] if shutil.which("ruff") else None
    if cmd is None:
        return None  # not installed here; CI lints
    r = run([*cmd, "check", "--quiet", rel], root)
    return None if r is None or r[0] == 0 else f"ruff check {rel} (ruff.toml):\n{tail(r[1])}"


def check_contract(root, rel):
    script = root / "tools/check_contract.py"
    if not script.exists():
        return None
    r = run([sys.executable, str(script)], root)
    return None if r is None or r[0] == 0 else f"tools/check_contract.py (after editing {rel}):\n{tail(r[1])}"


def check_docgen(root, rel):
    script = root / "tools/docgen.py"
    if not script.exists():
        return None
    r = run([sys.executable, str(script)], root)
    return None if r is None or r[0] == 0 else f"tools/docgen.py (after editing {rel}):\n{tail(r[1])}"


def check_json(root, rel):
    try:
        json.loads((root / rel).read_text(encoding="utf-8"))
    except ValueError as e:
        return f"{rel} isn't valid JSON: {e}"
    return None


def check_yaml(root, rel):
    try:
        import yaml  # optional: PyYAML
    except ImportError:
        return None
    try:
        yaml.safe_load((root / rel).read_text(encoding="utf-8"))
    except yaml.YAMLError as e:
        return f"{rel} isn't valid YAML: {e}"
    return None


def check_markdown(root, rel):
    bad = broken_links(root / rel)
    return f"{rel} links to files that don't exist: {', '.join(bad)}" if bad else None


def check_settings(root, rel):
    problems = settings_problems(root / rel, root)
    return f"{rel}: " + "; ".join(problems) if problems else None


def check_skill(root, rel):
    problems = skill_problems(root / rel, root)
    return f"{rel}: " + "; ".join(problems) if problems else None


def check_routine(root, rel):
    problems = routine_problems(root / rel, root)
    return f"{rel}: " + "; ".join(problems) if problems else None


def is_contract(rel):
    return rel in CONTRACT_FILES or rel.startswith(CONTRACT_DIRS) and rel.endswith(".py")


def post_edit_checks(rel):
    """The checks for an edited file (repo-relative, '/'-separated), cheapest first."""
    checks = []
    js = rel.endswith((".js", ".mjs"))
    if js and (rel.startswith(("web/", "tests/web/")) or rel == "eslint.config.js"):
        checks += [check_node_syntax, check_eslint]
    if rel.endswith(".py"):
        checks += [check_py_syntax, check_ruff]
    if rel.endswith(".json"):
        checks.append(check_json)
    if rel.startswith(".github/workflows/") and rel.endswith((".yml", ".yaml")):
        checks.append(check_yaml)
    if rel.endswith(".md"):
        checks.append(check_markdown)
    if rel == ".claude/settings.json":
        checks.append(check_settings)
    if re.fullmatch(r"\.claude/skills/[^/]+/SKILL\.md", rel):
        checks.append(check_skill)
    if re.fullmatch(r"\.claude/routines/[^/]+\.md", rel) and not rel.endswith("README.md"):
        checks.append(check_routine)
    if is_contract(rel):
        checks.append(check_contract)
    if rel.endswith(".md"):
        checks.append(check_docgen)  # a doc edit: does it still agree with the source?
    return checks


def edited_files(event):
    """Paths the tool call wrote."""
    ti = event.get("tool_input") or {}
    paths = [str(ti[k]) for k in ("file_path", "notebook_path") if ti.get(k)]
    for edit in ti.get("edits") or []:  # MultiEdit-style batches
        if isinstance(edit, dict) and edit.get("file_path"):
            paths.append(edit["file_path"])
    return paths


def post_edit(event):
    failures = []
    for path in edited_files(event):
        if not Path(path).is_absolute():
            path = Path(event.get("cwd") or os.getcwd()) / path
        root = repo_root(path)
        if root is None or not Path(path).exists():
            continue
        try:
            rel = Path(path).resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            continue
        for check in post_edit_checks(rel):
            msg = check(root, rel)
            if msg:
                failures.append(msg)
    if not failures:
        return 0
    # Exit 2: Claude sees stderr after the edit (the edit itself stands).
    print("GateLink post-edit checks failed (tools/agent/hooks.py); fix before going on:\n\n"
          + "\n\n".join(failures), file=sys.stderr)
    return 2


# ---- stop: things that change together ----------------------------------------------------------------------


def find_base(root, base=None):
    """The commit the branch's changes are measured from: where it left origin/main (or main)."""
    refs = [base] if base else ["origin/main", "main"]
    for ref in refs:
        mb = git(root, "merge-base", "HEAD", ref)
        if mb:
            return mb.strip()
    return None


def changed_files(root, base):
    """Files changed since `base` (or HEAD), committed or not, plus untracked files."""
    diff = git(root, "diff", "--name-only", "--no-renames", base or "HEAD") or ""
    untracked = git(root, "ls-files", "--others", "--exclude-standard") or ""
    return sorted({f for f in (diff + "\n" + untracked).splitlines() if f.strip()})


def show(root, base, rel):
    return git(root, "show", f"{base}:{rel}") if base else git(root, "show", f"HEAD:{rel}")


def fw_version(text):
    m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', text or "")
    return m.group(1) if m else None


class Finding:
    def __init__(self, rule, files, message):
        self.key = rule + ":" + hashlib.sha1("\n".join(sorted(files)).encode()).hexdigest()[:12]
        self.rule = rule
        self.message = message

    def __repr__(self):
        return f"Finding({self.rule})"


def names(files, limit=4):
    files = sorted(files)
    shown = ", ".join(f.rsplit("/", 1)[-1] for f in files[:limit])
    return shown + (f" and {len(files) - limit} more" if len(files) > limit else "")


CODE_PREFIXES = ("firmware/", "web/", "tools/", "tests/", ".github/", ".claude/")
TEST_SUFFIXES = (".py", ".cpp", ".h", ".js", ".mjs", ".ino", ".html", ".css", ".sh", ".yml", ".json")


def stop_findings(root, base=None):
    """What the branch changed without its usual companions. Each is a reminder, not a verdict."""
    base = find_base(root, base)
    changed = changed_files(root, base)
    ch = set(changed)
    out = []

    def touched(prefix):
        return any(f.startswith(prefix) for f in ch)

    fw = [f for f in changed if f.startswith(FW) and f.endswith((".cpp", ".h", ".ino"))]  # sources, not tool config
    if fw:
        old = fw_version(show(root, base, FW + "config.h"))
        new_path = root / FW / "config.h"
        new = fw_version(new_path.read_text(encoding="utf-8")) if new_path.exists() else None
        if old and new and old == new:
            out.append(Finding("fw-version", fw, (
                f"Firmware changed ({names(fw)}) but FW_VERSION in firmware/GateLink/config.h is still {new}: "
                "every firmware change bumps it (PATCH; MINOR for a CFG_VERSION, STATUS/DIAG or frame format "
                "change or a new feature; docs/development.md \"Versioning\").")))

    tested = [f for f in changed if f in (FW + "link.cpp", FW + "link.h", FW + "config.cpp", FW + "config.h",
                                          FW + "extflash.cpp", FW + "histlog.cpp", FW + "histlog.h")]
    if tested and not touched("tests/native/"):
        out.append(Finding("native-tests", tested, (
            f"{names(tested)} changed but nothing under tests/native: extend the host tests (`make -C "
            "tests/native`), above all for paths the bench can't reach (replays, restarts, wraps, power cuts "
            "mid-save).")))

    console = [f for f in changed if f in (FW + "console.cpp", FW + "log.cpp", FW + "log.h", FW + "roles.h")]
    if console and "docs/console.md" not in ch:
        out.append(Finding("console-docs", console, (
            f"{names(console)} changed but docs/console.md didn't: commands, status fields and log events are "
            "documented there (and parsed by web/js/, tools/gatelink_client/ and tests/e2e/).")))

    if any(is_contract(f) for f in changed) and (root / "tools/check_contract.py").exists():
        r = run([sys.executable, str(root / "tools/check_contract.py")], root)
        if r and r[0] != 0:
            out.append(Finding("contract", [r[1]], f"tools/check_contract.py fails:\n{tail(r[1], 15)}"))

    docgen = root / "tools/docgen.py"
    if docgen.exists() and any(f.startswith(DOCGEN_SOURCES) or f.endswith(".md") for f in changed):
        r = run([sys.executable, str(docgen)], root)
        if r and r[0] != 0:
            out.append(Finding("docgen", [r[1]], (
                "tools/docgen.py finds the docs out of step with the source (`python tools/docgen.py --write` "
                f"regenerates the generated tables; fix the rest by hand):\n{tail(r[1], 15)}")))

    if FW + "pins.h" in ch:
        missing = [f for f in ("web/js/wiring.js", "docs/hardware.md") if f not in ch]
        if missing:
            out.append(Finding("wiring", [FW + "pins.h"], (
                f"pins.h changed but {' and '.join(missing)} didn't: the WIRING table and the wiring docs "
                "describe every pin (and tools/bench-wiring/wiring.json if a bench wire moved).")))

    if FW + "config.cpp" in ch and base:
        diff = git(root, "diff", base, "--", FW + "config.cpp") or ""
        rows = re.findall(r'^[+-]\s*\{\s*\d+\s*,\s*"(\w+)"', diff, flags=re.M)
        if rows and "web/js/settings.js" not in ch:
            out.append(Finding("params", [FW + "config.cpp"] + sorted(set(rows)), (
                f"PARAMS rows changed ({', '.join(sorted(set(rows)))}) but web/js/settings.js didn't: a setting "
                "needs its group and help text there. Ids are permanent: a changed meaning or unit needs a new "
                "id.")))

    web = [f for f in changed if f.startswith("web/") and f.endswith((".js", ".html", ".css"))]
    if web and not touched("tests/web/"):
        out.append(Finding("web-tests", web, (
            f"The web console changed ({names(web)}) but nothing under tests/web: add or update a unit or "
            "browser test (the fake board in tests/web/fake-serial.js follows console changes).")))

    agent = [f for f in changed if f.startswith("tools/agent/") or f == ".claude/settings.json"]
    if agent and "tests/tools/test_agent_tooling.py" not in ch:
        out.append(Finding("hook-tests", agent, (
            f"The hooks changed ({names(agent)}) but tests/tools/test_agent_tooling.py didn't: hooks are "
            "tested like code.")))

    # Skills and routines that name a changed file: their steps may now be wrong.
    agent_docs = sorted(root.glob(".claude/skills/*/SKILL.md")) + sorted(root.glob(".claude/routines/*.md"))
    for doc in agent_docs:
        rel_doc = doc.relative_to(root).as_posix()
        if rel_doc in ch:
            continue
        text = doc.read_text(encoding="utf-8", errors="replace")
        named = [f for f in changed if f.startswith(("tools/", "tests/")) and f.endswith(".py")
                 and f in text]
        if named:
            out.append(Finding("agent-docs", [rel_doc] + named, (
                f"{rel_doc} uses {names(named)}, which changed: check its steps still match (options, output, "
                "file names).")))

    code = [f for f in changed if f.startswith(CODE_PREFIXES) and f.endswith(TEST_SUFFIXES)]
    if code and "TODO.md" not in ch and "ROADMAP.md" not in ch:
        out.append(Finding("trackers", code, (
            "Does this change close a TODO.md or ROADMAP.md item (remove it) or leave a finding or follow-up to "
            "record there (add it)?")))
    return out


def state_file(root, session_id):
    gd = git(root, "rev-parse", "--git-dir")
    if not gd:
        return None
    d = (root / gd.strip()).resolve() / "gatelink-hooks"
    safe = re.sub(r"[^\w-]", "_", session_id or "default")[:80]
    return d / f"{safe}.json"


def load_raised(path):
    try:
        return set(json.loads(path.read_text(encoding="utf-8")).get("raised", []))
    except (OSError, ValueError, AttributeError):
        return set()


def save_raised(path, raised):
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        now = time.time()
        for old in path.parent.glob("*.json"):
            if now - old.stat().st_mtime > STATE_MAX_AGE_S:
                old.unlink(missing_ok=True)
        path.write_text(json.dumps({"raised": sorted(raised)}), encoding="utf-8")
    except OSError:
        pass


def stop(event):
    if event.get("stop_hook_active"):
        return 0  # already reminded this turn; Claude's answer stands
    root = repo_root(event.get("cwd") or os.getcwd())
    if root is None:
        return 0
    path = state_file(root, event.get("session_id"))
    if path is None:
        return 0
    raised = load_raised(path)
    new = [f for f in stop_findings(root) if f.key not in raised]
    if not new:
        return 0
    save_raised(path, raised | {f.key for f in new})
    reason = ("Before finishing (GateLink's change checklist, tools/agent/hooks.py; each item is asked once): "
              "do what applies, and for anything that doesn't, say why in one line.\n"
              + "\n".join(f"- {f.message}" for f in new))
    print(json.dumps({"decision": "block", "reason": reason}))
    return 0


# ---- session start ------------------------------------------------------------------------------------------


def cache_dir():
    return Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache") / "gatelink"


def start_cloud_setup(root):
    """Start tools/agent/cloud_setup.sh in the background (once per machine) and say where it stands."""
    cache = cache_dir()
    done, log = cache / "setup.done", cache / "setup.log"
    bins = [Path.home() / ".local/bin", cache / "venv/bin"]
    env_file = os.environ.get("CLAUDE_ENV_FILE")
    if env_file:  # later Bash commands in this session get the tools on PATH
        try:
            with open(env_file, "a", encoding="utf-8") as f:
                f.write("export PATH=\"" + ":".join(str(b) for b in bins) + ":$PATH\"\n")
        except OSError:
            pass
    if done.exists():
        return f"GateLink toolchain installed ({done.read_text(encoding='utf-8').strip()})."
    if not (cache / "setup.pid").exists() or not _alive(cache / "setup.pid"):
        cache.mkdir(parents=True, exist_ok=True)
        pid = spawn(["bash", str(root / "tools/agent/cloud_setup.sh")], root, log)
        (cache / "setup.pid").write_text(str(pid), encoding="utf-8")
    return (f"GateLink toolchain (arduino-cli, the samd/avr cores and pinned libraries, npm ci, ruff, pytest) is "
            f"installing in the background: {log}. Before compiling or running tests, wait for it: "
            f"`until [ -e {done} ]; do sleep 10; done; cat {done}`.")


def spawn(cmd, cwd, log):
    """Start `cmd` detached from this hook, output appended to `log`; its pid."""
    with open(log, "ab") as out:
        return subprocess.Popen(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                start_new_session=True).pid


def _alive(pidfile):
    try:
        os.kill(int(pidfile.read_text(encoding="utf-8")), 0)
        return True
    except (OSError, ValueError):
        return False


def session_start(event):
    root = repo_root(event.get("cwd") or os.getcwd())
    if root is None:
        return 0
    path = state_file(root, event.get("session_id"))
    # What the branch lacked before this session isn't this session's to answer: count it as asked.
    if path is not None and (event.get("source") in (None, "startup", "clear") or not path.exists()):
        save_raised(path, load_raised(path) | {f.key for f in stop_findings(root)})
    if os.environ.get("CLAUDE_CODE_REMOTE") == "true":
        context = start_cloud_setup(root)
        print(json.dumps({"hookSpecificOutput": {"hookEventName": "SessionStart",
                                                 "additionalContext": context}}))
    return 0


# ---- entry point --------------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description="Claude Code hooks for GateLink")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("session-start")
    sub.add_parser("post-edit")
    sub.add_parser("stop")
    c = sub.add_parser("check", help="print the stop findings for this branch")
    c.add_argument("--base", help="ref to measure from (default: merge base with origin/main)")
    c.add_argument("--only", action="append", metavar="ITEM", help="only these items (CI: --only fw-version)")
    p = sub.add_parser("paths", help="repo paths named in these files that don't exist")
    p.add_argument("files", nargs="+")
    args = ap.parse_args(argv)

    if args.cmd == "check":
        root = repo_root(os.getcwd())
        findings = stop_findings(root, args.base) if root else []
        if args.only:
            findings = [f for f in findings if f.rule in args.only]
        for f in findings:
            print(f"[{f.rule}] {f.message}")
        return 1 if findings else 0
    if args.cmd == "paths":
        root = repo_root(os.getcwd())
        bad = 0
        for f in args.files:
            for p in stale_paths(Path(f).read_text(encoding="utf-8", errors="replace"), root):
                print(f"{f}: {p}")
                bad += 1
        return 1 if bad else 0

    event = read_event()
    handler = {"session-start": session_start, "post-edit": post_edit, "stop": stop}[args.cmd]
    try:
        return handler(event)
    except Exception as e:  # a hook bug must not stop the session
        print(f"GateLink hook {args.cmd} failed (ignored): {type(e).__name__}: {e}", file=sys.stderr)
        return 0


if __name__ == "__main__":
    sys.exit(main())
