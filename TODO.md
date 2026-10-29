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


- [ ] **A supply dip hidden by a loop stall becomes a CLOSE.** Found by the model checker
  (`tools/verify/esbmc/house_cmds.cpp`, `owner-only`, `-DSTALLS_ANYWHERE`, the Shelly following K1): the house's loop
  stops ~0.5 s in a flash save (with the radio restart; the soak's `loop_max_us` is 510 ms); a dip of the 12 V under it
  holds the board's power good low for only ~100–300 ms, all inside the stall, so the house never sees the power go,
  but the dip reboots the Shelly (the bench's did on 300 ms), whose relay drops ~0.5 s after the cut, after the stall:
  an OFF edge with power reading good, sent as CLOSE once `ctrl_confirm_ms` passes, against the open gate. Trace
  (ESBMC): cut at t, house passes at t+265 ms and t+1256 ms, power good low t+266..t+700, relay off t+384, CLOSE sent
  t+2.1 s. Host test `house_power_dip_hidden_by_a_flash_save_never_closes` (XFAIL; the simulator's Shelly now reboots
  on a short dip with `brownoutMs`). Fix: read power good in the stall's waits too (flash access yields) and keep a low
  reading for the next pass, so the house sees the dip before the relay's drop.

- [ ] **A Shelly reboot the house doesn't see restores an OPEN.** Found by the model checker
  (`tools/verify/esbmc/house_cmds.cpp`, `owner-only`, `esbmc-house_cmds-worst`, the Shelly following K1): a dip of the
  12 V too short for the board's power good (it drops ~150–300 ms into a cut) but long enough to reboot the Shelly (the
  bench's rebooted on a 300 ms dip, not on 200 ms: Bench, below). Just after K1 changes, before the Shelly has followed
  it, its relay drops (~0.35–0.7 s into the dip): an OFF edge begun after K1 moved, at the level K1's window expects, so
  it ends the window. The Shelly then boots back to its state before the cut (parameter 17 at its default 0, Shelly Wave
  1 manual), outside the window: the house takes that edge for the owner's. Trace (ESBMC): the owner switches off, the
  Shelly follows K1 on, a 96–162 ms dip, K1 off, the relay off 366 ms into the dip, the Shelly back ON 2.5 s later: OPEN
  against the gate just closed. No `ctrl_confirm_ms` covers it (an OPEN goes at once; with parameter 17 = 1 the relay's
  drop is a CLOSE nothing cancels). Host test `house_power_unseen_shelly_reboot_never_opens` (XFAIL; the simulator's
  Shelly restores its last state with `restoreLast`): OPEN sent 3.1 s after K1 went off. Covered at the install only if
  every dip that reboots the Shelly drops power good first (`esbmc-house_cmds-install`, `BROWNOUT_AFTER_PG`), as the
  bench's dips so far show (Bench, below); finish that measurement before choosing a fix.

## Bench and field tests

- [ ] **The shortest 12 V dip that reboots the Shelly, against the board's power good.** The house's command proofs
  rest on power good dropping before any dip that reboots the Shelly (`BROWNOUT_AFTER_PG`; finding above, "A Shelly
  reboot the house doesn't see restores an OPEN"). Cuts and 300 ms dips: power good first, 30 of 30 (PR #42). The e2e
  `test_house_supply_dips_ride_through` runs 20261013-102954 to 20261024-172921 (house log `supply` and IN1 per dip):
  30, 50 and 80 ms dips dropped neither (0 of 15); 150 and 200 ms dropped power good (10 of 10, 152–228 ms after the
  rig's command) but never rebooted the Shelly; 300 ms did both, power good first (5 of 5). So the threshold that
  reboots it lies in 200–300 ms, above power good's (80–150 ms). Step 200–300 ms in 10 ms (logic analyzer on the
  Shelly's relay output and the board's power good, as in docs/bench-testing.md) to the shortest that reboots it, and
  record power good's lag at it.

- [ ] **A rebooted gate's session is verified slowly at 17 dBm on the bench.** After a gate reboot the house accepted
  the new session within 1.4–1.9 s at 5 dBm, but in 1.5–4.2 s at 17 dBm (and once over 7.8 s, run
  `20261014-231116`), on 0.15.2 as well (1.5–4.2 s, `20261013-144010`). While the house still has the old session
  verified, a lost challenge waits for the HELLO backoff (`sendHello` in `link.cpp`: 1 s doubling to 8 s, plus up to
  as much at random), which limits what replayed HELLOs can provoke. Left: why frames are lost at 17 dBm on the bench
  (antennas close, RSSI about -36 dBm), and whether the house should challenge a rebooted peer's new session sooner,
  still bounded against replays. Availability only: a command waits for the gate's clock (up to `cmd_ttl_s`).

- [ ] **`lbt_forced` in `test_history_counts` at 5 dBm.** On 2026-10-15 the gate sent a STATUS (type 5) anyway after
  waiting 506–595 ms for the channel, in two of the five runs of `test_history.py` alone (`20261015-072034`,
  `20261015-073644`; both 5 dBm runs, none of the three at 17 dBm), but not in the full suite (`20261015-074616`) or
  any earlier run. The test sends a burst of pings, so the gate may be holding back for the house's own frames.
  Check that test's timeline for what the gate heard while it waited.
- [ ] **The bench boards drop off USB on the 1200-baud touch.** Twice on 2026-10-19 (…0C301C on COM21, both
  times right after a full suite run) `tools/flash.py` reported `didn't come back as a bootloader port`, and the
  board showed no USB device at all, neither its app port nor a bootloader one, until its cable was replugged. It then
  came up in its bootloader (COM18) and flashed fine. The house board (…183013) never did. Find out whether it is that
  board, its cable or hub port (swap them), or the touch landing during something the firmware does after a run (the
  suite restoring config), and whether `flash.py` can recover without a hand on the cable. On 2026-10-20, again
  right after full suite runs, both boards in turn (the gate, then the house) failed the touch differently: pyserial
  raised `PermissionError(13, 'A device attached to the system is not functioning.', None, 31)` configuring the port,
  yet the board had reset into its bootloader (COM18, COM19), and `flash.py <bin> <bootloader port>` flashed it with
  no replug. `flash.py` could catch that and go on looking for the bootloader port.
  On 2026-10-22, the house (…183013), with no suite before it: the touch worked and the bootloader came up on COM19,
  but bossac reported `No device found on COM19`; `flash.py <bin> COM19` a moment later flashed it. So the bootloader
  port can show up before it answers: `flash.py` could retry bossac once after a short wait. The same day the gate (…0C301C)
  dropped off USB entirely again on the touch (no app or bootloader port), after probe scripts and a flash.
- [ ] **Bench suite gaps and fixed waits** (from the 2026-10-08 review).
  - Fixed sleeps that bet on timing: the `sleep(2)`/`sleep(3)` before negative assertions (derive them from the
    profile).
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
- [ ] **What the limits read while the CSW24UL's controller restarts.** The closed limit is the AUX2 relay's NC
  contact, wetted by the accessory 24 V, so while the opener's controller restarts an open gate may read closed.
  Since 0.17.2 the gate reads that as `fault` when it comes straight from open, and holds a closed out of `no_power`
  for 3 s from when it reads (`BOOT_SETTLE_MS`). Not covered: the opener's own 24 V dropping while IN3 stays up (its
  fuse, a controller reset that dips the accessory output). That reads `between`, and a closed limit as it returns is
  trusted (docs/architecture.md); if the field check shows it, it needs a minimum travel time measured. Left, at the install (the GateSim doesn't model it), with the gate open:
  - cut the opener's power with its battery out, restore it, and time how long after IN3 returns the open limit reads
    (gate log `gate_state`). If it's more than 3 s, a `closed` shows first: lengthen that hold.
  - reset its controller if it has a way, and check the gate log shows `fault` (or `between`), then `open`, never
    `closed`; time the full travel both ways too.

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
- [ ] **Fit the closed sensor at the gate (0.20.0, `closed_sense`).** The second closed sensor closes
  `closed-limit-nc-fail-unsafe` only once it is fitted: an outdoor magnetic contact on the leaf, wetted from the
  opener's accessory 24 V into opto channel 4 (docs/hardware.md). Check on site that it closes within the last inches
  of travel: the gate reads `fault` if it and the closed limit disagree for 5 s, so a slow gate with a sensor that
  closes early would read fault on every close. Then turn `closed_sense` on and run the install checks (docs/install.md).

## Supply chain

- [ ] **Release binaries: what's left** (threat model `pages-malicious-firmware`, `release-asset-mutable`). The
  rulesets on `main` and `v*` tags and immutable releases are on (2026-10-14). Pages checks the `.bin` it bundles against
  GitHub's release attestation (`gh release verify-asset`, 2026-10-24). Left: build provenance attestations for the
  binary (`actions/attest-build-provenance`), if worth a third-party-free path to them.

## Housekeeping

- [ ] **Back up the bench link key.** It was rotated on 2026-09-30 so the wrong-key test could run, and is only in
  `~/.gatelink_key` (not in the repo; boards can't read it back). Make the encrypted backup with
  `python tools/gatelink.py key backup <dir>` and keep its passphrase in the password manager; then
  check `python tools/gatelink.py ports` shows the same key id on both boards as `key id` does for the
  file ([docs/key-management.md](docs/key-management.md)). The install pair gets its own key, generated there.
