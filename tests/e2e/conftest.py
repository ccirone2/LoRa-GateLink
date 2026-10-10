"""Bench end-to-end fixtures.

`bench` (session): finds the hardware, backs up both boards' config, applies the test profile and restores
everything at the end. Skips the whole run when the bench isn't connected.
`rig` (per test): brings the bench to the baseline, runs the test, then checks the behavioural invariants and
writes the test's timeline to results/<run>/.
At the end, results/<run>/summary.md (for people) and summary.json (for tools/release_evidence.py, which checks a
release against docs/release-criteria.md).
"""
import json
import os
import re
import shlex
import subprocess
import time
from datetime import datetime
from pathlib import Path

import pytest
import serial
import serial.tools.list_ports

from gatelink.bench import Bench
from gatelink.controller import Controller, ControllerError
from gatelink.gatesim import GateSim, GateSimError
from gatelink_client.board import BoardError, UartTap, find_boards, find_uarts, open_board
from gatelink_client.timeline import Timeline

RESULTS = Path(__file__).parent / "results"
REPO = Path(__file__).resolve().parents[2]
# The opt-in markers (pytest.ini deselects them by default); a test with none of them is in the "default" group.
GROUPS = ("longsoak", "soak", "power", "rf")
SUMMARY_SCHEMA = 1  # summary.json layout; tools/release_evidence.py reads it


def pytest_addoption(parser):
    g = parser.getgroup("gatelink")
    g.addoption("--house-port", help="house board COM port (default: auto-detect by role)")
    g.addoption("--gate-port", help="gate board COM port (default: auto-detect by role)")
    g.addoption("--sim-port", default=os.environ.get("GATELINK_SIM_PORT", "COM10"), help="GateSim Uno port")
    g.addoption("--house-uart", help="house board's USB-to-UART adapter port to tap (default: auto-detect FTDI ports)")
    g.addoption("--gate-uart", help="gate board's USB-to-UART adapter port to tap (default: auto-detect FTDI ports)")
    g.addoption("--no-uart", action="store_true", help="don't tap the boards' UART consoles")
    g.addoption("--cycles", type=int, default=20, help="open/close cycles for the soak test")
    g.addoption("--soak-minutes", type=float, default=120, help="duration of the long soak (-m longsoak)")
    g.addoption("--rf-cycles", type=int, default=5, help="open/close cycles on the marginal link (-m rf)")
    g.addoption("--tx-power", type=int,
                help="run both boards at this tx_power (dBm, applied unsaved like the rest of the test profile); "
                     "without it the saved value is kept and must be <= 5")
    g.addoption("--restore-key", action="store_true",
                help="first reboot both boards and re-apply GATELINK_KEY, e.g. after an interrupted wrong-key test")


def pytest_configure(config):
    config._gatelink = {"bench": None, "outcomes": {}, "details": {}, "preflight_failed": False, "run_dir": None,
                        "started": _now(), "boards": {}, "deselected": [], "groups": {}}


def _now():
    return datetime.now().astimezone().isoformat(timespec="seconds")


def _markers(item):
    return sorted({m.name for m in item.iter_markers()})


def _group(item):
    names = set(_markers(item))
    return next((g for g in GROUPS if g in names), "default")


def pytest_deselected(items):
    # -m, -k, --deselect and --lf all report here; with the selected items this gives what each group left out.
    if items and hasattr(items[0].config, "_gatelink"):
        items[0].config._gatelink["deselected"] += items


def pytest_collection_finish(session):
    """Per group (default and each opt-in marker), how many tests were collected and how many selected: the release
    evidence counts a group as run in full only if none of its tests was left out."""
    state = session.config._gatelink
    groups = {g: {"collected": 0, "selected": 0} for g in ("default", *GROUPS)}
    for item in session.items:
        g = _group(item)
        groups[g]["collected"] += 1
        groups[g]["selected"] += 1
        state["details"][item.nodeid] = {"group": g, "markers": _markers(item), "duration_s": 0.0}
    for item in state["deselected"]:
        groups[_group(item)]["collected"] += 1
    state["groups"] = groups


def _restore_key(boards):
    """Put the shared key back on both boards (e.g. the wrong-key test was killed and left the gate's key random)."""
    key = os.environ.get("GATELINK_KEY", "")
    if len(key) != 32:
        pytest.exit("--restore-key needs GATELINK_KEY (the 32-hex-char key both boards share)", returncode=4)
    for b in boards.values():
        # Reboot first so an interrupted run's unsaved test profile is dropped (since 0.3.5 key.set saves only
        # the key, but older firmware saved the whole config with it).
        b.reboot()
        b.request("key.set", key=key)
    deadline = time.monotonic() + 30
    while not all(b.status()["link"]["verified"] for b in boards.values()):
        if time.monotonic() > deadline:
            pytest.exit("--restore-key: the link didn't come back verified within 30 s", returncode=4)
        time.sleep(0.5)
    print("\n--restore-key: GATELINK_KEY applied to both boards, link verified")


def _tap_uarts(cfg, boards, tl, opened, sim_port):
    """Listen on each board's UART console, if it has an adapter: its events reach the timeline even while the
    board is unpowered or rebooting and USB is gone (`boot` the moment power returns). Requests stay on USB (see
    gatelink_client.board). Returns a summary for the report."""
    uarts = {} if cfg.getoption("--no-uart") else find_uarts(tl, exclude={sim_port})
    for role in ("house", "gate"):
        if cfg.getoption(f"--{role}-uart"):
            uarts[role] = cfg.getoption(f"--{role}-uart")
    for role, port in uarts.items():
        if role in boards:
            tap = UartTap(boards[role], port)
            opened.append(tap)
            tap.open()
    return ", ".join(f"{r} USB {boards[r].port}" + (f" + UART tap {uarts[r]}" if r in uarts else "")
                     for r in ("house", "gate"))


def _board_ids(boards):
    """Each board's firmware and USB serial number (the SAMD21's chip id: it names the board whatever its COM port),
    for summary.json. Read once at the start, before anything can reboot a board."""
    serials = {p.device.upper(): p.serial_number for p in serial.tools.list_ports.comports()}  # --house-port com5
    out = {}
    for role, b in boards.items():
        info = b.info()
        out[role] = {"fw": info.get("fw"), "usb_serial": serials.get(b.port.upper()), "port": b.port,
                     "cfg_store": info.get("cfg_store"), "flash_id": info.get("flash_id")}
    return out


def _close_all(opened):
    for o in opened:
        try:
            o.close()
        except Exception as e:  # noqa: BLE001 - closing the rest matters more
            print(f"\nWARNING: couldn't close {getattr(o, 'port', o)}: {e}")


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
                    b = open_board(cfg.getoption(opt), tl)
                    opened.append(b)
                    boards[b.name] = b
        else:
            boards = find_boards(tl, exclude={sim_port})
            opened += boards.values()
        missing = {"house", "gate"} - boards.keys()
        if missing:
            _close_all(opened)
            pytest.skip(f"bench not connected: no {' or '.join(sorted(missing))} board found "
                        f"(close the web console; boards found: {sorted(boards) or 'none'})")
        state["boards"] = _board_ids(boards)
        consoles = _tap_uarts(cfg, boards, tl, opened, sim_port)
        if cfg.getoption("--restore-key"):
            _restore_key(boards)
        sim = GateSim(sim_port, tl)
        opened.append(sim)  # before open(): close() is safe on a port that never opened
        sim.open()
        ctrl = Controller(tl)
        ctrl.state()
    except (serial.SerialException, BoardError, GateSimError, ControllerError) as e:
        _close_all(opened)
        pytest.skip(f"bench not available: {e}")
    except BaseException:
        _close_all(opened)
        raise

    b = None
    try:
        run_dir = RESULTS / time.strftime("%Y%m%d-%H%M%S")
        run_dir.mkdir(parents=True, exist_ok=True)
        state["run_dir"] = run_dir
        b = Bench(boards["house"], boards["gate"], sim, ctrl, tl, run_dir, tx_power=cfg.getoption("--tx-power"))
        b.facts["consoles"] = consoles
        (run_dir / "config_backup.json").write_text(json.dumps(b.backup, indent=2))
        state["bench"] = b
        b.apply_profile()
        b.facts["controller power"] = b.power.detect()
    except BaseException:
        try:
            if b is not None:
                b.restore()  # the profile may be half applied
        finally:
            _close_all(opened)
        raise

    try:
        yield b
    finally:
        b.begin_test("teardown")
        try:
            b.baseline()  # leave the bench at rest: gate closed, controller off
        except Exception as e:  # noqa: BLE001 - restore and close regardless
            print(f"\nWARNING: couldn't return the bench to rest: {(str(e).splitlines() or [repr(e)])[0]}")
        try:
            b.restore()
        finally:
            try:
                tl.dump(run_dir / "session.jsonl")
            finally:
                _close_all(opened)


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


def _reason(rep, call):
    """Why a test was skipped or failed, in one line (summary.json)."""
    if rep.skipped and isinstance(rep.longrepr, tuple):
        return str(rep.longrepr[2]).removeprefix("Skipped: ")
    if call.excinfo is not None:
        lines = str(call.excinfo.value).strip().splitlines()
        return f"{call.excinfo.typename}: {lines[0] if lines else ''}"[:300]
    return None


@pytest.hookimpl(wrapper=True)
def pytest_runtest_makereport(item, call):
    rep = yield
    state = item.config._gatelink
    prev = state["outcomes"].get(item.nodeid)
    d = state["details"].setdefault(item.nodeid, {"group": _group(item), "markers": _markers(item), "duration_s": 0.0})
    d["duration_s"] += rep.duration
    if (rep.failed or rep.skipped) and "reason" not in d:
        d["reason"] = _reason(rep, call)
    if hasattr(rep, "wasxfail"):
        # An xfail test that fails in its teardown (the invariant checks) after passing its body is an xfail too.
        if rep.skipped or rep.when == "call":
            state["outcomes"][item.nodeid] = "xfailed" if rep.skipped else "xpassed"
    elif rep.when == "call" or (rep.failed and prev != "failed") or (rep.skipped and prev is None):
        state["outcomes"][item.nodeid] = "failed" if rep.failed else rep.outcome
    if rep.failed and "test_00_preflight" in item.nodeid:
        state["preflight_failed"] = True
    return rep


def _git(*args):
    try:
        r = subprocess.run(["git", "-C", str(REPO), *args], capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout if r.returncode == 0 else None


def _git_state():
    """The checkout the run came from. firmware_tree is the git tree of firmware/GateLink at HEAD: with no local
    changes there it names the firmware source exactly, and the release evidence compares it with the release's."""
    status = _git("status", "--porcelain")
    paths = [line[3:] for line in status.splitlines() if line.strip()] if status is not None else []
    commit, tree = _git("rev-parse", "HEAD"), _git("rev-parse", "HEAD:firmware/GateLink")
    return {"commit": commit and commit.strip(), "dirty": bool(paths) if status is not None else None,
            "dirty_paths": paths[:200], "firmware_tree": tree and tree.strip()}


def _selection(config, state, stats):
    """What the run was asked for: -m and -k, the paths (relative to the repo) and the per-group counts, plus what
    else left tests out without reporting them as deselected (--ignore, --lf, --sw skip whole files; collection
    errors under --continue-on-collection-errors), so the release evidence doesn't take such a run as complete."""
    def rel(arg):
        path, sep, rest = str(arg).partition("::")
        p = (Path(config.invocation_params.dir) / path).resolve()
        try:
            path = p.relative_to(REPO).as_posix()
        except ValueError:
            path = p.as_posix()
        return path + sep + rest
    opt = config.getoption
    filters = [f"--ignore {p}" for p in opt("ignore", None) or []]
    filters += [f"--ignore-glob {p}" for p in opt("ignore_glob", None) or []]
    filters += [f"--deselect {p}" for p in opt("deselect", None) or []]
    filters += [flag for flag, dest in (("--lf", "lf"), ("--sw", "stepwise")) if opt(dest, False)]
    return {"markexpr": config.option.markexpr, "keyword": config.option.keyword,
            "paths": [rel(a) for a in config.args], "groups": state["groups"], "filters": filters,
            "collect_errors": sum(1 for r in stats.get("error", []) if getattr(r, "when", None) == "collect")}


def _summary_data(config, state, b, exitstatus, stats):
    """summary.json: what summary.md says, plus what tools/release_evidence.py needs to tell which release criteria
    the run meets (docs/release-criteria.md). Never holds the link key, only whether GATELINK_KEY was set."""
    tests = []
    for nodeid, outcome in state["outcomes"].items():
        d = state["details"].get(nodeid, {})
        tests.append({"nodeid": nodeid, "name": nodeid.split("::", 1)[-1], "outcome": outcome,
                      "group": d.get("group", "default"), "markers": d.get("markers", []),
                      "duration_s": round(d.get("duration_s", 0.0), 3), "reason": d.get("reason")})
    counts = {}
    for t in tests:
        counts[t["outcome"]] = counts.get(t["outcome"], 0) + 1
    opt = config.getoption
    return {
        "schema": SUMMARY_SCHEMA,
        "run": state["run_dir"].name,
        "start": state["started"],
        "end": _now(),
        "exitstatus": int(exitstatus),
        "command": "pytest " + " ".join(shlex.quote(str(a)) for a in config.invocation_params.args),
        "selection": _selection(config, state, stats),
        "git": _git_state(),
        "boards": state["boards"],
        "options": {"tx_power": opt("--tx-power"), "cycles": opt("--cycles"), "soak_minutes": opt("--soak-minutes"),
                    "rf_cycles": opt("--rf-cycles"), "gatelink_key": len(os.environ.get("GATELINK_KEY", "")) == 32,
                    "ha_power_entity": os.environ.get("GATELINK_HA_POWER_ENTITY") or None,
                    "no_uart": opt("--no-uart")},
        "counts": counts,
        "tests": tests,
        "latencies": [{"name": n, "n": c, "min": lo, "median": med, "max": hi}
                      for n, c, lo, med, hi in b.latency_rows()],
        "facts": {k: str(v) for k, v in b.facts.items()},
        "anomalies": list(b.anomalies),
    }


def pytest_terminal_summary(terminalreporter, exitstatus, config):
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
    try:
        data = _summary_data(config, state, b, exitstatus, terminalreporter.stats)
    except Exception as e:  # noqa: BLE001 - summary.md is written; say what's missing rather than fail the run
        terminalreporter.write_line(f"WARNING: couldn't write summary.json: {e!r}")
    else:
        (state["run_dir"] / "summary.json").write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    terminalreporter.write_sep("=", "GateLink bench summary")
    terminalreporter.write_line(text)
    terminalreporter.write_line(f"timelines and summary: {state['run_dir']}")
