# TODO

Open bugs, investigations and bench/field tasks. Desired features go in [ROADMAP.md](ROADMAP.md); what has been
fixed is in the [release notes](https://github.com/ccirone2/LoRa-GateLink/releases) (the 0.3.2–0.3.5 notes hold
the bench-suite findings and the 2026-10-02 code review).

Format: `- [ ] **Short title.** What's wrong or what's left, what's known, and how to check it.` Remove an item
once its fix is merged and record it in the pull request.

## Firmware

- [ ] **The house still misses ~2–3 % of pongs at SF9.** Only the house board has shown CRC errors, and moving
  the channel to 903 or 925 MHz made no difference. Ruled out on the bench: turnaround length, USB activity, the
  LED PWM. Suspect: the bench Shelly's Z-Wave radio (916 MHz, centimetres away) blocking the house receiver in
  bursts. Since 0.4.0 the noise floor is measured (status `link.noise`, history `noise_avg`/`noise_max`), and
  it fits. On the bench on 2026-10-02, 2 minutes of pings both ways gave:
  - house: noise peaks of −56 dBm (stronger than the gate's frames at −66) and 3 CRC errors;
  - gate: peaks of −95 dBm and no CRC errors;
  - both: an average near −110 dBm.
  In one test run that drove the Shelly through HA, the house averaged −94 dBm against −111 at the gate. That
  was on a build that still counted frame starts as noise; a later run gave −110 at both.
  **Z-Wave ruled out (2026-10-03, 0.5.1, SF9, tx_power 5):** 5 minutes of pings both ways with the Shelly
  powered, then unplugged. Powered: house 2.4 % / gate 2.8 % pong loss, 6 house CRC errors. Unplugged: 2.9 % /
  2.9 %, 6 house CRC errors. The gate had no CRC errors either time, and the noise floor was the same on both
  boards (avg ≈ −109/−110, max ≈ −94 dBm). The gate's lost pongs are most likely pings the house never received,
  so the loss is at the house receiver whichever side pings. With the antennas swapped between the boards:
  1.2 % / 4.1 % loss, 8 house CRC errors, still none at the gate, so it isn't the antenna. With the roles
  swapped in config (boards left in place): 2.3 % / 1.2 % loss, 0 CRC errors at the new house and 5 at the new
  gate, so the errors follow the physical board (the old house board, on COM6 on the bench), not the role. Left
  to separate: that board's radio, its shield and wiring, or its spot on the bench. With the MKR boards then
  swapped between the shields (COM6 now the gate on the gate shield): 0.4 % / 1.9 % loss, 0 CRC errors on
  COM5 (now on the house shield), 2 on COM6. So the CRC errors follow the COM6 MKR board itself, whatever
  shield, antenna or role it has. Its receive SNR was also lower in most runs (5.5–7.75 dB against 7.75–8 on
  COM5). Loss without any CRC error remains on both boards (frames never detected). In this run COM5 saw noise
  peaks of −38 dBm on the house shield with no CRC errors. Frequency error (0.5.2, `fei` in status and `pong`):
  COM5 hears COM6 at −343 Hz, COM6 hears COM5 at +343 Hz (spread ±35 Hz over 5 minutes), i.e. the crystals
  differ by 0.4 ppm: not the cause. That run: 8 CRC errors on COM6, none on COM5. The shield swap also swapped
  the USB cables and ports, so those are ruled out too. What's left is the COM6 MKR board itself (its radio
  module or on-board supply). Next: try a spare MKR WAN 1310 in its place; keep COM6 out of the install if it's
  confirmed. To debug RX, read the SX127x packet/header counters
  (0x14–0x17) with `readReg()` in `radio.cpp`.
- [ ] **Commands don't survive outages longer than ~4.7 s well.** Retries are spread over `cmd_ttl_s` with
  doubling gaps (about 0.3, 0.9, 2.2, 4.7 and 9.7 s at the defaults), so after a ~4.7 s outage only the last
  retry is left and one lost frame drops the command. Decide whether to raise the default `retries` or change
  the spacing once the install-site RF numbers are in.
- [ ] **A warm reset of the house blips its outputs for at least 0.5 s.** On a watchdog, crash, reset-button or
  software reset, K1/K2 drop until the house is back and the gate reports (~1.5 s). With the gate open the
  controller follows K1 off and on (Alarm.com shows it closed for a moment); with it closed the contact sensor
  blips open. Restoring K1/K2 from `.noinit` RAM in `setup()` was tried (2026-10-05) and only shortened the drop to
  ~0.6–0.8 s: the MKR bootloader's double-tap check waits ~0.5 s on every reset except power-on, with the relay pins
  released, before the firmware starts. Remaining options: a bootloader that skips that wait on watchdog/software
  resets (it's also what lets a double-tap rescue a board), or latching relays. A power-on reset of the house
  takes the controller down too (shared 12 V), so it's only the warm resets.
- [ ] **A gate supply cut logs a moment of `between`, cause `external`.** Cutting the 24 V that wets the gate's
  inputs (bench, 2026-10-06): the closed limit's opto dropped one debounce cycle before IN3, so the gate logged
  `gate_state` between/external, then no_power within the same second. Harmless (the house shows not-closed for
  `no_power` anyway) but the log reads as a move. Fix idea: when a limit drops, wait one more debounce period for
  IN3 before reporting.
- [ ] **USB stalls cut console lines.** In the 120-minute soak on 0.4.1, 13 replies (11 house, 2 gate) were cut
  off at 192, 256 or 320 bytes: the host stopped taking IN packets for over 70 ms and the SAMD core dropped the
  rest of the line. Since 0.4.1 only that line is lost (the suite retries), but the web console and
  `tools/gatelink.py` see a timed-out request. Unknown whether the host (Windows usbser, pyserial) or the board
  is to blame. To check: count cut lines with only one board connected, and with the radio idle.

## Bench and field tests

- [ ] **USB serial ports hang after uploads.** On 2026-10-05, twice after an upload every USB serial port on the
  bench (both boards and the FTDI adapter) stopped opening until the USB hub was replugged. Uploading one board at
  a time, and checking the ports between, avoided it since. Try another hub port or the PC's own ports to find the
  cause. Opening a port now gives up after 8 s (`open_serial` in `tests/e2e/gatelink/board.py`), so `ports`,
  `snapshot` and the suite report a stuck port instead of hanging. It also happens after power cuts: on
  2026-10-06 both boards came back cleanly (seen on their UART adapters) but Windows lost their USB ports until
  the hub was replugged. The trigger seems to be a board's USB vanishing abruptly (upload resets, power cuts)
  behind a power-blocked cable. The power tests must not depend on USB coming back: read-only requests should fall
  back to the UART consoles.

- [ ] **SPI flash on both boards.** 0.5.0 keeps config in the on-board SPI flash. Check `info` `flash_id` and
  `cfg_store` `spi` on both bench boards and on any replacement board (an unexpected chip falls back to
  program flash, which uploads erase).

- [ ] **Real controller power loss.** It's still simulated (house `in2_invert`). The suite is ready: wire the IN2
  opto to the Shelly's 12 V on an HA smart plug and set `GATELINK_HA_POWER_ENTITY`. Then run
  `test_controller_faults.py` and note which drops first (summary "Link" section).
- [ ] **Real RF.** `test_rf.py` (`-m rf`) passes at the bench with antennas on: SF12, 2 dBm, 3 cycles. Still to
  do: run it with an attenuator or the antennas off, and at the install site, and check ping/RSSI there from the
  web console.
- [ ] **Full power on a real supply.** Bench boards run at `tx_power` 5 on USB because 17 dBm with a relay
  energized caused watchdog resets. Verify 17 dBm is stable on the install supplies (24 V→5 V buck at the gate,
  and the house supply), with the antenna placed away from the relay shield.

## Install

- [ ] **Install checklist.** Write the on-site steps (wiring checks per board, Shelly SW mode, CSW24UL AUX relay
  settings, power sense checks, RF margin, final config export) as a doc once the install hardware is in hand.

## Housekeeping

- [ ] **Record the link key.** It was rotated on 2026-09-30 so the wrong-key test could run. The key is in
  `~/.gatelink_key` (not in the repo). Store it somewhere safe, e.g. a password manager; boards can't read it
  back, and the web console's config export doesn't include it.
