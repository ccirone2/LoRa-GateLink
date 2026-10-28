"""Tests for tools/mutate.py, and that the mutation catalog still applies to the firmware as it is.

The mutation runs themselves need Linux and minutes per mutation (tests/native/README.md); these checks are fast and
keep the catalog from rotting: a firmware change that moves a mutation's text fails here until the catalog follows.
"""

import json
from pathlib import Path

import pytest

import mutate

ROOT = Path(__file__).resolve().parents[2]


def write(tmp_path, data):
    p = tmp_path / "mutations.json"
    p.write_text(json.dumps(data), encoding="utf-8")
    return p


def mutation(name="m", **edit):
    return {"name": name, "invariant": "relays-pulsed", "why": "w",
            "edits": [{"file": "a.cpp", "find": "x", "replace": "y", **edit}]}


def test_catalog_shape_is_checked(tmp_path):
    assert mutate.load_catalog(write(tmp_path, [mutation()]))[0]["name"] == "m"
    bad = [
        ([{"name": "m", "edits": []}], "no 'invariant'"),
        ([mutation("Bad-Name")], "lower_snake_case"),
        ([mutation(), mutation()], "duplicate"),
        ([mutation(replace="x")], "different replace"),
        ([{**mutation(), "edits": []}], "no edits"),
    ]
    for data, msg in bad:
        with pytest.raises(mutate.CatalogError, match=msg):
            mutate.load_catalog(write(tmp_path, data))


def test_apply_edits_needs_exactly_one_match(tmp_path):
    (tmp_path / "a.cpp").write_text("int x = 1; int z = 2;\n", encoding="utf-8")
    changed = mutate.apply_edits(tmp_path, mutation(find="x = 1", replace="x = 0"))
    assert changed == [tmp_path / "a.cpp"]
    assert (tmp_path / "a.cpp").read_text(encoding="utf-8") == "int x = 0; int z = 2;\n"
    with pytest.raises(mutate.CatalogError, match="0 matches"):
        mutate.apply_edits(tmp_path, mutation(find="nowhere"))
    (tmp_path / "a.cpp").write_text("x x\n", encoding="utf-8")
    with pytest.raises(mutate.CatalogError, match="2 matches"):
        mutate.apply_edits(tmp_path, mutation(find="x"))


def test_failed_tests_parses_the_runner():
    out = ("ok    passes\nFAIL  breaks_k1\n      world.cpp:12: K1 on 700 ms\nxfail known (bug)\n"
           "XPASS fixed_now: fixed? remove XFAIL (bug)\n3 tests, 2 failed\n")
    assert mutate.failed_tests(out) == ["breaks_k1", "fixed_now"]


def test_a_checker_out_of_time_neither_kills_nor_spares(monkeypatch):
    # A proof that ran out of time proved nothing: counting it as a kill would pass a mutation no proof catches.
    class Done:
        def __init__(self, code, out):
            self.returncode, self.stdout, self.stderr = code, out, ""

    def outcome(result):
        def run(_cmd, **_kw):
            if isinstance(result, Exception):
                raise result
            return result
        monkeypatch.setattr(mutate.subprocess, "run", run)
        return mutate.run_verify(Path("fw"), Path("b"), ["esbmc-x"], 10)[0]

    assert outcome(Done(0, "x: holds (0 of 9 properties failed)")) is True
    assert outcome(Done(2, "x: violated (1 of 9 properties failed)\nx: UNEXPECTED")) is False
    assert outcome(mutate.subprocess.TimeoutExpired("make", 10)) is None
    assert outcome(Done(2, "x: timed out after 5400 s\nmake: *** Error 1")) is None
    assert outcome(Done(2, "x: ESBMC gave no verdict (exit -9):")) is None
    assert outcome(Done(2, "a: violated (1 of 9 properties failed)\nb: timed out after 60 s")) is False


CATALOG = mutate.CATALOG


@pytest.mark.skipif(not CATALOG.exists(), reason="no mutation catalog yet")
def test_catalog_applies_to_the_current_firmware(tmp_path):
    for m in mutate.load_catalog(CATALOG):
        fw = tmp_path / m["name"]
        fw.mkdir()
        for e in m["edits"]:
            if not (fw / e["file"]).exists():
                (fw / e["file"]).write_bytes((ROOT / "firmware/GateLink" / e["file"]).read_bytes())
        mutate.apply_edits(fw, m)  # raises with the mutation's name if its text moved


@pytest.mark.skipif(not CATALOG.exists(), reason="no mutation catalog yet")
def test_every_invariant_tag_is_documented():
    tags = {m["invariant"] for m in mutate.load_catalog(CATALOG)}
    readme = (ROOT / "tests/native/README.md").read_text(encoding="utf-8")
    missing = sorted(t for t in tags if f"`{t}`" not in readme)
    assert not missing, f"invariant tags not described in tests/native/README.md: {missing}"
