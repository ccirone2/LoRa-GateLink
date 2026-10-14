# TODO

Open bugs, investigations and bench/field tasks. Desired features go in [ROADMAP.md](ROADMAP.md); what has been
fixed is in the [release notes](https://github.com/ccirone2/LoRa-GateLink/releases) (the 0.3.2–0.3.5 notes hold
the bench-suite findings and the 2026-10-02 code review).

Format: `- [ ] **Short title.** What's wrong or what's left, what's known, and how to check it.` Remove an item
once its fix is merged and record it in the pull request.

## Firmware

- [ ] **A warm reset of the house blips its outputs for at least 0.5 s.** On a watchdog, crash, reset-button or
  software reset, K1/K2 drop until the house is back and the gate reports (~1.5 s). With the gate open the
  controller follows K1 off and on (Alarm.com shows it closed for a moment); with it closed the contact sensor
  blips open. Restoring K1/K2 from `.noinit` RAM in `setup()` was tried (2026-10-05) and only shortened the drop to
  ~0.6–0.8 s: the MKR bootloader's double-tap check waits ~0.5 s on every reset except power-on, with the relay pins
  released, before the firmware starts. Remaining option: a bootloader that skips that wait on watchdog/software
  resets (it's also what lets a double-tap rescue a board). Latching relays are ruled out: they keep a stale state
  through a crash or power loss, where a released relay fails toward the safe reading. A power-on reset of the house
  takes the controller down too (shared 12 V), so it's only the warm resets. The drops can now be timed on the logic
  analyzer (K1 = D1, K2 = D2 on two channels; docs/bench-testing.md), once those are wired.


## Bench and field tests

- [ ] **Bench suite gaps and fixed waits** (from the 2026-10-08 review).
  - `pulse_ms` isn't in the test profile: `test_reboots.py` assumes 500 ms and `test_options.py` uses the saved
    value. Add `pulse_ms: 500` to `PROFILE_GATE` (`gatelink/bench.py`) and check pulses against it.
  - Fixed sleeps that bet on timing: `test_reboots.py` (`sleep(0.2)` to catch the gate in its ~0.5 s bootloader;
    start from the UART `boot` event or `sim.restart` instead), `test_history.py` (`sleep(62)` against a 60 s
    bucket; poll until the bucket rolls), and the `sleep(2)`/`sleep(3)` before negative assertions (derive them from
    the profile).
  - The checks after every test cover K2 but not K1 (wiring K1 to the suite is ROADMAP B5): add one comparing house K1 with the gate state, allowing for
    `holdingTravel` and the sync window.
  - Not covered on the bench: `openSyncWindow` never shortening a window, the `settleUntil` floor, and a controller
    that toggles on SW edges (a scenario with `in1_invert` would do). The host system tests cover them since the
    host simulation; keep the bench gap only if a real-hardware angle remains.

- [ ] **USB serial ports hang after uploads.** On 2026-10-05, twice after an upload every USB serial port on the
  bench (both boards and the FTDI adapter) stopped opening until the USB hub was replugged. Uploading one board at
  a time, and checking the ports between, avoided it since. Try another hub port or the PC's own ports to find the
  cause. Opening a port now gives up after 8 s (`open_serial` in `tools/gatelink_client/board.py`), so `ports`,
  `snapshot` and the suite report a stuck port instead of hanging. It also happens after power cuts: on
  2026-10-06 both boards came back cleanly (seen on their UART adapters) but Windows lost their USB ports until
  the hub was replugged. The trigger seems to be a board's USB vanishing abruptly (upload resets, power cuts)
  behind a power-blocked cable. Since 0.12.0 the suite sends every request over the UART while USB is down (CRC
  checked), so the power tests don't depend on USB coming back.

- [ ] **Link history kept in the SPI flash (0.14.0): boot time with a full log.** Reboots, power cuts (now
  `test_gate_power_cut_keeps_history` in `-m power`), cuts inside a sector erase, the hourly write in a long soak and
  `free_ram` all passed on the bench (2026-10-11). Left: the boot time with a full log (512 records: the firmware reads
  every slot's header and ~100 records in full, timed from the UART `boot` event).
- [ ] **Fault output D5 (0.14.0): a relay module and the alarm side.** Checked with a meter on both bench boards
  (2026-10-11), each reading as status `fault_out` reported: 0.01 V with `fault_out` off, 3.31 V healthy; 0.01 V
  `fault_hold_s` (10 s) after a link loss (house) or an AC loss (both boards), and 3.31 V again within a second of the
  link's first STATUS or AC's return; through a house `reboot`, LOW from 128 ms into the sketch until it armed (~5 s).
  The suite has no D5 check (it needs a wire from D5 to something it reads, e.g. a GateSim input; ROADMAP B5). On the logic
  analyzer (house, `tools/logic.py`, 1 and 4 MHz, 2026-10-11), through a `reboot` with `fault_out` saved on: D5 falls at
  the reset, comes back HIGH for 1 µs 3 µs later (both runs alike), then reads LOW until the house arms (4.5–4.9 s) and
  rises cleanly; no edge when it becomes an output. Too short for a relay module or the alarm loop, but a fast opto
  input could see it. With `fault_out` off it reads LOW. The analyzer's unconnected inputs read HIGH, so neither
  shows whether the pin floats between the reset and `healthBegin`. Left: that (a known load, e.g. 100 kΩ to 3.3 V,
  through a reboot); the pull-down with `fault_out` off (a meter reads 0 V either way: 100 kΩ from 3.3 V reads 0.5–1.2 V
  across its 20–60 kΩ, 3.3 V without it); a relay module or opto switching reliably from 3.3 V at a few mA; the 2GIG
  sensor loop and how Alarm.com reports it (trouble vs intrusion); a radio fault (host-tested only).
- [ ] **0.13.9 fixes the bench can't trigger on demand** (host-tested, #86). A radio fault during a gate relay pulse
  (the pulse must not stretch, and the command's ACK still lands within `cmd_ttl_s`); five remote config writes
  during one pulse (four queued, the fifth refused and taken from the house's resend after the pulse, all within the
  house's 10 s write timeout); and a switch flipped back between a CLOSE's ACK and the gate's next STATUS, at SF9 on
  the real controller. Each needs a debug hook or a rig (a radio reset line, replayed CFG_SETs, a timed relay on the
  controller's input; ROADMAP B3).
- [ ] **SPI flash on the install boards.** 0.5.0 keeps config in the on-board SPI flash. Both bench boards report
  `cfg_store` `spi` and `flash_id` `ef4015` (v0.14.0 release evidence, `docs/releases/v0.14.0.md`). Left: check `info`
  on the install pair and on any replacement board (an unexpected chip falls back to program flash, which uploads
  erase).

- [ ] **Real RF.** `test_rf.py` (`-m rf`) passes at the bench with antennas on: SF12, 2 dBm, 3 cycles. Still to
  do: run it with an attenuator or the antennas off, and at the install site, and check ping/RSSI there from the
  web console.
- [ ] **Full power at the install.** On PC USB power, 17 dBm with a relay energized caused watchdog resets, so the
  bench boards keep `tx_power` 5 saved. On the bench's install-like supplies (gate on a 24 V→5 V buck, house on a
  12 V→5 V buck) 17 dBm passes, run with `--tx-power 17` (2026-10-07, 0.10.0): with both LiPos in, the full suite with
  `-m soak` and the LiPo-in power tests, and a 60-minute `-m longsoak`; with both LiPos out, `-m "soak or power"` (24
  passed) and a 60-minute `-m longsoak`. No resets, no `radio_faults`, no `lbt_forced`. Left: at the install, save
  `tx_power` 17 on both boards and watch `reset_cause` and `radio_faults` for a few days, with the antenna away from
  the relay shield.
- [ ] **How long the CSW24UL takes to leave a limit after a pulse.** Since 0.13.9 the gate's `leaving` mark lapses
  `debounce_ms` after the last relay releases with the gate still at that limit, so a reversal (CLOSE, then OPEN
  before the gate left open) is `cause: lora` only if the opener's limit drops within the pulse plus `debounce_ms`;
  later, that move is logged `external` (a label: K1, K2 and commands don't depend on it). The GateSim and the host
  simulation react within 20 ms. At the install, measure the time from a pulse to the limit input dropping (gate log
  `pulse` to `gate_state` between, less `BETWEEN_HOLD_MS`, both ways) and widen the lapse if it's longer.

## Install

- [ ] **Install checklist.** A draft is in [docs/install.md](docs/install.md): the site survey (web card and
  `tools/gatelink.py survey`) and a checklist skeleton built from hardware.md and the README (wiring checks per
  board, Shelly SW mode, CSW24UL AUX relay settings, power sense checks, the survey, config export and key backup).
  Left, once the install hardware is in hand: walk it on site and correct it, add the Shelly and CSW24UL menu steps
  as done, the readings at each check and photos, and record the first survey. Then drop the draft notice.
- [ ] **Check that the alarm records a 5 s sensor open.** 0.17.0 opens the contact sensor (K2) for 5 s to show a move
  the house never heard (`missed_move`). Untested against a real 2GIG sensor and panel: at the house, take the house
  board's antenna off while the gate opens and closes (within the 100 s link timeout), put it back, and check
  Alarm.com logs the gate opened and closed. If the sensor or panel needs longer, raise `MISSED_OPEN_MS`.
- [ ] **Troubleshooting: a limit that never reads may be unset limits.** On a real install the opener's AUX limit
  relays only switch once the CSW24UL's travel limits have been learned, so a limit input that never goes active (at
  either end of the travel) may mean the limits aren't set, not a wiring fault. Say so in the help text where an
  installer would look: `docs/install.md`'s limit checks, `docs/hardware.md`, and the web console's I/O card or
  wiring notes (`web/js/wiring.js`) and the health line for a gate stuck `no_power`/`between`.

## Supply chain

- [ ] **Repository settings** (threat model `pages-malicious-firmware`, `release-asset-mutable`,
  `agent-tooling-push-path`; owner's decision). A ruleset on `main` (pull request and CI required, no force push), a
  protected `v*` tag pattern, immutable releases, and build provenance attestations for release binaries.
- [ ] **Bench hygiene** (threat model, low). Home Assistant TLS verified by default (`ha-token-tls-off`), a Host
  check in `tools/bench-wiring/serve.py` (`bench-wiring-server`), a content security policy in `web/index.html` and
  numeric coercion of `hist.get` fields (`board-output-xss`).

## Housekeeping

- [ ] **Back up the bench link key.** It was rotated on 2026-09-30 so the wrong-key test could run, and is only in
  `~/.gatelink_key` (not in the repo; boards can't read it back). Make the encrypted backup with
  `python tools/gatelink.py key backup <dir>` and keep its passphrase in the password manager; then
  check `python tools/gatelink.py ports` shows the same key id on both boards as `key id` does for the
  file ([docs/key-management.md](docs/key-management.md)). The install pair gets its own key, generated there.
- [ ] **Nightly routines: the first week.** The routines in `.claude/routines/` run, and since the cloud environment
  got full network access (2026-10-12) their setup installs everything: on 2026-10-13 `setup.done` read `ok`, the
  bug-sweep ran the host tests (#112) and the deps-check reached the Arduino library index (#113). Left: watch the
  first week's pull requests (to about 2026-10-19) and tune the routine files.
