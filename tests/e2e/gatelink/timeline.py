"""One merged, time-ordered record of everything the bench saw during a run.

Every source (both boards' console events, the GateSim's evt lines, controller switching and the test's own
actions) appends here with a host timestamp, so a failure can print what happened in order and latencies can be
measured across devices.
"""
import json
import threading
import time


class Timeline:
    def __init__(self):
        self._t0 = time.monotonic()
        self._lock = threading.Lock()
        self._entries = []

    def now(self):
        return time.monotonic() - self._t0

    def add(self, src, kind, **data):
        e = {"t": round(self.now(), 3), "src": src, "kind": kind, **data}
        with self._lock:
            self._entries.append(e)
        return e

    def select(self, src=None, kind=None, since=0.0, **match):
        """Entries from `since` on, filtered by source, kind and exact field values (None = any)."""
        with self._lock:
            entries = list(self._entries)
        out = []
        for e in entries:
            if e["t"] < since or (src and e["src"] != src) or (kind and e["kind"] != kind):
                continue
            if any(v is not None and e.get(k) != v for k, v in match.items()):
                continue
            out.append(e)
        return out

    def logs(self, src, ev=None, since=0.0, a=None, b=None):
        return self.select(src=src, kind="log", since=since, ev=ev, a=a, b=b)

    def first(self, src, kind, since=0.0, **match):
        found = self.select(src=src, kind=kind, since=since, **match)
        return found[0] if found else None

    @staticmethod
    def fmt(e):
        rest = {k: v for k, v in e.items() if k not in ("t", "src", "kind")}
        if e["kind"] == "log":
            body = f"{rest.get('ev')} a={rest.get('a')} b={rest.get('b')}"
        elif e["kind"] == "status":
            body = " ".join(f"{k}={v}" for k, v in rest.items())
        elif "line" in rest:
            body = rest["line"]
        elif "text" in rest:
            body = rest["text"]
        else:
            body = json.dumps(rest)
        return f"{e['t']:9.3f}  {e['src']:<5} {e['kind']:<6} {body}"

    def tail(self, n=40, since=0.0):
        return "\n".join(self.fmt(e) for e in self.select(since=since)[-n:])

    def dump(self, path, since=0.0):
        with open(path, "w", encoding="utf-8") as f:
            for e in self.select(since=since):
                f.write(json.dumps(e) + "\n")
