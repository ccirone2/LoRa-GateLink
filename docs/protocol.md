# Radio, protocol and security

The implementation notes (radio driver details, why things are done the way they are) live in
[CLAUDE.md](../CLAUDE.md) under "Firmware architecture". This page is the overview.

## Radio defaults

915.0 MHz, 500 kHz bandwidth, SF9, CR 4/5, 17 dBm. 500 kHz keeps a fixed-channel LoRa link in the
FCC 15.247 digital-modulation category in the US. For EU use 868.x MHz and stay within the band's duty-cycle
limits. Radio settings (`freq_hz`, `sf`, `bw_hz`, `cr`, `sync_word`) must match on both boards (`tx_power`
may differ) and, with `tx_power`, are never writable over LoRa.

LoRa runs point-to-point with the `LoRa` library, not LoRaWAN. At the install site aim for ≥10 dB margin above
the SF's sensitivity; raise `sf` (and/or `tx_power`) on **both** boards if the link is marginal.

## Framing

Frame: `ver | type | net_id | src | dst | session | seq | payload | tag`, tag = HMAC-SHA256 (shared
128-bit key) truncated to 8 bytes. Node addresses are fixed by role (house = 1, gate = 2). The radio link is
off until a key is set, so a fresh or reset board can never be commanded.

## Sessions and replay protection

Each board picks a random session id at boot (and a random starting seq); a peer's session is accepted only after
it echoes a fresh challenge (HELLO / HELLO_ACK), and only seq numbers above that HELLO_ACK's are accepted, each once,
so recorded frames can't be replayed — even across reboots. A 32-frame sliding window tolerates reordering between
retried messages. A HELLO from an unknown session (a restarted peer, or a replayed old one) is challenged, but the
verified session stays in place until the new one answers. A challenge is good for 10 s after the last HELLO that
carried it, so an answer recorded earlier can't be played back later.

That only holds while session ids never repeat under one key. The random source hashes radio noise, chained with a
per-boot seed: a boot counter kept in the SPI flash (one 4-byte slot per boot, apart from the config record, so
`config.reset` doesn't restart it) and the chip's serial number. If the radio failed to initialise at boot, the
session is drawn again once it comes up, before anything is sent.

HELLOs can't be checked against the replay window (a restarted peer's must get through), so a recorded HELLO is
acted on: it is answered, and pending frames are renumbered. Each board answers at most one HELLO a second from the
verified session and one from all other sessions, so replayed HELLOs can't keep pushing pending messages back.

The frame payloads aren't encrypted: someone listening learns the message types, the gate state and when mains
power is lost. They can't forge or replay frames.

## Reliable delivery

Commands, status and remote config writes are acknowledged and retried: the `retries` resends are spread over
the message's lifetime (`cmd_ttl_s` for commands; STATUS: `heartbeat_s` capped at 10 s; config writes: 10 s). The
gaps double from a 32nd of the lifetime, so a lost frame is retried quickly, but never beyond an even share of
what's left, so the later resends come evenly up to just before the end: at 10 s and the default 8 retries about
0.3, 0.9, 2.2, 3.7, 5.2, 6.7, 8.2 and 9.7 s; with 5 retries, as before 0.13.7, about 0.3, 0.9, 2.2, 4.7 and 9.7 s. A
command still has four sends after a 5 s outage and two after 8 s, and is dropped, never fired late, after
`cmd_ttl_s`. Duplicate commands are detected by their command id and not re-pulsed. A message still waiting when the peer
answers a HELLO is renumbered, and if the peer had already taken it (its ACK lost) the resend is taken as new: every
reliable message must therefore be safe to repeat, which STATUS and config writes are and commands are by their id. If the gate restarts while a command is still waiting
for its ACK, the house drops the command instead of sending it again: the gate may already have pulsed for it and
lost the ACK to the reset, and its record of the last command went with it, so a resend would pulse twice. The house
then resyncs the controller to the real gate. A HELLO from a new gate session only holds the command (log
`cmd_hold`), since it may be an old HELLO replayed to make the house drop it. The command is dropped once that
session answers the house's challenge; it is sent after all if the verified session answers instead, or sends
anything new. A new command replaces a held one. A config write that reaches the gate while one of its relays
pulses is taken at once but saved and ACKed only when the pulse is over (a save stops the loop for ~1 s, which would
hold the relay on that much longer); the house's retries meanwhile are dropped quietly, not counted as replays.
Every transmission listens before talking: responses go
after a 25 ms turnaround, new frames after the response slot plus a random backoff, both counted from the end of
the last frame on the air (any frame heard, also one with a bad CRC). Frames heard more often than that (another
LoRa network on the same channel and sync word, or someone replaying ours) would hold new frames off for good, so a
frame held back longer than about twice the longest frame plus the longest backoff skips those gaps and waits only
for a channel that reads busy (counted in `lbt_forced`). Unreliable frames waiting for a clear channel
are queued four deep with responses first; when the queue is full the newest new frame is dropped, or with only
responses waiting the oldest response.

## Link supervision

The gate sends STATUS every `heartbeat_s` (and on every change); STATUS carries that heartbeat. The house
declares the link lost after max(`link_timeout_s`, 2.5 × the gate's heartbeat) without hearing from the gate,
and then fails its contact sensor open (`linkloss_open`).

## Wire formats

The STATUS payload layout is defined in `firmware/GateLink/roles.h` and parsed in `role_house.cpp`; the DIAG
layout is built in `role_gate.cpp` (`sendDiag`) and parsed in `console.cpp`. Message types and the other
payloads (CMD, ACK, CFG_SET, PING/PONG, HELLO/HELLO_ACK, DIAG_REQ) are listed in `link.h`. A change to them needs
both boards updated together; say so in the release notes. The tables below are generated from those headers.

<!-- docgen:messages begin -->
<!-- Generated by tools/docgen.py from firmware/GateLink/link.h (MsgType, AckResult): edit the source, then run python tools/docgen.py --write. -->

### Message types

**Reliable**: acknowledged (`MSG_ACK`) and resent until it is, or its lifetime ends. **Direction** and **Reliable** come from the comments in `link.h`; a type they give no direction for is sent by either board.

| Type | Message | Payload | Direction | Reliable |
|---|---|---|---|---|
| 1 | `MSG_HELLO` | challenge u32 | either | no |
| 2 | `MSG_HELLO_ACK` | challenge u32 | either | no |
| 3 | `MSG_ACK` | acked seq u32, result u8 | either | no |
| 4 | `MSG_CMD` | cmd_id u16, action u8 | house → gate | yes |
| 5 | `MSG_STATUS` | STATUS payload, below | gate → house | yes |
| 6 | `MSG_PING` | ping_id u16 | either | no |
| 7 | `MSG_PONG` | ping_id u16, rssi i16, snr i8 | either | no |
| 8 | `MSG_DIAG_REQ` | — | house → gate | no |
| 9 | `MSG_DIAG` | DIAG payload, below | gate → house | no |
| 10 | `MSG_CFG_SET` | param id u8, value i32 | house → gate | yes |

ACK result codes (`AckResult`; a gap is a retired code, never reused):

| Code | Result | Meaning |
|---|---|---|
| 0 | `RES_OK` | done |
| 1 | `RES_ALREADY` | command matched current state; no pulse |
| 2 | `RES_BAD` | malformed / rejected |
| 4 | `RES_NO_POWER` | no AC power (IN3 power sense); no pulse |
| 5 | `RES_NOT_SAVED` | `CFG_SET` applied, but the flash save failed: reverts at the next reboot |

<!-- docgen:messages end -->

<!-- docgen:status begin -->
<!-- Generated by tools/docgen.py from firmware/GateLink/roles.h (ST_*, STI_*, enums): edit the source, then run python tools/docgen.py --write. -->

### STATUS payload (gate → house)

26 bytes, little-endian. **Since**: the gate firmware that added the field (— = every version); an older gate's shorter STATUS still works.

| Offset | Size | Type | Field | Meaning | Since |
|---|---|---|---|---|---|
| 0 | 1 | u8 | `ST_STATE` | gate state (`GateState`, below) | — |
| 1 | 1 | u8 | `ST_INPUTS` | the gate's inputs and relays (bits below) | — |
| 2 | 1 | u8 | `ST_CAUSE` | cause of the last state change (`Cause`) | — |
| 3 | 1 | u8 | `ST_RESULT` | how the last command ended (`TravelResult`) | — |
| 4 | 2 | u16 | `ST_CMD_ID` | id of the last command the gate took | — |
| 6 | 4 | u32 | `ST_UPTIME` | gate uptime, s | — |
| 10 | 2 | i16 | `ST_RSSI` | RSSI at gate of last frame from house (0 = none yet) | — |
| 12 | 1 | i8 | `ST_SNR` | SNR at the gate of its last frame from the house, dB | — |
| 13 | 1 | u8 | `ST_TARGET` | where the gate's pulse is taking it (`GateState`; 0 = none) | — |
| 14 | 2 | u16 | `ST_HEARTBEAT` | gate `heartbeat_s`: the house's link timeout must cover it | — |
| 16 | 2 | u16 | `ST_RETRIES` | gate link retries (running total, low 16 bits; resets when its link restarts) | 0.4.0 |
| 18 | 2 | u16 | `ST_GIVEUPS` | gate link giveups (same) | 0.4.0 |
| 20 | 2 | u16 | `ST_CRC` | CRC errors at the gate radio (same) | 0.4.0 |
| 22 | 1 | i8 | `ST_NOISE` | gate noise floor since its previous STATUS, dBm (0 = no sample) | 0.4.0 |
| 23 | 1 | i8 | `ST_NOISE_MAX` | its peak | 0.4.0 |
| 24 | 2 | u16 | `ST_TRAVEL` | gate `travel_timeout_s`: the house holds K1 through a travel for as long | 0.13.0 |

`ST_INPUTS` bits:

| Bit | Mask | Name | Meaning |
|---|---|---|---|
| 0 | 0x01 | `STI_IN1` | open limit |
| 1 | 0x02 | `STI_IN2` | close limit |
| 2 | 0x04 | `STI_K1` | K1 on (OPEN) |
| 3 | 0x08 | `STI_K2` | K2 on (CLOSE) |
| 4 | 0x10 | `STI_IN3` | AC power sense |
| 5 | 0x20 | `STI_IN4` | IN4 (spare) |
| 6 | 0x40 | `STI_AC_LOST` | no AC power: IN3 off with `power_sense` on (since 0.7.0) |

Enums in STATUS (also the `status` reply's names):

- `GateState`: 0 `unknown`, 1 `closed`, 2 `open`, 3 `between`, 4 `fault`, 5 `no_power`
- `Cause`: 0 `none`, 1 `lora`, 2 `external`
- `TravelResult`: 0 `none`, 1 `reached`, 2 `timeout`, 3 `already`

<!-- docgen:status end -->

<!-- docgen:diag begin -->
<!-- Generated by tools/docgen.py from firmware/GateLink/roles.h (DIAG_*, DiagCounter) and config.cpp (PARAMS): edit the source, then run python tools/docgen.py --write. -->

### DIAG payload (gate → house, answering `MSG_DIAG_REQ`)

| Offset | Size | Type | Field | Meaning |
|---|---|---|---|---|
| 0 | 3 | u8 ×3 | `DIAG_FW` | firmware version: major, minor, patch |
| 3 | 4 | u32 | `DIAG_UPTIME` | gate uptime: seconds |
| 7 | 2 | u16 | `DC_TX` | counter `tx` (saturating) |
| 9 | 2 | u16 | `DC_RX` | counter `rx` (saturating) |
| 11 | 2 | u16 | `DC_MAC_FAIL` | counter `mac_fail` (saturating) |
| 13 | 2 | u16 | `DC_REPLAY` | counter `replay` (saturating) |
| 15 | 2 | u16 | `DC_RETRIES` | counter `retries` (saturating) |
| 17 | 2 | u16 | `DC_GIVEUPS` | counter `giveups` (saturating) |
| 19 | 5 each | u8 + i32 | — | (param id, value) per remote-writable param, in `PARAMS` order: `retries` (9), `heartbeat_s` (10), `link_timeout_s` (11), `debounce_ms` (13), `pulse_ms` (16), `travel_timeout_s` (17), `power_sense` (26) |

<!-- docgen:diag end -->

### History

Since 0.13.0, STATUS is 26 bytes: it ends with the gate's `travel_timeout_s`, and the house holds K1 through a travel
for that long instead of its own setting (only the gate's copy can be changed remotely, so the two could
differ). An older gate's 24-byte STATUS still works; the house then uses its own value.

Since 0.7.0, bit 6 of the STATUS inputs byte is set while the gate has no AC power (IN3 off with `power_sense` on);
an older house ignores it. Since 0.4.0, STATUS is 24 bytes: the gate also reports its link retries, giveups and CRC errors (running totals,
low 16 bits) and its noise floor since the previous STATUS (average and peak). The house uses them for the link
history and still accepts the 16-byte STATUS of 0.3.x gates (without those). The RSSI/SNR in STATUS are now
those of the gate's last authenticated frame from the house, ACKs included (before, only commands, config writes
and diagnostics requests updated them).
