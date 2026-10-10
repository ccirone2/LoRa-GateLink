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
pytest tests/e2e -v                       # about 30 min; boards found by role, simulator on COM10 (this bench's port: set --sim-port)
pytest tests/e2e -m soak --cycles 20      # repeated open/close cycles with latency stats
GATELINK_KEY=<32 hex> pytest tests/e2e -k wrong_key   # wrong-key test, opt-in (marker needs_key; rewrites the gate's saved key)
GATELINK_KEY=<32 hex> pytest tests/e2e --restore-key -k boards_and_link   # put the shared key back on both boards
pytest tests/e2e -m longsoak --soak-minutes 120   # hours-long run, outages and opener faults mixed in
pytest tests/e2e -m rf --rf-cycles 5      # marginal link (2 dBm, SF12); see "Real RF" below
pytest tests/e2e -m power                 # real power cuts through the power rig, LiPo in or out; see "Power rig" below
```

Other options: `--house-port` / `--gate-port` (default: found by role), `GATELINK_SIM_PORT`,
`GATELINK_HA_ENTITY` (default `switch.wave_1`), `--tx-power <dBm>` (both boards at that power for the run, applied
unsaved with the test profile and put back afterwards, e.g. `--tx-power 17` to check full power on the real
supplies; without it the saved `tx_power` must be 5 or less).

**UART taps.** A board with `uart_console` on and a USB-to-UART adapter on its Serial1 pins (see
[docs/console.md](../../docs/console.md)) is also listened to on the adapter. FTDI ports (VID 0x0403) are asked for
their role (`info`) at the start, or name them with `--house-uart` / `--gate-uart`; `--no-uart` turns the taps
off. The adapter keeps its port while the board is unpowered, so the tap records events USB can't (`boot` the
moment power returns); each event is recorded once, whichever console delivers it first. While a board's USB is
down (a power cut, or Windows losing the port until the hub is replugged), every request goes over its UART
instead. Requests sent over the UART arrive garbled (about 8 % with 1,000 back to back on 0.12.0), some still
valid JSON with a digit changed, so every request carries a CRC-32 of itself and the firmware refuses a UART
request without a matching one (`bad crc`, or `crc required` when garbling took the `crc` member; never run, so
resent). An unanswered request is resent only if repeating it is harmless (reads, `config.set`, `config.save`,
`key.set`, `identify`); otherwise it fails, since it may have run. The board-to-PC direction was
clean. If USB doesn't come back the power tests carry on over the UART; a reconnect that fails says whether the
UART still hears the board. The summary lists the consoles.

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
- **Power rig.** Relays on GateSim D7 and D9 switch the boards' supplies (wiring: `tools/bench-wiring`; commands:
  `tools/GateSim/README.md`): CH1 the gate buck's 24 V feed, CH3 the house 12 V rail (controller, IN2 opto, house
  buck). The LiPos are plugged in or out by hand. CH2 (D8) repeats house K1 to the controller's SW input, so K1's
  own contacts can be sensed on the Uno (D11/D12). `-m power` runs `test_power.py`, which
  first gives each board a 1.5 s cut to find out whether its LiPo is in (the summary's "LiPos" line) and runs
  the tests for that state, skipping the others. LiPo out: a cut takes the board down (the gate holds ~0.6 s on
  its buck, the house ~0.1 s), so those tests check boots and recovery. LiPo in: the board rides through, so
  those check what the site does while its supply is gone (AC loss with the board on the AC supply, the opener
  dead with the board on its accessory output, the controller's relay dropping before its power sense). Each test waits for the board's `boot` on
  its UART tap, its link, and then puts the test profile back; a missing boot points at a fitted LiPo or an
  unwired channel. Pulses that a gate power cut overlapped are exempt from the pulse-length check.
- **Real RF.** `-m rf` runs pings and open/close cycles at `tx_power` 2 and SF12 and reports pings, RSSI/SNR,
  retries and giveups in the summary. Run it with an attenuator in line or the antennas off at the bench, and at
  the install site; also check ping and RSSI from the web console there.

## What it covers

| File | Covers |
|---|---|
| `test_00_preflight.py` | Boards, firmware, key and link. Bench-safe settings. Ping. Simulator wiring. The controller reachable and following K1 (its SW input in follow mode, not edge-toggle) |
| `test_normal.py` | Open and close from the controller, with timing at every hop. Reversal mid-travel. Flip back before the gate leaves its limit. External moves, including one right after our command. `-m soak`: `test_soak_open_close_cycles`, `--cycles` open/close cycles with latency stats |
| `test_opener_faults.py` | AC loss with the opener on battery (limits trusted, commands refused, controller resynced; and mid-travel). Opener power loss at rest and mid-travel. The inputs' supply failing with the limit dropping before IN3 (and IN3 returning first): no `between` logged; a limit lost with AC on is still `between` after the hold. Relay test without power. Jammed gate. Opener ignoring the command (siren/override). Both limits active. Limit chatter |
| `test_link_faults.py` | Link loss and recovery. A command into a dead link (expires, never fires late). Short and 4 s outages covered by retries within `cmd_ttl_s`. A gate move missed during an outage. Replayed frames. Wrong key |
| `test_reboots.py` | Gate reset at rest and mid-travel. Gate reset right after a pulse, before its ACK (`debug.reboot_after_cmd`): one press only, the house drops the command. Gate and opener restarting together (GateSim `restart`): the house never shows not-closed. House reset with the controller wrong, and with the gate open |
| `test_controller_faults.py` | Controller toggled while unpowered. Relay dropping before the power sense. Controller coming back at the wrong level. Rapid toggling |
| `test_options.py` | Gate relay test on a closed gate (our move, house follows) and at the open limit (no target: a local close right after is external). `power_sense` 0 (IN3 ignored), `linkloss_open` 0 (K2 held through an outage), `sensor_invert` 1, `ctrl_sync` 0 |
| `test_remote.py` | `remote.set` over LoRa (applied, saved by the gate, put back), refused for non-remote params, `busy` while one is pending. `remote.diag`. A gate heartbeat longer than the house's `link_timeout_s` (house `link_timeout_eff_s`). The console's `line too long` reply |
| `test_history.py` | Link history: bucket counters against the status totals, levels and the gate's side filled in. Rollover with 60 s buckets, and `hist.get` paging. Link-down seconds and the gate's STATUS retries across an outage |
| `test_soak.py` | `-m longsoak`: open/close cycles, outages, opener power blips, external moves and jams in rotation; no resets or radio faults; counters to `soak_counters.csv` |
| `test_power.py` | `-m power`: gate cut at rest (house doesn't notice), beyond the link timeout (sensor fails open, recovers), mid-pulse (one short press, no re-pulse), bounce (5 cuts, config intact), during a remote config save (old or new value, never defaults); the opener moved by hand while the gate is down (real position after the boot, cause none); AC loss with the board on the AC supply (dies with AC, back without a press); house cut with the gate open (no command, controller back on), with a command pending (not sent again after the reboot), during a local config save (old or new record, never defaults or a mix); house 12 V dips; both sites at once. LiPo in: the gate and house supply cut and dips, AC loss and a dead opener with the board riding through, both supplies at once |
| `test_rf.py` | `-m rf`: the full loop over a marginal link (minimum power, SF12) |

## How it works

- **Faster timings.** The suite applies shorter timings to both boards for the run, unsaved: `heartbeat_s` 5,
  `link_timeout_s` 15, `travel_timeout_s` 15, `mismatch_timeout_s` 20, `cmd_ttl_s` 10, and simulator travel 8 s
  (saved in the Uno's EEPROM, put back at the end). House `ctrl_power_sense` and `ctrl_power_pmic` are off (with
  `ctrl_confirm_ms` 3000, as IN2 alone needs) except where a test turns them on: the controller faults and switching
  the controller on without a command fake IN2, and the power tests use the install's senses (`REAL_CTRL_POWER`), and the baseline turns the controller off in HA if HA still shows it on after a reboot
  left its relay off. It also pins the settings scenarios rely on at the firmware defaults (`retries`, `debounce_ms`,
  `sync_window_ms`, `resync_ms`, `ctrl_settle_ms`, the input inverts, `power_sense`,
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
- **Results.** `tests/e2e/results/<run>/` holds a time-ordered timeline per test (all four devices, JSONL),
  `summary.md` (results, latencies, link quality, anomalies such as a gate → house status that needed a retry) and
  `summary.json`: the same plus the start and end time, the command line and `-m`/`-k` selection (per marker group,
  how many tests were collected and selected; `--ignore`, `--lf`, `--sw` and collection errors, which leave whole
  files out), the git commit, local changes and `firmware/GateLink` tree, both
  boards' firmware and USB serial numbers, the suite options (`--tx-power`, `--cycles`, `--soak-minutes`, whether
  `GATELINK_KEY` was set, never the key) and each test's outcome, duration and skip or failure reason.
  `tools/release_evidence.py` reads it to check a release against [docs/release-criteria.md](../../docs/release-criteria.md).

## Changing the suite

The suite parses console replies, log event names/values and status fields (`gatelink/`, and the console client
`tools/gatelink_client/` it shares with `tools/gatelink.py`: `board.py`, `timeline.py`, and the firmware's enum values
in `wire.py`), so change it together with `console.cpp`, `log.cpp` and `roles.h`. `pytest.ini` puts `tools/` on the
import path. Scenario timings assume the test profile in `gatelink/bench.py`.
