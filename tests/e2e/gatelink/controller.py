"""The house-side controller (on the bench, a Shelly Wave 1 switched through Home Assistant).

Configured from the environment so no address or token lands in the repo:
  GATELINK_HA_URL         e.g. https://homeassistant.local:8123
  GATELINK_HA_ENTITY      default switch.wave_1
  GATELINK_HA_TOKEN_FILE  default ~/.ha_token (long-lived access token)
The controller's real relay level is read from the house board (status `ctrl` = IN1), not from HA, which
reports Z-Wave state with its own delay.
"""
import json
import os
import ssl
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
