# TODO

Issues flagged by the bench end-to-end suite (`tests/e2e`), first runs on 2026-09-30. The suite-side fixes and
firmware 0.3.2 landed on 2026-10-01; what's left needs hardware, the install site or a person.

## Firmware

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

## Test suite

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

## Bench / housekeeping

- [ ] **Record the link key.** It was rotated on 2026-09-30 so the wrong-key test could run. The new key is in
  `~/.gatelink_key` (not in the repo). Store it somewhere safe, e.g. a password manager; boards can't read it
  back, and the web console's config export doesn't include it.
