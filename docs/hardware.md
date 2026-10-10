# Hardware and wiring

Wiring and device settings for both boards. The web console's **Install** tab shows the same wiring per board
(diagram, terminal table and notes); it is driven by the `WIRING` table in `web/js/wiring.js`, so keep that, this page
and `firmware/GateLink/pins.h` in sync.

## Board and pins

- Arduino MKR WAN 1310 on an MKR Relay Proto Shield, one per end. The same firmware runs on both; the role
  (house or gate) is set in config.
- Relays: K1 = D1, K2 = D2 on the Relay Proto Shield. Inputs: IN1–IN4 = A1–A4, all with the SAMD21's **internal
  pull-down**: an input is active when driven to 3.3 V (a PNP opto output or a contact to the board's 3.3 V) and
  reads off when open. **Never put more than 3.3 V on an input.** Change pins in `firmware/GateLink/pins.h` if your
  wiring differs; check the shield silkscreen.
- The saved config and key live in the board's on-board flash chip (W25Q16JV, select on internal pin 32). It
  shares the radio module's internal SPI bus, so no header pins are used. Each save holds the radio module in
  reset and re-initialises the radio: about 0.5 s off the air.
- Use relay NO/COM contacts for everything. Add TVS/RC suppression on long input runs.

| Terminal | House board | Gate board |
|---|---|---|
| IN1 (A1) | Shelly relay output (O) — the Alarm.com switch state | CSW24UL AUX "open limit" (via opto) |
| IN2 (A2) | Shelly 12 V supply (via opto) — `ctrl_power_sense` | CSW24UL AUX "closed limit" (via opto) |
| IN3 (A3) | spare | AC power: the 24 V supply on mains (via opto) — `power_sense` |
| IN4 (A4) | spare | spare |
| K1 (D1) | Shelly SW input (energized = gate not closed) | CSW24UL OPEN + COM (pulsed only) |
| K2 (D2) | 2GIG contact sensor terminals (energized = gate closed) | CSW24UL CLOSE + COM (pulsed only) |

## Gate board

- **CSW24UL**: set AUX relay A to *open limit* and AUX relay B to *closed limit* (per the LiftMaster manual)
  and wire their contacts to GATE IN1 / IN2 through the opto board (below). The closed-limit relay (AUX 2)
  *energizes when not at the close limit*, so take IN2 from its **NC** contact; the open-limit relay from NO. K1 NO/COM → OPEN + COM,
  K2 NO/COM → CLOSE + COM. The OPEN/CLOSE inputs are shared with an AES Prime Edge cellular controller and a
  siren sensor, which is why the relays are only ever pulsed (default 500 ms), never held.
- **Opto board:** a 4-channel PNP-output opto isolator (NOYITO MT-301R4P-P), OUT1–OUT4 → IN1–IN4, output
  GND → board GND, output VCC → the board's **3.3 V only** (a PNP output passes VCC straight to the pin). On the
  24 V side, wet each AUX limit contact from the opener's 24 V accessory output (24 V → AUX C; open limit
  AUX NO → opto ch1, closed limit AUX NC → opto ch2) and put channel 3
  across the 24 V; OUT4 is spare. A lit opto reads active; power loss, a dead opto or a cut wire reads off — "not
  at a limit", never closed. The NC closed-limit contact keeps that: with the opener dead its accessory output has
  no 24 V to wet the contact, so IN2 reads off even though the relay has dropped. Wet the limits from the
  accessory output, not the AC 24 V supply, or a dead opener on AC would read closed. Leave the gate's `inN_invert` toggles off: inverting would make those faults read
  active.
- **Gate IN3 = AC power sense** (`power_sense`, default on). An opto channel across the 120 VAC → 24 V supply
  (the one feeding the AES controller) drives IN3; the CSW24UL has battery backup and keeps running without AC.
  IN3 off → OPEN/CLOSE commands are acknowledged as refused without pulsing, and STATUS reports AC lost (status
  `ac_power`). A limit that still reads is trusted; with none reading the gate reads `no_power` instead of
  `between`, since the opener's battery may be dead too (its AUX limit relays drop without power). Moves into or
  out of `no_power` have cause none. With `power_sense` on, a move into `between` is reported only after 0.5 s
  (3 s out of `no_power`): when the 24 V that wets the inputs fails, a limit's opto can drop before IN3's (and on
  its return IN3 can come back first), and that is `no_power`, not a move. A relay test while `no_power` still pulses (a wiring check) but sets no
  target, so the limit read when power returns isn't attributed to it. Turn the `power_sense` toggle off (also
  possible remotely over LoRa) if IN3 isn't wired.
- **MKR VIN is 5 V max.** Power the gate board through a 24 V→5 V buck from the opener's 24 V accessory output
  (battery-backed) or the AC 24 V supply (dies with AC; add a LiPo to ride through).

## House board

- **Shelly Wave 1**: power from a low-voltage supply per Shelly's wiring (12 V DC here) so the SW input that K1
  drives is low voltage — do not switch mains with the shield. Wire the Shelly relay output **I→3.3 V, O→HOUSE
  IN1**. Set the SW input to *toggle switch, contact closed = ON / open = OFF* (see the "SW1 switch type"
  parameter in the Shelly Wave 1 manual). Don't use detached mode. If it is left on "changes status when
  switch changes status", the Alarm.com switch flips whenever K1 moves (house log: `sync` right after
  `gate_state`, then repeated `resync`); the house ignores those flips, so no gate commands result.
- **House IN2 = Shelly power sense** (`ctrl_power_sense`, default on). When the Shelly loses power its relay drops,
  which looks exactly like the Alarm.com switch being turned off; without this a power blip would close the gate
  and reopen it when the Shelly came back. A PNP opto channel across the Shelly's 12 V supply drives IN2 (use a
  channel rated for 12 V input; output side from the board's 3.3 V, as on the gate). While IN2 is off, IN1 edges
  are logged but never sent and resync pauses. The relay drops before the opto does: on the bench the Shelly's relay
  dropped ~0.46 s into a 12 V cut and the opto only ~2.1 s in, since the rail's capacitors keep it lit, and a 300 ms
  dip rebooted the Shelly without the opto dropping at all. So the house board's own supply counts too
  (`ctrl_power_pmic`, default on): the board runs off the same 12 V through its buck, and the MKR's charger chip
  (BQ24195L, read over I2C) reports VIN power good lost ~0.2 s *before* the Shelly's relay drops, on cuts and on
  300 ms dips alike, whatever the buck's set point (bench, 2026-10-07: 196–222 ms over 30 runs at 4.81, 5.02 and
  5.21 V). The controller counts as powered only while both IN2 and the supply are good. This needs the house
  board fed from the Shelly's supply, and with its LiPo to stay up through the cut; a USB cable that carries power
  keeps the supply reading good. A power loss can only drop the relay (OFF), so an OFF edge (CLOSE) is still held
  for `ctrl_confirm_ms` (default 0.5 s, a backstop; with `ctrl_power_pmic` off, set it to about 3 s, above the IN2
  opto's ~1.65 s lag) and discarded if power drops meanwhile, while an ON edge (OPEN) is sent at once. With both
  power senses off there is no wait. After power returns (and
  after a house boot) IN1 edges count as sync for at least `ctrl_settle_ms` (default 10 s), and up to
  `sync_window_ms` longer while the Shelly doesn't match K1 yet; one still out of step then is resynced at once. A
  Shelly that reboots internally without losing its supply isn't covered. Turn `ctrl_power_sense`
  off if IN2 isn't wired.
- **2GIG contact sensor**: any 2GIG-compatible door/window sensor with external terminal input. Wire K2 NO/COM
  to its terminals; name it "Gate" in Alarm.com. (`sensor_invert` flips the sense if needed.)

## Bench UART console

Pins 13 (RX) and 14 (TX) carry an optional second console (`uart_console`, off by default) for bench power
tests with a 3.3 V USB-to-UART adapter: adapter TX → 10 kΩ → pin 13, pin 14 → adapter RX, GND to GND, adapter VCC
unconnected. See [console.md](console.md). Leave the pins unused and `uart_console` at 0 at the install.

## Spare inputs

IN4 (and IN3 on the house board) are pull-down inputs (contact to 3.3 V) reserved for future use such as a
beam-break sensor or alarm status (see [ROADMAP.md](../ROADMAP.md)). They're debounced, logged (`input` events),
shown in Status (the gate's are also sent to the house), with `inN_invert` toggles, but don't affect behaviour
yet. Leave them unwired if unused.

## Power and antenna

- Always attach the antenna before the radio transmits, and keep it away from the relay shield and field wiring
  (a U.FL→SMA pigtail lets it sit outside the enclosure).
- A full-power transmit while a relay is energized can crash a weakly powered board (seen on PC USB power, as
  watchdog resets). Use a solid 5 V supply, and consider a bulk capacitor (~470 µF) across 5 V/GND or a LiPo on
  the MKR battery connector. On the bench, on USB power, keep `tx_power` at about 5 dBm.
- The shield's relays (Omron G5V-1, 3 V coils) run from the MKR's 3.3 V rail, not the 5V pin, so they keep
  working on the LiPo alone. Bench check 2026-10-06 (0.10.0): with the 12 V rail cut, the house board on its LiPo
  switched K1 and K2 (test pulses, and following the gate open and closed; both on together for 5 s) while
  pinging at 17 dBm, each contact change seen by the GateSim on the relays' NC contacts, with no reset, no
  `radio_faults` and 82/82 pongs; with its buck cut, the gate board on its LiPo pulsed OPEN and CLOSE into the
  GateSim (507 ms each).
- If `radio_ok` shows false, update the Murata module firmware with the `MKRWANFWUpdate_standalone` example from
  the MKRWAN library and retry.
