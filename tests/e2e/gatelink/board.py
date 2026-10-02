"""Client for a GateLink board's USB JSON console (the same contract web/app.js uses).

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
        self._reader = None
        self._closing = False
        self.dropped = 0  # times the port vanished without us asking for a reboot

    # --- connection -------------------------------------------------------------------------------------------
    def open(self):
        self._closing = False
        self.ser = serial.Serial(self.port, 115200, timeout=0.1, write_timeout=2)
        self._reader = threading.Thread(target=self._read_loop, name=f"board-{self.name}", daemon=True)
        self._reader.start()

    def close(self):
        self._closing = True
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
        if self._reader:
            self._reader.join(timeout=2)
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

    def _handle_line(self, line):
        if not line:
            return
        try:
            msg = json.loads(line)
        except ValueError:
            self.timeline.add(self.name, "raw", line=line)
            return
        ev = msg.get("event")
        if ev == "log":
            self.timeline.add(self.name, "log", ev=msg.get("ev"), a=msg.get("a"), b=msg.get("b"), bt=msg.get("t"))
        elif ev == "status":
            s = msg.get("status", {})
            io = s.get("io", {})
            self.timeline.add(self.name, "status", gate=s.get("gate"), cause=s.get("cause"),
                              result=s.get("last_result"), target=s.get("target"),
                              k1=io.get("k1"), k2=io.get("k2"), ctrl=s.get("ctrl"))
        elif ev:
            self.timeline.add(self.name, ev, **{k: v for k, v in msg.items() if k != "event"})
        elif "id" in msg:
            with self._cond:
                self._replies[msg["id"]] = msg
                self._cond.notify_all()

    # --- requests ---------------------------------------------------------------------------------------------
    def request(self, cmd, timeout=3.0, check=True, **kw):
        if self.ser is None or not self._reader.is_alive():
            self._reconnect()
        rid = next(self._ids)
        data = (json.dumps({"id": rid, "cmd": cmd, **kw}) + "\n").encode()
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
        if check and not res.get("ok"):
            raise BoardError(f"{self.name}: {cmd} failed: {res}")
        return res

    def _reconnect(self, timeout=20.0):
        """Reopen the same port after the board reset (USB re-enumerates; Windows keeps the COM number)."""
        self.close()
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                self.ser = serial.Serial(self.port, 115200, timeout=0.1, write_timeout=2)
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

    def status(self):
        return self.request("status")["status"]

    def config_get(self):
        return self.request("config.get")["params"]

    def config_set(self, **params):
        """Apply (not save) params. config.set skips unchanged values, so this is cheap to repeat."""
        res = self.request("config.set", params=params, check=False)
        if not res.get("ok"):
            raise BoardError(f"{self.name}: config.set {params} rejected: {res.get('errors')}")
        if res.get("applied"):
            self.timeline.add("test", "action", text=f"{self.name} config.set {params}")
        return res

    def log_get(self):
        return self.request("log.get")

    def reboot(self):
        self.timeline.add("test", "action", text=f"{self.name} reboot")
        self.request("reboot")
        self.close()
        time.sleep(1.5)
        self._reconnect()


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
            raise BoardError(f"two boards report role {role!r} ({found[role].port}, {p.device})")
        b.name = role
        found[role] = b
    return found
