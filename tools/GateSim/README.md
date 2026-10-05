# GateSim — bench opener simulator

Bench only, not GateLink firmware: an Arduino Uno with a relay module stands in for the CSW24UL so the gate
board sees real limit and power signals through its opto board, and the whole loop (controller → house → LoRa →
gate → "opener" → limits → house) runs without the opener. Relay COMs are wetted with 24 V and feed the opto
channels, as the opener's AUX relays would: D2 and D4 through NO, D3 through **NC**, because the CSW24UL's
closed-limit AUX relay energizes when *not* at the close limit. D3's coil is driven inverted to match.

| Uno | Connects to | Simulates |
|---|---|---|
| D2 relay NO | opto ch1 → gate IN1 | AUX "open limit" (on only when powered and fully open) |
| D3 relay **NC** | opto ch2 → gate IN2 | AUX "closed limit" (coil energized when not closed; signal on only when powered and fully closed) |
| D4 relay NO | opto ch3 → gate IN3 | AC power (the 24 V supply on mains) |
| D5 | gate K1 NO (K1 COM → Uno GND) | OPEN input (dry contact, pulled up) |
| D6 | gate K2 NO (K2 COM → Uno GND) | CLOSE input |
| D7 relay **NC** | 24 V into the gate board's buck | gate board supply (power rig) |
| D8 relay **NC** | gate board LiPo + lead | gate battery connected (power rig) |
| D9 relay **NC** | house 12 V rail (controller, IN2 opto, house buck) | house supply (power rig) |
| D10 relay **NC** | house board LiPo + lead | house battery connected (power rig) |

The power rig channels (D7–D10) are optional and wired through NC, so a released coil means powered/connected:
a Uno reset (opening its port) never cuts a board. Power their relay module's coils from a separate 5 V (JD-VCC),
not the Uno.

```sh
arduino-cli compile --fqbn arduino:avr:uno --warnings all tools/GateSim
arduino-cli upload  --fqbn arduino:avr:uno -p COMx tools/GateSim
```

## Commands

Serial 115200, one command per line: `status`, `open`, `close` (a local button, reported as an external
move), `stop` (strand it between), `power on|off` (AC and the opener's battery together: on, or a dead opener),
`ac on|off` (AC only: IN3; the opener carries on on its battery), `battery on|off` (the opener's battery),
`travel <s>` (default 15), `fault none|stuck|both|flicker|deaf`,
`polarity low|high`, `relay <1-3> on|off|auto` (force the signal to gate IN1/IN2/IN3 for wiring checks, not
saved; for relay 2, `on` releases D3's coil so its NC contact closes), `help`. `status` reports the signals
(`relays open= closed= power=`), not the coils.

Power rig (not saved; reset by the suite's baseline): `supply none|acc|psu` (what feeds the gate board: `none`,
the default, only explicit cuts; `acc` the opener's accessory output, so the board loses power only when the
opener is dead; `psu` the AC supply, so it loses power with AC), `rail gate|house on|off|auto` (force a supply
rail, or follow the model), `rail gate|house cut <ms>` (cut it for that long, timed on the Uno, ~10 ms
resolution with a mechanical relay), `lipo gate|house on|off` (connect or disconnect a board's LiPo). `status`
adds `ac= battery= supply= rail_gate= rail_house= lipo_gate= lipo_house=`.

It prints `evt ...` lines: `evt pulse open|close` on each debounced press of an input (`evt pulse both` whenever
one input closes while the other is still held: the K1/K2 interlock failed), `evt release open|close <ms>` when it
opens again, with how long it was held (edge to edge, so the 20 ms debounce cancels out), `evt cmd ...`,
`evt state <state>`, and on every change `evt ac on|off`, `evt rail gate|house on|off` and
`evt lipo gate|house on|off`. The end-to-end suite relies on these lines; change `tests/e2e/gatelink/gatesim.py` with them.

## Model

OPEN heads for open and CLOSE for closed, reversing mid-travel. The opener runs on AC or its battery: without
AC only IN3 drops; with neither it's dead (`no_power`): the limits drop, motion freezes and pulses are ignored.

Faults: `stuck` leaves the limit and jams (exercises `travel_timeout_s`), `both` asserts both limits (gate
`fault`), `flicker` chatters a limit on arrival (debounce), `deaf` ignores the gate's pulses.

Opening the port resets the Uno, which restarts it closed and powered (meanwhile the relays release: the gate
briefly sees AC lost, with D3's NC contact keeping the closed limit on); scripts
can avoid that by opening it with DTR off (pyserial: set `dtr = False` before `open()`).

## Relay polarity

The bench relay module switches on when the pin is HIGH (`polarity high`, the default); many modules are the
other way round. After boot (closed and powered), only the power relay (D4) should be energized: D2 off, and
D3 off as well (its NC contact gives the closed limit). If D2 and D3 are energized and D4 isn't, send
`polarity low` (saved in EEPROM). Travel time is saved too.
