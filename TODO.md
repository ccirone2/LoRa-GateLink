# TODO

## Code review (2026-10-02)

Found by reading the code at 0.3.3 (`9e54c60`). The build compiles with no warnings in project files. P1 = a
bug likely to bite (or a security hole), P2 = an edge-case bug, a robustness gap or a test gap, P3 = cleanup.

### P1 (fixed in 0.3.4)

Bench results on 0.3.4: the full suite passes (32 passed, plus the opt-in wrong-key test), with no radio faults,
replays (apart from the deliberate replay test) or corrupted console lines. House pings got 296/300 pongs at SF9.
A gate reboot re-establishes the session in about 2.6 s (it was 3–4.6 s).

- [x] **Re-verifying a peer reopened the replay window.** HELLO_ACK set `peerLastSeq = seq; peerWindow = 1`, so a
  frame recorded before a reboot (seq within 31 of the HELLO_ACK) was accepted again. For example, a recorded
  house CMD(OPEN) replayed after a gate reboot pulsed the gate.
  - **Fix:** a new session now accepts only seqs above its HELLO_ACK (the window starts full).
  - A board answering a HELLO renumbers everything still waiting (`reframePending`: the slots and the TX queue).
  - Re-verifying the same session keeps its window and ACK memo.
  - `sessions` now counts only new sessions.
- [x] **A replayed old HELLO knocked the link down.** Now a HELLO from an unknown session is challenged, but the
  verified session stays in place until the new one answers. While a board is verified, the HELLO interval
  limits how many HELLOs replays can provoke.
- [x] **Blocking TX could trip the watchdog and stretched gate pulses.** TX is now asynchronous.
  - `radioSend` starts the frame, and the DIO0 interrupt (`onDio0`) puts the radio straight back into RX.
    `radioTxBusy()` is the polled fallback and enforces the deadline.
  - The turnaround wait moved into `clearToSend()`, and only one frame is on the air at a time.
  - **Found on the bench:**
    - **FSTX:** right after `endPacket()` the radio is briefly in FSTX (0x82). The first check counted that as
      "left TX mode", so the house logged a `radio_fail` on almost every ACK. FSTX now counts as transmitting.
    - **Lost USB bytes:** once USB output and TX overlapped, single bytes went missing from console lines, about
      one per two commands. Examples: `{"event":"lo",...` and a status reply without its closing `}`. Each
      character was its own USB transfer. Lines are now built in a buffer and written once
      (`console.cpp` `send`). The soak test ran 40 commands with no corrupted lines; it had 2 in 4 before.
- [x] **`radioRandom32()` no longer leaves RX** (it aborted a frame being received). It hashes into a chained
  state and doesn't sample during TX.

### P2 and P3 (fixed in 0.3.5)

Bench results: the full suite passes, 45 tests including the opt-in wrong-key test. That covers the new
scenarios (`test_remote.py`, `test_options.py`) and the stricter invariant checks, which caught one more bug (the
heartbeat item under Firmware). Two more suite fixes came out of the runs:
- `Board.close()`/`GateSim.close()` now join the reader thread before closing the port. Closing it under a
  blocked read crashed Python (access violation in pyserial) on the reopen after a reboot.
- The wrong-key test no longer restores the saved timings before `key.set`. Older firmware needed that, but now
  it stretched the house's link timeout past the test's wait.

- [x] **Firmware**
  - **Radio faults:**
    - A radio reset seen while receiving (out of LoRa mode) is now logged as `radio_fail` a=2 and the radio is
      re-initialised.
    - A failed `radioBegin()` is retried every 5 s, and a success is logged as a=3.
    - An all-0xFF SPI read is no longer taken as TX done.
  - **Gate `cause` follows the direction of travel.** Leaving the target limit counts as `lora` only when our
    pulse moves the gate off it (`leaving`, after a reversal). Reaching the opposite limit ends the command with
    `timeout`, so the house resyncs at once. A relay test sets no target if the gate is already at that limit.
  - **House settle window:** a matching IN1 edge can't end it before `ctrl_settle_ms` (`settleUntil`).
  - **Heartbeat in STATUS** (`ST_HEARTBEAT`, `ST_LEN` 16; both boards need 0.3.5). The house's link timeout is
    `max(link_timeout_s, 2.5 × gate heartbeat_s)` (status `link_timeout_eff_s`, `remote.heartbeat_s`). The
    one-board cross-check is gone. **Found by the new test:** the gate now sends a STATUS as soon as its
    `heartbeat_s` changes. Before, the house's old timeout ran out while it waited for the new, longer heartbeat.
  - **Remote and saved config:**
    - `inN_invert` and `cmd_ttl_s` are no longer remote-writable.
    - A remote write saves only that param (`configSaveParam`), and `key.set` saves only the key.
    - `remote.set` needs an integer value and is refused with `busy` while one is still pending.
  - **Console:**
    - The line buffer is 1024 characters.
    - An overlong line is answered `line too long`, and bad JSON `bad json`. Both replies carry the request id
      when it can be found in the line.
  - **Cleanup:**
    - Shared signed `elapsed()` (`link.h`), now also used by `updateLed` (which uses the house's effective
      timeout).
    - `mismatchSince` can no longer be 0 when running.
    - `RES_BUSY` and `linkCancel()` removed (ACK code 3 is reserved).
    - The ACK memo is searched once.
    - `static_assert` on the log names.
    - `startRx()` no longer writes the FIFO pointer.
- [x] **Web console**
  - **Fixes:**
    - The poll timer can't leak.
    - Write errors are caught.
    - Remote diagnostics time out after 10 s.
    - Device strings are escaped.
  - **Config:** Apply/Import sends at most 8 params per `config.set`.
  - **Wording:**
    - The `in1_invert` advice is replaced by "fix polarity in the wiring, keep 0".
    - The heartbeat/timeout help is updated, and a "Gate heartbeat / link timeout" row was added.
  - **Accessibility:** ARIA tabs with keyboard support, labelled inputs, and a non-modal `role="status"` toast
    instead of `alert()`.
  - **Pages workflow:** runs `node --check` before deploying.
- [x] **e2e suite and GateSim**
  - **Test profile:** pins `retries`, `debounce_ms`, `sync_window_ms`, `resync_ms`, `ctrl_confirm_ms` and
    `ctrl_settle_ms`, and `config.set` is sent in chunks.
  - **Invariant checks:**
    - Each gate pulse must follow a matching `cmd_rx` or a recorded relay test.
    - GateSim reports any OPEN/CLOSE overlap as `pulse both`, and prints `release <input> <ms>`, so pulse
      lengths are measured (±80 ms).
    - Any house status with K2 closed while the gate isn't closed fails the test.
  - **New scenarios:**
    - `remote.set`, its refusal for local params, and `busy`.
    - `remote.diag`.
    - Slow gate heartbeat, and the console line limit.
    - Gate relay tests, including at the open limit.
    - `power_sense=0`, `linkloss_open=0`, `sensor_invert=1` and `ctrl_sync=0`.
  - **Robustness:**
    - Teardown and startup always close ports and restore config.
    - Late replies are dropped.
    - Timings come from the profile, and there's a `Bench.ping()` helper.
    - Opt-in `GATELINK_HA_CA` for HA certificate checking.
    - README test table updated.

## Bench suite findings

Issues flagged by the bench end-to-end suite (`tests/e2e`), first runs on 2026-09-30. The suite-side fixes and
firmware 0.3.2 landed on 2026-10-01; what's left needs hardware, the install site or a person.

### Firmware

- [x] **Boards re-received the frame they had just accepted** (`replay` with a == b, ~30 per run).
  - **Cause (measured):** a FIFO re-read, not a second reception. The SX127x packet and header counters didn't
    move, and the RSSI was identical. The LoRa lib's `parsePacket()` clears IRQ flags mid-packet; RX_DONE then
    sometimes survived its clear, so the next poll returned the same packet again.
  - **Fix (0.3.2):** `radioReceive()` does RX-single with its own register access and touches the flags only in
    standby. It went from 0 replays in 4 min of pings vs ~25 before. `Bench.check_invariants` now fails on any
    replay.
- [x] **Commands gave up long before `cmd_ttl_s`.** The TTL governs now: the `retries` resends double their gaps
  across the whole TTL (about 0.3, 0.9, 2.2, 4.7 and 9.7 s at 10 s and 5 retries), and the slot gives up when the
  TTL ends. STATUS's TTL is `heartbeat_s` capped at 10 s, so its first retry stays quick. New test:
  `test_long_outage_command_delivered_within_ttl` (4 s outage). An outage longer than ~4.7 s leaves only the last
  retry (~9 s), so one lost frame then drops the command; raise `retries` if that matters.
- [x] **Found by the new RF test: at SF12 the gate never verified the house.** Its STATUS retries, sent while it
  wasn't verified yet (so their ACKs were dropped anyway), kept the channel busy, and the HELLO_ACK never got
  through. Reliable slots now hold until the peer is verified. SF12 at 2 dBm now verifies in about 4 s.
- [x] **Slow resync after a house boot with the controller out of step.** After a boot or controller power
  return, the house resyncs as soon as the settle window (`ctrl_settle_ms`) closes, instead of after
  `mismatch_timeout_s`.
- [x] **Frame loss on a strong link** (0.3.3, 2026-10-01; measured, see below).
  - **Main cause: RX-single.** It gives up after 100 symbols and sits in standby until the loop re-arms it;
    a frame whose preamble straddled that moment was lost. The radio now stays in RX continuous.
  - **Also fixed:** RX is re-armed inside `radioSend()`, because a USB write could stall the loop for ~40 ms.
    Listen-before-talk with response priority and a random backoff. A board whose HELLO was lost now retries
    the handshake by itself. 0.3.2's slot hold had removed the traffic that used to trigger a retry, so the
    link could stay down until a command was sent. That bug is in 0.3.2 on `main`.
  - **Results:**
    - Gate receives 298/300 pings at SF9 (up to 8 % lost along the way).
    - SF12 / 2 dBm: 19/20 pings in the RF test, was 11/20.
    - Gate reboot → session in 3 s, was 8.4 s.
- [ ] **The house still misses ~2–3 % of pongs at SF9.** Only the house board has shown CRC errors, and moving
  the channel to 903 or 925 MHz made no difference. Suspect: the bench Shelly's Z-Wave radio (916 MHz,
  centimetres away) blocking the house receiver in bursts. Check with the Shelly moved away, and at the
  install site (`-m rf`).

### Test suite

- [x] **Timeline timestamps** use `time.perf_counter()`.
- [x] **Controller SW mode** is checked in preflight (`test_controller_follow_mode`).
- [x] **Key recovery:** `--restore-key` (README "End-to-end tests").
- [x] **README bench checklist** names the test that automates each item and keeps a "still manual" list.
- [ ] **Controller power loss is still simulated** (house `in2_invert`). The code is ready: wire the IN2 opto to
  the Shelly's 12 V on an HA smart plug and set `GATELINK_HA_POWER_ENTITY`. Then run `test_controller_faults.py`
  and note which drops first (summary "Link" section).
- [ ] **Real RF:** `test_rf.py` (`-m rf`) passes at the bench with antennas on: SF12, 2 dBm, 3 cycles. Still to
  do: run it with an attenuator or the antennas off, and at the install site, and check ping/RSSI there from the
  web console.
- [ ] **Long soak:** a 15-minute smoke run passed (43 scenarios, no resets, radio faults or replays). Still to do:
  run `pytest tests/e2e -m longsoak --soak-minutes 120` (or longer) and check `soak_counters.csv` for drift.

### Bench / housekeeping

- [ ] **Record the link key.** It was rotated on 2026-09-30 so the wrong-key test could run. The new key is in
  `~/.gatelink_key` (not in the repo). Store it somewhere safe, e.g. a password manager; boards can't read it
  back, and the web console's config export doesn't include it.
