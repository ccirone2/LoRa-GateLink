# USB console reference

Each board speaks newline-delimited JSON over USB serial (115200 baud). The web console, the end-to-end suite
(`tests/e2e/gatelink/board.py`) and `tools/gatelink.py` all use this contract. It is implemented in
`firmware/GateLink/console.cpp`; change it together with `web/app.js` and `tests/e2e/gatelink/`.

Only one program can hold the port: disconnect the web console before scripting. The firmware writes only while
DTR is asserted (pyserial does that by default).

If the host stops reading for more than 70 ms mid-line, the SAMD USB core drops the rest of that line. The
firmware then starts its next line with an extra newline, so a client sees one cut line (not valid JSON; skip it)
and then an empty line. Clients should ignore both and time out the request the cut line belonged to.

## Requests

A request is `{"id": <int>, "cmd": "<name>", ...}`; the reply echoes the id: `{"id": <int>, "ok": true|false, ...}`,
with `"error"` on failure. A line that isn't valid JSON is answered `bad json`, one over 1023 characters
`line too long`, both with the id if it can be found in the raw text.

| Command | Arguments | Reply / effect |
|---|---|---|
| `info` | | `fw`, `board`, `role` (running), `saved_role`, `key_set`, `cfg_store`, `flash_id` (SPI flash JEDEC id, hex; `000000` if it doesn't answer) |
| `status` | | `status` object (below) |
| `config.get` | | `params` (name → value), `meta` (per param: `name`, `id`, `min`, `max`, `radio`, `remote`, `reboot`), `key_set` |
| `config.set` | `params`: {name: int} | Applies (doesn't save). `applied`, `errors` (names rejected), `reboot_required`. Unchanged values are skipped. Send at most ~8 params per request |
| `config.save` | | Writes the running config to flash |
| `config.reset` | | Defaults, saved (key cleared); reboot required |
| `key.set` | `key`: 32 hex chars | Sets and saves only the key; restarts the radio and sessions. The key can't be read back |
| `relay.test` | `k`: 1\|2, `ms`: 50–5000 | Pulses a relay (a gate test pulse sets a target like a command) |
| `radio.ping` | | Sends a PING; a `pong` event follows if the peer answers |
| `remote.diag` | | House only. Requests the gate's diagnostics; a `remote_diag` event follows |
| `remote.set` | `name`, `value` (int) | House only, remote-writable params only. `busy` while one is pending; a `remote_set` event follows |
| `log.get` | | `log`: the ring buffer (64 entries: `t`, `ev`, `a`, `b`), `now` (board millis) |
| `hist.get` | `from` (bucket number, default the oldest), `n` (1–12, default 12) | One page of the link history (below): `period_s`, `now_s`, `oldest`, `current`, `fields`, `rows` |
| `hist.clear` | `period_s` (60–3600, default unchanged) | Empties the history and restarts it at bucket 0. The period lasts until the next boot (then 3600) |
| `reboot` | | Replies, then resets the board (USB re-enumerates) |
| `identify` | `ms` (default 6000, max 60000) | Strobes the LED |
| `debug.replay` | | Re-sends the last frame as-is, to test the peer's replay protection |

Settings are listed in the `PARAMS[]` table in `firmware/GateLink/config.cpp`; the web console's Config tab
shows each with help text. Saved config and the key live in the board's SPI flash chip (`cfg_store` `spi`) and
survive firmware uploads; they're stored per param id, so a newer firmware keeps every setting it still knows
(the `cfg` log event counts the ones it dropped). If the chip doesn't answer, config falls back to program
flash (`cfg_store` `internal`), which every upload erases. `config.save`, `config.reset` and `key.set` reply
`ok: false`, `error: "flash write failed"` if the write doesn't verify (the change still applies until reboot).
Each save re-initialises the radio (the flash chip shares its bus): about 0.5 s off the air, which the link's
retries cover.
`config.reset` erases the saved config and key.

## Status

Common fields: `fw`, `role`, `reboot_pending`, `uptime_ms`, `radio_ok`, `radio_faults`, `reset_cause`
(`watchdog`, `brownout`, `power_on`, `reset_pin`, `software`), `cfg_loaded`, `cfg_store` (`spi` or
`internal`, as in `info`), `key_set`, `io` (`in1`–`in4`, `k1`,
`k2`) and `link` (`verified`, `age_ms`, `rssi`, `snr`, `tx`, `rx`, `retries`, `giveups`, `mac_fail`, `replay`,
`sessions`, `lbt_defers`, `lbt_forced`, `crc_err` (frames received with a bad CRC), `noise` (smoothed noise floor,
dBm; null before the first sample)), and `free_ram` (bytes between the heap's high-water mark and the stack).

- **Gate:** `gate` (`unknown`, `closed`, `open`, `between`, `fault`, `no_power`), `cause` (`none`, `lora`,
  `external`), `last_result` (`none`, `reached`, `timeout`, `already`), `target`, `last_cmd_id`, `power_sense`.
- **House:** the gate's `gate`, `cause`, `last_result` and `target` as last reported, plus `link_up`,
  `link_timeout_eff_s`, `armed`, `ctrl` (controller level), `ctrl_power`, `sync_window`, `resyncing`, `cmd_id`,
  `cmd_pending`, `cmd_result` (ACK result: 0 ok, 1 already, 2 rejected, 4 opener unpowered; −1 none, −2 gave up)
  and `remote` (the gate's `uptime_s`, `rssi`, `snr`, `heartbeat_s`, `open_limit`, `close_limit`, `k1`, `k2`,
  `in3`, `in4`; from gate firmware 0.4.0 also its `retries`, `giveups`, `crc_err` (running totals, low 16 bits)
  and `noise`).

## Link history

Each board keeps a RAM ring of buckets: by default an hour each, the last 96 plus the one in progress (four
days). Buckets are numbered from boot (or `hist.clear`), so bucket `i` started `now_s − i × period_s` seconds
before the reply. Everything is lost on a reset; the reboot is in the log. It is for diagnostics only and never
affects the gate or the outputs. The web console charts it (Tools → Link history), and
`tools/gatelink.py <board> hist [--csv FILE]` fetches every page as CSV.

Each row lists the values in `fields` order. Counters are what happened during the bucket:

| Field | Meaning |
|---|---|
| `idx` | Bucket number |
| `tx`, `rx` | Frames sent; authenticated frames received |
| `retries`, `giveups` | Reliable-message resends, and messages given up |
| `crc_err`, `mac_fail` | Frames received with a bad CRC; frames failing authentication |
| `lbt_defers`, `lbt_forced` | Frames held for a busy channel; sent anyway after the cap |
| `sessions`, `radio_faults` | Peer sessions verified; radio faults |
| `down_s` | Seconds with the link down (house: `link_up` false; gate: nothing heard for `link_timeout_s`) |
| `rssi_min`, `rssi_avg`, `snr_min`, `snr_avg` | Levels of the frames received (dBm, dB; null if none) |
| `noise_avg`, `noise_max` | In-channel noise floor (dBm), read 4× a second while no LoRa frame is on the air. A reading followed by a frame is dropped, as it may be that frame's start, so busy periods have fewer readings. Non-LoRa signals (e.g. Z-Wave) count as noise |
| `peer_n` | House: STATUS frames from the gate that carried its RSSI (the `peer_` levels average over them) |
| `peer_rssi_min`, `peer_rssi_avg`, `peer_snr_min`, `peer_snr_avg` | House: levels at the gate of the house's frames |
| `peer_noise_avg`, `peer_noise_max` | House: the gate's noise floor |
| `peer_retries`, `peer_giveups`, `peer_crc_err` | House: the gate's counters, from its STATUS (its retries are lost STATUS frames or their ACKs) |

Averages are rounded to 0.25 dB. On the gate the `peer_` columns stay empty.

## Events

Unsolicited lines carry `"event"` instead of `"id"`:

| Event | Fields | When |
|---|---|---|
| `log` | `t`, `ev`, `a`, `b` | Every log entry, as it is logged |
| `status` | `status` | House: each STATUS received from the gate |
| `pong` | `ping_id`, `rtt_ms`, `rssi`, `snr`, `peer_rssi`, `peer_snr` | Answer to `radio.ping` |
| `remote_diag` | `fw`, `uptime_s`, `counters`, `params` | Answer to `remote.diag` |
| `remote_set` | `acked`, `ok` | Outcome of `remote.set` |

## Log events

From `firmware/GateLink/log.h` (`a`/`b` meanings):

| Event | a | b |
|---|---|---|
| `boot` | reset cause (PM RCAUSE bits) | role |
| `radio_fail` | 0 init failed, 1 TX fault, 2 reset seen in RX, 3 init retry succeeded | fault count |
| `link_up`, `link_down`, `mac_fail` | | |
| `session` | peer session accepted | |
| `replay` | seq | last seq |
| `tx_giveup` | message type | seq |
| `cmd_sent` | action (1 open, 2 close) | command id |
| `cmd_suppressed` | action | gate state |
| `cmd_dropped` | action (link down / TTL) | |
| `cmd_rx` | action | command id |
| `cmd_dup` | command id | |
| `cmd_refused` | action | command id (opener unpowered) |
| `pulse` | relay | ms |
| `gate_state` | state | cause |
| `travel_timeout` | target state | |
| `ctrl` | controller level | 1 if ignored (controller unpowered) |
| `ctrl_power` | controller power level | action discarded by the power loss (0 none) |
| `sync` | level (edge caused by our K1 sync) | |
| `resync` | target level | |
| `cfg_remote` | param id | value |
| `input` | spare input number (house 2/3/4, gate 3/4) | level |
| `lbt_forced` | message type | ms the channel stayed busy |
| `cfg` | at boot, config source: 0 defaults, 1 SPI flash, 2 program flash | saved settings dropped (unknown id or out of range) |

## Example

```sh
python tools/gatelink.py ports                 # boards, roles, firmware, link
python tools/gatelink.py house status
python tools/gatelink.py gate relay.test k=1 ms=500
```
