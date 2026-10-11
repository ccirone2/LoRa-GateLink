"""Client for a GateLink board's JSON console over USB (the same contract the web console uses), plus an optional
read-only tap on its UART console (`uart_console`, a USB-to-UART adapter on its Serial1 pins).

Requests go over USB while it's up. The adapter keeps its port while the board is unpowered, so the tap records the
board's events (above all `boot` the moment power returns, when USB hasn't re-enumerated yet), and while USB is
down (a power cut; Windows sometimes loses the port until the hub is replugged) requests go over the UART instead.
On the bench ~1 % of requests arrived garbled over the UART, some into still-valid JSON with a number
changed, so every request ends with a CRC-32 of itself (`with_crc`; firmware 0.12.0 refuses a UART request without a
matching one and checks it on USB too). A refused request never ran, so it is sent again; one that went unanswered
is sent again only if that is harmless (RESEND_SAFE). The board-to-PC direction was clean.

Requests are `{"id","cmd",...}` lines answered by `{"id","ok",...}`; unsolicited `{"event":...}` lines (log
entries, house status updates, pongs) go to the shared timeline. The firmware only writes while DTR is asserted,
which pyserial does by default.
"""
import itertools
import json
import threading
import time
import zlib

import serial
import serial.tools.list_ports

ARDUINO_VID = 0x2341
FTDI_VID = 0x0403  # USB-to-UART adapters on the boards' Serial1 consoles (not the GateSim's CH340)
# Requests that change nothing on the board, and the ones that change nothing more when repeated: an unanswered one
# may be sent again over the UART (see the module docstring).
READ_ONLY = ("status", "info", "log.get", "hist.get", "config.get")
RESEND_SAFE = READ_ONLY + ("config.set", "config.save", "key.set", "identify")
# Errors for requests the board refused unread: they never ran, so sending them again is safe.
REFUSED = ("bad json", "bad crc", "crc required")  # garbling can take the crc member with it
# USB CDC ignores the rate; a USB-to-UART adapter on the board's Serial1 console (uart_console) needs it.
BAUD = 250_000
# config.set params per request: keeps each line far below the firmware's console line limit.
CONFIG_CHUNK = 8
# Checked against each other up to firmware 0.3.4 (heartbeat_s <= link_timeout_s / 2): keep them in one request.
LINKED_PARAMS = ("heartbeat_s", "link_timeout_s")


class BoardError(Exception):
    pass


def with_crc(rid, cmd, **kw):
    """A request line ending with a CRC-32 of itself: the JSON without the `crc` member, then `,"crc":"<hex>"}`."""
    body = json.dumps({"id": rid, "cmd": cmd, **kw})
    return (body[:-1] + ',"crc":"%08x"}\n' % zlib.crc32(body.encode())).encode()


OPEN_TIMEOUT_S = 8.0


def open_serial(port, **kw):
    """serial.Serial(port, **kw), giving up after OPEN_TIMEOUT_S. Windows sometimes wedges USB serial ports (on the
    bench after uploads, every port at once until the USB hub was replugged) and the open then blocks for minutes.
    Raises serial.SerialException like a failed open, so callers treat a stuck port as an unavailable one."""
    box = {}
    lock = threading.Lock()

    def work():
        try:
            ser = serial.Serial(port, **kw)
        except BaseException as e:  # noqa: BLE001 - handed to the caller
            box["err"] = e
            return
        with lock:
            if box.get("gave_up"):
                ser.close()  # opened after the caller stopped waiting: don't leave it held
            else:
                box["ser"] = ser

    t = threading.Thread(target=work, name=f"open-{port}", daemon=True)
    t.start()
    t.join(OPEN_TIMEOUT_S)
    with lock:
        if "ser" not in box and "err" not in box:
            box["gave_up"] = True
            raise serial.SerialException(f"could not open port {port!r}: no answer within {OPEN_TIMEOUT_S:g} s "
                                         "(port stuck? replug the board or the USB hub)")
    if "err" in box:
        raise box["err"]
    return box["ser"]


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
        self.ser = open_serial(self.port, baudrate=BAUD, timeout=0.1, write_timeout=2)
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
    def usb_up(self):
        return self.ser is not None and self._reader is not None and self._reader.is_alive()

    def _usb_back(self):
        """One quick try to reopen USB, only if Windows lists the port again."""
        if not any(p.device == self.port for p in serial.tools.list_ports.comports()):
            return False
        try:
            self._reconnect(timeout=1.0)
            return True
        except BoardError:
            return False

    def request(self, cmd, timeout=3.0, check=True, **kw):
        if not self.usb_up() and self.uart is not None and not self._usb_back():
            res = self.uart.request(cmd, timeout=timeout, **kw)
            self.timeline.add(self.name, "note", text=f"{cmd} over the UART (USB down)")
            if check and not res.get("ok"):
                raise BoardError(f"{self.name}: {cmd} failed: {res}")
            return res
        # A request the board refused unread (REFUSED) never ran, so sending it again is safe.
        for _ in range(3):
            res = self._request_once(cmd, timeout, **kw)
            if res.get("error") not in REFUSED:
                break
            self.timeline.add(self.name, "note", text=f"{cmd}: {res['error']}, resent")
        if check and not res.get("ok"):
            raise BoardError(f"{self.name}: {cmd} failed: {res}")
        return res

    def _request_once(self, cmd, timeout, **kw):
        if self.ser is None or not self._reader.is_alive():
            self._reconnect()
        rid = next(self._ids)
        data = with_crc(rid, cmd, **kw)
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
                self.ser = open_serial(self.port, baudrate=BAUD, timeout=0.1, write_timeout=2)
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
        hint = ""
        if self.uart is not None and self.uart.heard_within(10):
            hint = "; its UART still hears it, so the board is up: Windows may have lost the port (replug the USB hub)"
        raise BoardError(f"{self.name}: did not come back on {self.port} within {timeout}s ({last}){hint}")

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

        Returns (buckets, header): header has `period_s`, `now_s` (seconds since the history started, counted on
        across resets), `oldest`, `current` and `persist` (kept in the board's flash; None before firmware 0.14.0) from
        the last page."""
        buckets, frm = [], None
        while True:
            res = self.request("hist.get", **({} if frm is None else {"from": frm}))
            buckets += [dict(zip(res["fields"], r, strict=True)) for r in res["rows"]]
            # A bucket that closes while paging moves `current` on: keep going until the last page reaches it.
            if not res["rows"] or res["rows"][-1][0] >= res["current"]:
                return buckets, {k: res.get(k) for k in ("period_s", "now_s", "oldest", "current", "persist")}
            frm = res["rows"][-1][0] + 1

    def request_reboot(self):
        """Ask the board to reset. It replies first and resets ~100 ms later, but the reply doesn't always reach us
        before USB drops, so a missing reply is fine."""
        self.timeline.add("test", "action", text=f"{self.name} reboot")
        try:
            self.request("reboot", timeout=1.5)
        except BoardError:
            pass

    def reboot(self):
        self.request_reboot()
        self.close()
        time.sleep(1.5)
        self._reconnect()


class UartTap:
    """Listener on a board's UART console: its events go to the board's timeline (each event only once, whichever
    console delivers it first). It sends only READ_ONLY requests, and only while the board's USB is down (see the
    module docstring)."""

    def __init__(self, board, port):
        self.board, self.port = board, port
        self.ser = None
        self._reader = None
        self._closing = False
        self._ids = itertools.count(1_000_000)  # apart from the USB ids, for the timeline's sake
        self._lock = threading.Lock()
        self._cond = threading.Condition()
        self._replies = {}
        self._waiting = set()
        self.last_rx = 0.0  # monotonic time of the last line heard

    def heard_within(self, seconds):
        return time.monotonic() - self.last_rx < seconds

    def request(self, cmd, timeout=3.0, tries=5, **kw):
        """A request over the UART, with its CRC. Sent again when the board refused it unread (REFUSED), and when it
        went unanswered (garbling can take the newline or the id with it) if that is harmless
        (RESEND_SAFE); otherwise an unanswered one raises BoardError, as it may have run."""
        for _ in range(tries):
            rid = next(self._ids)
            with self._cond:
                self._waiting.add(rid)
            try:
                with self._lock:
                    self.ser.write(with_crc(rid, cmd, **kw))
                deadline = time.monotonic() + timeout
                with self._cond:
                    while rid not in self._replies and time.monotonic() < deadline:
                        self._cond.wait(deadline - time.monotonic())
                    res = self._replies.pop(rid, None)
            except (serial.SerialException, OSError) as e:
                raise BoardError(f"{self.board.name}: UART write failed: {e}") from e
            finally:
                with self._cond:
                    self._waiting.discard(rid)
            if res is not None and res.get("error") not in REFUSED:
                return res
            if res is None and cmd not in RESEND_SAFE:
                raise BoardError(f"{self.board.name}: no reply over the UART to {cmd!r} (not resent: it may have run)")
        raise BoardError(f"{self.board.name}: no reply over the UART to {cmd!r} in {tries} tries")

    def _handle(self, line):
        self.last_rx = time.monotonic()
        if line.startswith('{"id"') or line.startswith('{"ok"'):
            try:
                msg = json.loads(line)
            except ValueError:
                msg = None
            if isinstance(msg, dict) and "event" not in msg:
                with self._cond:
                    if msg.get("id") in self._waiting:
                        self._replies[msg["id"]] = msg
                    elif "id" not in msg and len(self._waiting) == 1:
                        self._replies[next(iter(self._waiting))] = msg  # `bad json`: no id to match
                    self._cond.notify_all()
                return  # a reply (or a stray one under a garbled id): not for the timeline
        self.board._handle_line(line, via="uart")

    def open(self):
        self.ser = open_serial(self.port, baudrate=BAUD, timeout=0.1)
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
                line = line.decode("utf-8", "replace").strip()
                if line:
                    self._handle(line)


def find_uarts(timeline, exclude=()):
    """Which board each FTDI port not in `exclude` taps (the board needs uart_console on): asks `info`, retried, as a
    damaged copy is refused (`bad crc`) or goes unanswered. Returns {role: port}."""
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


def board_ports(exclude=()):
    """The Arduino-VID serial ports (GateLink boards, or another Arduino) not in `exclude`, sorted."""
    return sorted(p.device for p in serial.tools.list_ports.comports()
                  if p.vid == ARDUINO_VID and p.device not in exclude)


def open_board(port, timeline, timeout=3.0):
    """Open the board on `port` and name it by the role it reports. Raises serial.SerialException or BoardError
    (closed again) if the port won't open or nothing answers `info`."""
    b = Board(port, timeline)
    b.open()
    try:
        b.name = b.request("info", timeout=timeout)["role"]
    except BaseException:
        b.close()
        raise
    return b


def find_boards(timeline, exclude=()):
    """Open every Arduino port not in `exclude` and ask its role; ports that don't answer are skipped. Returns
    {role: Board}; raises BoardError (all closed) if two boards report the same role."""
    found = {}
    for port in board_ports(exclude):
        try:
            b = open_board(port, timeline, timeout=2)
        except (serial.SerialException, BoardError):
            continue
        if b.name in found:
            b.close()
            for other in found.values():
                other.close()
            raise BoardError(f"two boards report role {b.name!r} ({found[b.name].port}, {port})")
        found[b.name] = b
    return found
