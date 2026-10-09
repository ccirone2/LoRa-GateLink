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
verified session stays in place until the new one answers.

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
the message's lifetime with doubling gaps (`cmd_ttl_s` for commands: at 10 s and 5 retries about 0.3, 0.9, 2.2,
4.7 and 9.7 s; STATUS: `heartbeat_s` capped at 10 s; config writes: 10 s), so a command survives an outage of nearly `cmd_ttl_s` and is dropped, never fired late, after
it. Duplicate commands are detected and not re-pulsed. If the gate restarts while a command is still waiting
for its ACK, the house drops the command instead of sending it again: the gate may already have pulsed for it and
lost the ACK to the reset, and its record of the last command went with it, so a resend would pulse twice. The house
then resyncs the controller to the real gate. A HELLO from a new gate session only holds the command (log
`cmd_hold`), since it may be an old HELLO replayed to make the house drop it. The command is dropped once that
session answers the house's challenge; it is sent after all if the verified session answers instead, or sends
anything new. A new command replaces a held one. Every transmission listens before talking: responses go
after a 25 ms turnaround, new frames after the response slot plus a random backoff.

## Link supervision

The gate sends STATUS every `heartbeat_s` (and on every change); STATUS carries that heartbeat. The house
declares the link lost after max(`link_timeout_s`, 2.5 × the gate's heartbeat) without hearing from the gate,
and then fails its contact sensor open (`linkloss_open`).

## Wire formats

The STATUS payload layout is defined in `firmware/GateLink/roles.h` and parsed in `role_house.cpp`; the DIAG
layout is built in `role_gate.cpp` (`sendDiag`) and parsed in `console.cpp`. Message types and the other
payloads (CMD, ACK, CFG_SET, PING/PONG, HELLO/HELLO_ACK, DIAG_REQ) are listed in `link.h`. A change to them needs both boards updated together; say so in the release notes.

Since 0.13.0, STATUS is 26 bytes: it ends with the gate's `travel_timeout_s`, and the house holds K1 through a travel
for that long instead of its own setting (only the gate's copy can be changed remotely, so the two could
differ). An older gate's 24-byte STATUS still works; the house then uses its own value.

Since 0.7.0, bit 6 of the STATUS inputs byte is set while the gate has no AC power (IN3 off with `power_sense` on);
an older house ignores it. Since 0.4.0, STATUS is 24 bytes: the gate also reports its link retries, giveups and CRC errors (running totals,
low 16 bits) and its noise floor since the previous STATUS (average and peak). The house uses them for the link
history and still accepts the 16-byte STATUS of 0.3.x gates (without those). The RSSI/SNR in STATUS are now
those of the gate's last authenticated frame from the house, ACKs included (before, only commands, config writes
and diagnostics requests updated them).
