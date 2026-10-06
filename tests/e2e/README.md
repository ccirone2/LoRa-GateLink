# End-to-end tests

A pytest suite drives the real bench end to end: both boards over their USB JSON console, the opener simulator
([GateSim](../../tools/GateSim/README.md)) over serial, and the controller (the bench Shelly) through Home
Assistant. It runs the [bench checklist](../../docs/bench-testing.md), plus failure modes that are hard to test
by hand, and asserts the outcome at every hop. Bench only; there are no unit tests.

## Prerequisites

- Both boards are flashed and configured with role, shared key and `tx_power` of about 5
  (`python tools/gatelink.py restore` after flashing).
- The GateSim is wired to the gate board and runs the current `tools/GateSim` (the pulse-length checks need its
  `evt release` lines), and the web console is disconnected.
- `pip install -r tests/e2e/requirements.txt`.

## Running

```sh
export GATELINK_HA_URL=https://<home-assistant>:8123    # token read from ~/.ha_token (GATELINK_HA_TOKEN_FILE)
export GATELINK_HA_CA=<ca.pem>            # optional: verify HA's certificate (by default it isn't checked)
pytest tests/e2e -v                       # about 30 min; boards found by role, simulator on COM10 (--sim-port)
pytest tests/e2e -m soak --cycles 20      # repeated open/close cycles with latency stats
GATELINK_KEY=<32 hex> pytest tests/e2e -k wrong_key   # wrong-key test, opt-in (rewrites the gate's saved key)
GATELINK_KEY=<32 hex> pytest tests/e2e --restore-key -k boards_and_link   # put the shared key back on both boards
pytest tests/e2e -m longsoak --soak-minutes 120   # hours-long run, outages and opener faults mixed in
pytest tests/e2e -m rf --rf-cycles 5      # marginal link (2 dBm, SF12); see "Real RF" below
```

Other options: `--house-port` / `--gate-port` (default: found by role), `GATELINK_SIM_PORT`,
`GATELINK_HA_ENTITY` (default `switch.wave_1`).

**UART taps.** A board with `uart_console` on and a USB-to-UART adapter on its Serial1 pins (see
[docs/console.md](../../docs/console.md)) is also listened to on the adapter. FTDI ports (VID 0x0403) are asked for
their role (`info`) at the start, or name them with `--house-uart` / `--gate-uart`; `--no-uart` turns the taps
off. Requests always go over USB: on the bench ~1 % of requests sent over the UART arrived garbled, some still
valid JSON with a digit changed. The board-to-PC direction was clean, and the adapter keeps its port while the
board is unpowered, so the tap records events USB can't (`boot` the moment power returns). Each event is recorded
once, whichever console delivers it first. The summary lists the consoles.

`--restore-key` reboots both boards (dropping any unsaved test profile an interrupted run left behind), then
`key.set`s `GATELINK_KEY` on both and waits for the link. Use it if a wrong-key run was killed mid-test and left the
gate with a random key. Keep the key itself somewhere safe (e.g. a password manager): boards can't read it back,
and the web console's config export doesn't include it.

Without the bench connected, every test is skipped. If `test_00_preflight` fails, the scenarios are skipped.

## Optional hardware

- **Real controller power.** Wire house IN2's opto to the Shelly's 12 V and put that supply on an HA smart plug,
  then set `GATELINK_HA_POWER_ENTITY=switch.<plug>`. `test_controller_faults.py` then cuts real power, so the real
  relay-drops-before-opto race is tested (either order passes as long as nothing is commanded). Without it,
  controller power is simulated with house `in2_invert`.
- **Real RF.** `-m rf` runs pings and open/close cycles at `tx_power` 2 and SF12 and reports pings, RSSI/SNR,
  retries and giveups in the summary. Run it with an attenuator in line or the antennas off at the bench, and at
  the install site; also check ping and RSSI from the web console there.

## What it covers

| File | Covers |
|---|---|
| `test_00_preflight.py` | Boards, firmware, key and link. Bench-safe settings. Ping. Simulator wiring. The controller reachable and following K1 (its SW input in follow mode, not edge-toggle) |
| `test_normal.py` | Open and close from the controller, with timing at every hop. Reversal mid-travel. Flip back before the gate leaves its limit. External moves, including one right after our command. `-m soak`: `test_soak_open_close_cycles`, `--cycles` open/close cycles with latency stats |
| `test_opener_faults.py` | AC loss with the opener on battery (limits trusted, commands refused, controller resynced; and mid-travel). Opener power loss at rest and mid-travel. Relay test without power. Jammed gate. Opener ignoring the command (siren/override). Both limits active. Limit chatter |
| `test_link_faults.py` | Link loss and recovery. A command into a dead link (expires, never fires late). Short and 4 s outages covered by retries within `cmd_ttl_s`. A gate move missed during an outage. Replayed frames. Wrong key |
| `test_reboots.py` | Gate reset at rest and mid-travel. Gate reset right after a pulse, before its ACK (`debug.reboot_after_cmd`): one press only, the house drops the command. Gate and opener restarting together (GateSim `restart`): the house never shows not-closed. House reset with the controller wrong, and with the gate open |
| `test_controller_faults.py` | Controller toggled while unpowered. Relay dropping before the power sense. Controller coming back at the wrong level. Rapid toggling |
| `test_options.py` | Gate relay test on a closed gate (our move, house follows) and at the open limit (no target: a local close right after is external). `power_sense` 0 (IN3 ignored), `linkloss_open` 0 (K2 held through an outage), `sensor_invert` 1, `ctrl_sync` 0 |
| `test_remote.py` | `remote.set` over LoRa (applied, saved by the gate, put back), refused for non-remote params, `busy` while one is pending. `remote.diag`. A gate heartbeat longer than the house's `link_timeout_s` (house `link_timeout_eff_s`). The console's `line too long` reply |
| `test_history.py` | Link history: bucket counters against the status totals, levels and the gate's side filled in. Rollover with 60 s buckets, and `hist.get` paging. Link-down seconds and the gate's STATUS retries across an outage |
| `test_soak.py` | `-m longsoak`: open/close cycles, outages, opener power blips, external moves and jams in rotation; no resets or radio faults; counters to `soak_counters.csv` |
| `test_rf.py` | `-m rf`: the full loop over a marginal link (minimum power, SF12) |

## How it works

- **Faster timings.** The suite applies shorter timings to both boards for the run, unsaved: `heartbeat_s` 5,
  `link_timeout_s` 15, `travel_timeout_s` 15, `mismatch_timeout_s` 20, `cmd_ttl_s` 10, and simulator travel 8 s
  (saved in the Uno's EEPROM, put back at the end). House `ctrl_power_sense` is off except where a test turns it
  on (the controller faults, and to switch the controller on without a command). It also pins the settings scenarios rely on at the firmware defaults (`retries`, `debounce_ms`,
  `sync_window_ms`, `resync_ms`, `ctrl_confirm_ms`, `ctrl_settle_ms`, the input inverts, `power_sense`,
  `ctrl_sync`, `sensor_invert`, `linkloss_open`; see `PROFILE_*` in `gatelink/bench.py`). At the end it restores
  every param from `results/<run>/config_backup.json`. Saved config is never written, except by the opt-in
  wrong-key test and `test_remote_set` (the gate saves a remote write): the latter saves the backup again
  afterwards, and so does the end of the session.
- **Simulated faults.** A radio outage is the gate moved to another `net_id`. Controller power is house
  `in2_invert` unless `GATELINK_HA_POWER_ENTITY` is set: with IN2 unwired it fakes power, with IN2's opto on the
  live 12 V (shared with the house board, so it can't be cut) it fakes a loss. The session finds which at the start
  (summary "controller power").
- **Baseline.** Each test starts from the same point: opener powered, gate closed, controller off, house armed
  and in sync.
- **Invariant checks after every test.** OPEN and CLOSE are never pulsed together (the simulator reports any
  overlap). Every gate pulse directly follows a received command for that relay (OPEN = K1, CLOSE = K2) or is a
  relay test the scenario issued; a commanded pulse is `pulse_ms` long, and the simulator measured it at that
  length (a relay test at its requested length), ±80 ms. House K2 never reads closed unless the gate is closed.
  No board resets or radio faults. No MAC failures or replays. The house sent exactly the number of commands the
  scenario expects. The opener saw no press the gate didn't log as a pulse (relay chatter, e.g. while a board
  powers up or down).
- **Results.** `tests/e2e/results/<run>/` holds a time-ordered timeline per test (all four devices, JSONL) and
  `summary.md` (results, latencies, link quality, anomalies such as a gate → house status that needed a retry).

## Changing the suite

The suite parses console replies, log event names/values and status fields (`gatelink/`), so change it together
with `console.cpp`, `log.cpp` and `roles.h`. Scenario timings assume the test profile in `gatelink/bench.py`.
