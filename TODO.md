# TODO

Open bugs, investigations and bench/field tasks. Desired features go in [ROADMAP.md](ROADMAP.md); what has been
fixed is in the [release notes](https://github.com/ccirone2/LoRa-GateLink/releases) (the 0.3.2–0.3.5 notes hold
the bench-suite findings and the 2026-10-02 code review).

Format: `- [ ] **Short title.** What's wrong or what's left, what's known, and how to check it.` Remove an item
once its fix is merged and record it in the pull request.

## Firmware

- [ ] **Commands don't survive outages longer than ~4.7 s well.** Retries are spread over `cmd_ttl_s` with
  doubling gaps (about 0.3, 0.9, 2.2, 4.7 and 9.7 s at the defaults), so after a ~4.7 s outage only the last
  retry is left and one lost frame drops the command. Decide whether to raise the default `retries` or change
  the spacing once the install-site RF numbers are in. The host test `command_outage_coverage` (`tests/native`)
  prints the numbers for the defaults. Sends left after an outage that starts as the command is queued: 6
  without one, 4 up to ~1 s, 3 up to ~2 s, 2 up to ~4.5 s, and 1 from ~5 s up to the TTL. At 30 % frame loss
  after the outage the command gets through 97 %, 91 % and 70 % of the time with 3, 2 and 1 sends left. Change
  `retries`/TTL in the test to compare alternatives.
- [ ] **Frames heard more often than the new-frame backoff hold our new frames off for good.** Every frame
  received restarts the listen-before-talk gap (`lastAirAt` in `handleFrame`, before any check, so another
  net_id's frames count too). A new frame waits a backoff of up to ~106 ms at SF9/500 kHz, drawn once and kept
  until it's sent. A frame arriving every ~100 ms therefore keeps every slot and queued frame waiting until its TTL
  runs out, and the forced send (`lbt_forced`) only covers a channel that reads busy, not this. Found with the
  host tests (a replayed HELLO every 100 ms). It needs a very busy channel (a neighbour's LoRa traffic on our
  frequency, SF and sync word, or someone replaying frames), close to jamming. Possible fix: count only our
  peer's authenticated frames for the backoff, or cap the total wait as for a busy channel.
- [ ] **A warm reset of the house blips its outputs for at least 0.5 s.** On a watchdog, crash, reset-button or
  software reset, K1/K2 drop until the house is back and the gate reports (~1.5 s). With the gate open the
  controller follows K1 off and on (Alarm.com shows it closed for a moment); with it closed the contact sensor
  blips open. Restoring K1/K2 from `.noinit` RAM in `setup()` was tried (2026-10-05) and only shortened the drop to
  ~0.6–0.8 s: the MKR bootloader's double-tap check waits ~0.5 s on every reset except power-on, with the relay pins
  released, before the firmware starts. Remaining options: a bootloader that skips that wait on watchdog/software
  resets (it's also what lets a double-tap rescue a board), or latching relays. A power-on reset of the house
  takes the controller down too (shared 12 V), so it's only the warm resets.
- [ ] **A resync can turn into a command** (host test `robustness_resync_window_ended_by_a_matching_edge_lets_the_resync_command_the_gate`,
  found by `robustness_chaos_seed_8`; both XFAIL). The mismatch resync does `k1.set(!t); openSyncWindow(now, t, ...)`,
  so an IN1 edge at `t` inside it (the user's own edge still in its debounce, or chatter) ends the window early; the
  controller then follows K1 to `!t` outside any window, and that edge is sent as a command: the user switched on
  with the gate open and the gate closed. About 30–50 ms per resync. Fix: the window must expect the level K1 drives
  (and its return), and a matching edge must not end it before `resync_ms`.
- [ ] **A command overridden mid-travel waits 75 s for its resync** (`gate_state_override_to_the_far_limit_resyncs_the_controller_at_once`,
  `house_sync_siren_override_mid_travel_resyncs_at_once`; XFAIL). The house fast-forwards a `timeout` result only
  when `mismatchSince` is set, but `holdingTravel()` keeps it at 0 all through the travel, so the controller shows
  the wrong state for `mismatch_timeout_s`.
- [ ] **The boot checkSoon reverts a user command** (`house_sync_edge_on_arming_is_not_reverted_by_the_boot_check_soon`;
  XFAIL). `sendCommand()` clears `mismatchSince` but not `checkSoon`, so once the command's ACK lets the mismatch
  check run, the gate still reads closed and the house resyncs the controller off while the gate opens.
- [ ] **A quick switch-back between the ACK and the gate's STATUS is dropped** (no host test yet). Gate open; off,
  CLOSE sent and ACKed; on again ~150 ms later: `sendCommand`'s opposing check sees no pending CMD and no target yet,
  logs `cmd_suppressed`, and the gate closes with the controller off. Repro in the host sim: `openByUser; user(false);`
  wait for `cmd_sent`; `run(150); user(true)`.
- [ ] **`leaving` outlives a pulse that never moved the gate** (`gate_state_leaving_mark_does_not_outlive_our_pulse`;
  XFAIL). After a reversal whose first pulse the opener ignored (siren holding OPEN), a later external move off that
  limit is reported with cause `lora`.
- [ ] **A second pulse of a relay already pulsing restarts its timer** (`gate_relays_second_open_cmd_mid_pulse_keeps_pulse_ms`,
  `gate_relays_second_relay_test_mid_pulse_keeps_its_ms`; XFAIL). Two OPENs ~200 ms apart hold K1 for 738 ms. Don't
  restart a running pulse (ACK the command as done; refuse the test `busy`).
- [ ] **A radio restart can run during a pulse** (`gate_relays_radio_reinit_waits_for_the_pulse`; XFAIL). The 5 s retry
  in `radioReceive` and `fault()` re-initialise the radio without checking `appRelaysPulsing()`; `LoRa.begin()` holds the
  loop ~470 ms, stretching the pulse.
- [ ] **HELLO answer stamps go stale after 24.9 days** (`robustness_quiet_26_days_then_*_reboot_relinks`; XFAIL): fixed
  on the link-robustness branch (0.13.7), which flips these tests.

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

## Install

- [ ] **Install checklist.** Write the on-site steps (wiring checks per board, Shelly SW mode, CSW24UL AUX relay
  settings, power sense checks, RF margin, final config export) as a doc once the install hardware is in hand.

## Housekeeping

- [ ] **Record the link key.** It was rotated on 2026-09-30 so the wrong-key test could run. The key is in
  `~/.gatelink_key` (not in the repo). Store it somewhere safe, e.g. a password manager; boards can't read it
  back, and the web console's config export doesn't include it.
- [ ] **Create the nightly routines** defined in `.claude/routines/` on claude.ai (`/schedule` from the CLI, one per
  file, each with the two-line prompt in its README), and give the cloud environment network access to
  `downloads.arduino.cc` and `github.com` so `tools/agent/cloud_setup.sh` can install the toolchain
  ([docs/agent-tooling.md](docs/agent-tooling.md)). Watch the first week's pull requests and tune the routine files.
- [ ] **CI, part 2: static analysis, sanitizers, coverage.** Once the host simulation of both boards is merged:
  cppcheck (`warning,performance,portability` are clean apart from `LineWriter::buf` uninitialised and the
  `memset` of the link stats; style noise like `badBitmaskCheck` off), clang-tidy over the host build, the host tests
  under ASan/UBSan, and gcovr line coverage of the firmware reported per run.
