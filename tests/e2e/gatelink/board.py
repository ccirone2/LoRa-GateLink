"""Client for a GateLink board's JSON console over USB (the same contract web/app.js uses), plus an optional
read-only tap on its UART console (`uart_console`, a USB-to-UART adapter on its Serial1 pins).

Requests only ever go over USB: on the bench the UART garbled ~1 % of requests on their way into the board, some
into still-valid JSON with a digit changed, so nothing may be sent that way. Its board-to-PC direction was clean,
and the adapter keeps its port while the board is unpowered, so the tap records the board's events (above all
`boot` the moment power returns, when USB hasn't re-enumerated yet).

Requests are `{"id","cmd",...}` lines answered by `{"id","ok",...}`; unsolicited `{"event":...}` lines (log
entries, house status updates, pongs) go to the shared timeline. The firmware only writes while DTR is asserted,
which pyserial does by default.
"""
import itertools
import json
import threading
import time

import serial
import serial.tools.list_ports

ARDUINO_VID = 0x2341
FTDI_VID = 0x0403  # USB-to-UART adapters on the boards' Serial1 consoles (not the GateSim's CH340)
# USB CDC ignores the rate; a USB-to-UART adapter on the board's Serial1 console (uart_console) needs it.
BAUD = 250_000
# config.set params per request: keeps each line far below the firmware's console line limit.
CONFIG_CHUNK = 8
# Checked against each other up to firmware 0.3.4 (heartbeat_s <= link_timeout_s / 2): keep them in one request.
LINKED_PARAMS = ("heartbeat_s", "link_timeout_s")


class BoardError(Exception):
    pass


class Board:
    def __init__(self, port, timeline, name=None):
        self.port = port
        self.name = name or port
        self.timeline = timeline
        self.ser = None
        self._ids = itertools.count(1)
        self._lock = threading.Lock()
        self._cond = threading.Condition()
        self._replies = {}
        self._waiting = set()  # ids of requests still waiting for their reply
        self._reader = None
        self._closing = False
        self.dropped = 0  # times the port vanished without us asking for a reboot
        self.uart = None  # UartTap, if the board's UART console is tapped
        # Every event goes out on both consoles: recent event lines, so the second copy is dropped.
        self._recent = {}
        self._recent_lock = threading.Lock()

    # --- connection -------------------------------------------------------------------------------------------
    def open(self):
        self._closing = False
        self.ser = serial.Serial(self.port, BAUD, timeout=0.1, write_timeout=2)
        self._reader = threading.Thread(target=self._read_loop, name=f"board-{self.name}", daemon=True)
        self._reader.start()

    def close(self):
        # Let the reader leave its read (0.1 s timeout) before the port closes under it: closing a port another
        # thread is reading crashed Python on Windows (access violation in pyserial on the next open).
        self._closing = True
        if self._reader and self._reader is not threading.current_thread():
            self._reader.join(timeout=2)
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
        self.ser = None

    def _read_loop(self):
        buf = b""
        while not self._closing:
            try:
                chunk = self.ser.read(256)
            except (serial.SerialException, OSError, TypeError, AttributeError):
                if not self._closing:
                    self.dropped += 1
                    self.timeline.add(self.name, "note", text="serial port dropped")
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._handle_line(line.decode("utf-8", "replace").strip())

    def _fresh(self, line):
        """False if this exact event line was recorded in the last 0.5 s (the other console's copy). Copies
        arrive within milliseconds of each other; the same event from another boot is seconds apart."""
        now = time.monotonic()
        with self._recent_lock:
            if len(self._recent) > 64:
                self._recent = {k: t for k, t in self._recent.items() if now - t < 0.5}
            if now - self._recent.get(line, -1.0) < 0.5:
                return False
            self._recent[line] = now
            return True

    def _handle_line(self, line, via="usb"):
        if not line:
            return
        try:
            msg = json.loads(line)
        except ValueError:
            msg = None
        if via == "uart":
            # Only events are taken from the tap (no requests go that way); junk is kept for power tests.
            if isinstance(msg, dict) and msg.get("event"):
                if self._fresh(line):
                    self._record_event(msg)
            elif msg is None:
                self.timeline.add(self.name, "raw", line=line, via="uart")
            return
        if msg is None:
            # A line cut short by a USB stall (no newline; firmware before 0.4.1) runs into the next one: keep the
            # whole line that follows it.
            start = max(line.rfind('{"id":'), line.rfind('{"event":'))
            if start > 0:
                self.timeline.add(self.name, "raw", line=line[:start])
                self._handle_line(line[start:])
                return
        if not isinstance(msg, dict):
            self.timeline.add(self.name, "raw", line=line)
            return
        if msg.get("event"):
            if self._fresh(line):
                self._record_event(msg)
            return
        if "id" in msg:
            with self._cond:
                if msg["id"] in self._waiting:
                    self._replies[msg["id"]] = msg
                    self._cond.notify_all()
                    return
            # Its request already timed out: drop it, or it would sit in _replies forever.
            self.timeline.add(self.name, "note", text=f"late reply dropped: {line}")
        elif "ok" in msg:
            # The firmware couldn't read the request (`bad json`, `line too long`), so the reply has no id. With
            # exactly one request waiting it can only be that one's: hand it over rather than let it time out.
            self.timeline.add(self.name, "note", text=f"reply without id: {line}")
            with self._cond:
                if len(self._waiting) == 1:
                    self._replies[next(iter(self._waiting))] = msg
                    self._cond.notify_all()

    def _record_event(self, msg):
        ev = msg.get("event")
        if ev == "log":
            self.timeline.add(self.name, "log", ev=msg.get("ev"), a=msg.get("a"), b=msg.get("b"), bt=msg.get("t"))
        elif ev == "status":
            s = msg.get("status", {})
            io = s.get("io", {})
            self.timeline.add(self.name, "status", gate=s.get("gate"), cause=s.get("cause"),
                              result=s.get("last_result"), target=s.get("target"),
                              k1=io.get("k1"), k2=io.get("k2"), ctrl=s.get("ctrl"))
        else:
            self.timeline.add(self.name, ev, **{k: v for k, v in msg.items() if k != "event"})

    # --- requests ---------------------------------------------------------------------------------------------
    def request(self, cmd, timeout=3.0, check=True, **kw):
        # A request the board couldn't parse ("bad json") never ran, so sending it again is safe.
        for attempt in range(3):
            res = self._request_once(cmd, timeout, **kw)
            if res.get("error") != "bad json":
                break
            self.timeline.add(self.name, "note", text=f"{cmd}: bad json, resent")
        if check and not res.get("ok"):
            raise BoardError(f"{self.name}: {cmd} failed: {res}")
        return res

    def _request_once(self, cmd, timeout, **kw):
        if self.ser is None or not self._reader.is_alive():
            self._reconnect()
        rid = next(self._ids)
        data = (json.dumps({"id": rid, "cmd": cmd, **kw}) + "\n").encode()
        with self._cond:
            self._waiting.add(rid)
        try:
            with self._lock:
                try:
                    self.ser.write(data)
                except (serial.SerialException, OSError) as e:
                    raise BoardError(f"{self.name}: write failed: {e}") from e
            deadline = time.monotonic() + timeout
            with self._cond:
                while rid not in self._replies:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        raise BoardError(f"{self.name}: no reply to {cmd!r} within {timeout}s")
                    self._cond.wait(left)
                res = self._replies.pop(rid)
        finally:
            with self._cond:
                self._waiting.discard(rid)
                self._replies.pop(rid, None)
        return res

    def _reconnect(self, timeout=20.0):
        """Reopen the same port after the board reset (USB re-enumerates; Windows keeps the COM number)."""
        self.close()
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                self.ser = serial.Serial(self.port, BAUD, timeout=0.1, write_timeout=2)
            except serial.SerialException as e:
                last = e
                time.sleep(0.5)
                continue
            self._closing = False
            self._reader = threading.Thread(target=self._read_loop, name=f"board-{self.name}", daemon=True)
            self._reader.start()
            try:
                self.request("info", timeout=1.5)
                self.timeline.add(self.name, "note", text="reconnected")
                return
            except BoardError as e:
                last = e
                self.close()
                time.sleep(0.5)
        raise BoardError(f"{self.name}: did not come back on {self.port} within {timeout}s ({last})")

    def info(self):
        return self.request("info")

    def status(self, timeout=3.0):
        return self.request("status", timeout=timeout)["status"]

    def config_get(self):
        return self.request("config.get")["params"]

    def config_set(self, **params):
        """Apply (not save) params. config.set skips unchanged values, so this is cheap to repeat.

        Sent CONFIG_CHUNK params per request, the linked ones together in the first. Returns the names applied."""
        keys = [k for k in LINKED_PARAMS if k in params] + [k for k in params if k not in LINKED_PARAMS]
        applied = []
        for i in range(0, len(keys), CONFIG_CHUNK):
            chunk = {k: params[k] for k in keys[i:i + CONFIG_CHUNK]}
            res = self.request("config.set", params=chunk, check=False)
            if not res.get("ok"):
                raise BoardError(f"{self.name}: config.set {chunk} rejected: {res.get('errors') or res.get('error')}")
            if res.get("applied"):
                self.timeline.add("test", "action", text=f"{self.name} config.set {chunk}")
                applied += res["applied"]
        return applied

    def log_get(self):
        return self.request("log.get")

    def history(self):
        """The link history (hist.get, every page), oldest bucket first, each a dict keyed by the reply's `fields`.

        Returns (buckets, header): header has `period_s`, `now_s` (seconds since the history started), `oldest` and
        `current` from the last page."""
        buckets, frm = [], None
        while True:
            res = self.request("hist.get", **({} if frm is None else {"from": frm}))
            buckets += [dict(zip(res["fields"], r)) for r in res["rows"]]
            # A bucket that closes while paging moves `current` on: keep going until the last page reaches it.
            if not res["rows"] or res["rows"][-1][0] >= res["current"]:
                return buckets, {k: res[k] for k in ("period_s", "now_s", "oldest", "current")}
            frm = res["rows"][-1][0] + 1

    def reboot(self):
        self.timeline.add("test", "action", text=f"{self.name} reboot")
        self.request("reboot")
        self.close()
        time.sleep(1.5)
        self._reconnect()


class UartTap:
    """Read-only listener on a board's UART console: its events go to the board's timeline (each event only once,
    whichever console delivers it first). Nothing is ever sent, see the module docstring."""

    def __init__(self, board, port):
        self.board, self.port = board, port
        self.ser = None
        self._reader = None
        self._closing = False

    def open(self):
        self.ser = serial.Serial(self.port, BAUD, timeout=0.1)
        self._closing = False
        self._reader = threading.Thread(target=self._read_loop, name=f"uart-{self.board.name}", daemon=True)
        self._reader.start()
        self.board.uart = self

    def close(self):
        self._closing = True
        if self._reader and self._reader is not threading.current_thread():
            self._reader.join(timeout=2)
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass

    def _read_loop(self):
        buf = b""
        while not self._closing:
            try:
                buf += self.ser.read(256)
            except (serial.SerialException, OSError, TypeError, AttributeError):
                return
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.board._handle_line(line.decode("utf-8", "replace").strip(), via="uart")


def find_uarts(timeline, exclude=()):
    """Which board each FTDI port not in `exclude` taps (the board needs uart_console on). The only request ever
    sent on a UART is this `info`, retried: a garbled copy can't turn into a command that does anything, and the
    reply direction is clean. Returns {role: port}."""
    found = {}
    for p in serial.tools.list_ports.comports():
        if p.vid != FTDI_VID or p.device in exclude:
            continue
        b = Board(p.device, timeline)
        try:
            b.open()
            for _ in range(5):
                try:
                    res = b.request("info", timeout=1.0, check=False)
                except BoardError:
                    continue
                if res.get("role"):
                    found.setdefault(res["role"], p.device)
                    break
        except serial.SerialException:
            pass
        finally:
            b.close()
    return found


def find_boards(timeline, exclude=()):
    """Open every Arduino-VID port not in `exclude` and ask its role. Returns {role: Board}."""
    found = {}
    for p in serial.tools.list_ports.comports():
        if p.vid != ARDUINO_VID or p.device in exclude:
            continue
        b = Board(p.device, timeline)
        try:
            b.open()
            role = b.request("info", timeout=2)["role"]
        except (serial.SerialException, BoardError):
            b.close()
            continue
        if role in found:
            b.close()
            for other in found.values():
                other.close()
            raise BoardError(f"two boards report role {role!r} ({found[role].port}, {p.device})")
        b.name = role
        found[role] = b
    return found
