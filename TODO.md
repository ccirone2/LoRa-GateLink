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

### P2 — firmware

- [ ] **A radio reset during RX is never detected** (`radioReceive`).
  - **Problem:** after a reset the SX1276 is in FSK mode, and `startRx()` can't switch it to LoRa outside Sleep.
    So `radioReceive()` re-arms forever, `radioChannelBusy()` reads false, and nothing is counted.
  - **Scenario:** the house mostly listens, so it can go deaf with no `radio_fail`.
  - **Fix:** if `REG_OP_MODE & 0x80` is clear, count a fault, log `radio_fail` a=2, and call `radioBegin()`.
- [ ] **A failed `radioBegin()` is permanent** (`radio.cpp`). `ok` stays false until a reboot. **Fix:** retry
  every 5–10 s while `!ok`.
- [ ] **Gate `cause` ignores the direction of travel** (`role_gate.cpp` `gateLoop`, `gateRelayTest`).
  - **Problem:** any move to `between` while `target` is set is labelled `lora`, even when the gate leaves the
    target limit.
  - **Scenarios:**
    - A K1 relay test while the gate is already OPEN, then an AES close within `travel_timeout_s`.
    - An OPEN overridden to CLOSED by the siren, after which the next external open reads `lora`.
  - **Fix:**
    - Count `between` as ours only when the previous state wasn't the target.
    - Clear `target` (with `TR_TIMEOUT`) when the gate reaches the opposite limit.
    - In `gateRelayTest`, don't set a target the gate is already at, and reset `lastResult`.
- [ ] **The first matching IN1 edge closes the settle window early** (`role_house.cpp` IN1 handling).
  - **Problem:** at boot or on controller power return, the `ctrl_settle_ms` window ends as soon as the Shelly
    matches `syncExpect`.
  - **Scenario:** a Shelly that restores its Z-Wave state or chatters a few seconds later has that edge sent as a
    command. This goes against the purpose of the settle window.
  - **Fix:** track `settleUntil` separately and allow the early close only after it has passed.
- [ ] **`heartbeat_s` vs `link_timeout_s` is checked on the wrong board** (`paramSet`).
  - **Problem:** only the gate uses `heartbeat_s`, and only the house uses `link_timeout_s`, but each board checks
    its own pair.
  - **Scenario:** gate `heartbeat_s=60` with house `link_timeout_s=100` gives false link loss, and K2 fails open.
  - **Fix:** have the house use `max(link_timeout_s, 2.5 × the gate's heartbeat)`, learning the heartbeat from
    STATUS/DIAG.
- [ ] **Input inverts are remote-writable** (`PARAMS`: `in1_invert`–`in4_invert` have `P_REMOTE`). CLAUDE.md
  says to keep them at 0. A remote `in2_invert=1` on the gate makes a cut closed-limit wire read CLOSED.
  **Fix:** drop `P_REMOTE` from them. `cmd_ttl_s` is also `P_REMOTE` but the gate doesn't use it.
- [ ] **A remote `CFG_SET` or `key.set` also saves unsaved console edits** (`configSave()` writes the whole
  `cfg`).
  - **Scenario:** a bench `in1_invert` flip gets persisted.
  - **Fix:** save a copy of the persisted config with only that field changed.
- [ ] **Small console and link fixes:**
  - `remote.set` turns a missing or non-integer `value` into 0. **Fix:** require `is<int32_t>()`.
  - A second `remote.set` replaces the CFG slot with no callback, so the first never gets a `remote_set` event.
    **Fix:** reject it while `linkPending(SLOT_CFG)`, or fail the old request.
  - An overlong console line (over 383 chars) gets no reply, and `bad json` replies have no `id`, so callers
    just time out. **This affects users:** after a firmware upload wipes the config, importing a saved config in
    the web console sends nearly every param in one `config.set`. That line is too long, so the import times
    out (found while re-flashing the bench). **Fix:** reply with an error, raise `LINE_MAX`, and/or have
    `applyConfig` send changes in chunks.
  - The `updateLed` link-up check is unsigned (`GateLink.ino`), against the timing rule; it's only cosmetic.
  - A `mismatchSince` of exactly 0 collides with the "not started" sentinel. **Fix:** OR it with 1, like `armAt`.

### P2 — web console

- [ ] **The status poll timer can leak** (`app.js` `openPort`). `setInterval` runs after the awaited initial
  queries without checking the port is still open, so a drop mid-connect leaves an orphan 2 s poll forever.
  **Fix:** `if (port !== p) return;` before it.
- [ ] **The Install tab says "Set `in1_invert` if ON and OFF come out reversed"** (`WIRING`, house IN1). That
  contradicts the keep-at-0 rule. **Fix:** tell users to fix polarity in the wiring, and add "keep 0" to the
  invert HELP text.

### P2 — e2e suite

- [ ] **The test profile doesn't pin timings the scenarios rely on** (`bench.py` `PROFILE_*`).
  - Missing: `debounce_ms` (the limit-chatter test needs >30 ms), `ctrl_settle_ms`, `ctrl_confirm_ms`, `retries`,
    `sync_window_ms` and `resync_ms`.
  - **Fix:** add their defaults to the profile.
- [ ] **The "pulse without a command" invariant can be masked** (`check_invariants`). Every gate `cmd_rx`
  raises `pending`, including refused (`RES_NO_POWER`) and already-there commands that never pulse, so a later
  stray pulse is absorbed. **Fix:** require each `pulse` to follow a matching `cmd_rx` directly.
- [ ] **The interlock check rarely fires** (GateSim reports `pulse both` only when both press edges land in the
  same loop pass). **Fix:** report it whenever one input is pressed while the other is still active.
- [ ] **Pulse length isn't measured.** The check compares the firmware's own logged `pulse_ms`, so a relay
  that stays held passes. **Fix:** have GateSim print the release time, and assert it.
- [ ] **No continuous check of the K2 invariant.** **Fix:** flag any house status event in the timeline with
  `k2` closed while the gate isn't `closed`.
- [ ] **Untested features:**
  - `remote.set`/`remote.diag` over LoRa, including P_REMOTE gating.
  - A gate `relay.test` and its cause/target (`check_invariants` would currently flag it as a stray pulse).
  - `power_sense=0`, `linkloss_open=0`, `sensor_invert=1` and `ctrl_sync=0`.
- [ ] **Session teardown isn't protected** (`conftest.py` `bench` fixture).
  - **Problem:** only `AssertionError` from `baseline()` is caught.
  - **Scenario:** a `GateSimError`/`BoardError` skips `restore()`, so sim travel stays at 8 s in EEPROM and
    the ports stay open.
  - **Fix:** catch `Exception`, and put `restore()`, `dump()` and the closes in `finally`.

### P3 — cleanup

- [ ] Firmware:
  - `transmit()` ignores `radioSend()`'s result, so failed sends still count as `tx` and still use up a retry.
  - An all-0xFF SPI read counts as TX success. **Fix:** also check `OP_MODE` or `REG_VERSION`.
  - Dead code: `linkCancel()` and `RES_BUSY` (never produced).
  - The ACK memo is searched twice (`resendAck` plus the lookup). `linkAck` and `resendAck` build the same
    payload.
  - `log.cpp` `NAMES[EV_COUNT]` can't catch a missing name, which would emit `null`. **Fix:** declare it `NAMES[]`
    and `static_assert` its size.
  - Move the gate's signed `elapsed()` to a shared header and use it in `role_house.cpp`, which inlines the
    pattern about 6 times, and in `updateLed`.
  - `link.h` comments are out of date: they say "seq higher than the last accepted" (it's a 32-frame window),
    and that the STATUS/DIAG formats are in role_gate.cpp (they're in `roles.h`).
  - `startRx()` writes `REG_FIFO_ADDR_PTR` needlessly.
- [ ] Web:
  - `writer.write()` isn't awaited or caught, so a USB drop gives an unhandled rejection and a 4 s timeout.
  - Remote diagnostics have no client-side timeout, because DIAG is unreliable.
  - Device strings go into `innerHTML` for param options and attributes. **Fix:** use `esc()` or `textContent`.
  - Accessibility: there are no tab ARIA roles, some inputs have no label, and `toast()` uses a blocking
    `alert()` even for "Saved".
  - `pages.yml` deploys without running `node --check web/app.js` first.
- [ ] Tests:
  - Serial ports leak on startup errors (`find_boards`, `sim.open()`, `Bench(...)` outside the try).
  - Timings are hard-coded in `test_link_faults.py`, `test_opener_faults.py` and `test_soak.py`; derive them from
    `PROFILE_COMMON`.
  - The ping/pong loop is copied three times; add a `Bench.ping()` helper.
  - Late replies stay in `_replies` forever.
  - The HA client disables TLS verification; add an opt-in CA file.
- [ ] README: the test table doesn't mention the `-m soak` test (`test_soak_open_close_cycles` in
  `test_normal.py`).

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
