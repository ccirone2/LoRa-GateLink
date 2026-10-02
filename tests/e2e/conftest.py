"""Bench end-to-end fixtures.

`bench` (session): finds the hardware, backs up both boards' config, applies the test profile and restores
everything at the end. Skips the whole run when the bench isn't connected.
`rig` (per test): brings the bench to the baseline, runs the test, then checks the behavioural invariants and
writes the test's timeline to results/<run>/.
"""
import json
import os
import re
import time
from pathlib import Path

import pytest
import serial

from gatelink.board import Board, BoardError, find_boards
from gatelink.bench import Bench
from gatelink.controller import Controller, ControllerError
from gatelink.gatesim import GateSim, GateSimError
from gatelink.timeline import Timeline

RESULTS = Path(__file__).parent / "results"


def pytest_addoption(parser):
    g = parser.getgroup("gatelink")
    g.addoption("--house-port", help="house board COM port (default: auto-detect by role)")
    g.addoption("--gate-port", help="gate board COM port (default: auto-detect by role)")
    g.addoption("--sim-port", default=os.environ.get("GATELINK_SIM_PORT", "COM10"), help="GateSim Uno port")
    g.addoption("--cycles", type=int, default=20, help="open/close cycles for the soak test")
    g.addoption("--soak-minutes", type=float, default=120, help="duration of the long soak (-m longsoak)")
    g.addoption("--rf-cycles", type=int, default=5, help="open/close cycles on the marginal link (-m rf)")
    g.addoption("--restore-key", action="store_true",
                help="first reboot both boards and re-apply GATELINK_KEY, e.g. after an interrupted wrong-key test")


def pytest_configure(config):
    config._gatelink = {"bench": None, "outcomes": {}, "preflight_failed": False, "run_dir": None}


def _restore_key(boards):
    """Put the shared key back on both boards (e.g. the wrong-key test was killed and left the gate's key random)."""
    key = os.environ.get("GATELINK_KEY", "")
    if len(key) != 32:
        pytest.exit("--restore-key needs GATELINK_KEY (the 32-hex-char key both boards share)", returncode=4)
    for b in boards.values():
        # key.set saves the whole config. Reboot first so an interrupted run's unsaved test profile is dropped
        # rather than saved along with the key.
        b.reboot()
        b.request("key.set", key=key)
    deadline = time.monotonic() + 30
    while not all(b.status()["link"]["verified"] for b in boards.values()):
        if time.monotonic() > deadline:
            pytest.exit("--restore-key: the link didn't come back verified within 30 s", returncode=4)
        time.sleep(0.5)
    print("\n--restore-key: GATELINK_KEY applied to both boards, link verified")


def _open_board(port, timeline):
    b = Board(port, timeline)
    b.open()
    b.name = b.info()["role"]
    return b


@pytest.fixture(scope="session")
def bench(request):
    cfg = request.config
    state = cfg._gatelink
    tl = Timeline()
    sim_port = cfg.getoption("--sim-port")
    opened = []
    try:
        if cfg.getoption("--house-port") or cfg.getoption("--gate-port"):
            boards = {}
            for opt in ("--house-port", "--gate-port"):
                if cfg.getoption(opt):
                    b = _open_board(cfg.getoption(opt), tl)
                    boards[b.name] = b
        else:
            boards = find_boards(tl, exclude={sim_port})
        opened += boards.values()
        missing = {"house", "gate"} - boards.keys()
        if missing:
            for o in opened:
                o.close()
            pytest.skip(f"bench not connected: no {' or '.join(sorted(missing))} board found "
                        f"(close the web console; boards found: {sorted(boards) or 'none'})")
        if cfg.getoption("--restore-key"):
            _restore_key(boards)
        sim = GateSim(sim_port, tl)
        sim.open()
        opened.append(sim)
        ctrl = Controller(tl)
        ctrl.state()
    except (serial.SerialException, BoardError, GateSimError, ControllerError) as e:
        for o in opened:
            o.close()
        pytest.skip(f"bench not available: {e}")

    run_dir = RESULTS / time.strftime("%Y%m%d-%H%M%S")
    run_dir.mkdir(parents=True, exist_ok=True)
    state["run_dir"] = run_dir
    b = Bench(boards["house"], boards["gate"], sim, ctrl, tl, run_dir)
    (run_dir / "config_backup.json").write_text(json.dumps(b.backup, indent=2))
    state["bench"] = b
    b.apply_profile()
    yield b
    b.begin_test("teardown")
    try:
        b.baseline()  # leave the bench at rest: gate closed, controller off
    except AssertionError as e:
        print(f"\nWARNING: couldn't return the bench to rest: {str(e).splitlines()[0]}")
    b.restore()
    tl.dump(run_dir / "session.jsonl")
    for o in opened:
        o.close()


@pytest.fixture
def rig(bench, request):
    if request.config._gatelink["preflight_failed"]:
        pytest.skip("preflight failed")
    bench.begin_test(request.node.name)
    start = bench.mark()
    try:
        bench.baseline()
        m = bench.mark()
        snap = bench.snapshot()
        bench.note(f"=== {request.node.name}")
        yield bench
        bench.check_invariants(m, snap)
    finally:
        name = re.sub(r"[^\w.-]+", "_", request.node.name)
        bench.timeline.dump(bench.run_dir / f"{name}.jsonl", since=start)


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    rep = yield
    state = item.config._gatelink
    prev = state["outcomes"].get(item.nodeid)
    if rep.when == "call" or (rep.failed and prev != "failed") or (rep.skipped and prev is None):
        state["outcomes"][item.nodeid] = "failed" if rep.failed else rep.outcome
    if rep.failed and "test_00_preflight" in item.nodeid:
        state["preflight_failed"] = True
    return rep


def pytest_terminal_summary(terminalreporter, config):
    state = config._gatelink
    b = state["bench"]
    if b is None:
        return
    lines = [f"# GateLink bench run {state['run_dir'].name}", "", "## Scenarios", "",
             "| Result | Test |", "|---|---|"]
    lines += [f"| {o} | `{n.split('::', 1)[-1]}` |" for n, o in state["outcomes"].items()]
    rows = b.latency_rows()
    if rows:
        lines += ["", "## Latencies (seconds)", "", "| Measurement | n | min | median | max |", "|---|---|---|---|---|"]
        lines += [f"| {n} | {c} | {lo:.3f} | {med:.3f} | {hi:.3f} |" for n, c, lo, med, hi in rows]
    if b.anomalies:
        lines += ["", "## Anomalies (tolerated, worth a look)", ""] + [f"- {a}" for a in b.anomalies]
    if b.facts:
        lines += ["", "## Link", ""] + [f"- {k}: {v}" for k, v in b.facts.items()]
    text = "\n".join(lines) + "\n"
    (state["run_dir"] / "summary.md").write_text(text, encoding="utf-8")
    terminalreporter.write_sep("=", "GateLink bench summary")
    terminalreporter.write_line(text)
    terminalreporter.write_line(f"timelines and summary: {state['run_dir']}")
