"""Client for the GateSim Uno (tools/GateSim): a plain-text serial protocol, one command per line.

The port is opened with DTR (and RTS) off: asserting DTR resets the Uno, which restarts the simulation closed
and powered and makes the gate briefly see `no_power`.
"""
import queue
import re
import threading
import time

import serial


class GateSimError(Exception):
    pass


class GateSim:
    def __init__(self, port, timeline):
        self.port = port
        self.timeline = timeline
        self.ser = None
        self.state = None  # last state from an `evt state` line or `status`
        self._lines = queue.Queue()
        self._closing = False
        self._reader = None

    def open(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = 115200
        s.timeout = 0.1
        s.dtr = False
        s.rts = False
        s.open()
        self.ser = s
        self._closing = False
        self._reader = threading.Thread(target=self._read_loop, name="gatesim", daemon=True)
        self._reader.start()
        try:
            self.status()
        except BaseException:
            self.close()  # e.g. another sketch on the port: don't leave it open
            raise

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
        self._reader = None

    def _read_loop(self):
        buf = b""
        while not self._closing:
            try:
                chunk = self.ser.read(256)
            except (serial.SerialException, OSError, TypeError, AttributeError):
                return
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip()
                if not line:
                    continue
                if line.startswith("evt "):
                    self.timeline.add("sim", "evt", line=line[4:])
                    m = re.match(r"evt state (\S+)", line)
                    if m:
                        self.state = m.group(1)
                self._lines.put(line)

    def _cmd(self, line, done, timeout=2.0):
        """Send a command and return the first reply line for which done(line) is true."""
        while not self._lines.empty():
            self._lines.get_nowait()
        self.timeline.add("test", "action", text=f"sim {line}")
        self.ser.write((line + "\n").encode())
        deadline = time.monotonic() + timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise GateSimError(f"no reply to {line!r}")
            try:
                reply = self._lines.get(timeout=left)
            except queue.Empty:
                continue
            if reply.startswith("err"):
                raise GateSimError(f"{line!r}: {reply}")
            if done(reply):
                return reply

    def _ok(self, line):
        self._cmd(line, lambda r: r == "ok")

    def status(self):
        """Parse `state=closed pos=0% power=1 relays open=0 closed=1 power=1 travel=15s fault=none polarity=high`."""
        line = self._cmd("status", lambda r: r.startswith("state="))
        out = {}
        for key, val in re.findall(r"(\w+)=(\S+)", line.replace("relays ", "relay_")):
            out.setdefault(key, val)  # first `power=` is the simulated power, the second the relay
        self.state = out.get("state")
        out["relay_closed"] = out.pop("closed", None)
        out["pos"] = int(out.get("pos", "0").rstrip("%"))
        out["travel"] = int(out.get("travel", "0").rstrip("s"))
        return out

    def open_gate(self):
        """Local button: an external move as far as GateLink is concerned."""
        self._cmd("open", lambda r: r.startswith("evt cmd"))

    def close_gate(self):
        self._cmd("close", lambda r: r.startswith("evt cmd"))

    def stop(self):
        self._ok("stop")

    def power(self, on):
        self._ok(f"power {'on' if on else 'off'}")

    def fault(self, name):
        self._ok(f"fault {name}")

    def travel(self, seconds):
        self._ok(f"travel {seconds}")

    def ac(self, on):
        """AC mains (gate IN3). Without it the opener carries on on its battery: limits and motion as normal."""
        self._ok(f"ac {'on' if on else 'off'}")

    def battery(self, on):
        """The opener's battery backup. With neither it nor AC the opener is dead (`power(False)` does both)."""
        self._ok(f"battery {'on' if on else 'off'}")

    def restart(self, ms):
        """The opener's controller rebooting: limits off and pulses ignored for `ms`, AC stays."""
        self._ok(f"restart {int(ms)}")

    def supply(self, source):
        """Where the gate board's buck is fed from: "acc" (accessory output: alive while the opener is),
        "psu" (the AC supply), or "none" (only explicit rail cuts). Needs the power rig's D7 relay."""
        self._ok(f"supply {source}")

    def rail(self, site, on):
        """Force a site's supply rail ("gate" or "house") on/off, or None to follow the model."""
        self._ok(f"rail {site} {'auto' if on is None else 'on' if on else 'off'}")

    def cut(self, site, ms):
        """Cut a site's supply rail for `ms`, timed on the Uno."""
        self._ok(f"rail {site} cut {int(ms)}")

    def power_rig_auto(self):
        """Both rails on and following the model (gate supply: explicit cuts only)."""
        self._ok("supply none")
        for site in ("gate", "house"):
            self._ok(f"rail {site} auto")

    def relays(self, steps, gap_s):
        """Force gate input signals in order, e.g. [(2, "off"), (3, "off")] (relay 1-3 = IN1-IN3), gap_s apart.
        Replies are collected at the end: waiting for each takes ~0.1 s, too coarse for the gaps tested."""
        while not self._lines.empty():
            self._lines.get_nowait()
        for i, (n, mode) in enumerate(steps):
            if i:
                time.sleep(gap_s)
            self.timeline.add("test", "action", text=f"sim relay {n} {mode}")
            self.ser.write(f"relay {n} {mode}\n".encode())
        deadline = time.monotonic() + 2
        oks = 0
        while oks < len(steps):
            try:
                reply = self._lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                raise GateSimError(f"no reply to relay steps {steps}") from None
            if reply.startswith("err"):
                raise GateSimError(f"relay steps {steps}: {reply}")
            oks += reply == "ok"

    def relay_auto(self):
        for n in (1, 2, 3):
            self._ok(f"relay {n} auto")
