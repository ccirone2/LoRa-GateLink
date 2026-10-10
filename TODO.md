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

## Bench and field tests

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
