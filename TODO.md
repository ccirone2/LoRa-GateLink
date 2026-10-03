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
  was on a build that still counted frame starts as noise; a later run gave −110 at both. Check with the Shelly
  moved away, comparing `tools/gatelink.py house hist` (`noise_max`, `crc_err`), and at the install site
  (`-m rf`). To debug RX, read the SX127x packet/header counters (0x14–0x17) with `readReg()` in `radio.cpp`.
- [ ] **Commands don't survive outages longer than ~4.7 s well.** Retries are spread over `cmd_ttl_s` with
  doubling gaps (about 0.3, 0.9, 2.2, 4.7 and 9.7 s at the defaults), so after a ~4.7 s outage only the last
  retry is left and one lost frame drops the command. Decide whether to raise the default `retries` or change
  the spacing once the install-site RF numbers are in.

## Bench and field tests

- [ ] **Real controller power loss.** It's still simulated (house `in2_invert`). The suite is ready: wire the IN2
  opto to the Shelly's 12 V on an HA smart plug and set `GATELINK_HA_POWER_ENTITY`. Then run
  `test_controller_faults.py` and note which drops first (summary "Link" section).
- [ ] **Real RF.** `test_rf.py` (`-m rf`) passes at the bench with antennas on: SF12, 2 dBm, 3 cycles. Still to
  do: run it with an attenuator or the antennas off, and at the install site, and check ping/RSSI there from the
  web console.
- [ ] **Long soak.** A 15-minute smoke run passed (43 scenarios, no resets, radio faults or replays). Still to do:
  `pytest tests/e2e -m longsoak --soak-minutes 120` (or longer); check `soak_counters.csv` for drift. The
  120-minute run on 2026-10-02 stopped after 18 minutes (51 scenarios). The link and gate were fine: no resets,
  radio faults, lbt_forced or new replays. It stopped because a USB stall cut off a house console reply
  and the next reply ran into it (fixed in 0.4.1).
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
