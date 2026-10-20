# Radio, protocol and security

The implementation notes (radio driver details, why things are done the way they are) live in
[architecture.md](architecture.md), under `radio.cpp` and `link.cpp`. This page is the overview.

## Radio defaults

915.0 MHz, 500 kHz bandwidth, SF9, CR 4/5, 17 dBm. 500 kHz keeps a fixed-channel LoRa link in the
FCC 15.247 digital-modulation category in the US. For EU use 868.x MHz and stay within the band's duty-cycle
limits. Radio settings (`freq_hz`, `sf`, `bw_hz`, `cr`, `sync_word`) must match on both boards (`tx_power`
may differ) and, with `tx_power`, are never writable over LoRa.

LoRa runs point-to-point with the `LoRa` library, not LoRaWAN. At the install site aim for ≥10 dB margin above
the SF's sensitivity; raise `sf` (and/or `tx_power`) on **both** boards if the link is marginal.

## Framing

Frame: `ver | type | net_id | src | dst | session | seq | payload | tag`. The payload is encrypted with AES-128 in
counter mode, then tag = HMAC-SHA256 (shared 128-bit key) of the header and the encrypted payload, truncated to 8
bytes (encrypt-then-MAC). The cipher key is the first 16 bytes of HMAC-SHA256(key, `GateLink payload v1`), so it is
apart from the MAC key; the counter block is the header's `src`, `dst`, `session` and `seq` (bytes 3–12), six zero
bytes and the block number (one byte), distinct for every frame while session ids don't repeat, so no keystream is
used twice. `ver` is 2 since 0.16.0 (1 before: payloads in the clear, 32-bit challenges); a board drops frames of
another version, so both boards must run 0.16.0 or later to link. Node addresses are fixed by role (house = 1,
gate = 2). The radio link is off until a key is set, so a fresh or reset board can never be commanded. Generating,
backing up, checking (key ids) and replacing the key: [key-management.md](key-management.md).

## Sessions and replay protection

Each board picks a new session id at boot and at every link restart (a new key or radio setting), and a random
starting seq; a peer's session is accepted only after it echoes a fresh 128-bit challenge (HELLO / HELLO_ACK), and only seq
numbers above that HELLO_ACK's are accepted, each once, so recorded frames can't be replayed — even across reboots. A
32-frame sliding window tolerates reordering between retried messages. A HELLO from an unknown session (a restarted peer, or a replayed old one) is challenged, but the
verified session stays in place until the new one answers. A challenge is good for 10 s after the last HELLO that
carried it, so an answer recorded earlier can't be played back later. A challenge is the link key's HMAC of the
challenger's session, a count and radio noise (16 bytes of it): distinct for every challenge and unpredictable
without the key. With 32 bits (before 0.16.0), answers recorded over weeks (old HELLOs replayed to make a board
challenge, and the live peer's answers recorded) could come to match a later challenge, re-verify a superseded
session and let its recorded frames through.

That only holds while session ids never repeat under one key, so since 0.13.9 they are unique by construction
(before, they were random: by the birthday bound, even odds of a repeat within ~77,000 sessions). The id is a
permutation of three fields: the boot count (20 bits), the ids drawn so far this boot (4 bits: link restarts) and 8
random bits. The permutation, a 4-round Feistel network on HMAC-SHA256, is keyed with the link key, the board's node
id and its chip serial number: distinct fields give distinct ids, nobody without the key can tell the next one, and
another board given the same key (its count starting over) draws from another permutation. The random bits make an
id whose count repeats anyway (the flash rolled back or replaced) differ but for a 1 in 256 chance.

The boot counter is kept in the SPI flash, apart from the config record (so `config.reset` doesn't restart it): one
4-byte slot per boot, written in two steps, the count with its top bit set and then that bit cleared, so a slot torn
by a power cut (bits left at 1) is recognised and skipped. It never returns a count twice; should it ever run out
(2^31 boots, or a slot torn by older firmware that reads that high) it returns 0, as it does for a read garbled on the
bus (a slot reading 0, which no slot is ever written, twice, with the chip's id garbled too), which could hide the
largest count. Zeros the chip really holds (an erase cut short by a power cut) are slots without a count (since
0.17.5; before, the counter returned 0 from then on). Past a million boots, from
the 16th id in one boot, or without a count (no flash chip, a garbled read), ids are random as before: radio noise
hashed with a per-boot seed (the count and the chip's serial number). If the radio failed to initialise at boot, the session is drawn again
once it comes up, before anything is sent.

HELLOs can't be checked against the replay window (a restarted peer's must get through), so a recorded HELLO is
acted on: it is answered, and pending frames are renumbered. Each board answers at most one HELLO a second from the
verified session and one from all other sessions, so replayed HELLOs can't keep pushing pending messages back.

Someone listening learns the message types, frame lengths and times, not the payloads: that the gate's state
changed, not to what, nor why (a move or the mains power). They can't forge frames, or replay frames that were
delivered. A frame **withheld** (jammed at its receiver and recorded) and delivered later is taken if its seq is
still inside the replay window, so each message type that matters is checked for that: the house takes STATUS
only in seq order (0.13.9), and a command carries a deadline (below).

## Command deadlines

Every CMD carries a deadline in the gate's clock: when the house sent it plus the house's `cmd_ttl_s`, the time the
house gives it up. The clock is milliseconds, 64 bits (48 in the frames), from an origin each boot draws at random
(47 bits), so it never wraps and a deadline reckoned in another boot's clock lands nowhere near this one's. The gate
refuses a command past its deadline by more than 2 s (`CMD_LATE_SLACK_MS`) plus a STATUS's and a command's time on
the air (the house's reckoning lags by the one, the command takes the other to arrive: a few ms at SF9 / 500 kHz,
seconds at SF12 / 125 kHz), or with one further ahead than the longest `cmd_ttl_s` allows (122 s): ACKed
`RES_LATE`, log `cmd_late`, no pulse. So a command jammed at the gate and recorded, played back later, never moves
it, however long its seq stays inside the window (before 0.16.0 it could fire whenever the attacker chose while the
gate heard fewer than 32 frames from the house).

The house learns the gate's clock from STATUS, which carries it (`ST_CLOCK`). A STATUS that was resent (its first
frames lost) carries the clock of when it was built, seconds behind, so a reading replaces the one in use only if it
puts the gate's clock later, or earlier by no more than the crystals can have drifted apart since (100 ppm); none for
3 hours (a dead link) leaves it unknown. The reading is kept for the gate session it came from. With none for the
verified gate session (the gate just restarted and waits for its inputs to settle, a few seconds, or a link restarted)
a command can't be dated: it waits (log `cmd_hold` a=3) for up to `cmd_ttl_s` from the switch, losing that much of its
lifetime, and is dropped after it (log `cmd_dropped`, the controller put back), never sent undated. The gate reports
as soon as its link has verified the house, a new session or its own restarted, so the wait is short. A remote config
write carries no deadline: a withheld one can only set a value the installer chose, after it was reported failed.

## Reliable delivery

Commands, status and remote config writes are acknowledged and retried: the `retries` resends are spread over
the message's lifetime (`cmd_ttl_s` for commands; STATUS: `heartbeat_s` capped at 10 s; config writes: 10 s). The
gaps double from a 32nd of the lifetime, so a lost frame is retried quickly, but never beyond an even share of
what's left, so the later resends come evenly up to just before the end: at 10 s and the default 8 retries about
0.3, 0.9, 2.2, 3.7, 5.2, 6.7, 8.2 and 9.7 s; with 5 retries, as before 0.13.7, about 0.3, 0.9, 2.2, 4.7 and 9.7 s. A
command still has four sends after a 5 s outage and two after 8 s, and is dropped, never fired late, after
`cmd_ttl_s`. Duplicate commands are detected by their command id and not re-pulsed, and a
command for the relay that is still pulsing (a second OPEN while K1 still pulses for the first) is the same press:
ACKed, and the pulse isn't restarted. A message still waiting when the peer
answers a HELLO is renumbered, and if the peer had already taken it (its ACK lost) the resend is taken as new: every
reliable message must therefore be safe to repeat, which STATUS and config writes are and commands are by their id. If the gate restarts while a command is still waiting
for its ACK, the house drops the command instead of sending it again: the gate may already have pulsed for it and
lost the ACK to the reset, and its record of the last command went with it, so a resend would pulse twice. The house
then resyncs the controller to the real gate. A HELLO from a new gate session only holds the command (log
`cmd_hold`), since it may be an old HELLO replayed to make the house drop it. The command is dropped once that
session answers the house's challenge; it is sent after all if the verified session answers instead, or sends
anything new. A new command replaces a held one. A config write that reaches the gate while one of its relays
pulses is taken at once but saved and ACKed only when the pulse is over (a save stops the loop for ~1 s, which would
hold the relay on that much longer); the house's retries meanwhile are dropped quietly, not counted as replays. The
gate holds four that way. The house sends one at a time, but a renumbered resend (after it answers a HELLO, a
replayed one too) arrives as a new message, so copies can fill the queue: one arriving when it's full isn't taken
at all. It isn't ACKed and its seq is forgotten, as if the frame had been lost, so the house's next resend of it is
taken once there's room (a frame lost on the way can arrive late as well, so that replays nothing new).
Every transmission listens before talking: responses go
after a 25 ms turnaround, new frames after the response slot plus a random backoff, both counted from the end of
the last frame on the air (any frame heard, also one with a bad CRC). Frames heard more often than that (another
LoRa network on the same channel and sync word, or someone replaying ours) would hold new frames off for good, so a
frame held back longer than about twice the longest frame plus the longest backoff stops waiting out those gaps: it
goes as soon as a frame heard ends (mid-gap it would run into the next one), or once the channel has been quiet for
its backoff after all, and still waits while the channel reads busy (counted in `lbt_forced`). A channel that reads busy without a break for twice the
longest frame (noise taken for a signal) doesn't hold a frame any longer; a clear reading restarts that count, so a
frame never goes into the next frame heard. Unreliable frames waiting for a clear channel
are queued four deep with responses first; when the queue is full the newest new frame is dropped, or with only
responses waiting the oldest response.

## Link supervision

The gate sends STATUS every `heartbeat_s` (and on every change); STATUS carries that heartbeat. The house
declares the link lost after max(`link_timeout_s`, 2.5 × the gate's heartbeat) without a STATUS from the gate,
and then fails its contact sensor open (`linkloss_open`). Only a STATUS counts, and only one later than the last it
took (a higher seq in that gate session): the link layer takes any frame it hasn't seen inside its window, also one
jammed at the house while it was recorded and played back later, which would set the house back to an old state
(a "closed" after the gate opened) and keep the link up. Such a STATUS is ACKed and dropped (log `replay`, with the
seq of the last one taken). The house's own link restart (a new key or radio setting) takes the link down until the
gate's next STATUS, which it sends as soon as it has verified the house's new session. Since 0.13.9; before, any
frame from the gate kept the link up.

The gate hears the house at least once a heartbeat (the ACK of its STATUS) and counts the link down (its LED, link
history and fault output) after max(`link_timeout_s`, 2.5 × its own heartbeat) without a frame from the house (since
0.14.0; before, after `link_timeout_s` alone).

## SPI flash

Each board's on-board SPI flash (W25Q16JV, 4 KB sectors) holds what must survive a reset or a firmware upload. Every
access holds the radio module in reset, so the radio is restarted after it: about 0.5 s off the air, with the loop
stopped (never while a relay pulses).

| Sectors | What | Written |
|---|---|---|
| 0–1 | Config records (settings and key), alternately; the newest valid one is loaded (`config.cpp`) | On a save |
| 2–3 | The boot counter: a 4-byte slot per boot (`configCountBoot`, above) | At every boot |
| 4–19 | The link history log (`histlog.cpp`; [console.md](console.md#link-history)) | Once per completed bucket, and by `hist.clear` |

The history log is cut into 128-byte slots written in turn, two to a page and 512 in all, each holding one record:
`magic(2) fmt(1) kind(1) lseq(4) idx(4) period_s(2) 0(2) bucket(68) crc(4)`, little-endian, the CRC-32 over the rest.
`lseq` numbers every record ever written, so the newest has the largest; `idx` is the bucket's number and `kind` a
bucket (1) or a CLEARED record (2, written by `hist.clear`: the history starts again at bucket 0 with its period).
`fmt` versions the bucket's layout (`history.cpp`); a firmware loads only its own, but finds the newest record
whatever its format and writes after it. Writing into a sector's first slot erases that sector first, which drops the
oldest records as the log wraps. At boot the board finds the newest record, the valid one with the largest `lseq` (it
reads every slot's first 8 bytes, then in full the few with the largest `lseq` until one is valid, or every slot if
none of those is), puts the next write after it and after any slot past it that isn't erased (one torn by a power
cut), and loads back up to 96 buckets: going back from the newest, while each is the bucket before it,
written before it and of the same period and format. A record with a bad CRC is passed over; an erased slot or a
CLEARED record ends the run. A read that comes back garbled twice (a slot starting with four zero bytes, which no
record does, and the chip's JEDEC id not reading back either) leaves the log unread and unwritten until the next boot,
rather than write from the wrong place. Zeros read while the id still reads back are what the flash holds (an erase
cut short may leave a sector so): such a slot is passed over like any that isn't a valid record.

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
| 1 | `MSG_HELLO` | challenge u8 x16 | either | no |
| 2 | `MSG_HELLO_ACK` | challenge u8 x16: the HELLO's | either | no |
| 3 | `MSG_ACK` | acked seq u32, result u8 | either | no |
| 4 | `MSG_CMD` | cmd_id u16, action u8, deadline u48 (the gate's clock, ms) | house → gate | yes |
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
| 4 | `RES_NO_POWER` | no AC power (IN3 power sense) and no limit reading (nor our pulse under way); no pulse |
| 5 | `RES_NOT_SAVED` | `CFG_SET` applied, but the flash save failed: reverts at the next reboot |
| 6 | `RES_LATE` | CMD past its deadline by the gate's clock (or too far ahead of it); no pulse |

<!-- docgen:messages end -->

<!-- docgen:status begin -->
<!-- Generated by tools/docgen.py from firmware/GateLink/roles.h (ST_*, STI_*, enums): edit the source, then run python tools/docgen.py --write. -->

### STATUS payload (gate → house)

33 bytes, little-endian. **Since**: the gate firmware that added the field (— = every version); a gate older than a field leaves it off, and the house still takes its STATUS, back to 0.16.0 (frame format 2: an older gate can't link at all).

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
| 26 | 6 | u48 | `ST_CLOCK` | the gate's clock (ms, from a random origin each boot): commands' deadlines are in it | 0.16.0 |
| 32 | 1 | u8 | `ST_LEFT_CLOSED` | times the gate's reported state has left closed since its boot (mod 256) | 0.17.0 |

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
| 7 | 0x80 | `STI_REBOOTS` | the gate is in a reboot loop (health `reboots`) (since 0.15.0) |

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
| 19 | 5 each | u8 + i32 | — | (param id, value) per remote-writable param, in `PARAMS` order: `retries` (9), `heartbeat_s` (10), `link_timeout_s` (11), `debounce_ms` (13), `pulse_ms` (16), `travel_timeout_s` (17), `power_sense` (26), `fault_hold_s` (33) |

<!-- docgen:diag end -->

### History

Since 0.17.0, STATUS is 33 bytes, ending with the gate's count of departures from closed (`ST_LEFT_CLOSED`): a
house that gets a move it never heard (the count advanced while it showed the gate closed) opens the contact sensor
for 5 s, so the alarm records it late (log `missed_move`). A 0.17.0 house still takes a 0.16 gate's 32-byte STATUS,
without that check, and a 0.16 house ignores the byte.

Since 0.16.0 the frame format is 2 (encrypted payloads, 128-bit challenges, deadlines on commands): a board drops
frames from a board on an older version, so the house needs the whole STATUS of then, 32 bytes with the gate's clock
at the end. The notes below are about gates before 0.16.0, which can't link with a 0.16.0 house.

Since 0.13.0, STATUS is 26 bytes: it ends with the gate's `travel_timeout_s`, and the house holds K1 through a travel
for that long instead of its own setting (only the gate's copy can be changed remotely, so the two could
differ). An older gate's 24-byte STATUS still works; the house then uses its own value.

Since 0.7.0, bit 6 of the STATUS inputs byte is set while the gate has no AC power (IN3 off with `power_sense` on);
an older house ignores it. Since 0.4.0, STATUS is 24 bytes: the gate also reports its link retries, giveups and CRC errors (running totals,
low 16 bits) and its noise floor since the previous STATUS (average and peak). The house uses them for the link
history and still accepts the 16-byte STATUS of 0.3.x gates (without those). The RSSI/SNR in STATUS are now
those of the gate's last authenticated frame from the house, ACKs included (before, only commands, config writes
and diagnostics requests updated them).
