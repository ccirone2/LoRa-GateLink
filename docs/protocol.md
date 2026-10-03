# Radio, protocol and security

The implementation notes (radio driver details, why things are done the way they are) live in
[CLAUDE.md](../CLAUDE.md) under "Firmware architecture". This page is the overview.

## Radio defaults

915.0 MHz, 500 kHz bandwidth, SF9, CR 4/5, 17 dBm. 500 kHz keeps a fixed-channel LoRa link in the
FCC 15.247 digital-modulation category in the US. For EU use 868.x MHz and stay within the band's duty-cycle
limits. Radio settings (`freq_hz`, `sf`, `bw_hz`, `cr`, `tx_power`, `sync_word`) must match on both boards and
are never writable over LoRa.

LoRa runs point-to-point with the `LoRa` library, not LoRaWAN. At the install site aim for ≥10 dB margin above
the SF's sensitivity; raise `sf` (and/or `tx_power`) on **both** boards if the link is marginal.

## Framing

Frame: `ver | type | net_id | src | dst | session | seq | payload | tag`, tag = HMAC-SHA256 (shared
128-bit key) truncated to 8 bytes. Node addresses are fixed by role (house = 1, gate = 2). The radio link is
off until a key is set, so a fresh or reset board can never be commanded.

## Sessions and replay protection

Each board picks a random session id at boot; a peer's session is accepted only after it echoes a fresh
challenge (HELLO / HELLO_ACK), and only seq numbers above that HELLO_ACK's are accepted, each once, so recorded
frames can't be replayed — even across reboots, with no counters in flash. A 32-frame sliding window tolerates
reordering between retried messages. A HELLO from an unknown session (a restarted peer, or a replayed old one)
is challenged, but the verified session stays in place until the new one answers.

## Reliable delivery

Commands, status and remote config writes are acknowledged and retried: the `retries` resends are spread over
the message's lifetime with doubling gaps (`cmd_ttl_s` for commands: at 10 s and 5 retries about 0.3, 0.9, 2.2,
4.7 and 9.7 s), so a command survives an outage of nearly `cmd_ttl_s` and is dropped, never fired late, after
it. Duplicate commands are detected and not re-pulsed. Every transmission listens before talking: responses go
after a 25 ms turnaround, new frames after the response slot plus a random backoff.

## Link supervision

The gate sends STATUS every `heartbeat_s` (and on every change); STATUS carries that heartbeat. The house
declares the link lost after max(`link_timeout_s`, 2.5 × the gate's heartbeat) without hearing from the gate,
and then fails its contact sensor open (`linkloss_open`).

## Wire formats

STATUS and DIAG payload layouts are defined in `firmware/GateLink/roles.h` and parsed in `role_house.cpp` and
`console.cpp`. A change to them needs both boards updated together; say so in the release notes.

Since 0.4.0, STATUS is 24 bytes: the gate also reports its link retries, giveups and CRC errors (running totals,
low 16 bits) and its noise floor since the previous STATUS (average and peak). The house uses them for the link
history and still accepts the 16-byte STATUS of 0.3.x gates (without those). The RSSI/SNR in STATUS are now
those of the gate's last authenticated frame from the house, ACKs included (before, only commands, config writes
and diagnostics requests updated them).
