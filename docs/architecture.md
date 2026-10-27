# Architecture

How the firmware and the web console are built, layer by layer: the design, the numbers measured on the bench, and
the approaches already tried and abandoned, with why ("don't go back to ..."). [CLAUDE.md](../CLAUDE.md) has a
one-line summary of each layer and the behavioural invariants every change must keep; read a layer's section here
before changing it, and change the section with the code. The overview of the radio link and the wire formats is
[protocol.md](protocol.md), the console contract [console.md](console.md), every setting [config.md](config.md).

`firmware/GateLink/` is one Arduino sketch flashed to both boards; the role (house/gate) is stored in the flash
config. Layers, bottom up: `radio.cpp`, `link.cpp`, the roles, the history, `health.cpp`, the console, the config,
and `app.cpp` with the rest. The [web console](#web-console) is last.

## `radio.cpp`

Wraps the LoRa lib in **polling** mode. The radio sits in **RX continuous** with its own register access:
`radioReceive()` touches the IRQ flags only once RX_DONE is up and clears only the bits it read; after TX-done the
radio goes straight back to RX from the DIO0 interrupt (`onDio0`; `SPI.usingInterrupt` masks it during every bus
transaction), since the peer answers 25 ms after our frame and a loop stalled on USB writes would miss its preamble.

Don't go back to the lib's `parsePacket()` (it clears IRQ flags mid-packet, and RX_DONE then sometimes survived its
clear, so the same FIFO contents were returned twice: `replay` a == b) or to RX-single (it times out every 100
symbols and a frame straddling the re-arm was lost: ~2 % at SF9).

`radioChannelBusy()` is the listen-before-talk check: `RegModemStat` & 0x0B (bit 2 is always set while receiving) or
an unread RX_DONE.

TX is **asynchronous**: `radioSend()` starts the frame (async `endPacket(true)`) and returns; `radioTxBusy()` polls
TX-done as a fallback (FSTX, 0x82, right after `endPacket()`, counts as transmitting) and enforces a deadline
(airtime + 200 ms) via its own SX127x register reads (the lib's are private); a stuck or reset radio is logged
(`radio_fail`, a=1), counted (`radio_faults`) and left down until `radioRecover()` re-initialises it, as it retries
every 5 s a radio that didn't start: `appLoop` calls it only while no relay pulses (`LoRa.begin()` stops the loop
~0.5 s). Don't go back to waiting for TX-done: at SF12 a frame takes seconds, so it stretched gate relay pulses by
the ACK's airtime and back-to-back frames could reach the watchdog; and never to the lib's blocking `endPacket()`
(it spins forever if the radio resets mid-TX). The loop must stay non-blocking.

`radioRandom32()` samples wideband RSSI in continuous RX (never leaving RX, which would drop a frame) and hashes it
into a chained state; don't replace it with `LoRa.random()` alone (near-constant in standby).

## `link.cpp`

Every frame on the air uses authenticated framing: `ver|type|net_id|src|dst|session|seq|payload|tag`, tag =
HMAC-SHA256(key) of the header and the encrypted payload, truncated to 8 bytes. The link is disabled entirely while
`cfg.key_set == 0`.

**Encryption** (frame format 2, 0.16.0). `cryptPayload` XORs the payload with AES-128-CTR (the Crypto library's
`AES128`, its key the first 16 bytes of the link key's HMAC of `GateLink payload v1`): `buildFrame` encrypts before
tagging, `handleFrame` checks the tag first and only then decrypts in place, and `reframePending` decrypts a queued
frame under its old seq before renumbering it. The counter block is the header's bytes 3–12 (`src`, `dst`,
`session`, `seq`), so a keystream repeats only if a session id does; the node ids in it keep the directions (and the
gates of a house serving several, ROADMAP F8) apart. The cipher key is derived again whenever `cfg.key` differs from
the copy it was derived from (`cipherKeyOf`), not only in `linkBegin`: a frame encrypted under an old key and tagged
under a new one would pass the peer's MAC and be parsed as garbage. About 2 KB of flash and 100 B of RAM with the
challenge below. Type, length and timing stay visible: a listener can tell a command went and that the gate's state
changed, not which (padding to one length wouldn't hide it, since STATUS goes on every change; not done).

**Sessions and replay protection.** Replay protection has no persisted seq counters: each boot (and link restart)
picks a new session id; a peer session is accepted only after a HELLO challenge is echoed in HELLO_ACK, and from
then on only seqs above that HELLO_ACK's pass (then a 32-frame sliding window), so frames recorded before a reboot
can't be replayed. Challenges are 16 bytes (`drawChallenge`: the link key's HMAC of our session, a count and one
`radioRandom32()`, so one draw costs 20 ms of RSSI sampling, as the 32-bit one did): with 32 bits, answers to the
challenges an attacker provokes by replaying stale HELLOs, recorded over weeks, came to match a later one within
hours and re-verified a superseded session (`challenge-library-session-rebind`). Binding the answer to the
challenger's session, or refusing a session superseded since boot, would add nothing with 128 bits: not done.
Answering a HELLO renumbers everything still waiting (`reframePending`; the renumbered resend
escapes the peer's ACK memo, so reliable messages must be idempotent: CMDs by `lastCmdId`), except that a HELLO from
a new session after a verified one holds a pending CMD (`cmdHeld`, log `cmd_hold`): if that session verifies (the
peer restarted) the CMD is dropped, since the restarted gate may have pulsed and lost the ACK and its `lastCmdId`,
so a resend would pulse twice; if the verified session answers our re-challenge or sends a fresh frame, it was a
replayed HELLO and the CMD goes. HELLOs bypass the replay window, so each board answers at most one a second from
the verified session and one from others (`HELLO_ANSWER_GAP_MS`).

**Session ids** must never repeat under one key, so `drawSession` makes them unique by construction: a keyed
permutation (`permute`, a Feistel network on HMAC-SHA256 with the link key, node id and chip serial) of the boot
count (`configCountBoot`, sectors 2–3, append-only slots written in two steps so a torn one is skipped, 0 once it
runs out or for a garbled read (a slot reading 0); passed in by `linkSetBoot`; 20 bits), the draws this boot (4
bits) and 8 random bits, so a count that repeats anyway (flash rolled back) still gives another id; past those
ranges or without a count the id is random (`radioRandom32()`, seeded per boot with the count and the chip serial).
The session is redrawn if the radio was down when it was drawn (`sessionWeak`), and seq starts at a random 31-bit
value. A HELLO from an unknown session is challenged but doesn't drop the verified session until the new one answers
(a replayed HELLO can't take the link down), and re-verifying the same session keeps its window and ACK memo.

**Reliable messages** use per-type `Slot`s (CMD/STATUS/CFG) with retry/TTL; a new send replaces the slot. The TTL
governs: `retryDelay` doubles the gaps from TTL/32 but never beyond an even share of what's left, so the `retries`
resends (default 8) span the whole TTL (last one just before it) and an outage mid-TTL still leaves several, and the
slot gives up only when the TTL ends. STATUS's TTL is `heartbeat_s` capped at 10 s so its first retry stays quick.

**Link loss.** STATUS carries the gate's `heartbeat_s`, and the house declares link loss after
`houseLinkTimeoutMs()` = max(`link_timeout_s`, 2.5 × that heartbeat) without a STATUS (`houseStatusAt`, `appLinkUp`;
other frames don't count, and its own link restart, `houseLinkRestarted`, starts it over); it takes a STATUS only if
its seq is above the last one taken in that gate session (`linkPeerSession`), so one withheld and delivered late
changes nothing (log `replay`); the gate counts its own link down (LED, history, D5) after `gateLinkTimeoutMs()`,
the same with its own heartbeat and any frame from the house. Only the gate uses `heartbeat_s`, so there is no
cross-check between them on one board.

**Verification first.** Slots don't transmit until the peer is verified (their ACKs would be dropped anyway); at
SF12 those retries crowded out the HELLO_ACK and the gate never verified. So an unverified board repeats its HELLO
by itself (randomized gaps of base + up to base again, the base doubling to 8 s); nothing else may be on the air to
provoke one.

**Listen-before-talk.** One frame is on the air at a time (`txIdle()`); every transmission goes through
`clearToSend()`: responses (ACK, PONG, HELLO_ACK, DIAG) after the 25 ms turnaround, new frames only after the
response slot plus a random backoff drawn once per frame, measured from the last air activity (any RX_DONE,
`radioRxDoneCount`, so bad-CRC frames count too); a busy channel holds the frame (slots re-poll, unreliable frames
wait in a 4-deep queue with responses at the front; full, the tail goes, never the newest response) and after 2× the
longest frame busy without a break it is sent anyway (`lbt_forced`; a clear reading restarts that count); a frame
held back past `waitCapMs()` no longer waits out the gaps (frames heard every ~60 ms held them shut for good): it goes
as soon as a frame heard ends (within `CAPPED_SLOT_MS`, 3 ms; mid-gap it would run into the next one) or once its
backoff has passed after all, and a busy channel still holds it. Counters: status `link.lbt_defers`/`lbt_forced`.

**ACK memo.** Already-accepted retransmits are re-ACKed from an ACK memo, not re-processed, at most `REACK_MAX` (10)
times each since 0.19.1 (held copies, below, count too): as many resends as a sender can make (`retries` is at most
10), so a real one always gets its answer, while further copies are replays (counted, logged `replay`) and draw no
ACK, so a recorded frame replayed over and over doesn't make the board transmit for each. Replays sent before a real
resend can take its place, but an attacker who can keep the sender from hearing the first ACK could as well jam
every ACK (not one re-ACK a second, which the first resends, 0.3 s
apart, would run into). A message answered later
(`linkAckLater`: the gate's remote config writes during a relay pulse) holds its retransmits quietly until its
`linkAck`; one refused (`linkRefuse`: a fifth such write) is neither ACKed nor remembered, as if lost, so its resend
(the same seq) is taken later.

## Roles: `role_gate.cpp` and `role_house.cpp`

The application state machines; the rules they keep are the behavioural invariants in [CLAUDE.md](../CLAUDE.md). The
STATUS payload layout is defined in `roles.h` (built in `role_gate.cpp`, parsed in `role_house.cpp`); DIAG's too
(`DIAG_*`, `DiagCounter`), built in `role_gate.cpp` (`sendDiag`) and parsed in `console.cpp` (`consoleEventDiag`);
keep them in sync. The longest loop pass since boot is status `loop_max_us` (the soak fails it past 4 s).

**Command deadlines** (0.16.0, [protocol.md](protocol.md#command-deadlines)). The gate's clock (`gateClock`) is
`millis()` extended to 64 bits (`uptimeMs`, read every `gateLoop` pass so its base stays within 2^31 ms) plus
`clockOrigin`, 47 random bits drawn in `gateBegin`; STATUS carries it (`ST_CLOCK`, 48 bits) and its uptime fields come
from `uptimeMs`, so they no longer wrap at 49.7 days either. The house keeps one reading per gate session
(`clockSession`, `clockGate`, `clockAt`; `clockAt` is the handler's `millis()`, later than the loop's `now`, so every
age is signed): a reading replaces it unless it's behind by more than `CLOCK_DRIFT` (100 ppm) of the age, and
`capStamps` forgets it after `CLOCK_MAX_AGE_MS` (3 h). `sendCommand` stamps `CMD_DEADLINE` = the gate's clock now +
the slot's TTL; with no reading for the verified gate session (`linkPeerSession`, 0 during our own link restart too) it
returns false and the press stays in `pendingAction` (`clockWait`, log `cmd_hold` a=3) until there is one, its TTL
shortened by the wait, or `cmd_ttl_s` from the switch has passed (`cmd_dropped`). The gate sends STATUS as soon as its
link verifies the house, a new session or after its own restart (`houseSession`, `houseVerified`), so that wait is a
second or two. `handleCmd` checks the deadline after the duplicate check (a resend of a command already run gets its
result), with a slack of `CMD_LATE_SLACK_MS` plus a STATUS's and a CMD's airtime, and refuses with `RES_LATE`, which
the house treats as `RES_NO_POWER`: resync at once. CMD stays 9 bytes (a 30-byte frame): the 4 more a gate session id
would take made the frame 34 bytes, which no longer fit the gaps of a neighbour sending every 100 ms; the random
origin binds a deadline to its boot instead. Don't use 32 bits for the deadline (a command withheld 2^32 ms would read
as current, host-tested) nor a time window on the gate alone (it can't tell a delayed frame from a retry).
**Missed moves** (0.17.0, ROADMAP F17 when it was one). The gate counts each time its reported state leaves
`closed` (`leftClosed`, STATUS `ST_LEFT_CLOSED`, a byte: every departure the house would have shown as not closed,
`no_power` and `fault` too). The house keeps the latest count (`gateLeft`) and the count when the contact sensor last
went closed (`leftWhenClosed`, in `applyOutputs`); a STATUS from the same gate session (`statusSession`) that reads
closed with the sensor still closed (`sensorClosed`) and the count past that one opens the sensor for
`MISSED_OPEN_MS` (5 s, `missedUntil`, expired in `applyOutputs`). The sensor's decision, not its open moments: at the
first STATUS after a link loss `applyOutputs` still sees the link down for a pass, and a sensor that was open while
the gate left closed again (the house last heard it open) showed the alarm all it needed. A new gate session takes the
count as it comes: a reboot starts it over (and a link restart without a reboot, rare, gives up a move hidden just
before it), and a 0.16 gate's 32-byte STATUS carries none. It drives K2 only: K1 (the Alarm.com switch) still shows the real state, and
D5 isn't involved (the move isn't a fault; the link history shows the outage).

**A closed limit that has read alongside the open one** (0.17.1, ROADMAP F14). Both limits reading at once is
`fault`, and once they have for `BOTH_LIMITS_HOLD_MS` (500 ms) `readState` keeps it `fault` while the closed limit
reads, alone too, until the open limit has read alone
(`closedUnproven`, updated in `gateLoop` as the limits are read; since 0.17.4 kept through a crash, a watchdog or brown-out reset, in the reset memo's `kept` flags (`appKeep`), but a deliberate restart (the console's `reboot`, the reset pin, an upload) or a power-on forgets it). The closed limit is the AUX2 relay's NC contact
([hardware.md](hardware.md)): it reads closed whenever that relay isn't energized (a failed coil or driver, the AUX
setting left at "always off"), wherever the gate is. Before this a stuck one read `fault` only at the open limit and
`closed` everywhere else, mid-travel too; now it is `fault` from the first full opening on, and the house shows not
closed. Which of the two lied can't be told, so a stuck open limit read at the closed one is held the same way (a
nuisance only until the next full opening); a shorter overlap (a glitch, the relays switching over) isn't held. Without
AC a held `fault` refuses commands as any `fault` does: the position can't be verified. Only the open limit reading alone clears it, since it is wetted by the
same 24 V: the closed one reading off with nothing else (`between`) may only be that 24 V gone. Before the first full
opening a stuck closed limit can't be told from a closed gate: the second closed sensor below is the answer to that.
Considered and not built (F14, 2026-10-15): a fault when the closed limit stays on after our OPEN pulse (a gate that
doesn't move, the opener locked out or another controller holding CLOSE, is far likelier than a stuck limit and a gate
stopped short of open, and the travel timeout already reports it: `last_result` `timeout`).

**A second closed sensor** (0.20.0, F12, `closed_sense`, off by default). Gate IN4 takes a reed and magnet on
the leaf, wetted from the opener's accessory 24 V like the limits, so it shares nothing with the AUX2 relay but that
supply. With `closed_sense` on, `readState` reads closed only while the closed limit and IN4 both read; either alone
is no closed limit (the state is what the others make it: `between`, `open` or `no_power`), and once they have
disagreed for `CLOSED_DISAGREE_MS` (5 s, `disagreed`, set in `gateLoop` after the spare inputs are read) it is
`fault` until they agree. A false closed then needs two failures, the limit relay and the sensor. Normal travel parts
them only between their switching points (the sensor mounted to read in the last inches), and the gate reads not
closed meanwhile, so the 5 s costs only the diagnosis: a failed limit relay with the gate part-open, a sensor knocked
off or a cut wire with it closed, a sensor stuck on as the gate leaves closed. The both-limits hold, `fromOpen` and
the boot settle still take the closed limit alone: they are about the AUX relays. Not remote-writable: whether IN4
has a sensor is the install's wiring, and a remote write could otherwise drop the second check. Considered and not
built: a sensor-only closed (the opener's own limit then counts for nothing), and a disagreement threshold as a
setting (a constant until an install shows a sensor that can't be mounted close enough).

**A closed limit with no travel before it** (0.17.2). No travel goes from one limit to the other without reading
`between`, but the opener's controller can make it look so: restarting, it drops its AUX relays, so the open limit
releases and the closed one's NC contact reads, for as long as the restart takes (unverified on the CSW24UL; TODO.md
has the field check). Both cases go to not closed:
- **Straight from open:** `readState` reads `fault` while the closed limit reads and `fromOpen` is set. `fromOpen` is
  set once the open limit reads alone and cleared once neither limit has read for `BETWEEN_HOLD_MS` (`neitherSince`).
  Relays dropping together read both or neither for a moment, which doesn't count as travel. It holds however long the
  restart takes, since a delay would have to guess that length. It isn't latched like `closedUnproven`: a real move
  afterwards (neither, then closed) is trusted.
- **Out of `no_power`:** the restarting opener's 24 V comes back before it energizes AUX2, so the closed limit reads
  for a moment wherever the gate is. A move into closed waits `BOOT_SETTLE_MS`, as one into `between` does
  (`heldAt`), started over whenever the held reading changes (`heldTo`): a closed limit that reads 2.8 s after IN3
  gets the full 3 s, not what is left. It is held whatever IN3 reads, since IN3 can settle after the limit. It isn't
  latched, since a gate that was closed through the outage is the common case.

Cost: every closed out of `no_power` reports 3 s late. That covers a supply blip at the gate, and a close the opener
drives on battery during an outage. The house shows not closed meanwhile (K1 energized, K2 open), as it already did
for the `no_power` before it.

Left open (not built, 2026-10-15): the opener's own 24 V dropping while IN3 stays up (its fuse or breaker, a
controller reset that dips the accessory output). Neither limit reads for longer than `BETWEEN_HOLD_MS`, so it is
reported `between`, and a closed limit reading as the 24 V returns before AUX2 is trusted. Telling that from a real
close would need a minimum travel time, which isn't measured (TODO.md).

Considered and not built: deadlines on every reliable
frame in the link, with the peer's clock carried in HELLO_ACK. One reading a session drifts past the slack within
hours (100 ppm: 8.6 s a day), so it needs STATUS's readings anyway, and gate-to-house freshness would need the house's
clock too; STATUS has its own (seq order) and config writes rarely matter (`withheld-config-write`).

## History: `history.cpp` and `histlog.cpp`

Link quality history: a RAM ring of hourly buckets (96 + the one in progress; RAM-bound, as a `config.get` reply
takes ~5 KB of heap — check `free_ram` in status after changing it). `histPoll` diffs the `linkStats()`/radio totals
into the bucket in progress each loop (one frame per `linkPoll`, so the last RSSI/SNR are that frame's), samples the
noise floor (`radioNoiseDbm`; a reading is kept only if no frame followed it by the next one, since the modem flags
a preamble a few symbols late and peer frames showed up as noise peaks) and counts link-down seconds (`appLinkUp`,
shared with the LED and the house's `linkUp`). The house adds the gate's side from STATUS (`histPeer`).

Each completed bucket is appended to the SPI flash (`histlog.cpp`, sectors 4–19, on `flashlog.cpp`: config sectors
0–1, boot counter 2–3, event journal 20–35), once and never during a relay pulse (`histSave`, in `appLoop`), and boot loads the newest back, so a reset
loses only the bucket in progress (each bucket carries its `boot`). Diagnostics only: nothing may read it to make a
decision.

## `health.cpp`

The fault output D5 (`fault_out`), polled after the roles: it reads their state through `houseDecided`/`gateDecided`
and the like, and nothing reads it back. The problems it tracks are bits (`Problem` in `health.h`; status `health`
names them, log `health` carries them): the role hasn't decided yet since boot, the radio isn't working, the link is
down, no AC power at the gate, the gate reads `no_power` or `fault`, a reboot loop (this board's, `appRebootLoop`, or
on the house the gate's, from STATUS bit 7), the gate `between` for longer than its `travel_timeout_s` (`stuck`:
`gateStuck`, timed from leaving a limit or our last pulse; on the house `houseGateStuck`, from the STATUS that said so
and started over by any that may begin a move it didn't see begin, and not while the link is down), and on the house a dead IN2 (`ctrl_sense`: `houseCtrlSenseDark`, IN2 off
for `CTRL_SENSE_DARK_MS` (30 s) while the board's supply, which shares the controller's 12 V, reads good; with both
`ctrl_power_sense` and `ctrl_power_pmic` on and the charger answering). That last one is only reported: the controller
still counts as unpowered, as IN2 says. A real 12 V cut never shows it, since the supply drops ~0.2 s before the relay
and ~1.9 s before the opto, and on its return the opto lights well inside 30 s. What D5 does with them is a behavioural
invariant (CLAUDE.md).

## Console: `console.cpp` and `console_io.cpp`

Newline-delimited JSON request/response over USB serial (`{"id","cmd",...}` → `{"id","ok",...}`) plus unsolicited
`{"event":...}` lines; with `uart_console` also on Serial1 at 250 kbaud (1 Mbaud garbled ~4 % of requests, 250 kbaud
~1 % while busy, ~8 % sent back to back) (a USB-to-UART adapter for bench power tests: its port survives a power
cut). `console_io.cpp` is the transport (USB and UART); the protocol is `console.cpp`.

A request may end with a CRC-32 of itself (`"crc"` as its last member, `checkCrc`); a mismatch is refused `bad crc`,
and on the UART a request without one is refused, so the suite can send anything there while USB is down. Each
`ConsolePort` has its own request line; replies go to the asking port, events to both; `poll` handles at most one
request per port per loop pass (handling them while more kept arriving starved the loop into a watchdog reset).

Each line is serialized into a buffer and written in 64-byte pieces (`LineWriter`), to USB each only once the
endpoint's BK1RDY is clear: streamed per character, bytes went missing, and a longer write lets the core's
`USBDevice.send()` wait on a transfer-complete flag its own USB interrupt clears, which dropped the rest of the line
after 70 ms. Status `usb_cut` counts lines lost on USB.

A reply is built as one `JsonDocument` on the heap, whose high-water mark is what status `free_ram` reports. The
largest is `config.get` (its setting names linked, not copied), except that `log.get`'s ring of 64 entries is streamed
one entry's document at a time (`sendLogRing`, 0.19.1): built whole, a ring of entries from an earlier boot (each with
its `boot`) took another ~1 KB slot pool and left 1283 B. The suite's preflight builds each of these replies before
reading `free_ram`.

This is the contract the web console depends on (and its fake board in `tests/web/fake-serial.js`), as do the e2e
suite and `tools/gatelink_client/`; change them together, with [console.md](console.md) (`tools/check_contract.py`
checks the enums, log event names and commands against the suite and that doc).

## Config: `config.cpp` and `extflash.cpp`

`Config` saved in the on-board SPI flash (`extflash.cpp`, a small NOR driver), which **survives firmware uploads**.
The chip shares `SPI1`/SERCOM4 with the radio, and the radio module's own (unused) MCU garbles flash traffic unless
held in reset (`LORA_RESET` low), which resets the SX1276 too: so every flash access is wrapped in `FlashAccess`
(`config.h`: hold the module, then `radioRestart()`), and a save takes the radio off the air for ~0.5 s
(`LoRa.begin()`'s reset delays). Never hold `LORA_RESET` low permanently: the radio stops.

It's stored as (param id, value) pairs plus the key, in two sectors written alternately (newest seq wins, so a power
cut mid-save keeps the old record), read back after writing; once the new record verifies, a different key in the
older one is overwritten with zeros (`scrubOldKey`: that record then fails its CRC), so a key change leaves no old
key on the chip; on load, values out of range are dropped and ids this firmware doesn't know are kept aside
(`extras`) and written back by every save, so a downgrade doesn't lose a newer firmware's settings (both counted by
the `cfg` log event); missing params take defaults. So **PARAMS ids are permanent**: a changed meaning or unit needs
a new id. If the chip doesn't answer, config falls back to program flash (FlashStorage struct image,
`CFG_VERSION`-checked, erased by uploads; status `cfg_store`). `config.reset` erases both sectors.

The `PARAMS[]` table drives console get/set, the web form (via `meta`), and remote-over-LoRa writes (`P_REMOTE`
flag; radio params and `inN_invert` are never remote-writable). A remote write saves only that param on top of
what's in flash (`configSaveParam`; `key.set` uses `configSaveKey`), so unsaved console edits stay unsaved; these
re-read the record first (`persisted`) and refuse if it reads garbled twice or older than the cached `spiSeq`, since
a save on top of the wrong base would drop settings or lose to the newer record at the next boot.

Adding a setting = field in `Config` + default + `PARAMS` row with a new id (+ group/help text in
`web/js/settings.js`); the record must fit one 256-byte page (static_assert, 45 params). Bump `CFG_VERSION` when the
struct layout changes (only the fallback uses it).

## `app.cpp` and the rest

`app.cpp`: boot, loop, PING/PONG, dispatch by `activeRole`; status LED (`updateLed`: PWM on `LED_BUILTIN`; identify
strobe / solid = no role / breathing = link up / heartbeat = no link), spare inputs IN3/IN4 (`updateSpareInputs`,
called by both roles; reported in STATUS bits 4–5; on the gate IN3 is the AC power sense, IN4 the closed sensor with `closed_sense` on);
house IN2 is the controller power sense (`ctrl_power_sense`, handled in `role_house.cpp`, not reported to the gate),
the board supply (`supply.cpp`, polled every 5 ms before the roles; status `supply`, log `supply`), the reset cause
(`PM->RCAUSE`, logged in `boot` a= and reported as `reset_cause`), the reset memo, and the fault output D5
(`health.cpp`, after the roles). `activeRole` is latched at boot; `cfg.role` changes only take effect after reboot. Use
`activeRole`, not `cfg.role`, for runtime behaviour.

The reset memo (0.15.0, ROADMAP F9) is 16 bytes in RAM that resets other than power-on keep (`boardKeep`), checked by a
CRC-32 so a power-on's garbage reads as none. It carries three things to the next boot:
- **Where a watchdog reset hung.** The watchdog's early warning fires 4 s into a pass that hasn't kicked it, 4 s before
  the reset; its handler (`WDT_Handler` in `GateLink.ino`, then `appWatchdogWarning`) writes only the stacked program
  counter and `stage` (which step of `appLoop` was running, or `setup`) into the memo. After a watchdog reset the boot
  reports them (status `hang_stage`/`hang_pc`, log `hang`); any other reset ignores them, and a pass that runs again
  after a warning (a stall of 4–8 s that ended) drops them, so a later reset the warning never saw (interrupts off)
  isn't blamed on it. `hang_pc` is looked up in the build's `.elf` (`arm-none-eabi-addr2line`).
- **The role's kept flags** (0.17.4, `appKeep`/`appKept`): read back only after a watchdog or brown-out reset, so a
  crash doesn't drop what a deliberate restart should. The gate keeps its both-limits hold there
  (`KEEP_CLOSED_UNPROVEN`).
- **Reboot loops.** `crash_resets` counts watchdog and brown-out resets in a row, each within `REBOOT_SETTLE_MS` (1 h)
  of the boot before; a deliberate reset (software, the reset pin) or a power-on ends the run. Three or more, until
  this boot has run an hour, is `appRebootLoop()`: health `reboots`, so D5 stays LOW through the loop instead of
  flickering with each boot, and on the gate STATUS bit 7, so the house (whose D5 is the one usually wired) raises it
  too. The hour and the count are constants, not settings. A loop of power-on resets (a supply collapsing completely)
  isn't counted here; at the gate the house sees it as a link that keeps dropping.

The memo, and after it `log.cpp`'s ring, live in an orphan `.noinit` section (NOBITS, so neither loaded nor zeroed; ld places it after `.bss`): the
core's linker script has none, and the bootloader only uses the bottom of RAM and a stack below 0x20007C00. The
watchdog is driven directly (8 s, the early warning at 4 s, from a 1.024 kHz GCLK2), not through Adafruit SleepyDog,
whose `WDT_Handler` turns the watchdog off on an early warning. The 0.5 s double-tap wait in the bootloader runs before
any of this, on every reset but power-on (TODO.md, the house's warm-reset blip).

- `GateLink.ino` only reads the reset cause and chip serial, runs the watchdog and its early warning, places the kept
  RAM and calls `appSetup`/`appLoop`.
- `board.h` is what needs the chip: kick, reset, free RAM, the kept RAM (implemented in `GateLink.ino`, and by the host
  simulation in `tests/native`, which keeps the words across a reset and fills them with garbage at power-on, so
  everything from `app.cpp` down except the radio, flash, charger and console drivers builds on the PC).
- `supply.cpp`: the board's VIN power good, read from the BQ24195L charger over I2C (read-only).
- `io.cpp`: `Input` (debounced, internal pull-down, active HIGH) and `Relay` (non-blocking pulses, optionally
  delayed; `gapLeft` for the interlock).
- `log.cpp`: the event log, a ring of `LOG_SIZE` entries in the kept RAM (after the reset memo, all of it checked by a
  CRC), so a crash or a reboot loses none; written to the flash journal (`evlog.cpp`, sectors 20–35) in records of
  14 from `appLoop`, never during a relay pulse (`logSave`, after `histSave`: one flash access a pass), once 14 are
  waiting or the oldest has waited 10 min, at most once a minute so a flood can't keep the radio off the air; repeats
  of the flood-prone events coalesce into an entry of theirs among the newest 4 unwritten (`rep`; an input's keeps its
  latest level), and `logSave` counts what its own radio restart logs while it writes. The journal is paged by a
  cursor, a record's `lseq` (`log.get` `before`), reading only headers until the record it returns. Its two stamps are compared as unsigned
  ages (a quiet site may log nothing for weeks). The `LogCode` order in `log.h` and the names in `NAMES` are part of
  the console contract ([console.md](console.md), `web/js/logdecode.js`, the e2e suite). The kept ring isn't in
  arduino-cli's "Global variables" figure (nor `tools/fw_size.py`'s budget): ~1 KB more than it shows.
- `flashlog.cpp`: the record log both flash logs use (`histlog.cpp`'s history, `evlog.cpp`'s journal): slots in a run
  of sectors written in turn, erasing a sector on entering it; the newest found at boot by `lseq`; torn records passed
  over ([protocol.md](protocol.md#spi-flash)).
- `pins.h`: the pin map ([hardware.md](hardware.md) and the web console's `WIRING` table follow it). `crc32.h`: the
  CRC-32 of the config record, the history log's records and the console's request check.

## Verification

`tools/verify` proves the safety properties rather than testing them: ESBMC (a bounded model checker with a clang
front end) on the firmware's own C++, and TLC on a TLA+ model of the two boards and the air between them, with an
attacker in it. `make -C tools/verify` runs it all (CI's `verify` job; locally in `tools/verify/docker`). A proof that
fails prints its counterexample; the judge is the checker, not a test someone wrote to pass.

**On the C++ (ESBMC, `tools/verify/esbmc`).** Each harness `#include`s the firmware's `.cpp` files, so it checks the
code that ships, and answers everything around it nondeterministically: every setting anywhere in its `PARAMS` range,
every input on every read, any frame payload, length and seq, the house session changing, console relay tests, and
time (any gap between loop passes up to just under 2^30 ms while no relay pulses, 50 ms while one does; millis() from
anywhere, so every run crosses its wrap; a frame handler's millis() up to 20 ms ahead of the pass's `now`).

| Harness | Properties | Kind |
|---|---|---|
| `gate.cpp` | **interlock**: a gate coil never energizes while the other is, nor within `INTERLOCK_MS` of its release. **pulse-only**: a coil releases within its pulse's length plus one pass, and energizes only for a command, a relay test or a pulse waiting out the interlock. **no-restart**: a running or waiting pulse's start and end never move. **fresh-cmd**: a command pulses only inside its deadline by the gate's clock (`CMD_LATE_SLACK_MS` plus two frames' airtime late, `CMD_AHEAD_MAX_MS` early), stated in physical time since boot, not with the firmware's clock. **once**: the last command taken never pulses again. **no-stall**: no flash write, radio restart or log save while a relay pulses or waits to | inductive, any run length |
| `house.cpp` | **sensor-truth**: K2 closes the alarm's contact only while the last STATUS taken said closed, and (`linkloss_open`) no longer than `houseLinkTimeoutMs()` (+1 ms: stamps are `now \| 1`) after it was taken, as of the loop pass, across the wrap. **in-order**: a STATUS is taken only if later than the last one taken in the gate session | inductive, any run length |
| `link_window.cpp` | **once**: a frame reaches the role at most once per verified peer session unless the role refused it (`linkRefuse`). **after-hello**: nothing at or before the HELLO_ACK that verified the session does. Both with the MAC left to chance, so they hold even for forged frames: the window alone keeps them. The transmitter idle and the channel clear (nothing queued; the window never reads the sending side); one query per frame length | inductive, any run length |
| `house_cmds.cpp` | **owner-only**: every command the house sends is one the owner switched the controller to since its last command, and none goes before it is armed. Run in a model of the site: the Shelly (follows K1, toggles on SW edges, or detached; SW answered within 200 ms), its 12 V rail cut and restored at any moment, the board's power good, the IN2 opto and the Shelly's reboot each at any delay in a range (below) | bounded: 8 passes from boot |

The inductive proofs: `inv()` in each harness is an invariant over the relays (or the STATUS taken), ghosts in
physical time and the clocks. The harness checks that it holds after boot (`-DBOOT`: `appRelaysBegin`, the reset memo
from a crash that may have kept anything, `gateBegin`/`houseBegin`), and that each segment of a loop pass, from any
state satisfying it, with everything else in the firmware arbitrary (the gate's whole state machine, the house's sync
windows...), ends in one satisfying it again. The segments are the loop's cut points: the pass's start (Relay::update,
the radio and flash work `appLoop` guards), one frame handled, one console relay test, the role's loop. A ghost tracks
where in a pass a segment may start, so any real run is a sequence of them: the invariant, and the properties checked
inside every segment, hold for runs of any length, at every setting. Each harness also runs with `-DREACH`, an
assertion that fails if the end of the segment can be reached at all, and must fail: a harness whose assumptions
excluded every run would otherwise pass by proving nothing (the first draft did, cut by a loop bound).

**The protocol (TLC, `tools/verify/tla/GateLink.tla`).** Both boards' sessions, seqs, replay windows, commands and
deadlines, STATUS and the sensor, reboots (new sessions, RAM lost, a new clock origin), the gate moved by others; an
attacker who records every frame and delivers any of them, any number of times, at any later time, in any order, or
never, but can't forge one or answer a challenge (HMAC-SHA256, 128-bit challenges). Each action cites the firmware it
stands for; the boards' local steps are the contracts the ESBMC harnesses prove (the link's acceptance is
`link_window.cpp`'s, the gate's command checks `gate`'s, the sensor `house`'s). Properties: **SensorTruth** (the sensor
shows closed at most LAG after the gate left closed), **CmdOrder** (a command never pulses after a later one was taken),
**CmdOnce** (one command, one pulse, across reboots), **CmdFresh** (never past its TTL plus the slack). Each `.cfg`
states what it expects: the configurations of the firmware before a fix keep finding the bug, so the model is shown to
see it. Bounds: a few time units (one unit stands for any span: the model's constants are scaled), 4 to 6 frames from
the house and 3 to 6 from the gate, 1 or 2 commands, 2 boots each, 2 moves off closed by others per gate boot
(`MaxLeft`: without it the departure count, which every STATUS carries, never ends; the command properties'
configurations merge states that differ only in the sensor's bookkeeping, `CmdView`); exact clocks (the 100 ppm the
house allows is analysed, not modelled).

**Mutations.** `tools/mutate.py --verify` breaks each property in a copy of the firmware (`tools/verify/mutations.json`:
the interlock's gap, a pulse ending late, the busy checks, the deadline, the dedup, the save guards, the sensor's link
check, the stamps' cap, the window's bits...) and the proof must then fail; one that still passes is a property no proof
checks.

**What the checker found** (each a host test and a TODO.md item until its fix merges, each fix its own pull request):

1. *A withheld older command runs after a newer one* (TLC, CmdOrder; also CmdOnce, the same mechanism running a
   renumbered resend twice). Within its deadline, the gate checked only the last command's id.
2. *A withheld STATUS is taken however old* (TLC, SensorTruth). The house took any STATUS later than the last one it
   took: withheld frames played back in order, one per link timeout, held the sensor closed for as long as the attacker
   had kept them, not "up to its link timeout" as the threat model said.
3. *A user edge begun before K1's change ends its sync window* (ESBMC, `house_cmds.cpp`, a toggling Shelly, and the
   install's following one when the owner switches twice within its SW response): the Shelly's answer to K1, after the
   window closed early, became an OPEN.
4. *A supply dip hidden by a loop stall becomes a CLOSE* (ESBMC, `house_cmds.cpp`, `-DSTALLS_ANYWHERE`): a flash save
   stalls the house's loop ~0.5 s; a dip under it is never seen (power good is low for 100 ms or so), yet reboots the
   Shelly, whose relay drop then reads as the owner closing.
5. *A Shelly reboot the house doesn't see restores an OPEN* (ESBMC, `house_cmds.cpp`, `esbmc-house_cmds-worst`): a dip
   too short for power good that still reboots the Shelly, just as K1 changes. The relay's drop matches K1 and ends its
   window early; the Shelly boots back to its last state (parameter 17's default), an edge outside the window, taken
   for the owner's. Open: covered at the install only if every such dip drops power good first. On the bench it does:
   dips of 150 and 200 ms drop power good without rebooting the Shelly, 300 ms ones do both (TODO.md; 200–300 ms not
   yet stepped).
6. *A HELLO_ACK for session 0 verifies the link with no session* (ESBMC, `link_window.cpp`, after-hello): 0 means no
   session and no board draws it, but an answer naming it (echoing the challenge) was taken, the link verified with
   no session and nothing marking where its window starts. Only a key holder can make one: the window alone didn't
   keep it, as the properties claim. Fixed in 0.20.2: such an answer isn't taken (`link_window.cpp` holds).

**The numbers it rests on.** Every timing below is a range the checker tries in full; the Makefile runs the measured
range and the worst case the sources allow.

| Number | Source | What depends on it | Worst case |
|---|---|---|---|
| `INTERLOCK_MS` 100 ms | chosen (PR #27) after the GateSim saw both inputs at once; the opener's input filter and the relays' contact timing are unmeasured (the G5V-1's datasheet: ~5 ms each) | the gap is proved in firmware time; that 100 ms is enough for the opener is an assumption | not provable here: the opener's input timing isn't published |
| power good ~0.2 s before the relay into a cut | bench, 30 runs (PR #42); the BQ24195L datasheet gives no deglitch for it at all | a CLOSE from a cut is dropped (`ctrl_confirm_ms`); **owner-only** at the install | run with the order reversed (`BROWNOUT_AFTER_PG` off, `esbmc-house_cmds-worst`): finding 5, a dip that reboots the Shelly without dropping power good (the bench's 150–200 ms dips drop power good and don't reboot it; 200–300 ms unstepped); no setting covers it |
| power good low ≥ 100 ms in a dip | BQ24195L datasheet: REGN turn-on delay 100 ms min before power good returns | the house sees every dip that drops it, when its loop runs | a loop stall longer than that: finding 4 |
| IN2 opto 1.6–2.1 s into a cut, misses 300 ms dips | bench (PR #35, #42); the opto itself is µs (PC817): it's the rail's capacitors | only with `ctrl_power_pmic` off | — |
| Shelly relay drops ~0.46 s into a cut; a 300 ms dip reboots it, a 200 ms one doesn't; boots in ~3.1 s, to its last state | bench (PR #35, e2e dip runs); its manual: LED blinks 4–5 s at boot, parameter 17 (default: restore the last state); no SW response time published | the settle window covers its boot | the checker's Shelly boots at any level | the simulator's Shelly rode through dips under 460 ms and booted in 1.5 s: corrected for the new tests (`brownoutMs`) |
| Shelly SW response | never documented; bench logs: K1 to the debounced IN1 edge 136–184 ms (p95) | sync windows (`sync_window_ms`, 500 ms at least) | 200 ms modelled |
| `BOOT_SETTLE_MS` 3 s, `BETWEEN_HOLD_MS`, `BOTH_LIMITS_HOLD_MS` 500 ms, `CLOSED_DISAGREE_MS` 5 s | chosen; the CSW24UL's restart time, AUX timing and travel are unmeasured and unpublished (its manual: none) | the gate's reported state (truth), not the relay proofs | not covered (below) |
| crystal drift 100 ppm (`CLOCK_DRIFT`) | assumed; two ±20 ppm tuning-fork crystals, one outdoors at −20 °C (−0.036 ppm/°C²), reach ~115 ppm | commands' slack over 3 h; the house's clock readings | over 100 ppm, readings behind its reckoning by more aren't taken (availability) |
| loop pass while pulsing ≤ 50 ms; stalls ~0.5 s | soak `loop_max_us` 510 ms (flash save with radio restart); docs said "up to ~1 s" | pulse-only's overrun bound; finding 4 | 1 s modelled |

**Not covered, and why.**

- The crypto: the MAC and cipher are taken as ideal (the stand-in SHA256 and AES compute nothing; a forged frame's tag
  matches or not at the checker's choice, and the window proofs hold either way). The 128-bit challenge's freshness is
  an assumption of the protocol model.
- `radio.cpp`, `extflash.cpp`, `console.cpp` and `GateLink.ino` aren't run: the drivers are answered by the harness, and
  the console's JSON is beyond the checker. So the console's holding `config.save` and the like during a pulse is tested
  (systests), not proved. `sendDiag` and three config functions reach settings through pointers to member, which ESBMC
  can't follow: the harnesses skip `sendDiag` (it only reads) and use name-for-name equivalents of
  `paramSet`/`paramValid` generated from `PARAMS`.
- The gate's reported state (the limit holds, `fault`, `no_power`) is proved only not to touch the relays: whether it
  is true depends on the opener, whose timings are unpublished. The host tests and the bench check it.
- The house's commands are proved over 8 passes from boot, not inductively; K1's consistency with the gate (resync)
  isn't a safety property and isn't proved. **owner-only** can't tell the owner from the Shelly where the house can't
  either: in follow mode an owner switching on just as K1 does makes the same IN1 edge as the Shelly following K1, so
  such an edge counts as the owner's.
- `supply.cpp` runs only in `house_cmds.cpp`, on an I2C bus that always answers. A failed read keeps the last reading,
  so a dip during bus errors would go unseen; the bus failing isn't modelled.
- ESBMC 8.5 calls a method on the wrong operand of a conditional (`(c ? a : b).f()`); the firmware binds a reference
  first, and `tests/tools/test_verify.py` keeps the form out of it (it made a proof fail on correct code; read the other
  way, it could pass on wrong code).
- The protocol model is bounded and its clocks exact; the house's 100 ppm allowance lets an attacker who relays every
  STATUS late drag its reckoning back by 100 ppm of the attack's length (8.6 s a day).
- The first STATUS a house takes in a gate session (after either board restarts) has no reckoning to be dated against
  (`status_first.cfg` once finding 2 is fixed): an attacker who withholds STATUS from the handshake on can deliver one
  late. Closing it needs the STATUS to answer a challenge of the house's (a frame format change: ROADMAP).
- The checker runs on a 64-bit host; the firmware's arithmetic is in fixed-width types, which the checks rely on.
- Power cuts mid-save and the config record (`config.cpp`): host tests, not proofs.

## Web console

Plain ES modules, no build step. `web/index.html` carries a content security policy (scripts and data from the
page's own site only, no inline script: `web/nofile.js`, the one classic script, says how to open a page loaded from a
file), and board output reaches markup only escaped (`esc`) or as a number (`numOrNull`; `tests/web/unit/rules.test.js`
keeps the policy). `web/app.js` starts the page and runs the connection (what happens on open, close and
each board event); each card's module wires its own controls in its `init…()` and keeps its timers. In `web/js/`:
`serial.js` transport, picker and reconnect; `board.js` the board's info reply, role, header line and title (cards that
show info register `onInfo`, since they import `refreshInfo` from it; `serial.js` knows nothing of the tabs);
`status.js` the Status tab and the health line above every tab, with `healthline.js` its verdict and `peer.js` the
other board as last seen;
`config.js` the Config tab, with `settings.js` setting groups, labels and help (the other role's groups and
*Advanced* fold away below the rest, opening for an edit, and *Advanced* while any of it is on); `log.js` the Log tab, with `logdecode.js` log
events as text. The Link tab: `history.js`, `surveycard.js` (the site survey run; `survey.js` its verdict, twin of
`tools/gatelink_client/survey.py`, shared vectors in `tests/tools/fixtures/`; [install.md](install.md)), `ping.js`,
`remote.js` (the gate's diagnostics and remote writes, from the house). The Board tab: `security.js` the link key card
(and the replay self-test under *Developer*), with `keys.js` the key ids and encrypted backups
([key-management.md](key-management.md)); `firmware.js` + `samba.js`; factory reset (wired by `config.js`). The
Install tab: `wiring.js` (it imports nothing of the page: `tools/docgen.py` reads its tables in node), `relay.js`
(the wiring check's relay test; `status.js` fills its inputs) and `checklist.js` (the setup steps, ticked from each
status; pure, unit-tested). Links to the tabs the console had before (`#tools`,
`#security`) open their successors. `ui.js` holds the log view (every module writes to it), toasts and the
click guard; `util.js` small helpers; shared state is `state.js`'s `S`. Controls marked `data-offline` stay usable
without a board; connecting enables, and disconnecting disables, every other control in `main`.
Lint and tests: `npm test` (ESLint, `node --test` unit tests of the pure modules, Playwright browser tests of the
real page against fake boards in `tests/web/fake-serial.js`; `tests/web/README.md`).

Notable pieces:

- Auto-reconnect (`startReconnect`/`tryReconnect`: after a reboot or unexpected drop, reopen the already-granted
  port for 30 s; the Web Serial `connect` event identifies the returning board since both boards share VID/PID).
  After an update started from the bootloader there is no port to reopen (`expectBack(null)`): the one connected
  last may be the other board's, so only the `connect` event finds the updated one.
- `disconnect()` must await both stream pipes before `port.close()` or the port stays open and blocks uploads.
- The Install tab is driven by the `WIRING` table (keep it in sync with `pins.h` and the role behaviour).
- 0/1 params without a `SELECTS` entry render as toggle checkboxes, so read/write form fields through
  `fieldValue`/`setField`, not `.value`.
- The Board tab's firmware card flashes boards over the SAM-BA bootloader (`SamBa`, `flashFirmware`: 1200-baud
  touch, bootloader PID 0x0059 needs its own port grant, `X`/`S`+`Y`/`Z` at 0x2000) and recognises images by
  `FW_MARKER` (`GATELINK_FW=x.y.z`, `config.cpp`); the `pages` workflow bundles the latest release `.bin` +
  `firmware/latest.json` into the deploy after each release.
- The link history card (`loadHistory`/`drawHistory`) pages `hist.get` and draws one SVG of stacked panels on a
  shared time axis, so one crosshair/tooltip serves them all; the here/peer colours (`--chart-here`/`--chart-peer`)
  were chosen with the dataviz palette validator (blue/violet failed colour-blind separation), so re-validate if you
  change them.
- Log lines show decoded (`logdecode.js`; a unit test checks it covers every event in `log.cpp`); the downloaded log
  keeps the raw form.
- *Other board* on the Status tab (`peer.js`, ROADMAP F10): the must-match settings are the ones that stop the link
  verifying when they differ, so the boards can't compare them over the air. The page remembers each board it
  connects, by role, in `localStorage` (`gatelink.boards`: firmware, key id, the `MUST_MATCH` settings, when seen),
  and compares the connected board with the other one; connected to the house with the link up it also asks the gate
  for its firmware once (`remote.diag`). A firmware difference in MAJOR.MINOR is flagged (a change both boards need
  together); a PATCH apart matches. Storage failing (a private window) only means nothing to compare.
- The health line above every tab while a board answers (`healthLine` in `healthline.js`, ROADMAP C1): the most important
  thing wrong as the connected board sees it, with what to do next, from the status reply, the running `sf` and
  `peerDiffs()`. Order follows the project's goals: setup (role, key, radio), a reboot loop, the link (naming what
  differs from the other board when it can't verify), the gate's power and limits, AC, the controller's power,
  start-up (first half minute only, so a link that never comes is called that), then firmware or settings last seen
  differing, a weak margin (the median of the last ten polls' latest frames below the survey's fair line: a hint
  to run the survey), the board's own
  supply, a past watchdog hang. Pure, unit-tested; the raw link and board counters sit under *Details*.

The naming rule (no install hardware brand names under `web/`) is in CLAUDE.md.
