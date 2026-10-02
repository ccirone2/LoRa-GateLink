"""The whole bench: house board, gate board, GateSim opener and the controller, plus the shared timeline.

Holds the test profile (shorter timings so scenarios run in seconds, applied unsaved and restored at the end),
the per-test baseline, the domain waits the scenarios use, and the invariant checks run after every test.
"""
import time
from collections import defaultdict

from .board import BoardError
from .controller import ControllerError, CtrlPower
from .gatesim import GateSimError

# Wire values as they appear in log entries (roles.h / link.h).
GS = {"unknown": 0, "closed": 1, "open": 2, "between": 3, "fault": 4, "no_power": 5}
CAUSE = {"none": 0, "lora": 1, "external": 2}
ACT_OPEN, ACT_CLOSE = 1, 2
RES_OK, RES_ALREADY, RES_BAD, RES_BUSY, RES_NO_POWER = 0, 1, 2, 3, 4

SIM_TRAVEL_S = 8
# Same order of timings as the defaults (heartbeat < link timeout, travel timeout < mismatch timeout), shorter.
PROFILE_COMMON = {"heartbeat_s": 5, "link_timeout_s": 15, "travel_timeout_s": 15, "cmd_ttl_s": 10}
PROFILE_HOUSE = {"mismatch_timeout_s": 20, "ctrl_power_sense": 0, "in2_invert": 0, "ctrl_sync": 1,
                 "sensor_invert": 0, "linkloss_open": 1}
PROFILE_GATE = {"power_sense": 1, "in1_invert": 0, "in2_invert": 0, "in3_invert": 0}

_ERRORS = (BoardError, GateSimError, ControllerError, KeyError, TypeError)


def _get(d, path):
    for part in path.split("__"):
        if not isinstance(d, dict):
            return None
        d = d.get(part)
    return d


class Bench:
    def __init__(self, house, gate, sim, ctrl, timeline, run_dir):
        self.house, self.gate, self.sim, self.ctrl = house, gate, sim, ctrl
        self.power = CtrlPower(ctrl, house)
        self.timeline = timeline
        self.run_dir = run_dir
        self.backup = {"house": house.config_get(), "gate": gate.config_get()}
        self.profile = {
            "house": {**PROFILE_COMMON, **PROFILE_HOUSE, "net_id": self.backup["house"]["net_id"]},
            "gate": {**PROFILE_COMMON, **PROFILE_GATE, "net_id": self.backup["gate"]["net_id"]},
        }
        self.pulse_ms = self.backup["gate"]["pulse_ms"]
        self.sim_travel_orig = sim.status()["travel"]
        self.latencies = defaultdict(list)
        self.facts = {}  # one-off measurements for the summary (RSSI, ping RTT, ...)
        self.anomalies = []  # tolerated oddities worth a look, listed in the summary
        self.begin_test("session")

    # --- profile ----------------------------------------------------------------------------------------------
    def board(self, name):
        return self.house if name == "house" else self.gate

    def apply_profile(self, name=None):
        for n in ([name] if name else ["house", "gate"]):
            self.board(n).config_set(**self.profile[n])
        if self.sim.status()["travel"] != SIM_TRAVEL_S:
            self.sim.travel(SIM_TRAVEL_S)  # saved in the Uno's EEPROM, so only when it differs

    def reboot(self, name):
        """Reboot a board and re-apply the profile (a reboot reloads the saved config)."""
        self.allow_reboot(name)
        self.board(name).reboot()
        self.board(name).config_set(**self.profile[name])

    def restore(self):
        """Put back every param that differs from the session-start backup, and the simulator's travel time."""
        for n in ("house", "gate"):
            b = self.board(n)
            try:
                cur = b.config_get()
                diff = {k: v for k, v in self.backup[n].items() if k in cur and cur[k] != v}
                if diff:
                    b.config_set(**diff)
            except BoardError as e:
                print(f"\nWARNING: couldn't restore {n} config: {e}")
        try:
            self.sim.fault("none")
            self.sim.relay_auto()
            self.sim.power(True)
            self.sim.travel(self.sim_travel_orig)
        except GateSimError as e:
            print(f"\nWARNING: couldn't restore the simulator: {e}")
        try:
            self.power.ensure_on()
        except ControllerError as e:
            print(f"\nWARNING: couldn't switch the controller's supply back on: {e}")

    # --- waits ------------------------------------------------------------------------------------------------
    def mark(self):
        return self.timeline.now()

    def note(self, text):
        self.timeline.add("test", "note", text=text)

    def fail(self, msg, since=None):
        tail = self.timeline.tail(50, since=since if since is not None else self._test_start)
        raise AssertionError(f"{msg}\n--- timeline (host s, latest last) ---\n{tail}")

    def wait_for(self, pred, timeout, what, poll=0.25):
        deadline = time.monotonic() + timeout
        err = None
        while True:
            try:
                v = pred()
                if v:
                    return v
            except _ERRORS as e:
                err = e
            if time.monotonic() >= deadline:
                self.fail(f"timed out after {timeout}s waiting for {what}" + (f" (last error: {err})" if err else ""))
            time.sleep(poll)

    def wait_house(self, timeout=20, what=None, **fields):
        """Wait for house status fields (nested with __, e.g. io__k1=True). Returns the matching status."""
        def ok():
            st = self.house.status()
            return st if all(_get(st, k) == v for k, v in fields.items()) else None
        return self.wait_for(ok, timeout, what or f"house {fields}")

    def wait_gate(self, state, cause=None, timeout=20, **fields):
        want = {"gate": state, **({"cause": cause} if cause else {}), **fields}

        def ok():
            st = self.gate.status()
            return st if all(_get(st, k) == v for k, v in want.items()) else None
        return self.wait_for(ok, timeout, f"gate {want}")

    def wait_ctrl(self, level, timeout=30):
        """The controller's relay as the house sees it on IN1."""
        return self.wait_house(timeout, f"controller {'on' if level else 'off'} (house IN1)", ctrl=level)

    def wait_log(self, src, ev, a=None, b=None, since=None, timeout=20):
        since = self._test_start if since is None else since
        return self.wait_for(lambda: self.timeline.first(src, "log", since, ev=ev, a=a, b=b), timeout,
                             f"{src} log {ev}" + (f" a={a}" if a is not None else "") + (f" b={b}" if b is not None else ""),
                             poll=0.05)

    def wait_sim(self, line, since=None, timeout=SIM_TRAVEL_S + 6):
        """Wait for a simulator `evt` line (without the `evt ` prefix), e.g. 'state open'."""
        since = self._test_start if since is None else since
        return self.wait_for(lambda: self.timeline.first("sim", "evt", since, line=line), timeout,
                             f"simulator evt {line!r}", poll=0.05)

    def logs(self, src, ev, since=None, a=None, b=None):
        return self.timeline.logs(src, ev, self._test_start if since is None else since, a=a, b=b)

    def expect_no(self, src, ev, seconds=0, since=None, **match):
        """Assert `src` logged no `ev` since `since`, after waiting `seconds` more."""
        if seconds:
            time.sleep(seconds)
        found = self.logs(src, ev, since, **match)
        if found:
            self.fail(f"unexpected {src} {ev}: {found[0]}")

    def latency(self, name, seconds):
        self.latencies[name].append(seconds)
        self.note(f"latency {name}: {seconds:.3f}s")

    # --- per-test bookkeeping ---------------------------------------------------------------------------------
    def begin_test(self, name):
        self.test_name = name
        self._test_start = self.timeline.now()
        self._allowed_reboot = set()
        self._allowed_counters = set()
        self._expected_cmds = (0, 0)

    def allow_reboot(self, name):
        self._allowed_reboot.add(name)
        self._allowed_counters.add(name)

    def allow_counters(self, name):
        """The test expects mac_fail/replay changes (or a link restart that resets counters) on this board."""
        self._allowed_counters.add(name)

    def expect_commands(self, lo, hi=None):
        """How many commands (house cmd_sent) this test should cause. Checked after the test."""
        self._expected_cmds = (lo, lo if hi is None else hi)

    def baseline(self):
        """Known start: opener powered and healthy, gate closed, controller off, house synced and armed."""
        self.note(f"baseline for {self.test_name}")
        self.sim.fault("none")
        self.sim.relay_auto()
        self.sim.power(True)
        self.power.ensure_on()
        self.apply_profile()
        self.wait_for(lambda: self.house.status()["link_up"] and self.gate.status()["link"]["verified"],
                      45, "link up and verified")
        if self.sim.status()["state"] != "closed":
            self.sim.close_gate()
            self.wait_for(lambda: self.sim.status()["state"] == "closed", SIM_TRAVEL_S + 5, "simulator closed")
        self.wait_gate("closed", timeout=10)
        if self.house.status()["ctrl"]:
            self.ctrl.off()  # gate already closed: the house suppresses this, no command
        self.wait_house(45, "house settled (gate closed, controller off and synced, armed, no window open)",
                        gate="closed", link_up=True, armed=True, cmd_pending=False, resyncing=False,
                        sync_window=False, ctrl=False, io__k1=False, io__k2=True)
        self.wait_for(lambda: self.gate.status()["target"] == "", 20, "gate command target cleared")

    def snapshot(self):
        snap = {}
        for n in ("house", "gate"):
            b = self.board(n)
            st = b.status()
            snap[n] = {"uptime": st["uptime_ms"], "mac_fail": st["link"]["mac_fail"],
                       "replay": st["link"]["replay"], "dropped": b.dropped}
        return snap

    def check_invariants(self, since, snap):
        """CLAUDE.md's behavioural invariants, checked over everything the test caused."""
        problems = []
        for e in self.timeline.select(src="sim", kind="evt", since=since):
            if e["line"] == "pulse both":
                problems.append(f"opener saw OPEN and CLOSE together at {e['t']}s (K1/K2 interlock broken)")
        pending = 0
        for e in self.timeline.logs("gate", since=since):
            if e["ev"] == "cmd_rx":
                pending += 1
            elif e["ev"] == "pulse":
                if e["b"] != self.pulse_ms:
                    problems.append(f"gate pulse of {e['b']} ms at {e['t']}s (pulse_ms {self.pulse_ms})")
                if pending:
                    pending -= 1
                else:
                    problems.append(f"gate pulsed relay {e['a']} at {e['t']}s without a received command")
        for n in ("house", "gate"):
            for e in self.timeline.logs(n, "radio_fail", since):
                problems.append(f"{n} radio_fail at {e['t']}s (a={e['a']})")
        try:
            end = self.snapshot()
        except BoardError as e:
            problems.append(f"couldn't read status after the test: {e}")
            end = None
        if end:
            for n in ("house", "gate"):
                if n in self._allowed_reboot:
                    continue
                if end[n]["uptime"] < snap[n]["uptime"] or end[n]["dropped"] != snap[n]["dropped"]:
                    problems.append(f"{n} reset during the test (reset_cause {self.board(n).status()['reset_cause']})")
                elif n not in self._allowed_counters and end[n]["mac_fail"] != snap[n]["mac_fail"]:
                    problems.append(f"{n} mac_fail went {snap[n]['mac_fail']} -> {end[n]['mac_fail']}")
        for n in ("house", "gate"):
            if n in self._allowed_counters:
                continue
            for e in self.timeline.logs(n, "replay", since):
                problems.append(f"{n} rejected a replayed frame at {e['t']}s (seq {e['a']}, last {e['b']})")
        cmds = len(self.timeline.logs("house", "cmd_sent", since))
        lo, hi = self._expected_cmds
        if not lo <= cmds <= hi:
            want = str(lo) if lo == hi else f"{lo}..{hi}"
            problems.append(f"house sent {cmds} command(s), expected {want}")
        if problems:
            self.fail("invariant check failed:\n  " + "\n  ".join(problems), since=since)

    # --- summary ----------------------------------------------------------------------------------------------
    def latency_rows(self):
        rows = []
        for name, vals in self.latencies.items():
            s = sorted(vals)
            rows.append((name, len(s), s[0], s[len(s) // 2], s[-1]))
        return rows
