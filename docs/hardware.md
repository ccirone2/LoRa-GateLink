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
| IN4 (A4) | spare | Closed sensor on the leaf (via opto) — `closed_sense`; else spare |
| K1 (D1) | Shelly SW input (energized = gate not closed) | CSW24UL OPEN + COM (pulsed only) |
| K2 (D2) | 2GIG contact sensor terminals (energized = gate closed) | CSW24UL CLOSE + COM (pulsed only) |
| FAULT (D5) | optional fault output (`fault_out`, HIGH = healthy) to a relay module for a second 2GIG sensor | optional fault output (`fault_out`), as on the house |

## Gate board

- **CSW24UL**: set AUX relay 1 to *energizes at open limit* (switches 1-2-3 OFF OFF ON) and AUX relay 2 to
  *energizes when not at close limit* (OFF ON OFF), per the opener's wiring diagram (01392401, "Auxiliary relays"),
  and wire their contacts to GATE IN1 / IN2 through the opto board (below). The opener has no setting that energizes
  a relay *at* the close limit: that is its only close-limit mode, so take IN2 from AUX 2's **NC** contact; the
  open-limit relay from NO. The opener can say "closed" only by releasing that relay, and no wiring of it changes
  that: its NO contact would make a cut wire or a dead opto read closed, and a relay of our own on its NC contact
  stays on when AUX 2 never energizes. Only a sensor of its own on the leaf is independent of it: gate IN4 with
  `closed_sense` (below). The
  AUX limit relays switch only once the opener's travel limits are set, so a limit that never reads may be unset
  limits rather than a wiring fault. K1 NO/COM → OPEN + COM,
  K2 NO/COM → CLOSE + COM. The OPEN/CLOSE inputs are shared with an AES Prime Edge cellular controller and a
  siren sensor, which is why the relays are only ever pulsed (default 500 ms), never held.
- **Opto board:** a 4-channel PNP-output opto isolator (NOYITO MT-301R4P-P), OUT1–OUT4 → IN1–IN4, output
  GND → board GND, output VCC → the board's **3.3 V only** (a PNP output passes VCC straight to the pin). On the
  24 V side, wet each AUX limit contact from the opener's 24 V accessory output (24 V → AUX C; open limit
  AUX NO → opto ch1, closed limit AUX NC → opto ch2) and put channel 3
  across the 24 V; channel 4 takes the closed sensor (below), or is spare. A lit opto reads active; power loss, a dead opto or a cut wire reads off — "not
  at a limit", never closed. The NC closed-limit contact keeps that: with the opener dead its accessory output has
  no 24 V to wet the contact, so IN2 reads off even though the relay has dropped. Wet the limits from the
  accessory output, not the AC 24 V supply, or a dead opener on AC would read closed. Leave the gate's `inN_invert` toggles off: inverting would make those faults read
  active. What the NC contact can't show is its own relay failing with the 24 V present (or AUX 2 left at "always
  off"): IN2 then reads closed wherever the gate is. Since 0.17.1 the gate reads `fault` from the first time both
  limits read together for half a second (at the open limit, with such a fault) until IN1 reads with IN2 off, so after one full opening
  the alarm sees not closed; before it, nothing can tell. Check it at the install (threat model,
  `closed-limit-nc-fail-unsafe`): relay 2's switches read OFF ON OFF (OFF OFF OFF, "relay always off", gives exactly
  this failure), and with the gate part-open IN2 reads off.
- **Gate IN4 = closed sensor** (`closed_sense`, since 0.20.0; off by default). A magnetic contact (reed and magnet)
  on the leaf, its contact closed with the gate closed: 24 V from the opener's accessory output through the contact
  into opto channel 4, as the limits are wetted, so a dead opener reads it off too. Use an outdoor-rated contact with
  a gap that suits the leaf, and mount it to close only in the last inches of travel: the gate reads `fault` once the
  sensor and the closed limit have disagreed for 5 s, and in normal travel they part only between their switching
  points. With `closed_sense` on, the gate reads closed only while both read, so a failed AUX2 relay
  (`closed-limit-nc-fail-unsafe`) can no longer make an open gate read closed; a cut wire, a dead channel or a
  knocked-off magnet reads `fault` with the gate closed (not closed at the house: fail safe). Turn it on only once the
  sensor is fitted: without one the gate never reads closed. A closed limit the sensor doesn't confirm counts as no
  limit, so without AC (on the opener's battery) commands are then refused, as in `no_power` or `fault`; and a gate
  stopped in the sensor's last inches reads `fault` after 5 s, as it would read `stuck` once its travel timeout passed. Check it at the install: with the gate closed the I/O
  card shows IN2 and IN4 lit; part-open by hand, IN4 goes off; unplug the sensor with the gate closed and the gate
  reads `fault` 5 s later.
- **Gate IN3 = AC power sense** (`power_sense`, default on). An opto channel across the 120 VAC → 24 V supply
  (the one feeding the AES controller) drives IN3; the CSW24UL has battery backup and keeps running without AC.
  IN3 off → a command still runs while one limit reads (not `fault`) or reverses ours mid-travel (CLOSE from 0.15.1, OPEN from 0.15.2: cutting the AC mustn't
  keep the gate open; with no limit reading it's acknowledged as refused without pulsing), and STATUS reports AC lost (status
  `ac_power`). A limit that still reads is trusted; with none reading the gate reads `no_power` instead of
  `between`, since the opener's battery may be dead too (its AUX limit relays drop without power). Moves into or
  out of `no_power` have cause none. With `power_sense` on, a move into `between` is reported only after 0.5 s
  (3 s out of `no_power`): when the 24 V that wets the inputs fails, a limit's opto can drop before IN3's (and on
  its return IN3 can come back first), and that is `no_power`, not a move. From 0.17.2 a move into `closed` out of
  `no_power` waits 3 s from when the limit reads: the restarting opener's 24 V wets the closed limit's NC contact
  before it energizes AUX2, so an open gate can read closed for a moment. A relay test while `no_power` still pulses (a wiring check) but sets no
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
  off if IN2 isn't wired. A dead IN2 opto or a cut wire reads as a controller without power, its switch ignored; from
  0.17.3 IN2 off for 30 s while the board's supply reads good is reported (health `ctrl_sense`, D5), since a real
  12 V cut takes the supply down first. With a USB cable that carries power plugged in, the supply always reads good,
  so a controller power cut over 30 s then shows `ctrl_sense` too.
- **2GIG contact sensor**: any 2GIG-compatible door/window sensor with external terminal input. Wire K2 NO/COM
  to its terminals; name it "Gate" in Alarm.com. (`sensor_invert` flips the sense if needed.)

## Fault output (D5)

The house shows `no_power`, `fault` and a lost link the way it shows an open gate: the 2GIG contact sensor opens and
the Alarm.com switch turns on. To have the alarm system report "gate needs attention" separately, either board can
drive D5 (`fault_out`, off by default): HIGH while the board is healthy, LOW once a problem has lasted `fault_hold_s`
(default 10 s), and LOW after a reset until the board has started up. The problems each board counts (its radio, the
link, AC power at the gate, `no_power`, `fault`, a reboot loop, a dead IN2 opto, a gate stuck between its limits) are listed in [console.md](console.md#fault-output); status `health`
shows them, and log `health` each change of D5.

- **D5 is a 3.3 V logic output good for a few mA.** Drive an opto or a relay module's input with it, never a relay
  coil: a relay module with an active-high input that switches at 3.3 V (energized while D5 is HIGH), powered from the
  board's 5 V supply (VIN) and GND.
- **Alarm side:** wire the relay module's NO and COM contacts to the terminals of a second 2GIG wireless contact
  sensor (or a hardwired zone of the 2GIG panel) as a normally-closed loop: closed while healthy, open on a problem.
  Since D5 has to be driven HIGH to keep the loop closed, a dead board, a reset, a cut wire or a dead relay module
  opens it too. Name it e.g. "Gate link trouble" in Alarm.com, set to report a trouble or notification rather than an
  intrusion.
- **Which board:** the house's D5 covers what matters to the alarm (the link, and the gate's AC, `no_power`, `fault`,
  reboot loop and a gate stuck between its limits, as its STATUS reports them), so most installs wire only the house. At the gate the same output can feed a
  sensor near the opener, which also covers the gate board itself going dead.
- Set `fault_out` on the board whose D5 is wired; it applies at once. It isn't remote-writable (it's a wiring
  decision); `fault_hold_s` can also be set on the gate from the house. With `fault_out` off, D5 stays an input with
  its pull-down: leave it unwired.

## Bench UART console

Pins 13 (RX) and 14 (TX) carry an optional second console (`uart_console`, off by default) for bench power
tests with a 3.3 V USB-to-UART adapter: adapter TX → 10 kΩ → pin 13, pin 14 → adapter RX, GND to GND, adapter VCC
unconnected. See [console.md](console.md). Leave the pins unused and `uart_console` at 0 at the install.

## Spare inputs

House IN3/IN4, and gate IN4 with `closed_sense` off, are pull-down inputs (contact to 3.3 V) reserved for future use
such as a beam-break sensor or alarm status (see [ROADMAP.md](../ROADMAP.md), F15). They're debounced, logged (`input` events),
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
