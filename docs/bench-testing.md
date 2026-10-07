# Bench testing

Every bench wire, and whether it's confirmed, is in [tools/bench-wiring](../tools/bench-wiring/README.md)
(`wiring.json`, viewed and edited in a local page).

The bench: both MKR boards powered into VIN from bench bucks (gate 24 V → 5 V, house 12 V → 5 V, the house rail shared
with the Shelly and the IN2 opto) and on USB through power-blocked cables, the [GateSim](../tools/GateSim/README.md) Uno standing in for the opener on
the gate board, and the real Shelly on the house board (driven through Home Assistant). Most of the checklist
below is automated by the [end-to-end suite](../tests/e2e/README.md); the test covering each item is named after
it.

## Bench rules

- **Uploads keep the saved config and key** from 0.5.0 (SPI flash; `ports` shows `cfg spi`). Uploading 0.5.0
  over an older version, a board showing `cfg internal`, or `config.reset` leaves defaults (no role or key,
  `tx_power` 17; `power_sense` and `ctrl_power_sense` on). `python tools/gatelink.py snapshot` before flashing
  and `restore` after puts them back; the key is read from `~/.gatelink_key`. Boards can't read the key back,
  so keep it safe (e.g. a password manager).
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
- Shelly power sense: with the gate open and the Shelly on, remove the Shelly's 12 V → house log `ctrl_power 0`
  (plus `ctrl` b=1, or `ctrl_power` b=2 if the relay dropped first), no `cmd_sent`, gate no `pulse`; restore it →
  `ctrl_power 1`, then `sync 1` when the Shelly comes back on. Compare the `ctrl` and `ctrl_power` times to check
  `ctrl_confirm_ms` (3 s, OFF edges only) covers the gap. *e2e: `test_controller_faults.py` (simulated unless
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
