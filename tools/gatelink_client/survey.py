"""Site survey: how much margin the radio link has, judged from pings (radio.ping, answered by pong events).

Pure functions, no board: `tools/gatelink.py survey` runs them, and web/js/survey.js is the same logic for the web
console's survey card. Both are checked against tests/tools/fixtures/survey-vectors.json, so change them together.
Texts are plain ASCII (a Windows console or redirected output may not take more).

A sample is one ping: {"t", "lost", "rtt_ms", "rssi", "snr", "peer_rssi", "peer_snr", "fei", "noise",
"peer_noise"}. `rssi`/`snr` are this board's reception of the pong ("here"), `peer_rssi`/`peer_snr` the other
board's reception of the ping ("peer"); `noise` is this board's noise floor at the time and `peer_noise` the
other board's (on the house, the gate's from its STATUS; unknown on the gate).
"""

import math

# SX1276 demodulation SNR floor per spreading factor (Semtech SX1276 datasheet), dB: below it a frame is lost.
SNR_FLOOR = {6: -5.0, 7: -7.5, 8: -10.0, 9: -12.5, 10: -15.0, 11: -17.5, 12: -20.0}
GOOD_DB = 15  # the weaker direction's 10th-percentile margin for "good"
FAIR_DB = 10  # ... for "fair"; below it, "marginal"
# More pings unanswered than MAX_LOSS, and at least MIN_LOST of them, is "poor" whatever the margin: one lost ping in
# a short survey is chance (the bench house misses a few % of pongs), a steady loss is not. Above NOTE_LOSS the advice
# says frames are being lost.
MAX_LOSS = 0.05
MIN_LOST = 2
NOTE_LOSS = 0.02
# The SNR reading saturates near +10 dB on a strong link: from here up, RSSI minus the noise floor estimates it better.
SNR_SATURATES_DB = 5
TX_POWER_MIN, TX_POWER_MAX = 2, 20  # tx_power's range in config.cpp PARAMS, dBm
SF_MAX = 12
FEW_PONGS = 20  # fewer answered pings than this: say the result is a rough one
DIRECTIONS = ("here", "peer")
FIELDS = {"here": ("rssi", "snr", "noise"), "peer": ("peer_rssi", "peer_snr", "peer_noise")}


def is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def snr_floor(sf):
    return SNR_FLOOR.get(sf)


def fmt1(x):
    """One decimal, ties rounded up: the same digits as the web console (format() alone rounds 0.25 down)."""
    return f"{math.floor(x * 10 + 0.5) / 10:.1f}"


def other_role(role):
    return {"house": "gate", "gate": "house"}.get(role, "other board")


def dir_label(role, d):
    return f"at the {role if d == 'here' else other_role(role)}"


def sample_margin(sf, snr, rssi, noise):
    """dB above the SF's demodulation floor for one received frame. RSSI minus the noise floor stands in for the
    SNR only when the SNR reads 5 dB or more and the noise floor is known."""
    floor = snr_floor(sf)
    if floor is None or not is_num(snr):
        return None
    by_snr = snr - floor
    if snr < SNR_SATURATES_DB or not is_num(noise) or not is_num(rssi):
        return by_snr
    return max(by_snr, rssi - noise - floor)


def quantile(values, q):
    """Linear interpolation between the closest ranks (numpy's default): q = 0.5 is the median."""
    s = sorted(v for v in values if is_num(v))
    if not s:
        return None
    pos = q * (len(s) - 1)
    lo, hi = math.floor(pos), math.ceil(pos)
    return s[lo] + (s[hi] - s[lo]) * (pos - lo)


def noise_from_status(status):
    """The noise floors a sample is judged against, from a status reply: this board's, and on the house the
    gate's. Returns {"noise", "peer_noise"} (None where unknown)."""
    status = status or {}
    noise = (status.get("link") or {}).get("noise")
    peer = (status.get("remote") or {}).get("noise") if status.get("role") == "house" else None
    return {"noise": noise if is_num(noise) else None, "peer_noise": peer if is_num(peer) else None}


def sample_from_pong(pong, t, noise=None):
    """A survey sample: a pong event (or None: no answer in time) at `t` seconds into the survey, with the noise
    floors then (noise_from_status)."""
    noise = noise or {}
    floors = {"noise": noise.get("noise"), "peer_noise": noise.get("peer_noise")}
    if not pong:
        return {"t": t, "lost": True, **floors}
    out = {"t": t, "lost": False}
    for k in ("rtt_ms", "rssi", "snr", "peer_rssi", "peer_snr", "fei"):
        out[k] = pong.get(k) if is_num(pong.get(k)) else None
    return {**out, **floors}


def summarize_direction(sf, samples, d):
    kr, ks, kn = FIELDS[d]
    got = [s for s in samples if not s.get("lost") and is_num(s.get(ks))]
    rssi = [s.get(kr) for s in got if is_num(s.get(kr))]
    snr = [s[ks] for s in got]
    margin = [m for m in (sample_margin(sf, s[ks], s.get(kr), s.get(kn)) for s in got) if is_num(m)]
    return {"n": len(got), "rssi_min": min(rssi, default=None), "rssi_median": quantile(rssi, 0.5),
            "snr_min": min(snr, default=None), "snr_median": quantile(snr, 0.5),
            "margin_min": min(margin, default=None), "margin_p10": quantile(margin, 0.1)}


def summarize(settings, samples):
    """Both directions, the loss, and the weaker direction (the lower 10th-percentile margin; `here` on a tie)."""
    sent = len(samples)
    lost = sum(1 for s in samples if s.get("lost"))
    out = {"sent": sent, "answered": sent - lost, "lost": lost, "loss": lost / sent if sent else None}
    for d in DIRECTIONS:
        out[d] = summarize_direction(settings.get("sf"), samples, d)
    worst = None
    for d in DIRECTIONS:
        p = out[d]["margin_p10"]
        if p is not None and (worst is None or p < out[worst]["margin_p10"]):
            worst = d
    out["worst"] = worst
    out["worst_p10"] = None if worst is None else out[worst]["margin_p10"]
    return out


def too_lossy(summary):
    """Enough pings went unanswered to call the link poor."""
    return summary["lost"] >= MIN_LOST and summary["loss"] > MAX_LOSS


def verdict(summary):
    """good / fair / marginal from the weaker direction's 10th-percentile margin; poor if too many pings went
    unanswered; None if nothing was sent."""
    if not summary["sent"]:
        return None
    if too_lossy(summary) or summary["worst_p10"] is None:
        return "poor"
    if summary["worst_p10"] >= GOOD_DB:
        return "good"
    if summary["worst_p10"] >= FAIR_DB:
        return "fair"
    return "marginal"


def headline(role, settings, summary, v):
    """One line saying what the verdict rests on."""
    if v is None:
        return "No result: no pings were sent."
    name = v.capitalize()
    margin = None if summary["worst"] is None else (
        f"the weaker direction ({dir_label(role, summary['worst'])}) keeps {fmt1(summary['worst_p10'])} dB above "
        f"the SF{settings.get('sf')} limit in 90 % of pings")
    if too_lossy(summary):
        return (f"{name}: {fmt1(100 * summary['loss'])} % of pings went unanswered (more than {fmt1(100 * MAX_LOSS)} %)"
                + (f", though {margin}." if margin else "."))
    if margin is None:
        return f"{name}: no answer carried a signal report."
    return f"{name}: {margin} (good from {GOOD_DB} dB, fair from {FAIR_DB} dB)."


def advice(settings, summary, v):
    """What to do about it, in plain words."""
    if v is None:
        return ["Run the survey with the link up."]
    tx = settings.get("tx_power") if is_num(settings.get("tx_power")) else None
    sf = settings.get("sf") if is_num(settings.get("sf")) else None
    out = []
    if 0 < summary["answered"] < FEW_PONGS:
        out.append(f"Only {summary['answered']} pings were answered: run the survey longer for a firmer result.")
    if summary["loss"] > NOTE_LOSS:
        out.append("Frames are being lost: check the antennas and their cables, and both boards' noise floor for "
                   "interference.")
    if v == "good":
        room = 0 if tx is None else min(math.floor(summary["worst_p10"] - GOOD_DB), tx - TX_POWER_MIN)
        out.append(f"Plenty of margin. If you like, tx_power ({tx} dBm on this board) could be lowered by up to "
                   f"{room} dB on both boards and still keep {GOOD_DB} dB." if room >= 1
                   else "Plenty of margin: nothing to change.")
    elif v == "fair":
        out.append(f"Usable, with less than {GOOD_DB} dB in hand for rain, foliage and interference. If it is easy, "
                   "mount the antennas higher with a clearer line of sight between the boards, and survey again.")
    else:
        out.append("Mount the antennas higher, clear of metal, walls and the relay shield, with as clear a line of "
                   "sight between the boards as you can get: that gains more than any setting.")
        if tx is not None and tx < TX_POWER_MAX:
            out.append(f"Raise tx_power ({tx} dBm on this board, at most {TX_POWER_MAX}) on both boards, each on a "
                       "solid supply: a weak one can reset the board at full power.")
        if sf is not None and sf < SF_MAX:
            out.append(f"Or raise sf (now {sf}) on BOTH boards, as radio settings must match or the link stops: "
                       "each step adds about 2.5 dB of margin and roughly doubles the airtime.")
        if tx is not None and sf is not None and tx >= TX_POWER_MAX and sf >= SF_MAX:
            out.append("tx_power and sf are at their limits: only antenna placement, or a higher-gain antenna, "
                       "can help.")
        out.append("Then run the survey again.")
    return out


def analyze(settings, samples, role=None):
    """Everything the survey says about these samples: {"summary", "verdict", "headline", "advice"}."""
    summary = summarize(settings, samples)
    v = verdict(summary)
    return {"summary": summary, "verdict": v, "headline": headline(role, settings, summary, v),
            "advice": advice(settings, summary, v)}


def _num(x, unit=""):
    return f"{fmt1(x)}{unit}" if is_num(x) else "-"


def _pair(a, b, unit):
    return f"{_num(a)} / {_num(b)}{unit}"


TABLE_HEAD = ("Direction", "Received", "RSSI min / median", "SNR min / median", "Margin min / 10th pct")


def table_rows(role, summary):
    """The rows of the per-direction table: (label, received, RSSI, SNR, margin)."""
    rows = []
    for d in DIRECTIONS:
        x = summary[d]
        rows.append((dir_label(role, d), str(x["n"]), _pair(x["rssi_min"], x["rssi_median"], " dBm"),
                     _pair(x["snr_min"], x["snr_median"], " dB"), _pair(x["margin_min"], x["margin_p10"], " dB")))
    return rows


def format_text(report):
    """The report as plain text: report = {"time", "board": {"role", "fw", "port"}, "settings", "elapsed_s",
    "stopped", "summary", "headline", "advice"} (what `gatelink.py survey --json` writes, less the samples)."""
    s, st, b = report["summary"], report["settings"], report["board"]
    bw = st.get("bw_hz")
    elapsed = int(report.get("elapsed_s") or 0)
    lines = [
        f"GateLink site survey, {report['time']}",
        f"Board: {b.get('role')}, firmware {b.get('fw')}" + (f", {b['port']}" if b.get("port") else ""),
        f"Radio: SF{st.get('sf')}, {f'{bw / 1000:g} kHz' if is_num(bw) else '?'}, tx_power {st.get('tx_power')} dBm "
        "on this board",
        f"Ran {elapsed // 60}:{elapsed % 60:02d}" + (f" ({report['stopped']})" if report.get("stopped") else "")
        + f": {s['sent']} pings, {s['lost']} unanswered" + (f" ({fmt1(100 * s['loss'])} % loss)" if s["sent"] else ""),
        "",
    ]
    rows = [TABLE_HEAD, *table_rows(b.get("role"), s)]
    widths = [max(len(r[i]) for r in rows) for i in range(len(TABLE_HEAD))]
    lines += ["  ".join(c.ljust(w) for c, w in zip(r, widths, strict=True)).rstrip() for r in rows]
    lines += ["", "Each row is what that board received. Margin: dB above the SF's demodulation limit; 10th pct: 90 % "
              "of", "pings had at least this.", "", report["headline"]]
    lines += [f"- {a}" for a in report["advice"]]
    return "\n".join(lines) + "\n"
