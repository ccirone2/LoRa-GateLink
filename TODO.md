# TODO

Issues flagged by the bench end-to-end suite (`tests/e2e`), first runs on 2026-09-30.
Results of those runs:
- Full suite: 31/31 passed, wrong-key test included.
- Soak: 20 cycles, 40/40 moves.
Per-run timelines are in `tests/e2e/results/` (not committed).

## Firmware

- [ ] **Boards re-receive the frame they just accepted.**
  - **Symptom:** about 30 ms after accepting a frame, the peer sometimes receives the same frame again, logged as
    `replay` with a == b. There is no link-level retransmission and no `radio_fail`.
  - **How often:** 31–34 times per 8–9 minute run, on both boards.
  - **Effect today:** the frames are rejected safely and nothing is processed twice. But the `replay` counter grows
    and reads like an attack.
  - **Suspect:** the RX-single re-arm in `radio.cpp` `radioReceive()` / LoRa `parsePacket()` polling.
  - **After a fix:** make `Bench.check_invariants` (`tests/e2e/gatelink/bench.py`) fail on a == b replays too.
    Today they're listed under "Anomalies" in `summary.md`.
- [ ] **Commands give up long before `cmd_ttl_s`.**
  - With `retries` = 5 the CMD slot exhausts its attempts about 4 s after `cmd_sent` (bench: dropped at 4.1 s),
    while `cmd_ttl_s` is 10 (also the default).
  - A radio outage of 4–10 s therefore drops a command that the TTL suggests would survive.
  - Options: decide which limit should govern, then scale the CMD retries/backoff to the TTL, or document that
    `retries` is the real limit.
- [ ] **Slow resync after a house reboot with the controller out of step.**
  - When the controller disagrees with the gate at boot, the house waits the full `mismatch_timeout_s` after arming
    before resyncing: 27 s on the bench with 20 s, so about 80 s at the default 75 s. Alarm.com shows the wrong
    state for that long.
  - Consider resyncing as soon as the first status has settled after boot.
- [ ] **One slow gate → house status in the soak.** It took 0.83 s; the median is 0.14 s. It looks like a lost
  STATUS or ACK followed by a retry. Low priority: watch whether it grows at install range.

## Test suite

- [ ] **Timeline timestamps:** `time.monotonic()` has about 15.6 ms resolution on Windows, which produced a
  negative latency sample (cmd_sent → pulse −0.015 s). Switch `Timeline` to `time.perf_counter()`.
- [ ] **Shelly SW mode is assumed, not checked.** Normal-operation tests assume follow mode: they expect no
  `resync` after K1 changes. Add a preflight check that flags toggle mode clearly.
- [ ] **Controller power loss is simulated** with house `in2_invert`, because IN2 isn't wired on the bench. Wire the
  IN2 opto and a switchable 12 V supply (e.g. an HA smart plug), so the real relay-drop-before-opto race is tested.
- [ ] **Radio outages are simulated** by changing the gate's `net_id`. Add a real RF test: antenna off or an
  attenuator, and a marginal-link run at low `tx_power`/high SF. Repeat ping/RSSI at the install site.
- [ ] **The wrong-key test rewrites the gate's saved key.** If it is interrupted mid-test, the gate keeps a random
  key. Add a recovery helper, e.g. a `--restore-key` option that re-applies `GATELINK_KEY` to both boards.
- [ ] **Longer soak:** hours, with periodic outages and opener faults mixed in, and watch for watchdog resets and
  counter drift.
- [ ] **README bench checklist:** mark the items the suite now automates, so the manual list only keeps what still
  needs eyes on hardware (LEDs, web console UI).

## Bench / housekeeping

- [ ] **Record the link key.** It was rotated on 2026-09-30 so the wrong-key test could run. The new key is in
  `~/.gatelink_key` (not in the repo). Store it somewhere safe; boards can't read it back, and the web console's
  config export doesn't include it.
