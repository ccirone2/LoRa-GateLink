# USB console reference

Each board speaks newline-delimited JSON over USB serial (115200 baud). The web console, the end-to-end suite and
`tools/gatelink.py` all use this contract (the Python side through `tools/gatelink_client/`). It is implemented in
`firmware/GateLink/console.cpp`; change it together with the web console (`web/js/`), `tools/gatelink_client/` and `tests/e2e/`.

Only one program can hold the port: disconnect the web console before scripting. The firmware writes only while
DTR is asserted (pyserial does that by default).

**UART console.** With `uart_console` 1 the same console also runs on Serial1 (pins 13 RX / 14 TX, 3.3 V,
250,000 baud, 8N1), for a USB-to-UART adapter on the bench: the adapter's port survives the board losing power,
so a power test sees the `boot` event as soon as the board is back, without waiting for USB to re-enumerate. Each
port has its own request line; a reply goes to the port the request came from, and events and log lines go to
both. The UART is always written (no DTR), and a write waits while its 256-byte buffer is full: a ~5 KB
`config.get` reply holds the loop for ~200 ms. (At 1 Mbaud, the rate in 0.8.0, ~4 % of requests arrived garbled on
the bench.) Even at 250 kbaud ~1 % of requests arrived garbled while the board was busy (~8 % of 1,000 sent back to back on
0.12.0), some into
still-valid JSON with a number changed, so the UART refuses a request without a matching CRC (`crc required`,
`bad crc`; see Requests). A refused request never ran, so a client can safely send it again. Wire adapter TX → 10 kΩ → pin 13, pin 14 → adapter RX, GND to GND,
and leave the adapter's VCC unconnected. Without the resistor the bench gate board still powered down and came
back with a clean `power_on` reset, but the adapter's TX drove current into the unpowered chip's pins (its own
bytes came back garbled while the board was off); the resistor keeps that small. Expect junk on the adapter
around a power cut: the firmware starts its first UART line with a newline, so the junk ends there and the
`boot` event arrives intact, and a request garbled by it is refused.
`uart_console` takes effect at once; it is off by default and should stay off at the install.

Lines go to USB one 64-byte packet at a time. If the host leaves a packet untaken for more than 70 ms (a client that
stops reading until the host's buffer is full), the rest of that line is dropped, and so are later lines while that
packet is still waiting; status `usb_cut` counts the lines lost. The firmware then starts its next line with an
extra newline, so a client sees one cut line (not valid JSON; skip it) and then an empty line. Clients should ignore
both and time out the request the cut line belonged to. The board handles one request per port per loop pass, so
requests sent back to back wait in the port's buffer (USB holds back the rest).

## Requests

A request is `{"id": <int>, "cmd": "<name>", ...}`; the reply echoes the id: `{"id": <int>, "ok": true|false, ...}`,
with `"error"` on failure. A line that isn't valid JSON is answered `bad json`, one over 1023 characters
`line too long`, both with the id if it can be found in the raw text. An unrecognised command is answered
`unknown cmd`.

A request may end with a CRC-32 of itself (IEEE, as zlib's `crc32`) as its last member, `"crc":"<8 hex digits>"`,
taken over the request as it reads without that member: send `body[:-1] + ',"crc":"%08x"}' % crc32(body)`, where
`body` is the JSON text ending in `}`. Where it is present (or on the UART, where it is required) a mismatch is
answered `bad crc` and the request isn't run. The web console doesn't send one; the e2e suite always does.

| Command | Arguments | Reply / effect |
|---|---|---|
| `info` | | `fw`, `board`, `role` (running), `saved_role`, `key_set`, `key_id` (see below), `cfg_store`, `flash_id` (SPI flash JEDEC id, hex; `000000` if it doesn't answer), `boot_count` (boots counted in the SPI flash; session ids are drawn from it, and each link history bucket carries it; 0 if the chip didn't answer, its read was garbled, or should the count ever run out) |
| `status` | | `status` object (below) |
| `config.get` | | `params` (name → value), `meta` (per param: `name`, `id`, `min`, `max`, `radio`, `remote`, `reboot`), `key_set`, `key_id` |
| `config.set` | `params`: {name: int} | Applies (doesn't save). `applied`, `errors` (names rejected: unknown, not an integer or out of range; `ok` is false if any), `reboot_required`. Unchanged values are skipped. Radio params restart the radio. Send at most ~8 params per request |
| `config.save` | | Writes the running config to flash |
| `config.reset` | | Running config back to defaults at once (key cleared, so the link stops) and the saved config and key erased; `reboot_required` |
| `key.set` | `key`: 32 hex chars | Sets and saves only the key; restarts the radio and sessions. The key can't be read back; `info` then reports its key id. A weak key is refused (see Weak keys, below) |
| `relay.test` | `k`: 1\|2, `ms`: 50–5000 (default 500) | Pulses a relay (a gate test pulse sets a target like a command, unless the gate is already at that limit or the opener is unpowered). Needs a role. On the gate, `busy` while that relay is pulsing already (or waiting out the interlock to start): a running pulse is never restarted. Testing the other relay cuts it short, as a reversal does |
| `radio.ping` | | Sends a PING; a `pong` event follows if the peer answers. Needs a role, a key and a working radio |
| `remote.diag` | | House only. Requests the gate's diagnostics; a `remote_diag` event follows |
| `remote.set` | `name`, `value` (int) | House only, remote-writable params only. `busy` while one is pending; a `remote_set` event follows |
| `log.get` | | `log`: the ring buffer (64 entries: `t`, `ev`, `a`, `b`), `now` (board millis) |
| `hist.get` | `from` (bucket number, default the oldest), `n` (1–12, default 12) | One page of the link history (below): `period_s`, `now_s`, `oldest`, `current`, `persist`, `fields`, `rows` |
| `hist.clear` | `period_s` (60–3600, default unchanged) | Empties the history, in RAM and in the SPI flash, and restarts it at bucket 0. The period is kept with the history, across resets, until the next `hist.clear` (3600 on a board that has none). Held while a relay pulses, and fails as a save does if the flash wasn't cleared (below; a reset would then bring the old history back) |
| `reboot` | | Replies, then resets the board 100 ms later (USB re-enumerates). Held while a relay pulses, like a save (below) |
| `identify` | `ms` (default 6000, max 60000) | Strobes the LED |
| `debug.replay` | `hello` (bool, default false) | Re-sends the last frame as-is, to test the peer's replay protection. With `hello`, re-sends this board's first HELLO since boot instead (an old session's once the link has restarted, e.g. after a radio param change). Sent as-is, without listening first; `sent` is false if there was nothing to replay or the radio was busy |
| `debug.mute` | `ms` (max 60000; 0 stops) | The link ignores received frames for `ms`, as if the receiver had gone deaf (it still transmits) |
| `debug.reboot_after_cmd` | | Gate only, one-shot: the next command that pulses resets the gate right after the pulse, without ACKing it (a power cut or crash at the worst moment) |

**Key id.** `key_id` (from 0.13.8) is 8 lowercase hex digits: the first 4 bytes of HMAC-SHA256(key,
`"GateLink key id v1"`), null while no key is set. It tells keys apart without revealing them: two boards with the
same `key_id` hold the same key, and a backup file names the id of the key inside
([key-management.md](key-management.md)). Firmware before 0.13.8 doesn't send it.

**Weak keys.** From 0.13.8 `key.set` refuses, with `weak key: all bytes equal, counting by one, or 8 or fewer
distinct bytes`, a key whose 16 bytes are all equal, count up or down by one (mod 256: `00 01 02 … 0f`,
`fa fb … ff 00 … 09`, `0f 0e … 00`), or take 8 or fewer distinct values (`deadbeef` four times). A random key is
weak about once in 10^10; the web console and `tools/gatelink.py key gen` never offer one.

Settings are listed in [config.md](config.md) (range, default, when a change applies, what it does), generated
from the `PARAMS[]` table in `firmware/GateLink/config.cpp` and the Config tab's help text. Saved config and the
key live in the board's SPI flash chip (`cfg_store` `spi`) and survive firmware uploads; they're stored per param id, so a newer firmware keeps every setting it still knows
(the `cfg` log event counts the ones it didn't accept). Settings an older firmware doesn't know are written back
by its saves, so after a downgrade and back they're still there; a value out of the older firmware's range is
replaced by its own. If the chip doesn't answer, config falls back to program flash (`cfg_store` `internal`),
which every upload erases. `config.save`, `config.reset` and `key.set` reply `ok: false`, `error: "flash write
failed"` if the write doesn't verify (the change still applies until reboot). `key.set` and remote writes save on
top of what's saved, so they also fail, writing nothing, if the saved record can't be read back intact (twice),
or reads older than the newest this boot has written or loaded; `config.save` writes the running config and
clears that.
Each save re-initialises the radio (the flash chip shares its bus): about 0.5 s off the air, which the link's
retries cover. Saving, and restarting the radio, stops the loop for up to ~1 s, so while a relay pulse runs (or
waits for its interlock start) `config.set`, `config.save`, `config.reset`, `hist.clear`, `key.set` and `reboot` wait in the port's
buffer and are answered after it (at most the pulse's length, 5 s for a `relay.test`); a held relay would
otherwise stay on until the save was done (or, for `reboot`, until the reset 100 ms after its reply). A radio that
faulted (`radio_fail` 1 or 2) stays down until the pulse is over too, and is restarted then.
`config.reset` erases the saved config and key.

## Status

Common fields: `fw`, `role`, `reboot_pending`, `uptime_ms`, `radio_ok`, `radio_faults`, `reset_cause`
(`watchdog`, `brownout`, `power_on`, `reset_pin`, `software`, `unknown`), `cfg_loaded`, `cfg_store` (`spi` or
`internal`, as in `info`), `supply` (the board's VIN power good, from the charger chip; false = running on the
LiPo; null if the chip didn't answer), `key_set`, `io` (`in1`–`in4`, `k1`,
`k2`) and `link` (`verified`, `age_ms` (since the last authenticated frame; -1 = never; tops out at ~12.4 days), `rssi`, `snr`, `tx`, `rx`, `retries`, `giveups`, `mac_fail`, `replay`,
`sessions`, `lbt_defers`, `lbt_forced`, `crc_err` (frames received with a bad CRC), `noise` (smoothed noise floor,
dBm; null before the first sample), `fei` (frequency error of the last good frame, Hz: the peer's carrier against
ours, i.e. the two boards' crystal offset)), `free_ram` (bytes between the heap's high-water mark and the stack),
`usb_cut` (console lines lost on USB since boot, see above), `loop_max_us` (the longest loop pass since boot, µs:
how close the loop has come to the 8 s watchdog; a flash save or radio restart takes up to ~1 s), `fault_out` (the
fault output D5 as driven: true HIGH, false LOW, null while the setting is off; see Fault output, below) and `health`
(`starting`, `radio`, `link`, `ac`, `no_power`, `fault`: the problems this board counts right now, whether D5 is
used or not; an empty list when healthy). Both from 0.14.0.

- **Gate:** `gate` (`unknown`, `closed`, `open`, `between`, `fault`, `no_power`), `cause` (`none`, `lora`,
  `external`), `last_result` (`none`, `reached`, `timeout`, `already`), `target` (`""` when none), `last_cmd_id`,
  `power_sense`, `ac_power` (IN3, or true with `power_sense` off), `settling` (after boot: the first STATUS waits
  until the inputs have been steady for 3 s, at most 10 s).
- **House:** the gate's `gate`, `cause`, `last_result` and `target` as last reported, plus `link_up` (a STATUS from the
  gate within `link_timeout_eff_s`; other frames, and a STATUS older than the last one taken, don't count),
  `link_timeout_eff_s`, `armed`, `ctrl` (controller level), `ctrl_power`, `sync_window`, `resyncing`, `cmd_id`,
  `cmd_pending`, `cmd_result` (ACK result: 0 ok, 1 already, 2 rejected, 4 no AC power; −1 none, −2 gave up)
  and `remote` (the gate's `uptime_s`, `rssi`, `snr`, `heartbeat_s`, `open_limit`, `close_limit`, `k1`, `k2`,
  `in3`, `in4`, `ac_power` (from gate firmware 0.7.0; true from older gates); from gate firmware 0.4.0 also its `retries`, `giveups`, `crc_err` (running totals, low 16 bits)
  and `noise` (its average since the previous STATUS; null if it had no sample); from gate firmware 0.13.0 also its
  `travel_timeout_s`, which the house then uses for its travel hold).

## Fault output

From 0.14.0 either board can drive D5 as a "needs attention" output for the alarm system (`fault_out`, off by
default; wiring in [hardware.md](hardware.md#fault-output-d5)): HIGH while the board is healthy, LOW otherwise, so a
dead board, a cut wire or a reset reads as a fault too. A problem must last `fault_hold_s` (default 10 s) before D5
drops, so a short dropout doesn't trip the alarm; recovery raises it at once, and `fault_hold_s` 0 drops it as soon as
a problem shows. After a reset D5 stays LOW until the board has started up (the house armed: its first STATUS from the
gate plus `sync_window_ms`; the gate: its inputs settled and its first STATUS sent), so a reboot reads as a short fault.
With `fault_out` off D5 is an input with its pull-down, as before. D5 is not K1 or K2: it commands nothing, and nothing
the boards decide depends on it. Status `health` lists the problems either way, and log `health` records each change
of D5 with the problems then as bits:

| Bit | Problem | House | Gate |
|---|---|---|---|
| 1 | `starting` | not armed yet since boot | inputs still settling after boot |
| 2 | `radio` | the radio isn't working (`radio_ok` false) | same |
| 4 | `link` | no STATUS within `link_timeout_eff_s` (`link_up` false) | nothing heard from the house within `link_timeout_s`, at least 2.5 × its own `heartbeat_s` (the house answers each STATUS) |
| 8 | `ac` | the gate's STATUS says AC lost | IN3 off (with `power_sense`) |
| 16 | `no_power` | the gate reports `no_power` | the gate reads `no_power` |
| 32 | `fault` | the gate reports `fault` | the gate reads `fault` (both limits) |

With no role set, `starting`, `radio` and `link` stay listed.

## Link history

Each board keeps a RAM ring of buckets: by default an hour each, the last 96 plus the one in progress (four
days). It is for diagnostics only and never affects the gate or the outputs. The web console charts it (Tools → Link
history), and `tools/gatelink.py <board> hist [--csv FILE]` fetches every page as CSV.

Since firmware 0.14.0 each bucket is also written to the SPI flash as it completes (the reply's `persist` is true), so
a reset loses only the bucket in progress: at boot the newest buckets are loaded back, as they were, and the numbering
goes on from the newest one. Buckets are numbered from the last `hist.clear`, as if the history had run without a
break: bucket `i` started `now_s − i × period_s` seconds before the reply if no reset came after it (its `boot` is the
newest row's); one recorded before a reset (a lower `boot`) started earlier than that, by however long the board was
down plus the part of a bucket the reset lost. Each write stops the loop and takes the radio off the air for about
0.5 s, as a save does, once per bucket and never while a relay pulses (it waits for the pulse). A write cut short by a
power cut is skipped at the next boot and loses nothing else; one that failed (it didn't read back intact) ends what
the next boot loads, which is then only the buckets after it. `persist` is false without the flash chip (`cfg_store`
`internal`), or after a boot whose read of the log came back garbled twice (until the next boot): the history is then
kept in RAM only and lost on a reset, as before 0.14.0. The log takes sectors 4–19 of the chip: a lap
of 512 buckets (three weeks of hours), of which a boot loads the newest 96; the oldest sector is erased as it wraps
(see [protocol.md](protocol.md#spi-flash)).

Each row lists the values in `fields` order. Counters are what happened during the bucket:

| Field | Meaning |
|---|---|
| `idx` | Bucket number |
| `tx`, `rx` | Frames sent; authenticated frames received |
| `retries`, `giveups` | Reliable-message resends, and messages given up |
| `crc_err`, `mac_fail` | Frames received with a bad CRC; frames failing authentication |
| `lbt_defers`, `lbt_forced` | Frames held for a busy channel; sent anyway after the cap (a channel that stayed busy, or frames heard so often that the gaps never opened) |
| `sessions`, `radio_faults` | Peer sessions verified; radio faults |
| `down_s` | Seconds with the link down (house: `link_up` false; gate: nothing heard for `link_timeout_s`, at least 2.5 × its `heartbeat_s`) |
| `rssi_min`, `rssi_avg`, `snr_min`, `snr_avg` | Levels of the frames received (dBm, dB; null if none) |
| `noise_avg`, `noise_max` | In-channel noise floor (dBm), read 4× a second while no LoRa frame is on the air. A reading followed by a frame is dropped, as it may be that frame's start, so busy periods have fewer readings. Non-LoRa signals (e.g. Z-Wave) count as noise |
| `peer_n` | House: STATUS frames from the gate that carried its RSSI (the `peer_` levels average over them) |
| `peer_rssi_min`, `peer_rssi_avg`, `peer_snr_min`, `peer_snr_avg` | House: levels at the gate of the house's frames |
| `peer_noise_avg`, `peer_noise_max` | House: the gate's noise floor |
| `peer_retries`, `peer_giveups`, `peer_crc_err` | House: the gate's counters, from its STATUS (its retries are lost STATUS frames or their ACKs) |
| `boot` | The boot the bucket was recorded in (`info`'s `boot_count`; 0 if the chip didn't give one): where it changes from one bucket to the next, the board reset between them |

Averages are rounded to 0.25 dB. On the gate the `peer_` levels stay null and its `peer_` counts 0.

## Events

Unsolicited lines carry `"event"` instead of `"id"`:

| Event | Fields | When |
|---|---|---|
| `log` | `t`, `ev`, `a`, `b` | Every log entry, as it is logged |
| `status` | `status` | House: each STATUS received from the gate |
| `pong` | `ping_id`, `rtt_ms`, `rssi`, `snr`, `peer_rssi`, `peer_snr`, `fei` (Hz, as in status) | Answer to `radio.ping` |
| `remote_diag` | `fw`, `uptime_s`, `counters` (`tx`, `rx`, `mac_fail`, `replay`, `retries`, `giveups`), `params` (the gate's remote-writable params) | Answer to `remote.diag` |
| `remote_set` | `acked`, `ok` (the gate accepted and saved it), `applied` (accepted, maybe not saved: it reverts when the gate reboots) | Outcome of `remote.set` |

## Log events

From `firmware/GateLink/log.h` (`a`/`b` meanings):

| Event | a | b |
|---|---|---|
| `boot` | reset cause (PM RCAUSE bits) | role |
| `radio_fail` | 0 init failed (once until a retry succeeds, every 5 s), 1 TX fault, 2 reset seen in RX (after 1 or 2 the radio restarts at once, or once a relay pulse is over), 3 init retry succeeded | fault count |
| `link_up`, `link_down` | (house) | |
| `mac_fail` | message type | RSSI |
| `session` | peer session id (accepted) | |
| `replay` | seq | last seq (house: also a STATUS older than the last one it took, withheld and delivered late, which it drops: b = that one's seq) |
| `tx_giveup` | message type | seq |
| `cmd_sent` | action (1 open, 2 close) | command id |
| `cmd_suppressed` | action | gate state |
| `cmd_dropped` | action (not ACKed within `cmd_ttl_s`, the link restarted, or the gate restarted before ACKing it) | command id |
| `cmd_rx` | action | command id |
| `cmd_dup` | command id | |
| `cmd_refused` | action | command id (no AC power) |
| `pulse` | relay | ms |
| `gate_state` | state | cause |
| `travel_timeout` | target state | |
| `ctrl` | controller level | 1 if ignored (controller unpowered) |
| `ctrl_power` | controller power level | action discarded by the power loss (0 none) |
| `sync` | level (edge caused by our K1 sync) | |
| `resync` | target level | |
| `cfg_remote` | param id | value |
| `input` | spare input number (house 2/3/4, 2 only with `ctrl_power_sense` off; gate 3/4) | level |
| `lbt_forced` | message type | ms it waited (for a channel that stayed busy, or frames heard so often the gaps never opened) |
| `cfg` | at boot, config source: 0 defaults, 1 SPI flash, 2 program flash | saved settings dropped (unknown id or out of range) |
| `supply` | board supply (VIN) power good: 1 good, 0 lost (on the LiPo); at boot −1 if the charger didn't answer | charger status register (REG08) |
| `cmd_hold` | pending command: 1 held (a HELLO came from an unverified session), 0 sent after all (the verified session answered), 2 dropped (the new session verified: the gate restarted) | that session id |
| `health` | fault output D5 changed (`fault_out`, see Fault output): 1 HIGH (healthy), 0 LOW (needs attention), −1 released (`fault_out` turned off: an input again) | the problems then, as bits: 1 starting, 2 radio, 4 link, 8 ac, 16 no_power, 32 fault (0 = none) |

## Example

```sh
python tools/gatelink.py ports                 # boards, roles, firmware, link
python tools/gatelink.py house status
python tools/gatelink.py gate relay.test k=1 ms=500
```
