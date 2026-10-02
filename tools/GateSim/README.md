# GateSim — bench opener simulator

Bench only, not GateLink firmware: an Arduino Uno with a relay module stands in for the CSW24UL so the gate
board sees real limit and power signals through its opto board, and the whole loop (controller → house → LoRa →
gate → "opener" → limits → house) runs without the opener. Relay COMs are wetted with 24 V and the NO contacts
feed the opto channels, as the opener's AUX relays would.

| Uno | Connects to | Simulates |
|---|---|---|
| D2 relay | opto ch1 → gate IN1 | AUX "open limit" (on only when powered and fully open) |
| D3 relay | opto ch2 → gate IN2 | AUX "closed limit" |
| D4 relay | opto ch3 → gate IN3 | opener 24 V accessory power |
| D5 | gate K1 NO (K1 COM → Uno GND) | OPEN input (dry contact, pulled up) |
| D6 | gate K2 NO (K2 COM → Uno GND) | CLOSE input |

```sh
arduino-cli compile --fqbn arduino:avr:uno --warnings all tools/GateSim
arduino-cli upload  --fqbn arduino:avr:uno -p COMx tools/GateSim
```

## Commands

Serial 115200, one command per line: `status`, `open`, `close` (a local button, reported as an external
move), `stop` (strand it between), `power on|off`, `travel <s>` (default 15), `fault none|stuck|both|flicker|deaf`,
`polarity low|high`, `relay <1-3> on|off|auto` (force D2/D3/D4 for wiring checks, not saved), `help`.

It prints `evt ...` lines: `evt pulse open|close` on each debounced press of an input (`evt pulse both` whenever
one input closes while the other is still held: the K1/K2 interlock failed), `evt release open|close <ms>` when it
opens again, with how long it was held (edge to edge, so the 20 ms debounce cancels out), `evt cmd ...` and
`evt state <state>`. The end-to-end suite relies on these lines; change `tests/e2e/gatelink/gatesim.py` with them.

## Model

OPEN heads for open and CLOSE for closed, reversing mid-travel. Without power the limits drop, motion freezes
and pulses are ignored.

Faults: `stuck` leaves the limit and jams (exercises `travel_timeout_s`), `both` asserts both limits (gate
`fault`), `flicker` chatters a limit on arrival (debounce), `deaf` ignores the gate's pulses.

Opening the port resets the Uno, which restarts it closed and powered (the gate briefly sees `no_power`); scripts
can avoid that by opening it with DTR off (pyserial: set `dtr = False` before `open()`).

## Relay polarity

The bench relay module switches on when the pin is HIGH (`polarity high`, the default); many modules are the
other way round. After boot, the closed-limit (D3) and power (D4) relays should be on and D2 off. If it's the
other way round, send `polarity low` (saved in EEPROM). Travel time is saved too.
