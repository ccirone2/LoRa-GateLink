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
  released, before the firmware starts. Remaining options: a bootloader that skips that wait on watchdog/software
  resets (it's also what lets a double-tap rescue a board), or latching relays. A power-on reset of the house
  takes the controller down too (shared 12 V), so it's only the warm resets.

## Bench and field tests

- [ ] **Bench suite gaps and fixed waits** (from the 2026-10-08 review).
  - `pulse_ms` isn't in the test profile: `test_reboots.py` assumes 500 ms and `test_options.py` uses the saved
    value. Add `pulse_ms: 500` to `PROFILE_GATE` (`gatelink/bench.py`) and check pulses against it.
  - Fixed sleeps that bet on timing: `test_reboots.py` (`sleep(0.2)` to catch the gate in its ~0.5 s bootloader;
    start from the UART `boot` event or `sim.restart` instead), `test_history.py` (`sleep(62)` against a 60 s
    bucket; poll until the bucket rolls), and the `sleep(2)`/`sleep(3)` before negative assertions (derive them from
    the profile).
  - The checks after every test cover K2 but not K1: add one comparing house K1 with the gate state, allowing for
    `holdingTravel` and the sync window.
  - Not covered on the bench: `openSyncWindow` never shortening a window, the `settleUntil` floor, and a controller
    that toggles on SW edges (a scenario with `in1_invert` would do). The host system tests cover them since the
    host simulation; keep the bench gap only if a real-hardware angle remains.
  - `tests/e2e/requirements.txt` has no upper bounds (`pytest>=8`; the conftest relies on
    `hookimpl(wrapper=True)`). Pin them (see the threat model's bench hygiene item for hashes).

- [ ] **USB serial ports hang after uploads.** On 2026-10-05, twice after an upload every USB serial port on the
  bench (both boards and the FTDI adapter) stopped opening until the USB hub was replugged. Uploading one board at
  a time, and checking the ports between, avoided it since. Try another hub port or the PC's own ports to find the
  cause. Opening a port now gives up after 8 s (`open_serial` in `tools/gatelink_client/board.py`), so `ports`,
  `snapshot` and the suite report a stuck port instead of hanging. It also happens after power cuts: on
  2026-10-06 both boards came back cleanly (seen on their UART adapters) but Windows lost their USB ports until
  the hub was replugged. The trigger seems to be a board's USB vanishing abruptly (upload resets, power cuts)
  behind a power-blocked cable. Since 0.12.0 the suite sends every request over the UART while USB is down (CRC
  checked), so the power tests don't depend on USB coming back.

- [ ] **SPI flash on both boards.** 0.5.0 keeps config in the on-board SPI flash. Check `info` `flash_id` and
  `cfg_store` `spi` on both bench boards and on any replacement board (an unexpected chip falls back to
  program flash, which uploads erase).

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

- [ ] **Install checklist.** Write the on-site steps (wiring checks per board, Shelly SW mode, CSW24UL AUX relay
  settings, power sense checks, RF margin, final config export) as a doc once the install hardware is in hand.

## Housekeeping

- [ ] **Back up the bench link key.** It was rotated on 2026-09-30 so the wrong-key test could run, and is only in
  `~/.gatelink_key` (not in the repo; boards can't read it back). Make the encrypted backup with
  `python tools/gatelink.py key backup <dir>` and keep its passphrase in the password manager; after flashing
  0.13.8, check `python tools/gatelink.py ports` shows the same key id on both boards as `key id` does for the
  file ([docs/key-management.md](docs/key-management.md)). The install pair gets its own key, generated there.
- [ ] **Create the nightly routines** defined in `.claude/routines/` on claude.ai (`/schedule` from the CLI, one per
  file, each with the two-line prompt in its README), and give the cloud environment network access to
  `downloads.arduino.cc` and `github.com` so `tools/agent/cloud_setup.sh` can install the toolchain
  ([docs/agent-tooling.md](docs/agent-tooling.md)). Watch the first week's pull requests and tune the routine files.
- [ ] **CI, part 2: static analysis, sanitizers, coverage.** Once the host simulation of both boards is merged:
  cppcheck (`warning,performance,portability` are clean; style noise like `badBitmaskCheck` off), clang-tidy over
  the host build, the host tests under ASan/UBSan, and gcovr line coverage of the firmware reported per run.
