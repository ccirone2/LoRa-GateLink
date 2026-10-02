"""The house-side controller (on the bench, a Shelly Wave 1 switched through Home Assistant).

Configured from the environment so no address or token lands in the repo:
  GATELINK_HA_URL         e.g. https://homeassistant.local:8123
  GATELINK_HA_ENTITY      default switch.wave_1
  GATELINK_HA_TOKEN_FILE  default ~/.ha_token (long-lived access token)
  GATELINK_HA_POWER_ENTITY  optional: an HA switch (e.g. a smart plug) on the controller's supply, with house IN2
                            wired to it (see CtrlPower)
  GATELINK_HA_CA          optional: CA bundle (PEM) to verify HA's certificate with; without it the certificate
                            isn't checked (HA on the LAN with a self-signed certificate)
The controller's real relay level is read from the house board (status `ctrl` = IN1), not from HA, which
reports Z-Wave state with its own delay.
"""
import json
import os
import ssl
import time
import urllib.request


class ControllerError(Exception):
    pass


class Controller:
    def __init__(self, timeline, url=None, entity=None, token_file=None):
        self.timeline = timeline
        self.url = (url or os.environ.get("GATELINK_HA_URL", "")).rstrip("/")
        self.entity = entity or os.environ.get("GATELINK_HA_ENTITY", "switch.wave_1")
        token_file = token_file or os.environ.get("GATELINK_HA_TOKEN_FILE", os.path.expanduser("~/.ha_token"))
        if not self.url:
            raise ControllerError("GATELINK_HA_URL is not set")
        try:
            with open(token_file, encoding="utf-8") as f:
                self._token = f.read().strip()
        except OSError as e:
            raise ControllerError(f"can't read HA token file {token_file}: {e}") from e
        ca = os.environ.get("GATELINK_HA_CA", "")
        if ca:
            try:
                self._ctx = ssl.create_default_context(cafile=ca)
            except (OSError, ssl.SSLError) as e:
                raise ControllerError(f"can't load GATELINK_HA_CA {ca}: {e}") from e
        else:
            self._ctx = ssl._create_unverified_context()  # HA on the LAN with a self-signed certificate

    def _call(self, method, path, body=None):
        req = urllib.request.Request(
            self.url + path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={"Authorization": f"Bearer {self._token}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, context=self._ctx, timeout=10) as r:
                return json.load(r)
        except OSError as e:
            raise ControllerError(f"HA {method} {path}: {e}") from e

    def state(self):
        return self._call("GET", f"/api/states/{self.entity}")["state"]

    def set(self, on):
        self.timeline.add("ctrl", "action", text=f"controller {'ON' if on else 'OFF'}")
        self._call("POST", f"/api/services/switch/{'turn_on' if on else 'turn_off'}", {"entity_id": self.entity})

    def on(self):
        self.set(True)

    def off(self):
        self.set(False)


class CtrlPower:
    """Controller power as house IN2 (ctrl_power_sense) sees it.

    plug: GATELINK_HA_POWER_ENTITY switches the controller's real supply, with the IN2 opto wired to it. Cutting it
          drops the controller's relay too, so the real relay-before-opto race is exercised.
    sim:  IN2 isn't wired, so house in2_invert stands in: 1 makes the open input read powered, 0 unpowered. That's
          only safe because nothing is connected to IN2.
    """

    BOOT_S = 5  # controller boot after its supply returns

    def __init__(self, ctrl, house, entity=None):
        self.ctrl, self.house = ctrl, house
        self.entity = entity if entity is not None else os.environ.get("GATELINK_HA_POWER_ENTITY", "")

    @property
    def real(self):
        return bool(self.entity)

    def set(self, on):
        """Power the controller on or off (sim: only as the house sees it)."""
        if self.real:
            self.ctrl.timeline.add("ctrl", "action", text=f"controller power {'ON' if on else 'OFF'}")
            self.ctrl._call("POST", f"/api/services/switch/{'turn_on' if on else 'turn_off'}", {"entity_id": self.entity})
        else:
            self.house.config_set(in2_invert=1 if on else 0)

    def fake(self, unpowered):
        """Make the house read the controller as unpowered (its edges ignored) while it keeps its supply, so it
        can still be switched. fake(False) puts the normal reading back."""
        if self.real:
            self.house.config_set(in2_invert=1 if unpowered else 0)
        else:
            self.house.config_set(in2_invert=0 if unpowered else 1)

    def ensure_on(self):
        """Plug backend: make sure the supply is on (a failed test may have left it off). Returns True if it was off."""
        if not self.real or self.ctrl._call("GET", f"/api/states/{self.entity}")["state"] == "on":
            return False
        self.set(True)
        time.sleep(self.BOOT_S)
        return True
