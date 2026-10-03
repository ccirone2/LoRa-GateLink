# USB console reference

Each board speaks newline-delimited JSON over USB serial (115200 baud). The web console, the end-to-end suite
(`tests/e2e/gatelink/board.py`) and `tools/gatelink.py` all use this contract. It is implemented in
`firmware/GateLink/console.cpp`; change it together with `web/app.js` and `tests/e2e/gatelink/`.

Only one program can hold the port: disconnect the web console before scripting. The firmware writes only while
DTR is asserted (pyserial does that by default).

## Requests

A request is `{"id": <int>, "cmd": "<name>", ...}`; the reply echoes the id: `{"id": <int>, "ok": true|false, ...}`,
with `"error"` on failure. A line that isn't valid JSON is answered `bad json`, one over 1023 characters
`line too long`, both with the id if it can be found in the raw text.

| Command | Arguments | Reply / effect |
|---|---|---|
| `info` | | `fw`, `board`, `role` (running), `saved_role`, `key_set` |
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
| `reboot` | | Replies, then resets the board (USB re-enumerates) |
| `identify` | `ms` (default 6000, max 60000) | Strobes the LED |
| `debug.replay` | | Re-sends the last frame as-is, to test the peer's replay protection |

Settings are listed in the `PARAMS[]` table in `firmware/GateLink/config.cpp`; the web console's Config tab
shows each with help text. Every firmware upload erases saved config and the key.

## Status

Common fields: `fw`, `role`, `reboot_pending`, `uptime_ms`, `radio_ok`, `radio_faults`, `reset_cause`
(`watchdog`, `brownout`, `power_on`, `reset_pin`, `software`), `cfg_loaded`, `key_set`, `io` (`in1`–`in4`, `k1`,
`k2`) and `link` (`verified`, `age_ms`, `rssi`, `snr`, `tx`, `rx`, `retries`, `giveups`, `mac_fail`, `replay`,
`sessions`, `lbt_defers`, `lbt_forced`).

- **Gate:** `gate` (`unknown`, `closed`, `open`, `between`, `fault`, `no_power`), `cause` (`none`, `lora`,
  `external`), `last_result` (`none`, `reached`, `timeout`, `already`), `target`, `last_cmd_id`, `power_sense`.
- **House:** the gate's `gate`, `cause`, `last_result` and `target` as last reported, plus `link_up`,
  `link_timeout_eff_s`, `armed`, `ctrl` (controller level), `ctrl_power`, `sync_window`, `resyncing`, `cmd_id`,
  `cmd_pending`, `cmd_result` (ACK result: 0 ok, 1 already, 2 rejected, 4 opener unpowered; −1 none, −2 gave up)
  and `remote` (the gate's `uptime_s`, `rssi`, `snr`, `heartbeat_s`, `open_limit`, `close_limit`, `k1`, `k2`,
  `in3`, `in4`).

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

## Example

```sh
python tools/gatelink.py ports                 # boards, roles, firmware, link
python tools/gatelink.py house status
python tools/gatelink.py gate relay.test k=1 ms=500
```
