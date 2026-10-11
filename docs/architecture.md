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
HMAC-SHA256(key) truncated to 8 bytes. The link is disabled entirely while `cfg.key_set == 0`.

**Sessions and replay protection.** Replay protection has no persisted seq counters: each boot (and link restart)
picks a new session id; a peer session is accepted only after a HELLO challenge is echoed in HELLO_ACK, and from
then on only seqs above that HELLO_ACK's pass (then a 32-frame sliding window), so frames recorded before a reboot
can't be replayed. Answering a HELLO renumbers everything still waiting (`reframePending`; the renumbered resend
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
held back past `waitCapMs()` skips the gaps (frames heard every ~60 ms held them shut for good) and waits only for a
busy channel. Counters: status `link.lbt_defers`/`lbt_forced`.

**ACK memo.** Already-accepted retransmits are re-ACKed from an ACK memo, not re-processed; a message answered later
(`linkAckLater`: the gate's remote config writes during a relay pulse) holds its retransmits quietly until its
`linkAck`; one refused (`linkRefuse`: a fifth such write) is neither ACKed nor remembered, as if lost, so its resend
(the same seq) is taken later.

## Roles: `role_gate.cpp` and `role_house.cpp`

The application state machines; the rules they keep are the behavioural invariants in [CLAUDE.md](../CLAUDE.md). The
STATUS payload layout is defined in `roles.h` (built in `role_gate.cpp`, parsed in `role_house.cpp`); DIAG's too
(`DIAG_*`, `DiagCounter`), built in `role_gate.cpp` (`sendDiag`) and parsed in `console.cpp` (`consoleEventDiag`);
keep them in sync. The longest loop pass since boot is status `loop_max_us` (the soak fails it past 4 s).

## History: `history.cpp` and `histlog.cpp`

Link quality history: a RAM ring of hourly buckets (96 + the one in progress; RAM-bound, as a `config.get` reply
takes ~5 KB of heap — check `free_ram` in status after changing it). `histPoll` diffs the `linkStats()`/radio totals
into the bucket in progress each loop (one frame per `linkPoll`, so the last RSSI/SNR are that frame's), samples the
noise floor (`radioNoiseDbm`; a reading is kept only if no frame followed it by the next one, since the modem flags
a preamble a few symbols late and peer frames showed up as noise peaks) and counts link-down seconds (`appLinkUp`,
shared with the LED and the house's `linkUp`). The house adds the gate's side from STATUS (`histPeer`).

Each completed bucket is appended to the SPI flash (`histlog.cpp`, sectors 4–19: config sectors 0–1, boot counter
2–3), once and never during a relay pulse (`histSave`, in `appLoop`), and boot loads the newest back, so a reset
loses only the bucket in progress (each bucket carries its `boot`). Diagnostics only: nothing may read it to make a
decision.

## `health.cpp`

The fault output D5 (`fault_out`), polled after the roles: it reads their state through `houseDecided`/`gateDecided`
and the like, and nothing reads it back. The problems it tracks are bits (`Problem` in `health.h`; status `health`
names them, log `health` carries them): the role hasn't decided yet since boot, the radio isn't working, the link is
down, no AC power at the gate, the gate reads `no_power` or `fault`. What D5 does with them is a behavioural
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
called by both roles; reported in STATUS bits 4–5; on the gate IN3 is the AC power sense, IN4 has no behaviour yet);
house IN2 is the controller power sense (`ctrl_power_sense`, handled in `role_house.cpp`, not reported to the gate),
the board supply (`supply.cpp`, polled every 5 ms before the roles; status `supply`, log `supply`), the reset cause
(`PM->RCAUSE`, logged in `boot` a= and reported as `reset_cause`), and the fault output D5 (`health.cpp`, after the
roles). `activeRole` is latched at boot; `cfg.role` changes only take effect after reboot. Use `activeRole`, not
`cfg.role`, for runtime behaviour.

- `GateLink.ino` only reads the reset cause and chip serial, runs the watchdog and calls `appSetup`/`appLoop`.
- `board.h` is what needs the chip: kick, reset, free RAM (implemented in `GateLink.ino`, and by the host simulation
  in `tests/native`, so everything from `app.cpp` down except the radio, flash, charger and console drivers builds
  on the PC).
- `supply.cpp`: the board's VIN power good, read from the BQ24195L charger over I2C (read-only).
- `io.cpp`: `Input` (debounced, internal pull-down, active HIGH) and `Relay` (non-blocking pulses, optionally
  delayed; `gapLeft` for the interlock).
- `log.cpp`: the event log, a ring of `LOG_SIZE` entries; the `LogCode` order in `log.h` and the names in `NAMES`
  are part of the console contract ([console.md](console.md), `web/js/logdecode.js`, the e2e suite).
- `pins.h`: the pin map ([hardware.md](hardware.md) and the web console's `WIRING` table follow it). `crc32.h`: the
  CRC-32 of the config record, the history log's records and the console's request check.

## Web console

Plain ES modules, no build step: `web/app.js` wires the page; the work is in `web/js/` (`serial.js` transport,
picker and reconnect; `status.js`, `config.js`, `tools.js`, `history.js`, `firmware.js` + `samba.js` per tab;
`security.js` the Security tab, with `keys.js` the key ids and encrypted backups
([key-management.md](key-management.md)); `survey.js` the site survey's verdict, twin of
`tools/gatelink_client/survey.py` (shared vectors in `tests/tools/fixtures/`; [install.md](install.md)); `wiring.js`
Install tab; `settings.js` setting groups/help; `logdecode.js` log events as text; `ui.js` the log view and toasts;
`util.js` small helpers; shared state in `state.js`'s `S`).
Lint and tests: `npm test` (ESLint, `node --test` unit tests of the pure modules, Playwright browser tests of the
real page against fake boards in `tests/web/fake-serial.js`; `tests/web/README.md`).

Notable pieces:

- Auto-reconnect (`startReconnect`/`tryReconnect`: after a reboot or unexpected drop, reopen the already-granted
  port for 30 s; the Web Serial `connect` event identifies the returning board since both boards share VID/PID).
- `disconnect()` must await both stream pipes before `port.close()` or the port stays open and blocks uploads.
- The Install tab is driven by the `WIRING` table (keep it in sync with `pins.h` and the role behaviour).
- 0/1 params without a `SELECTS` entry render as toggle checkboxes, so read/write form fields through
  `fieldValue`/`setField`, not `.value`.
- The Tools tab's firmware card flashes boards over the SAM-BA bootloader (`SamBa`, `flashFirmware`: 1200-baud
  touch, bootloader PID 0x0059 needs its own port grant, `X`/`S`+`Y`/`Z` at 0x2000) and recognises images by
  `FW_MARKER` (`GATELINK_FW=x.y.z`, `config.cpp`); the `pages` workflow bundles the latest release `.bin` +
  `firmware/latest.json` into the deploy after each release.
- The link history card (`loadHistory`/`drawHistory`) pages `hist.get` and draws one SVG of stacked panels on a
  shared time axis, so one crosshair/tooltip serves them all; the here/peer colours (`--chart-here`/`--chart-peer`)
  were chosen with the dataviz palette validator (blue/violet failed colour-blind separation), so re-validate if you
  change them.
- Log lines show decoded (`logdecode.js`; a unit test checks it covers every event in `log.cpp`); the downloaded log
  keeps the raw form.

The naming rule (no install hardware brand names under `web/`) is in CLAUDE.md.
