# Bench testing

Every bench wire, and whether it's confirmed, is in [tools/bench-wiring](../tools/bench-wiring/README.md)
(`wiring.json`, viewed and edited in a local page). Always serve that page with `python tools/bench-wiring/serve.py`
(port 8001), which saves the page's edits back to the file; never with `python -m http.server` or any other server,
where Save falls back to a file dialog (the page shows a warning banner). If something else holds port 8001, stop it
and start `serve.py`. Update `wiring.json` in the same session whenever a wire moves or is confirmed; the user edits
it in the page.

The bench: both MKR boards powered into VIN from bench bucks (gate 24 V → 5 V, house 12 V → 5 V, the house rail shared
with the Shelly and the IN2 opto) and on USB through power-blocked cables, the [GateSim](../tools/GateSim/README.md) Uno standing in for the opener on
the gate board (a separate bench-only sketch, `--fqbn arduino:avr:uno`, not GateLink firmware), and the real Shelly
on the house board (driven through Home Assistant). Most of the checklist below is automated by the
[end-to-end suite](../tests/e2e/README.md); the test covering each item is named after it.

The boards can be driven from scripts over USB serial with the same JSON console the web page uses
([console.md](console.md); `python tools/gatelink.py house status`, or pyserial: `{"id":1,"cmd":"status"}`). With the
GateSim wired to the gate board, the full loop (controller → house → gate → simulated opener) can be scripted.

## Bench rules

- **Uploads keep the saved config and key** from 0.5.0 (SPI flash; `ports` shows `cfg spi`). Uploading 0.5.0
  over an older version, a board showing `cfg internal`, or `config.reset` leaves defaults (no role or key,
  `tx_power` 17; `power_sense`, `ctrl_power_sense` and `ctrl_power_pmic` on, `ctrl_confirm_ms` 500). `python tools/gatelink.py snapshot` before flashing
  and `restore` after puts them back (`/flash` in Claude Code does both); the key is read from `~/.gatelink_key`.
  Boards can't read the key back (if `~/.gatelink_key` is lost, `key.set` a fresh one on both boards), so keep an
  encrypted backup (`tools/gatelink.py key backup`, [key-management.md](key-management.md)); `ports`
  shows each board's key id (0.13.8 on), which should match `tools/gatelink.py key id` for the file.
- **On USB power, keep `tx_power` at about 5 dBm** on both boards: a full-power transmit while a relay is
  energized crashed the board into watchdog resets (`reset_cause` in Status).
- Inputs are active when jumpered to **3.3 V** (not GND). With `power_sense` on, keep gate IN3 at 3.3 V (or the
  GateSim powered), or the gate reads `no_power`.
- Without jumpers, turning on an input's `inN_invert` makes an open input read active, which is handy on a bare
  board; turn it back off afterwards. Never use invert to fix polarity on an installed board.
- Only one program can hold a board's port: disconnect the web console before running scripts or the suite.
- Opening the GateSim's port resets the Uno (the gate briefly sees `no_power`) unless DTR is off.
- **Identify** in the web console strobes a board's LED to tell the two apart.

On/off settings are toggle switches on the Config tab: flip one, click **Apply** (it takes effect immediately),
then **Save** to keep it across reboots.

## New board preflight

Optional, but worth doing before a MKR WAN 1310 goes into the install: one bench board had a weak receiver that
only showed up as occasional CRC errors and lost pongs. Pair the new board with a known-good one:

1. Put the new board in place of one of the bench boards (`python tools/gatelink.py snapshot` first), flash it
   (`/flash`) and give it that board's role, config and key (`restore`; a new board may come up on another COM
   number, so rename the port in `~/.gatelink_config.json`). If it doesn't show up on USB even after a
   double-tap of reset (Windows lists *Device Descriptor Request Failed*), try another USB port or cable.
2. Run `python tools/gatelink.py rftest` (5 minutes of pings both ways; `--seconds` to change). Keep the bench
   setup the same between runs: boards side by side, `tx_power` 5, SF9 (the defaults it was calibrated on).
3. It passes with no CRC errors at either board and at most 1 % of pongs lost (`--max-crc`, `--max-loss`). A
   board's `crc` column counts frames *it* received with a bad CRC, so it names the weak receiver; lost pongs
   can't be pinned on one side. Record the board's USB serial number (printed by `rftest`) with the result.

On a fail, rerun once, then swap the two boards' roles (or shields) and run again: if the CRC errors follow the
board, keep it out of the install.

Results on the bench (0.10.1, SF9, `tx_power` 5, 5 minutes, 2026-10-07):

| Pair | Pongs lost | CRC errors | Avg SNR | Result |
|---|---|---|---|---|
| …183013 (house) + …191117 (gate) | 12 / 372 (3.2 %) | 0 / **7** | 7.3 / 7.2 dB | FAIL: …191117 |
| …183013 (house) + …0C301C (gate) | 1 / 416 (0.2 %) | 0 / 0 | 7.9 / 8.0 dB | PASS |

Board …191117 (`8E4B6C235030534D4D2E3120FF191117`) had CRC errors in every run since 2026-10-02, whatever its role,
shield, antenna, USB cable or channel, with lower SNR than its partner (5.5–7.75 dB against 7.75–8). With the spare
…0C301C in its place both the CRC errors and most of the pong loss went away. Keep …191117 out of the install.

## Checklist

**Still manual:** status LED patterns and **Identify**; the web console itself (Config toggles: flip one → its
row is highlighted as unsaved, Apply → the highlight clears and Status reflects it, Save and reboot → it keeps
its position, Export shows it as 0/1; Tools → Send replay; Tools → remote setting); a board really unpowered
rather than a simulated outage; the Shelly's real 12 V removed (unless the suite has `GATELINK_HA_POWER_ENTITY`).

- House IN1 to 3.3 V (Shelly ON) → gate K1 pulses once; release → gate K2 pulses once.
  *e2e: `test_open_via_controller`, `test_close_via_controller`*
- Gate IN1 to 3.3 V (open limit) → house K1 energizes, K2 releases; gate IN2 to 3.3 V → K1 releases, K2 energizes.
  *e2e: `test_external_moves_followed_without_commands`*
- Travel: from closed (gate IN2 jumpered), remove IN2 → house K2 releases at once but K1 stays off; jumper IN1 → K1
  energizes. Leave both off for `travel_timeout_s` instead → K1 energizes when it expires.
  *e2e: every open/close (mid-travel outputs), `test_jammed_gate`*
- External move: with no command sent, jumper gate IN1 to 3.3 V → house status shows `cause external`,
  **no command sent** (house log shows `sync`, not `cmd_sent`, if the Shelly or a jumper follows K1).
  *e2e: `test_external_moves_followed_without_commands`, `test_external_move_right_after_our_command`*
- Override: jumper gate IN1 (open), turn house IN1 off (CLOSE) → gate reports `timeout` after `travel_timeout_s`,
  house log shows `resync`. *e2e: `test_opener_ignores_command`*
- AC power sense: with the gate closed, release gate IN3 → gate stays `closed`, `ac_power` false, house unchanged;
  toggle house IN1 → gate log `cmd_refused`, no `pulse`, house *Last command* shows *refused: no AC power*, and the
  Shelly is resynced. Also release the closed limit → gate `no_power` (`cause none`), house K2 releases and K1
  energizes. Re-jumper → state follows the limits again. *e2e: `test_ac_loss_limits_trusted`,
  `test_power_loss_at_rest`, `test_power_loss_mid_travel`.*
  Manual: turn the gate's `power_sense` toggle off and Apply (or from the house: Tools → remote setting
  `power_sense` = 0) → IN3 is ignored. *e2e: `test_options.py`*
- Shelly power sense: with the gate open, the Shelly on and the house board on its LiPo, remove the Shelly's 12 V →
  house log `supply 0` and `ctrl_power 0` ~0.2 s before `ctrl` b=1 (the relay drop, ignored); `ctrl_power` b=2
  means the relay dropped first and only `ctrl_confirm_ms` (0.5 s, OFF edges only) saved it. No `cmd_sent`, gate
  no `pulse`; restore it → `ctrl_power 1`, then `sync 1` when the Shelly comes back on. *e2e: `test_controller_faults.py` (simulated unless
  `GATELINK_HA_POWER_ENTITY` is set)*
- Unpower the gate board → after `link_timeout_s` (at least 2.5 × the gate's `heartbeat_s`) house K2 releases
  (sensor open), log `link_down`. *e2e: `test_link_loss_at_rest` (simulated outage)*
- Tools → Send replay on one board → the other board's replay counter increases (or it re-ACKs).
  *e2e: `test_replayed_frames_rejected`*
- Different key on one board → *Peer verified* stays no and `mac_fail` climbs. *e2e: `test_wrong_key_rejected`*
- Reboot the house board with IN1 jumpered to 3.3 V → the gate does not move.
  *e2e: `test_house_reboot_controller_on_gate_closed`*

## Before the install

Open bench and field tasks (real RF at the site, long soak, real controller power) are tracked in
[TODO.md](../TODO.md).
