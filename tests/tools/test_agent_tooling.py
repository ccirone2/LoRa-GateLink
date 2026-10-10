"""Tests for the agent tooling: the hooks (tools/agent/hooks.py, .claude/settings.json), the skills and the routines.

The stop and post-edit hooks run against throwaway git repos, so they're tested on the same changes they judge.
The repo's own skills, routines and docs are checked for broken front matter and stale paths.
"""

import io
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from agent import hooks

ROOT = Path(__file__).resolve().parents[2]


# ---- the repo's own agent files ------------------------------------------------------------------------------


def test_settings_runs_existing_hooks():
    settings = ROOT / ".claude/settings.json"
    assert hooks.settings_problems(settings, ROOT) == []
    data = json.loads(settings.read_text(encoding="utf-8"))["hooks"]
    assert set(data) == {"SessionStart", "PostToolUse", "Stop"}
    edit = data["PostToolUse"][0]
    assert set(edit["matcher"].split("|")) == {"Edit", "Write", "NotebookEdit"}
    for event, arg in (("SessionStart", "session-start"), ("PostToolUse", "post-edit"), ("Stop", "stop")):
        command = data[event][0]["hooks"][0]["command"]
        # sh, not ./hook.sh: a clone may lose the executable bit, and Windows runs hooks in Git Bash.
        assert command == f'sh "$CLAUDE_PROJECT_DIR/tools/agent/hook.sh" {arg}'


def test_settings_is_tracked_by_git():
    out = subprocess.run(["git", "check-ignore", "-q", ".claude/settings.json"], cwd=ROOT)
    assert out.returncode == 1, ".gitignore hides .claude/settings.json"


@pytest.mark.parametrize("skill", sorted(ROOT.glob(".claude/skills/*/SKILL.md")), ids=lambda p: p.parent.name)
def test_skill(skill):
    assert hooks.skill_problems(skill, ROOT) == []


ROUTINES = sorted(p for p in ROOT.glob(".claude/routines/*.md") if p.name != "README.md")


@pytest.mark.parametrize("routine", ROUTINES, ids=lambda p: p.stem)
def test_routine(routine):
    assert hooks.routine_problems(routine, ROOT) == []


def test_routines_index_lists_every_routine():
    index = (ROOT / ".claude/routines/README.md").read_text(encoding="utf-8")
    listed = set(re.findall(r"^\| \[([\w-]+)\]\(\1\.md\) \|", index, flags=re.M))
    assert listed == {p.stem for p in ROUTINES}
    for p in ROUTINES:
        schedule = hooks.frontmatter(p.read_text(encoding="utf-8"))["schedule"]
        hour = int(schedule.split()[1])
        assert re.search(rf"^\| \[{p.stem}\]\({p.stem}\.md\) \| nightly {hour:02d}:00 \|", index, flags=re.M), \
            f"{p.stem}: index time doesn't match its schedule {schedule!r}"


DOCS = ["CLAUDE.md", "README.md", "TODO.md", "ROADMAP.md", *sorted(str(p.relative_to(ROOT)) for p in
        [*ROOT.glob("docs/*.md"), *ROOT.glob("tests/*/README.md"), *ROOT.glob("tools/*/README.md"),
         ROOT / ".claude/routines/README.md"])]


@pytest.mark.parametrize("doc", DOCS)
def test_doc_names_only_existing_paths(doc):
    text = (ROOT / doc).read_text(encoding="utf-8")
    assert hooks.stale_paths(text, ROOT) == []


@pytest.mark.parametrize("doc", DOCS)
def test_doc_links_resolve(doc):
    assert hooks.broken_links(ROOT / doc) == []


def test_cloud_setup_pins_match_ci():
    setup = (ROOT / "tools/agent/cloud_setup.sh").read_text(encoding="utf-8")
    ci = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    core_lib = re.compile(r"arduino:\w+@[\d.]+|(?:LoRa|Crypto|FlashStorage|ArduinoJson|Adafruit SleepyDog Library)"
                          r"@[\d.]+")
    assert set(core_lib.findall(setup)) == set(core_lib.findall(ci))
    ruff = re.search(r"RUFF_VERSION=([\d.]+)", setup).group(1)
    assert f"ruff=={ruff}" in ci
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    for pin in core_lib.findall(setup):
        name, version = pin.rsplit("@", 1)
        assert re.search(rf"`{re.escape(name)}`[^|\n]*\| {re.escape(version)} \|", dev), f"{pin} not in development.md"


# ---- a throwaway repo for the hooks to judge ---------------------------------------------------------------


def git(repo, *args):
    subprocess.run(["git", *args], cwd=repo, check=True, capture_output=True)


@pytest.fixture
def repo(tmp_path, monkeypatch):
    """A repo on branch `work`, one commit past `main`'s firmware 1.0.0."""
    for k, v in {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com", "GIT_COMMITTER_NAME": "t",
                 "GIT_COMMITTER_EMAIL": "t@example.com", "GIT_CONFIG_NOSYSTEM": "1",
                 "GIT_CONFIG_GLOBAL": str(tmp_path / "gitconfig")}.items():
        monkeypatch.setenv(k, v)
    r = tmp_path / "repo"
    files = {
        "firmware/GateLink/config.h": '#define FW_VERSION "1.0.0"\n',
        "firmware/GateLink/config.cpp": 'const ParamDef PARAMS[] = {\n  { 1, "role", 0, 2 },\n};\n',
        "firmware/GateLink/link.cpp": "int link;\n",
        "firmware/GateLink/console.cpp": "int console;\n",
        "firmware/GateLink/pins.h": "#define PIN_K1 1\n",
        "docs/console.md": "# Console\n",
        "docs/hardware.md": "# Hardware\n",
        "web/js/wiring.js": "export const WIRING = [];\n",
        "web/js/settings.js": "export const GROUPS = {};\n",
        "web/app.js": "console.log(1);\n",
        "tests/native/test_link.cpp": "int t;\n",
        "tests/web/unit/a.test.js": "\n",
        "tools/gatelink.py": "print(1)\n",
        ".claude/skills/flash/SKILL.md": "---\nname: flash\ndescription: x\n---\nRun `python tools/gatelink.py ports`.\n",
        "TODO.md": "# TODO\n",
        "ROADMAP.md": "# Roadmap\n",
    }
    for rel, text in files.items():
        (r / rel).parent.mkdir(parents=True, exist_ok=True)
        (r / rel).write_text(text, encoding="utf-8")
    git(r, "init", "-q", "-b", "main")
    git(r, "add", "-A")
    git(r, "commit", "-q", "-m", "base")
    git(r, "checkout", "-q", "-b", "work")
    return r


def edit(repo, rel, text=None, append="// changed\n"):
    p = repo / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    if text is None:
        text = (p.read_text(encoding="utf-8") if p.exists() else "") + append
    p.write_text(text, encoding="utf-8")


def rules(repo):
    return sorted(f.rule for f in hooks.stop_findings(repo))


def test_clean_branch_has_no_findings(repo):
    assert rules(repo) == []


def test_firmware_change_wants_version_tests_and_trackers(repo):
    edit(repo, "firmware/GateLink/link.cpp")
    assert rules(repo) == ["fw-version", "native-tests", "trackers"]
    edit(repo, "firmware/GateLink/config.h", '#define FW_VERSION "1.0.1"\n')
    edit(repo, "tests/native/test_link.cpp")
    edit(repo, "TODO.md")
    assert rules(repo) == []


def test_committed_changes_count_too(repo):
    edit(repo, "firmware/GateLink/link.cpp")
    git(repo, "commit", "-qam", "change")
    assert "fw-version" in rules(repo)


def test_untracked_firmware_file_counts(repo):
    edit(repo, "firmware/GateLink/new.cpp", "int x;\n")
    assert "fw-version" in rules(repo)


def test_tool_config_in_the_firmware_folder_is_not_a_firmware_change(repo):
    edit(repo, "firmware/GateLink/.clang-tidy", "Checks: '-*'\n")
    assert "fw-version" not in rules(repo)


def test_console_change_wants_its_docs(repo):
    edit(repo, "firmware/GateLink/console.cpp")
    assert "console-docs" in rules(repo)
    edit(repo, "docs/console.md")
    assert "console-docs" not in rules(repo)


def test_pins_change_wants_wiring_table_and_docs(repo):
    edit(repo, "firmware/GateLink/pins.h")
    [wiring] = [f for f in hooks.stop_findings(repo) if f.rule == "wiring"]
    assert "web/js/wiring.js and docs/hardware.md" in wiring.message
    edit(repo, "web/js/wiring.js")
    edit(repo, "docs/hardware.md")
    assert "wiring" not in rules(repo)


def test_new_param_wants_settings_help(repo):
    edit(repo, "firmware/GateLink/config.cpp",
         'const ParamDef PARAMS[] = {\n  { 1, "role", 0, 2 },\n  { 2, "fault_out", 0, 1 },\n};\n')
    [params] = [f for f in hooks.stop_findings(repo) if f.rule == "params"]
    assert "fault_out" in params.message
    edit(repo, "web/js/settings.js")
    assert "params" not in rules(repo)


def test_other_config_change_is_not_a_param_change(repo):
    edit(repo, "firmware/GateLink/config.cpp")
    assert "params" not in rules(repo)


def test_web_change_wants_web_tests(repo):
    edit(repo, "web/app.js")
    assert "web-tests" in rules(repo)
    edit(repo, "tests/web/unit/a.test.js")
    assert "web-tests" not in rules(repo)


def test_skill_naming_a_changed_tool_is_flagged(repo):
    edit(repo, "tools/gatelink.py")
    [f] = [f for f in hooks.stop_findings(repo) if f.rule == "agent-docs"]
    assert ".claude/skills/flash/SKILL.md uses gatelink.py" in f.message
    edit(repo, ".claude/skills/flash/SKILL.md")
    assert "agent-docs" not in rules(repo)


def test_docs_only_change_needs_nothing(repo):
    edit(repo, "docs/hardware.md")
    assert rules(repo) == []


def test_base_is_where_the_branch_left_main(repo):
    edit(repo, "firmware/GateLink/link.cpp")
    edit(repo, "firmware/GateLink/config.h", '#define FW_VERSION "1.0.1"\n')
    git(repo, "commit", "-qam", "bump")
    # main moves on with the same version bump; the branch is still judged against where it left main.
    git(repo, "checkout", "-q", "main")
    edit(repo, "firmware/GateLink/config.h", '#define FW_VERSION "1.0.1"\n')
    git(repo, "commit", "-qam", "main bump")
    git(repo, "checkout", "-q", "work")
    assert "fw-version" not in rules(repo)


# ---- the stop hook ------------------------------------------------------------------------------------------


def call(handler, event, monkeypatch, capsys):
    monkeypatch.setattr(sys, "stdin", io.StringIO(json.dumps(event)))
    rc = hooks.main([handler])
    out = capsys.readouterr()
    return rc, out.out, out.err


def test_stop_asks_each_item_once(repo, monkeypatch, capsys):
    ev = {"session_id": "s1", "cwd": str(repo), "stop_hook_active": False}
    edit(repo, "firmware/GateLink/link.cpp")
    rc, out, _ = call("stop", ev, monkeypatch, capsys)
    assert rc == 0
    reply = json.loads(out)
    assert reply["decision"] == "block"
    assert "FW_VERSION" in reply["reason"] and "tests/native" in reply["reason"]
    # Claude answered; the same items aren't asked again.
    assert call("stop", ev, monkeypatch, capsys)[1] == ""
    # A new kind of gap is.
    edit(repo, "firmware/GateLink/console.cpp")
    reply = json.loads(call("stop", ev, monkeypatch, capsys)[1])
    assert "docs/console.md" in reply["reason"]
    assert "tests/native" not in reply["reason"]


def test_stop_never_blocks_twice_in_a_row(repo, monkeypatch, capsys):
    edit(repo, "firmware/GateLink/link.cpp")
    ev = {"session_id": "s2", "cwd": str(repo), "stop_hook_active": True}
    assert call("stop", ev, monkeypatch, capsys)[:2] == (0, "")


def test_sessions_are_asked_separately(repo, monkeypatch, capsys):
    edit(repo, "firmware/GateLink/link.cpp")
    assert call("stop", {"session_id": "a", "cwd": str(repo)}, monkeypatch, capsys)[1]
    assert call("stop", {"session_id": "b", "cwd": str(repo)}, monkeypatch, capsys)[1]


def test_session_start_takes_existing_gaps_as_asked(repo, monkeypatch, capsys):
    monkeypatch.delenv("CLAUDE_CODE_REMOTE", raising=False)
    edit(repo, "firmware/GateLink/link.cpp")
    ev = {"session_id": "s3", "cwd": str(repo), "source": "startup"}
    assert call("session-start", ev, monkeypatch, capsys)[:2] == (0, "")
    assert call("stop", ev, monkeypatch, capsys)[1] == ""
    # What this session adds is still asked.
    edit(repo, "web/app.js")
    assert "tests/web" in json.loads(call("stop", ev, monkeypatch, capsys)[1])["reason"]


def test_stop_outside_a_repo_does_nothing(tmp_path, monkeypatch, capsys):
    assert call("stop", {"session_id": "x", "cwd": str(tmp_path)}, monkeypatch, capsys)[:2] == (0, "")


def test_a_hook_bug_never_breaks_the_session(monkeypatch, capsys):
    def boom(event):
        raise RuntimeError("bug")
    monkeypatch.setattr(hooks, "stop", boom)
    rc, out, err = call("stop", {}, monkeypatch, capsys)
    assert rc == 0 and out == "" and "failed (ignored)" in err


def test_check_command_lists_findings(repo, monkeypatch, capsys):
    edit(repo, "firmware/GateLink/link.cpp")
    monkeypatch.chdir(repo)
    assert hooks.main(["check"]) == 1
    assert "[fw-version]" in capsys.readouterr().out


def test_check_only_one_item_as_ci_does(repo, monkeypatch, capsys):
    monkeypatch.chdir(repo)
    edit(repo, "web/app.js")
    assert hooks.main(["check", "--base", "main", "--only", "fw-version"]) == 0
    edit(repo, "firmware/GateLink/link.cpp")
    git(repo, "commit", "-qam", "change")
    assert hooks.main(["check", "--base", "main", "--only", "fw-version"]) == 1
    out = capsys.readouterr().out
    assert "[fw-version]" in out and "[native-tests]" not in out


# ---- post-edit ----------------------------------------------------------------------------------------------


def post_edit(repo, rel, monkeypatch, capsys):
    return call("post-edit", {"tool_name": "Write", "tool_input": {"file_path": str(repo / rel)}, "cwd": str(repo)},
                monkeypatch, capsys)


def test_post_edit_passes_good_files(repo, monkeypatch, capsys):
    for rel in ("web/app.js", "tools/gatelink.py", "docs/console.md", ".claude/skills/flash/SKILL.md"):
        edit(repo, rel, append="")
    edit(repo, ".claude/skills/flash/SKILL.md",
         "---\nname: flash\ndescription: Flash the bench boards and check their config survived the upload.\n---\n")
    for rel in ("web/app.js", "tools/gatelink.py", "docs/console.md", ".claude/skills/flash/SKILL.md"):
        assert post_edit(repo, rel, monkeypatch, capsys)[:3] == (0, "", ""), rel


def test_post_edit_reports_python_syntax_error(repo, monkeypatch, capsys):
    edit(repo, "tools/gatelink.py", "def f(:\n")
    rc, _, err = post_edit(repo, "tools/gatelink.py", monkeypatch, capsys)
    assert rc == 2 and "tools/gatelink.py:1" in err


@pytest.mark.skipif(not shutil.which("node"), reason="node not installed")
def test_post_edit_reports_js_syntax_error(repo, monkeypatch, capsys):
    edit(repo, "web/app.js", "function (\n")
    rc, _, err = post_edit(repo, "web/app.js", monkeypatch, capsys)
    assert rc == 2 and "node --check web/app.js" in err


def test_post_edit_reports_broken_link(repo, monkeypatch, capsys):
    edit(repo, "docs/console.md", "See [hw](hardware.md) and [gone](missing.md).\n")
    rc, _, err = post_edit(repo, "docs/console.md", monkeypatch, capsys)
    assert rc == 2 and "missing.md" in err and "hardware.md," not in err


def test_post_edit_reports_bad_json(repo, monkeypatch, capsys):
    edit(repo, "tools/bench-wiring/wiring.json", "{oops")
    rc, _, err = post_edit(repo, "tools/bench-wiring/wiring.json", monkeypatch, capsys)
    assert rc == 2 and "isn't valid JSON" in err


def test_post_edit_reports_broken_skill(repo, monkeypatch, capsys):
    edit(repo, ".claude/skills/flash/SKILL.md", "---\nname: flsh\ndescription: short\n---\nRun tools/gone.py.\n")
    rc, _, err = post_edit(repo, ".claude/skills/flash/SKILL.md", monkeypatch, capsys)
    assert rc == 2
    assert "isn't the directory name" in err and "too short" in err and "tools/gone.py" in err


def test_post_edit_reports_routine_using_a_missing_skill(repo, monkeypatch, capsys):
    edit(repo, ".claude/routines/nightly.md",
         "---\nname: nightly\nschedule: 0 2 * * *\ndescription: d\n---\nRun /flash, then /no-such-skill.\n")
    rc, _, err = post_edit(repo, ".claude/routines/nightly.md", monkeypatch, capsys)
    assert rc == 2 and "/no-such-skill" in err and "/flash," not in err


def test_post_edit_reports_settings_naming_a_missing_script(repo, monkeypatch, capsys):
    edit(repo, ".claude/settings.json", json.dumps({"hooks": {"Stop": [{"hooks": [
        {"type": "command", "command": 'sh "$CLAUDE_PROJECT_DIR/tools/agent/nope.sh" stop'}]}]}}))
    rc, _, err = post_edit(repo, ".claude/settings.json", monkeypatch, capsys)
    assert rc == 2 and "tools/agent/nope.sh" in err


def test_post_edit_ignores_files_outside_a_repo(tmp_path, monkeypatch, capsys):
    f = tmp_path / "x.py"
    f.write_text("def f(:\n", encoding="utf-8")
    rc, out, err = call("post-edit", {"tool_input": {"file_path": str(f)}}, monkeypatch, capsys)
    assert (rc, out, err) == (0, "", "")


def test_stale_paths_skips_placeholders_and_ignored(repo):
    edit(repo, ".gitignore", "results/\n")
    text = ("`tools/gatelink.py` `docs/releases/vX.Y.Z.md` `tests/e2e/results/<run>/` `web/js/*.js` "
            "results/x.json `tools/gone.py`.")
    assert hooks.stale_paths(text, repo) == ["tools/gone.py"]


# ---- session start in a cloud session ----------------------------------------------------------------------


def test_cloud_session_reports_an_installed_toolchain(repo, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("CLAUDE_CODE_REMOTE", "true")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    env_file = tmp_path / "env.sh"
    monkeypatch.setenv("CLAUDE_ENV_FILE", str(env_file))
    (tmp_path / "cache/gatelink").mkdir(parents=True)
    (tmp_path / "cache/gatelink/setup.done").write_text("ok: arduino-cli cores\n", encoding="utf-8")
    rc, out, _ = call("session-start", {"session_id": "c1", "cwd": str(repo), "source": "startup"}, monkeypatch,
                      capsys)
    ctx = json.loads(out)["hookSpecificOutput"]
    assert rc == 0 and ctx["hookEventName"] == "SessionStart"
    assert "installed (ok: arduino-cli cores)" in ctx["additionalContext"]
    assert env_file.read_text(encoding="utf-8").startswith('export PATH="')


def test_cloud_session_starts_the_install_once(repo, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("CLAUDE_CODE_REMOTE", "true")
    monkeypatch.setenv("XDG_CACHE_HOME", str(tmp_path / "cache"))
    monkeypatch.delenv("CLAUDE_ENV_FILE", raising=False)
    started = []

    def spawn(cmd, cwd, log):
        started.append(cmd)
        return os.getpid()  # alive, so a second session start doesn't start another

    monkeypatch.setattr(hooks, "spawn", spawn)
    ev = {"session_id": "c2", "cwd": str(repo), "source": "startup"}
    out = call("session-start", ev, monkeypatch, capsys)[1]
    assert "installing in the background" in json.loads(out)["hookSpecificOutput"]["additionalContext"]
    call("session-start", ev, monkeypatch, capsys)
    assert len(started) == 1 and started[0][1].endswith("cloud_setup.sh")


def test_local_session_prints_nothing(repo, monkeypatch, capsys):
    monkeypatch.delenv("CLAUDE_CODE_REMOTE", raising=False)
    assert call("session-start", {"session_id": "l", "cwd": str(repo)}, monkeypatch, capsys)[:3] == (0, "", "")


# ---- the launcher ------------------------------------------------------------------------------------------


@pytest.mark.skipif(not shutil.which("sh"), reason="no sh")
def test_launcher_passes_stdin_and_exit_code(repo):
    edit(repo, "tools/gatelink.py", "def f(:\n")
    ev = json.dumps({"tool_input": {"file_path": str(repo / "tools/gatelink.py")}})
    p = subprocess.run(["sh", str(ROOT / "tools/agent/hook.sh"), "post-edit"], input=ev, capture_output=True,
                       text=True)
    assert p.returncode == 2 and "tools/gatelink.py:1" in p.stderr


def test_cloud_setup_is_valid_bash():
    bash = shutil.which("bash")
    if not bash:
        pytest.skip("no bash")
    assert subprocess.run([bash, "-n", str(ROOT / "tools/agent/cloud_setup.sh")]).returncode == 0


def test_every_stop_item_is_documented():
    code = (ROOT / "tools/agent/hooks.py").read_text(encoding="utf-8")
    doc = (ROOT / "docs/agent-tooling.md").read_text(encoding="utf-8")
    items = set(re.findall(r'Finding\("([\w-]+)"', code))
    assert items == set(re.findall(r"^\| `([\w-]+)` \|", doc, flags=re.M))


def fake_docgen(repo, ok):
    edit(repo, "tools/docgen.py", "import sys\nprint('drift: docs/console.md')\nsys.exit(0)\n" if ok else
         "import sys\nprint('Docs out of step: `link_ok` names nothing')\nsys.exit(1)\n")


def test_docgen_drift_is_asked_at_stop(repo):
    fake_docgen(repo, ok=False)
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "docgen")
    git(repo, "branch", "-f", "main")  # docgen is part of the base
    edit(repo, "docs/console.md")
    [f] = [f for f in hooks.stop_findings(repo) if f.rule == "docgen"]
    assert "`link_ok` names nothing" in f.message
    fake_docgen(repo, ok=True)
    assert "docgen" not in rules(repo)


def test_post_edit_runs_docgen_on_a_doc(repo, monkeypatch, capsys):
    fake_docgen(repo, ok=False)
    edit(repo, "docs/console.md", "# Console\n")
    rc, _, err = post_edit(repo, "docs/console.md", monkeypatch, capsys)
    assert rc == 2 and "tools/docgen.py (after editing docs/console.md)" in err
    fake_docgen(repo, ok=True)
    assert post_edit(repo, "docs/console.md", monkeypatch, capsys)[0] == 0
