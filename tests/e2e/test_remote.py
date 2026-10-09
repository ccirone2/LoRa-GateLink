"""Remote config and diagnostics over LoRa (house remote.set / remote.diag), the gate's heartbeat as the house
sees it, and the console's line limit."""
import math

from gatelink.bench import PROFILE_COMMON
from gatelink.flows import outage

CFG_TTL_S = 10  # role_house.cpp houseRemoteSet: the CFG slot's TTL
# Never writable over LoRa: the inputs' polarity, cmd_ttl_s and the radio params.
NOT_REMOTE = ("in1_invert", "in2_invert", "in3_invert", "in4_invert", "cmd_ttl_s", "tx_power", "sf", "freq_hz")
DIAG_COUNTERS = {"tx", "rx", "mac_fail", "replay", "retries", "giveups"}


def _meta(board):
    return {p["name"]: p for p in board.request("config.get")["meta"]}


def _remote_set(rig, name, value):
    """remote.set from the house; returns its `remote_set` event (sent when the gate ACKs or the slot gives up)."""
    m = rig.mark()
    rig.house.request("remote.set", name=name, value=value)
    ev = rig.wait_for(lambda: rig.timeline.first("house", "remote_set", m), CFG_TTL_S + 5,
                      f"remote_set event for {name}={value}", poll=0.05)
    return ev, m


def test_remote_set(rig):
    """A remote-writable param written over LoRa lands on the gate (which saves it) and can be put back."""
    pid = _meta(rig.gate)["retries"]["id"]
    old = rig.gate.config_get()["retries"]
    new = old - 1 if old > 0 else 1
    # The gate saves a remote write to flash (that param only; whole config up to 0.3.4), and `old` is the profile's
    # value, not necessarily the saved one: resave() saves the backup again.
    rig.saved.add("gate")
    try:
        ev, m = _remote_set(rig, "retries", new)
        assert ev["acked"] and ev["ok"], f"remote.set retries={new} not applied: {ev}"
        rig.wait_log("gate", "cfg_remote", a=pid, b=new, since=m, timeout=2)
        assert rig.gate.config_get()["retries"] == new
        ev, m = _remote_set(rig, "retries", old)
        assert ev["acked"] and ev["ok"], f"remote.set retries={old} (restore) not applied: {ev}"
        rig.wait_log("gate", "cfg_remote", a=pid, b=old, since=m, timeout=2)
        assert rig.gate.config_get()["retries"] == old
    finally:
        rig.resave("gate")


def test_remote_set_refused_for_local_params(rig):
    """Input polarity, cmd_ttl_s and radio params can't be written over LoRa: refused at the house, nothing sent."""
    meta = _meta(rig.house)
    cur = rig.gate.config_get()
    m = rig.mark()
    accepted = []
    try:
        for name in NOT_REMOTE:
            assert not meta[name]["remote"], f"{name} is flagged remote-writable"
            res = rig.house.request("remote.set", check=False, name=name, value=cur[name])  # same value: harmless
            if res.get("ok"):
                accepted.append(name)
        assert not accepted, f"remote.set accepted {accepted}"
        rig.expect_no("gate", "cfg_remote", seconds=2, since=m)
        assert not rig.timeline.select(src="house", kind="remote_set", since=m), "a CFG_SET went out"
    finally:
        if accepted:
            rig.resave("gate")  # one got through and the gate saved: put the backup back


def test_remote_set_busy(rig):
    """The CFG slot holds one write: a second remote.set while the first is pending is refused, not queued over it.
    During an outage the first stays pending for the slot's whole TTL, then gives up unapplied."""
    old = rig.gate.config_get()["retries"]
    m = rig.mark()
    with outage(rig):
        rig.house.request("remote.set", name="retries", value=old - 1 if old > 0 else 1)
        res = rig.house.request("remote.set", check=False, name="retries", value=old + 1 if old < 10 else 9)
        assert not res["ok"] and res.get("error") == "busy", f"second remote.set while one is pending: {res}"
        ev = rig.wait_for(lambda: rig.timeline.first("house", "remote_set", m), CFG_TTL_S + 5,
                          "remote_set giving up", poll=0.05)
        assert not ev["acked"] and not ev["ok"], f"a write into a dead link can't be acked: {ev}"
    rig.expect_no("gate", "cfg_remote", since=m)
    assert rig.gate.config_get()["retries"] == old


HOLD_PULSE_MS = 2000  # long enough that a save during it is clearly inside it


def test_remote_set_waits_for_pulse(rig):
    """A remote write landing while a gate relay pulses waits until the pulse is over before the gate saves it
    (a save blocks the loop for ~1 s, which held the relay that much longer). The gate takes it at once and holds
    the house's retries quietly (not counted as replays), then saves and ACKs. The invariant checks measure the
    press at the simulator, so a stretched pulse fails them. K2 with the gate closed: nothing moves."""
    pid = _meta(rig.gate)["retries"]["id"]
    old = rig.gate.config_get()["retries"]
    new = old - 1 if old > 0 else 1
    replays = rig.gate.status()["link"]["replay"]
    rig.saved.add("gate")
    try:
        m = rig.mark()
        rig.relay_test("gate", 2, HOLD_PULSE_MS)
        pulse = rig.wait_log("gate", "pulse", a=2, since=m, timeout=2)
        ev, _ = _remote_set(rig, "retries", new)
        assert ev["acked"] and ev["ok"], f"remote.set retries={new} during a pulse not applied: {ev}"
        saved = rig.wait_log("gate", "cfg_remote", a=pid, b=new, since=m, timeout=2)
        assert saved["t"] >= pulse["t"] + HOLD_PULSE_MS / 1000 - 0.1, \
            f"saved {saved['t'] - pulse['t']:.2f} s into a {HOLD_PULSE_MS} ms pulse"
        assert rig.gate.config_get()["retries"] == new
        assert rig.gate.status()["link"]["replay"] == replays, "the held retries were counted as replays"
        ev, _ = _remote_set(rig, "retries", old)
        assert ev["acked"] and ev["ok"], f"remote.set retries={old} (restore) not applied: {ev}"
    finally:
        rig.resave("gate")


def test_console_save_waits_for_pulse(rig):
    """config.save on the gate's console during a relay pulse is answered only once the pulse is over, so the
    save doesn't hold the relay on. A short pulse, so a save that didn't wait would still be running when it should
    end (on 0.13.1 a 500 ms press measured 603 ms at the simulator, which the invariant checks fail)."""
    ms = 500
    m = rig.mark()
    rig.relay_test("gate", 2, ms)
    pulse = rig.wait_log("gate", "pulse", a=2, since=m, timeout=2)
    rig.saved.add("gate")
    try:
        rig.gate.request("config.save", timeout=ms / 1000 + 3)
        done = rig.mark()
        assert done >= pulse["t"] + ms / 1000 - 0.1, f"config.save answered {done - pulse['t']:.2f} s into a {ms} ms pulse"
    finally:
        rig.resave("gate")


def test_remote_diag(rig):
    """remote.diag: the gate answers with its firmware, uptime, link counters and remote-writable params."""
    meta = _meta(rig.gate)
    cfg = rig.gate.config_get()
    fw = rig.gate.info()["fw"]
    diag = None
    for _ in range(3):  # DIAG_REQ and DIAG aren't acknowledged: a lost frame is retried here
        m = rig.mark()
        rig.house.request("remote.diag")
        try:
            diag = rig.wait_for(lambda m=m: rig.timeline.first("house", "remote_diag", m), 5, "remote_diag", poll=0.05)
            break
        except AssertionError:
            continue
    if diag is None:
        rig.fail("no remote_diag from the gate after 3 requests")
    uptime_s = rig.gate.status()["uptime_ms"] / 1000
    assert diag["fw"] == fw, f"diag fw {diag['fw']}, gate reports {fw}"
    assert uptime_s - 5 <= diag["uptime_s"] <= uptime_s, f"diag uptime {diag['uptime_s']} s, gate {uptime_s:.0f} s"
    assert set(diag["counters"]) == DIAG_COUNTERS, f"diag counters {sorted(diag['counters'])}"
    assert diag["counters"]["tx"] > 0 and diag["counters"]["rx"] > 0
    remote = {n for n, p in meta.items() if p["remote"]}
    assert diag["params"] and set(diag["params"]) <= remote, f"diag params {sorted(diag['params'])}"
    wrong = {n: (v, cfg[n]) for n, v in diag["params"].items() if v != cfg[n]}
    assert not wrong, f"diag params differ from the gate's config (diag, config): {wrong}"


def test_slow_gate_heartbeat_stretches_link_timeout(rig):
    """The gate reports its heartbeat_s in STATUS; the house waits max(link_timeout_s, 2.5 heartbeats) before
    declaring the link down, so a gate heartbeat longer than the house's link_timeout_s doesn't flap the link."""
    hb = 20
    house_timeout = PROFILE_COMMON["link_timeout_s"]
    eff = max(house_timeout, math.ceil(2.5 * hb))
    m = rig.mark()
    try:
        rig.gate.config_set(heartbeat_s=hb)
        # The next STATUS (at most one old heartbeat plus the new one away) carries it.
        rig.wait_house(hb + PROFILE_COMMON["heartbeat_s"] + 5, f"house sees gate heartbeat_s {hb}",
                       remote__heartbeat_s=hb, link_timeout_eff_s=eff)
        # A whole heartbeat gap, longer than the house's own link_timeout_s, with nothing else on the air.
        rig.expect_no("house", "link_down", seconds=hb + 5, since=m)
        assert rig.house.status()["link_up"]
    finally:
        rig.gate.config_set(**rig.profile["gate"])
    rig.wait_house(hb + 10, "house back to the profile's heartbeat", link_up=True,
                   remote__heartbeat_s=PROFILE_COMMON["heartbeat_s"], link_timeout_eff_s=house_timeout)


def test_console_line_limit(bench):
    """A line over the console's 1023 chars is answered `line too long` (with the id if the firmware finds it in
    what it kept; Board hands an id-less reply to the one waiting request); one just under the limit is still
    parsed. The board keeps answering afterwards."""
    res = bench.house.request("info", check=False, pad="x" * 1100)
    assert not res["ok"] and res.get("error") == "line too long", f"overlong line: {res}"
    res = bench.house.request("info", check=False, pad="x" * 950)  # about 1000 chars with the id and cmd
    assert res["ok"] and res["role"] == "house", f"line under the limit: {res}"
    assert bench.house.info()["role"] == "house"


def test_relay_test_out_of_range_is_refused(rig):
    """relay.test checks its arguments before narrowing them: k 257 or -255 (K1 as a byte), a negative ms and one
    past int32 (which ArduinoJson's `| default` turned into the default 500) are refused, and nothing pulses (up to
    0.13.4, k 257 and ms 2^32 + 500 pulsed K1)."""
    m = rig.mark()
    accepted = []
    for kw in ({"k": 257}, {"k": 258}, {"k": -255}, {"k": 1, "ms": -1}, {"k": 1, "ms": 2**32 + 500}):
        res = rig.gate.request("relay.test", check=False, **kw)
        if res.get("ok"):
            accepted.append(kw)
    assert not accepted, f"relay.test accepted {accepted}"
    rig.expect_no("gate", "pulse", seconds=1, since=m)
