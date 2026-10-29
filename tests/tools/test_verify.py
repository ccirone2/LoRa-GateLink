"""tools/verify: the model checkers' plumbing (the proofs themselves run in CI's verify job, `make -C tools/verify`)."""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "tools/verify"
sys.path.insert(0, str(VERIFY))

import gen_params  # noqa: E402
import tlc_trace  # noqa: E402


def test_ci_and_docker_pin_the_same_checkers():
    """ESBMC and TLC: CI's verify job installs what the Docker image does, each checked against its SHA-256."""
    docker = (VERIFY / "docker/Dockerfile").read_text(encoding="utf-8")
    ci = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
    pins = dict(re.findall(r"^ARG (\w+)=(\S+)$", docker, re.M))
    assert set(pins) == {"ESBMC_VERSION", "ESBMC_SHA256", "TLA_VERSION", "TLA_SHA256"}
    for name, value in pins.items():
        assert value in ci, f"ci.yml doesn't pin {name} {value} as tools/verify/docker/Dockerfile does"
    dev = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    assert f"| ESBMC | {pins['ESBMC_VERSION']} |" in dev
    assert f"| TLC (`tla2tools.jar`) | {pins['TLA_VERSION']} |" in dev


def test_params_header_has_every_params_row(tmp_path):
    text = (ROOT / "firmware/GateLink/config.cpp").read_text(encoding="utf-8")
    table = text[text.index("const ParamDef PARAMS[] = {"):]
    table = table[:table.index("};")]
    expected = len(re.findall(r"^\s*\{\s*\d+,", table, re.M))
    out = tmp_path / "params.h"
    gen_params.main(["gen_params.py", str(out)])
    h = out.read_text(encoding="utf-8")
    assert h.count("  PARAM(") == expected
    assert "#define PARAM_ID_pulse_ms 16" in h
    assert "#define PARAM_MAX_pulse_ms (5000)" in h


def test_trace_parser_keeps_changed_variables():
    out = """Error: Invariant CmdOrder is violated.
State 1: <Initial predicate>
/\\ now = 0
/\\ air = {}

State 2: <DeliverCmd line 9, col 3 to line 9, col 9 of module GateLink>
/\\ now = 0
/\\ air = { [t |-> "CMD",
  q |-> 2] }

"""
    steps = tlc_trace.parse_trace(out)
    assert [s[0] for s in steps] == [1, 2]
    assert steps[1][2]["air"] == '{ [t |-> "CMD", q |-> 2] }'
    lines = [""] * 8 + ["DeliverCmd(f) =="]
    assert tlc_trace.action_name(steps[1][1], lines) == "DeliverCmd"


def test_every_model_config_names_its_model_and_real_invariants():
    tla = (VERIFY / "tla/GateLink.tla").read_text(encoding="utf-8")
    defined = set(re.findall(r"^(\w+) ==", tla, re.M))
    cfgs = sorted((VERIFY / "tla").glob("*.cfg"))
    assert cfgs
    for cfg in cfgs:
        text = cfg.read_text(encoding="utf-8")
        assert re.search(r"^\\\* model: GateLink$", text, re.M), cfg.name
        invs = re.search(r"^INVARIANT (.+)$", text, re.M).group(1).split()
        assert set(invs) <= defined, f"{cfg.name}: {set(invs) - defined}"
        expect = re.search(r"^\\\* expect: (\w+) violated$", text, re.M)
        if expect:
            assert expect.group(1) in invs, f"{cfg.name} expects a violation of an invariant it doesn't check"


def test_verify_mutations_name_makefile_targets():
    make = (VERIFY / "Makefile").read_text(encoding="utf-8")
    harnesses = re.search(r"^HARNESSES := (.+)$", make, re.M).group(1).split()
    targets = {f"esbmc-{h}" for h in harnesses} | set(re.findall(r"^(esbmc-[\w-]+):", make, re.M))
    for h in ("gate", "house"):  # one target a segment (the pattern rules)
        segments = re.search(rf"^{h.upper()}_SEGMENTS := (.+)$", make, re.M).group(1).split()
        targets |= {f"esbmc-{h}-{s}" for s in segments}
    catalog = json.loads((VERIFY / "mutations.json").read_text(encoding="utf-8"))
    assert catalog
    for m in catalog:
        assert m["targets"] and set(m["targets"]) <= targets, m["name"]
        for e in m["edits"]:
            src = (ROOT / "firmware/GateLink" / e["file"]).read_text(encoding="utf-8")
            assert src.count(e["find"]) == 1, f"{m['name']}: {e['file']} has {src.count(e['find'])} matches"


def test_firmware_avoids_conditional_method_calls():
    # ESBMC 8.5 calls a method on the other operand of `(c ? a : b).m()`: the proof then checks another program than
    # the one that ships (it found a gate command restarting a pulse that wasn't). Bind a reference first.
    pattern = re.compile(r"\?[^;]*:[^;]*\)\s*(\.|->)\s*\w+\s*\(")
    for src in sorted((ROOT / "firmware/GateLink").glob("*.[ch]*")):
        for n, line in enumerate(src.read_text(encoding="utf-8").splitlines(), 1):
            code = line.split("//")[0]
            assert not pattern.search(code), f"{src.name}:{n}: {line.strip()}"
