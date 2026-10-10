"""tools/release_evidence.py against fixture bench runs (fixtures/*.json, shaped like tests/e2e/conftest.py's
summary.json) and a fake git/GitHub (FakeRepo): release kinds, missing runs, failed scenarios and reruns, the power
runs' LiPo states, which runs count, and checking good and bad evidence files."""
import copy
import json
from pathlib import Path

import pytest
import release_evidence as rel

FIXTURES = Path(__file__).parent / "fixtures"
REPO_ROOT = Path(__file__).resolve().parents[2]
SHA = "c0ffee00c0ffee00c0ffee00c0ffee00c0ffee00"
TREE = "f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1f1"
OTHER_TREE = "abababababababababababababababababababab"


class FakeRepo(rel.Repo):
    """git and gh as a release checkout would answer them."""

    def __init__(self, root, version="0.14.0", tags=("v0.13.4", "v0.13.5"), tree=TREE, ci="success"):
        super().__init__(root)
        self.version, self._tags, self._tree, self.ci_state = version, list(tags), tree, ci

    def commit(self, rev):
        return SHA

    def tree(self, rev, path):
        return self._tree

    def show(self, rev, path):
        return f'#define FW_VERSION "{self.version}"\n'

    def tags(self):
        return self._tags

    def ci(self, sha):
        return {"state": self.ci_state, "url": "https://github.com/x/y/actions/runs/1",
                "jobs": [["firmware", self.ci_state], ["web-and-python", self.ci_state]]}


@pytest.fixture
def root(tmp_path):
    """A checkout: config.h at 0.14.0, a TODO.md, an empty results directory."""
    (tmp_path / "firmware" / "GateLink").mkdir(parents=True)
    set_fw(tmp_path, "0.14.0")
    (tmp_path / "TODO.md").write_text("# TODO\n", encoding="utf-8")
    (tmp_path / "tests" / "e2e" / "results").mkdir(parents=True)
    return tmp_path


def set_fw(root, version):
    (root / "firmware" / "GateLink" / "config.h").write_text(f'#define FW_VERSION "{version}"\n', encoding="utf-8")


def put(d, path, value):
    *head, last = path.split(".")
    for k in head:
        d = d[k]
    d[last] = value


def add_run(root, fixture, name, **changes):
    """Write results/<name>/summary.json from fixtures/<fixture>.json. `name` is the run's time (YYYYmmdd-HHMMSS),
    which also sets its start; changes are dotted paths, e.g. **{"options.tx_power": 17}."""
    data = json.loads((FIXTURES / f"{fixture}.json").read_text(encoding="utf-8"))
    data["run"] = name
    data["start"] = f"{name[:4]}-{name[4:6]}-{name[6:8]}T{name[9:11]}:{name[11:13]}:{name[13:15]}-05:00"
    data["end"] = data["start"][:11] + "23:59:00-05:00"
    for path, value in changes.items():
        put(data, path, value)
    d = root / "tests" / "e2e" / "results" / name
    d.mkdir(parents=True)
    (d / "summary.json").write_text(json.dumps(data), encoding="utf-8")
    return data


def outcome(data, nodeid, result, reason=None):
    """A copy of data["tests"] with one test's outcome changed (for add_run's "tests" change)."""
    tests = copy.deepcopy(data["tests"])
    for t in tests:
        if t["nodeid"] == nodeid:
            t["outcome"], t["reason"] = result, reason
    return tests


def fixture_tests(fixture):
    return json.loads((FIXTURES / f"{fixture}.json").read_text(encoding="utf-8"))["tests"]


def with_outcome(fixture, nodeid, result, reason=None):
    return outcome({"tests": fixture_tests(fixture)}, nodeid, result, reason)


def collect(root, version="v0.14.0", repo=None, extra=()):
    repo = repo or FakeRepo(root)
    code = rel.main(["collect", version, *extra], repo=repo)
    path = root / "docs" / "releases" / f"{version}.md"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    data = rel.read_block(text) if text else None
    return code, text, data


def met(data):
    return {c["id"]: c["met"] for c in data["criteria"]}


def check(root, version="v0.14.0", repo=None, extra=()):
    return rel.main(["check", version, *extra], repo=repo or FakeRepo(root))


def all_minor_runs(root):
    add_run(root, "full", "20261010-090000")
    add_run(root, "soak", "20261010-100000")
    add_run(root, "power_out", "20261010-110000")
    add_run(root, "power_in", "20261010-120000")
    add_run(root, "longsoak", "20261010-130000")
    add_run(root, "full", "20261010-150000", **{"options.tx_power": 17})


# --- release kinds ------------------------------------------------------------------------------------------------
def test_release_kind():
    v = rel.parse_version
    assert rel.release_kind(v("0.13.6"), v("0.13.5")) == "patch"
    assert rel.release_kind(v("0.14.0"), v("0.13.5")) == "minor"
    assert rel.release_kind(v("1.0.0"), v("0.14.2")) == "major"
    assert rel.release_kind(v("0.1.0"), None) == "minor"
    with pytest.raises(rel.UsageError):
        rel.release_kind(v("0.13.5"), v("0.13.5"))


def test_previous_release_is_the_newest_older_tag():
    tags = ["v0.13.5", "v0.9.0", "v0.13.10", "v0.14.0", "nightly", "v1.0.0-rc1"]
    assert rel.previous_release(rel.parse_version("0.14.0"), tags) == (0, 13, 10)
    assert rel.previous_release(rel.parse_version("0.9.0"), tags) is None


def test_patch_release_needs_full_suite_and_soak(root):
    add_run(root, "full", "20261010-090000")
    add_run(root, "soak", "20261010-100000")
    code, text, data = collect(root, "v0.13.6", FakeRepo(root, version="0.13.6"))
    assert code == 1  # the runs are on 0.14.0 fixtures: none counts for 0.13.6
    assert "No bench run on 0.13.6 counts yet" in text
    # The same runs on 0.13.6.
    for name in ("20261010-090000", "20261010-100000"):
        p = root / "tests" / "e2e" / "results" / name / "summary.json"
        d = json.loads(p.read_text(encoding="utf-8"))
        d["boards"]["house"]["fw"] = d["boards"]["gate"]["fw"] = "0.13.6"
        p.write_text(json.dumps(d), encoding="utf-8")
    code, text, data = collect(root, "v0.13.6", FakeRepo(root, version="0.13.6"))
    assert code == 0, text
    assert data["kind"] == "patch" and data["previous"] == "0.13.5"
    assert met(data)["ci"] and met(data)["full_suite"] and met(data)["soak"]
    assert not met(data)["power_lipo_out"]  # evaluated, but not required for a patch
    assert "**All criteria for this release are met.**" in text
    assert "| `soak`: Soak: -m soak, at least 20 cycles | every release | yes |" in text


def test_minor_release_needs_power_longsoak_and_full_power(root):
    add_run(root, "full", "20261010-090000")
    add_run(root, "soak", "20261010-100000")
    code, _, data = collect(root)
    assert code == 1 and data["kind"] == "minor"
    unmet = {c["id"] for c in data["criteria"] if c["required"] and not c["met"]}
    assert unmet == {"power_lipo_out", "power_lipo_in", "longsoak_60m", "full_suite_17dbm"}
    add_run(root, "power_out", "20261010-110000")
    add_run(root, "power_in", "20261010-120000")
    add_run(root, "longsoak", "20261010-130000")
    add_run(root, "full", "20261010-150000", **{"options.tx_power": 17})
    code, text, data = collect(root)
    assert code == 0, text
    assert met(data)["full_suite"] and met(data)["full_suite_17dbm"]
    # The tx 17 run doesn't stand in for the full suite at the bench's own power, and the reverse.
    by_id = {c["id"]: c for c in data["criteria"]}
    assert by_id["full_suite"]["runs"] == ["20261010-090000"]
    assert by_id["full_suite_17dbm"]["runs"] == ["20261010-150000"]
    assert {r["run"] for r in data["runs"]} == {"20261010-090000", "20261010-100000", "20261010-110000",
                                               "20261010-120000", "20261010-130000", "20261010-150000"}
    assert "### 20261010-110000" in text and "LiPos: gate out, house out" in text


def test_major_release_needs_a_day_long_soak_and_install_notes(root):
    repo = FakeRepo(root, version="1.0.0", tags=("v0.14.0",))
    all_minor_runs(root)
    for p in (root / "tests" / "e2e" / "results").glob("*/summary.json"):
        d = json.loads(p.read_text(encoding="utf-8"))
        d["boards"]["house"]["fw"] = d["boards"]["gate"]["fw"] = "1.0.0"
        p.write_text(json.dumps(d), encoding="utf-8")
    set_fw(root, "1.0.0")
    code, text, data = collect(root, "v1.0.0", repo)
    assert code == 1 and data["kind"] == "major"
    unmet = {c["id"] for c in data["criteria"] if c["required"] and not c["met"]}
    assert unmet == {"longsoak_24h", "install_rf", "install_full_power"}
    assert "- [ ] install_rf: " in text
    add_run(root, "longsoak", "20261011-000000",
            **{"options.soak_minutes": 1440, "boards.house.fw": "1.0.0", "boards.gate.fw": "1.0.0"})
    # The install checks are ticked by hand in the file; collecting again keeps the notes.
    path = root / "docs" / "releases" / "v1.0.0.md"
    path.write_text(text.replace("- [ ] install_rf: ", "- [x] install_rf: -m rf at the gate, 5/5 cycles, -97 dBm")
                    .replace("- [ ] install_full_power: ", "- [x] install_full_power: 17 dBm saved 10-12 to 10-16, "
                             "no resets"), encoding="utf-8")
    assert check(root, "v1.0.0", repo) == 1  # the table on file is from before: longsoak_24h not met
    code, text, data = collect(root, "v1.0.0", repo)
    assert code == 0, text
    assert "-m rf at the gate, 5/5 cycles" in text
    assert check(root, "v1.0.0", repo) == 0


def test_no_runs_writes_the_file_and_fails(root, capsys):
    code, text, data = collect(root)
    assert code == 1
    assert data["runs"] == [] and not met(data)["full_suite"]
    assert "no run yet: `GATELINK_KEY=<key> pytest tests/e2e -v`" in text
    assert "NOT MET] full_suite" in capsys.readouterr().out


def test_dry_run_writes_nothing(root):
    add_run(root, "full", "20261010-090000")
    rel.main(["collect", "v0.14.0", "--dry-run"], repo=FakeRepo(root))
    assert not (root / "docs" / "releases").exists()


# --- failed scenarios and reruns ----------------------------------------------------------------------------------
JAM = "test_opener_faults.py::test_jammed_gate"


def test_failed_scenario_fails_the_criterion(root):
    add_run(root, "full", "20261010-090000",
            tests=with_outcome("full", JAM, "failed", "AssertionError: timed out after 30s waiting for gate"))
    code, text, data = collect(root)
    assert code == 1 and not met(data)["full_suite"]
    assert "`test_jammed_gate` failed (AssertionError: timed out after 30s waiting for gate), no rerun of it yet" in text
    assert "failed: `test_jammed_gate` (AssertionError" in text  # in the run's section too


def test_two_failed_scenarios_cant_be_rerun(root):
    tests = with_outcome("full", JAM, "failed", "x")
    tests = outcome({"tests": tests}, "test_normal.py::test_open_via_controller", "failed", "y")
    add_run(root, "full", "20261010-090000", tests=tests)
    add_run(root, "full", "20261010-100000", tests=[t for t in fixture_tests("full") if t["nodeid"] == JAM],
            **{"selection.keyword": "test_jammed_gate", "selection.groups.default.selected": 1})
    _, text, data = collect(root)
    assert not met(data)["full_suite"] and "2 failed" in text


def rerun_of_jam(root, name, result="passed", **changes):
    """`pytest tests/e2e -k test_jammed_gate`: one default-group test selected out of five."""
    tests = [t for t in with_outcome("full", JAM, result, None if result == "passed" else "again")
             if t["nodeid"] == JAM]
    return add_run(root, "full", name, tests=tests, command="pytest tests/e2e -k test_jammed_gate",
                   **{"selection.keyword": "test_jammed_gate", "selection.groups.default.selected": 1,
                      "exitstatus": 0 if result == "passed" else 1, **changes})


def test_one_rerun_passes_a_flaky_scenario_once_todo_names_it(root):
    add_run(root, "full", "20261010-090000", tests=with_outcome("full", JAM, "failed", "AssertionError: timed out"))
    rerun_of_jam(root, "20261010-100000")
    _, text, data = collect(root)
    assert not met(data)["full_suite"]
    assert "TODO.md doesn't name `test_jammed_gate` as a flaky scenario" in text
    (root / "TODO.md").write_text("- [ ] **Flaky: test_jammed_gate.** Timed out once on 0.14.0.\n", encoding="utf-8")
    _, text, data = collect(root)
    assert met(data)["full_suite"]
    assert data["reruns"] == [{"run": "20261010-090000", "test": JAM, "reason": "AssertionError: timed out",
                               "rerun": "20261010-100000"}]
    assert "## Reruns" in text
    by_id = {c["id"]: c for c in data["criteria"]}
    assert by_id["full_suite"]["runs"] == ["20261010-090000", "20261010-100000"]


def test_a_rerun_that_fails_again_isnt_retried(root):
    (root / "TODO.md").write_text("test_jammed_gate is flaky\n", encoding="utf-8")
    add_run(root, "full", "20261010-090000", tests=with_outcome("full", JAM, "failed", "first"))
    rerun_of_jam(root, "20261010-100000", "failed")
    rerun_of_jam(root, "20261010-110000")  # a second rerun: not allowed
    _, text, data = collect(root)
    assert not met(data)["full_suite"]
    assert "its rerun 20261010-100000 did not pass (failed)" in text


def test_a_rerun_needs_the_same_conditions(root):
    (root / "TODO.md").write_text("test_jammed_gate is flaky\n", encoding="utf-8")
    add_run(root, "full", "20261010-090000", tests=with_outcome("full", JAM, "failed", "first"))
    rerun_of_jam(root, "20261010-100000", **{"options.tx_power": 17})  # not a rerun of a run at saved power
    _, _, data = collect(root)
    assert not met(data)["full_suite"]


def test_a_rerun_needs_the_criterions_options(root):
    """A failed 20-cycle soak can't be passed by rerunning its scenario with --cycles 5."""
    soak = "test_normal.py::test_soak_open_close_cycles"
    (root / "TODO.md").write_text("test_soak_open_close_cycles is flaky\n", encoding="utf-8")
    add_run(root, "soak", "20261010-090000", tests=with_outcome("soak", soak, "failed", "timed out"))
    add_run(root, "soak", "20261010-100000", **{"options.cycles": 5})
    _, text, data = collect(root)
    assert not met(data)["soak"] and "no rerun of it yet" in text


def test_the_latest_attempt_decides(root):
    add_run(root, "full", "20261010-090000")
    add_run(root, "full", "20261010-100000", tests=with_outcome("full", JAM, "failed", "x"))
    _, _, data = collect(root)
    assert not met(data)["full_suite"]
    add_run(root, "full", "20261010-110000")
    _, text, data = collect(root)
    assert met(data)["full_suite"]
    assert "earlier attempts that failed: 20261010-100000" in text


# --- skips and incomplete runs ------------------------------------------------------------------------------------
def test_wrong_key_test_skipped_without_gatelink_key(root):
    add_run(root, "full", "20261010-090000", **{"options.gatelink_key": False},
            tests=with_outcome("full", "test_link_faults.py::test_wrong_key_rejected", "skipped",
                               "set GATELINK_KEY (the 32-hex-char key both boards share) to run this"))
    _, text, data = collect(root)
    assert not met(data)["full_suite"]
    assert "unexpected skip of `test_wrong_key_rejected`: set GATELINK_KEY" in text


def test_partial_runs_dont_count(root):
    add_run(root, "full", "20261010-090000", **{"selection.keyword": "not jammed",
                                                "selection.groups.default.selected": 4},
            tests=[t for t in fixture_tests("full") if t["nodeid"] != JAM])
    add_run(root, "full", "20261010-100000", **{"selection.paths": ["tests/e2e/test_normal.py"]})
    add_run(root, "full", "20261010-110000", tests=fixture_tests("full")[:3], exitstatus=1)  # stopped early
    _, _, data = collect(root)
    assert not met(data)["full_suite"]


def test_runs_that_skip_whole_files_dont_count(root):
    """--ignore, --lf and --sw leave files uncollected, which the group counts can't show; so do collection errors
    under --continue-on-collection-errors."""
    add_run(root, "full", "20261010-090000", **{"selection.filters": ["--ignore tests/e2e/test_link_faults.py"]})
    add_run(root, "full", "20261010-100000", **{"selection.collect_errors": 1})
    _, text, data = collect(root)
    assert not met(data)["full_suite"]
    assert "other runs of its tests: 20261010-090000" not in text  # incomplete: not an attempt at all
    assert data["not_counted"] == [{"run": "20261010-100000", "why": ["1 test files failed to collect"]}]


def test_interrupted_run_isnt_counted(root):
    add_run(root, "full", "20261010-090000", exitstatus=2)
    _, text, data = collect(root)
    assert not met(data)["full_suite"]
    assert data["not_counted"][0]["why"] == ["pytest exit status 2: interrupted or broken"]


def test_soak_needs_twenty_cycles(root):
    add_run(root, "soak", "20261010-090000", **{"options.cycles": 5})
    _, text, data = collect(root)
    assert not met(data)["soak"]
    add_run(root, "soak", "20261010-100000")
    _, _, data = collect(root)
    assert met(data)["soak"]


def test_longsoak_needs_an_hour(root):
    add_run(root, "longsoak", "20261010-090000", **{"options.soak_minutes": 30})
    _, _, data = collect(root)
    assert not met(data)["longsoak_60m"] and not met(data)["longsoak_24h"]


# --- power runs ---------------------------------------------------------------------------------------------------
def test_power_runs_with_one_lipo_state_only(root):
    add_run(root, "power_out", "20261010-090000")
    _, text, data = collect(root)
    assert met(data)["power_lipo_out"]  # its LiPo-in tests were skipped, as expected
    assert not met(data)["power_lipo_in"]
    assert "other runs of its tests: 20261010-090000 (LiPos gate out, house out)" in text


def test_power_run_with_mixed_lipos_counts_for_neither(root):
    add_run(root, "power_out", "20261010-090000", **{"facts.LiPos": "gate out, house in"})
    _, _, data = collect(root)
    assert not met(data)["power_lipo_out"] and not met(data)["power_lipo_in"]


def test_power_run_unexpected_skip(root):
    tests = with_outcome("power_out", "test_power.py::test_gate_power_cut_at_rest", "skipped", "preflight failed")
    add_run(root, "power_out", "20261010-090000", tests=tests)
    _, _, data = collect(root)
    assert not met(data)["power_lipo_out"]


# --- which runs count ---------------------------------------------------------------------------------------------
def test_runs_on_the_bad_board_dont_count(root):
    add_run(root, "full", "20261010-090000", **{"boards.gate.usb_serial": "8E4B6C235030534D4D2E3120FF191117"})
    _, text, data = collect(root)
    assert not met(data)["full_suite"]
    assert "gate board …191117 is never used" in text


def test_runs_on_other_boards_dont_count(root):
    add_run(root, "full", "20261010-090000", **{"boards.house.usb_serial": "1234567890ABCDEF"})
    _, _, data = collect(root)
    assert "isn't the bench house board …183013" in data["not_counted"][0]["why"][0]


def test_runs_of_other_firmware_dont_count(root):
    add_run(root, "full", "20261010-090000", **{"git.firmware_tree": OTHER_TREE})
    add_run(root, "full", "20261010-100000", **{"git.dirty_paths": ["firmware/GateLink/link.cpp", "REVIEW.md"]})
    add_run(root, "full", "20261010-110000", **{"boards.gate.fw": "0.13.5"})  # not picked up at all
    _, _, data = collect(root)
    assert not met(data)["full_suite"]
    why = {r["run"]: r["why"] for r in data["not_counted"]}
    assert set(why) == {"20261010-090000", "20261010-100000"}
    assert "isn't the release commit's" in why["20261010-090000"][0]
    assert why["20261010-100000"] == ["local changes in firmware/GateLink/link.cpp"]


def test_listed_runs_only(root):
    add_run(root, "full", "20261010-090000")
    add_run(root, "full", "20261010-100000", **{"boards.gate.fw": "0.13.5"})
    _, _, data = collect(root, extra=("--runs", "20261010-100000"))
    assert data["runs"] == [] and "gate board on 0.13.5, not 0.14.0" in data["not_counted"][0]["why"]
    assert rel.main(["collect", "v0.14.0", "--runs", "nope"], repo=FakeRepo(root)) == 2


def test_collect_refuses_a_commit_without_the_version(root):
    assert rel.main(["collect", "v0.14.1"], repo=FakeRepo(root)) == 2


def test_ci_must_pass(root):
    all_minor_runs(root)
    code, text, data = collect(root, repo=FakeRepo(root, ci="failure"))
    assert code == 1 and not met(data)["ci"]
    assert "ci.yml run](https://github.com/x/y/actions/runs/1): failure" in text


# --- the evidence file --------------------------------------------------------------------------------------------
def test_notes_are_kept_when_collected_again(root):
    collect(root)
    path = root / "docs" / "releases" / "v0.14.0.md"
    path.write_text(path.read_text(encoding="utf-8").replace("anything a reviewer should know.",
                                                             "anything a reviewer should know.\n\nRan on a new hub."),
                    encoding="utf-8")
    _, text, _ = collect(root)
    assert "Ran on a new hub." in text


def test_json_block_survives_comment_markers(root):
    add_run(root, "full", "20261010-090000", **{"facts.odd": "a --> b"}, anomalies=["x -->"])
    _, text, data = collect(root)
    assert "a --> b" not in text.split("<!-- release-evidence", 1)[1]
    assert data["runs"][0]["facts"]["odd"] == "a --> b" and data["runs"][0]["anomalies"] == ["x -->"]


def test_check_good_evidence(root, capsys):
    all_minor_runs(root)
    assert collect(root)[0] == 0
    assert check(root) == 0
    assert check(root, extra=("--ci",)) == 0
    assert "complete" in capsys.readouterr().out
    # --file names it explicitly
    assert check(root, extra=("--file", str(root / "docs" / "releases" / "v0.14.0.md"))) == 0


def test_check_bad_evidence(root, capsys):
    assert check(root) == 1  # no file
    assert "is missing" in capsys.readouterr().out
    all_minor_runs(root)
    collect(root)
    path = root / "docs" / "releases" / "v0.14.0.md"
    good = path.read_text(encoding="utf-8")

    set_fw(root, "0.14.1")  # FW_VERSION moved on since
    assert check(root) == 1
    set_fw(root, "0.14.0")

    assert check(root, repo=FakeRepo(root, tree=OTHER_TREE)) == 1  # firmware changed after the runs
    assert "the firmware changed after them" in capsys.readouterr().out

    assert check(root, repo=FakeRepo(root, ci="failure"), extra=("--ci",)) == 1
    assert "CI on the release commit" in capsys.readouterr().out

    # Collected as a patch release, but the tags make it a minor one.
    assert check(root, repo=FakeRepo(root, tags=("v0.14.0", "v0.13.9"))) == 0  # previous 0.13.9: minor, as recorded
    path.write_text(good.replace('"kind": "minor"', '"kind": "patch"'), encoding="utf-8")
    assert check(root) == 1
    assert "it's a minor one" in capsys.readouterr().out
    assert check(root, repo=FakeRepo(root, tags=())) == 1  # no earlier tag: minor, as collect has it
    assert "with no earlier release tag it's a minor one" in capsys.readouterr().out

    # A criterion edited to not met, or dropped from the block.
    data = rel.read_block(good)
    for c in data["criteria"]:
        if c["id"] == "power_lipo_in":
            c["met"] = False
    path.write_text(good.split("<!-- release-evidence")[0] + "<!-- release-evidence\n" + json.dumps(data) + "\n-->\n",
                    encoding="utf-8")
    assert check(root) == 1
    data["criteria"] = [c for c in data["criteria"] if c["id"] != "power_lipo_in"]
    path.write_text("<!-- release-evidence\n" + json.dumps(data) + "\n-->\n", encoding="utf-8")
    assert check(root) == 1
    assert "power_lipo_in (Power: -m power with both LiPos in) missing" in capsys.readouterr().out

    path.write_text("# no block\n", encoding="utf-8")
    assert check(root) == 1
    assert check(root, "v0.14.0", extra=("--file", str(root / "nope.md"))) == 1


def test_check_wrong_version(root):
    all_minor_runs(root)
    collect(root)
    (root / "docs" / "releases" / "v0.14.0.md").rename(root / "docs" / "releases" / "v0.14.1.md")
    set_fw(root, "0.14.1")
    assert check(root, "v0.14.1") == 1  # the file inside is for 0.14.0


# --- the doc and the tool agree -----------------------------------------------------------------------------------
def test_criteria_doc_names_what_the_tool_checks():
    doc = (REPO_ROOT / "docs" / "release-criteria.md").read_text(encoding="utf-8")
    for c in rel.CRITERIA:
        assert f"`{c.id}`" in doc, f"docs/release-criteria.md doesn't name criterion {c.id}"
    for tail in [*rel.BENCH_BOARDS.values(), *rel.NEVER_BOARDS]:
        assert f"…{tail}" in doc, f"docs/release-criteria.md doesn't name board …{tail}"
    for n in (f"--cycles {rel.SOAK_CYCLES}", f"--soak-minutes {rel.LONGSOAK_MINUTES}",
              f"--soak-minutes {rel.LONGSOAK_MAJOR_MINUTES}", f"--tx-power {rel.FULL_POWER_DBM}"):
        assert n in doc, f"docs/release-criteria.md doesn't say {n}"
    for path in rel.SOURCE_PATHS:
        assert f"`{path}`" in doc, f"docs/release-criteria.md doesn't list {path}"
