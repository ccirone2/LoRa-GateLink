# GateLink — LoRa gate bridge for Alarm.com

Two Arduino MKR WAN 1310 boards on MKR Relay Proto Shields link an Alarm.com / 2GIG
system at the house to a LiftMaster CSW24UL swing-gate opener over point-to-point LoRa.

```
 Alarm.com ─Z-Wave─ Shelly Wave 1 ──relay contact (O/I)──▶ HOUSE IN1   (switch state → OPEN/CLOSE)
                    Shelly SW input ◀──────────────────── HOUSE K1    (mirror real gate state)
                    2GIG contact sensor ◀──────────────── HOUSE K2    (closed = gate closed)
                                  ~~~~ LoRa 915 MHz, HMAC-signed ~~~~
 CSW24UL OPEN  + COM ◀── GATE K1 (pulse)      CSW24UL AUX "open limit"  ──▶ GATE IN1
 CSW24UL CLOSE + COM ◀── GATE K2 (pulse)      CSW24UL AUX "close limit" ──▶ GATE IN2
        (shared with AES Prime Edge + siren sensor)
```

- `firmware/GateLink/` — one Arduino sketch for both boards; the role is set in config.
- `web/` — static configuration and diagnostics page (Web Serial over USB, no server).

## How it behaves

| Event | Result |
|---|---|
| Alarm.com switch turned **ON** | House sends `OPEN`; gate pulses the opener OPEN input |
| Alarm.com switch turned **OFF** | House sends `CLOSE`; gate pulses CLOSE |
| Gate moved by AES Prime Edge / siren / keypad | Gate reports it (`cause: external`); house flips K1, the Shelly follows, **no command is sent back** |
| Command ignored by opener (e.g. siren holding gate open) | Gate reports `timeout`; house re-syncs the Shelly to the real state |
| Command already satisfied (OPEN while open) | Suppressed at the house, or acknowledged as `already` at the gate |
| Link lost | Contact sensor reads **open** (fail-safe, configurable); commands expire after `cmd_ttl_s` rather than firing late |
| House board reboots | Never commands the gate from the Shelly's level at power-up; waits for gate status first |

Gate relays are **only ever pulsed** (default 500 ms), never held, so the other devices on the
opener's inputs keep working. Gate state always comes from the opener's limit outputs.

## Hardware notes

- Relays: K1 = D1, K2 = D2 on the Relay Proto Shield. Inputs: IN1 = A1, IN2 = A2 (contact to GND,
  internal pull-up). Change in `firmware/GateLink/pins.h` if your wiring differs; check the shield silkscreen.
- **Spare inputs** IN3 = A3, IN4 = A4: dry-contact inputs (pull-up, contact to GND) reserved for future use such
  as a beam-break sensor or alarm status. They're debounced, logged (`input` events), shown in Status and sent to
  the house, with `in3_invert` / `in4_invert`, but don't affect behaviour yet. Leave unwired if unused.
- **MKR VIN is 5 V max.** Power the gate board from the opener's 24 V accessory supply through a 24 V→5 V buck.
- **Power and antenna:** always attach the antenna before the radio transmits, and keep it away from the relay
  shield and field wiring (a U.FL→SMA pigtail lets it sit outside the enclosure). A full-power transmit while a
  relay is energized can crash a weakly powered board (seen on PC USB power); use a solid 5 V supply, and consider
  a bulk capacitor (~470 µF) across 5 V/GND or a LiPo on the MKR battery connector.
- Use relay NO/COM contacts for everything. Add TVS/RC suppression on long input runs.
- **Shelly Wave 1**: power from 24 V DC/AC per Shelly's low-voltage wiring so the SW input that K1 drives
  is low voltage — do not switch mains with the shield. Wire the Shelly relay output **I→GND, O→HOUSE IN1**.
  Set the SW input to *toggle switch, contact closed = ON / open = OFF* (see the "SW1 switch type"
  parameter in the Shelly Wave 1 manual). Don't use detached mode.
- **2GIG contact sensor**: any 2GIG-compatible door/window sensor with external terminal input. Wire K2 NO/COM
  to its terminals; name it "Gate" in Alarm.com. (`sensor_invert` flips the sense if needed.)
- **CSW24UL**: set AUX relay A to *open limit* and AUX relay B to *close limit* (per the LiftMaster manual)
  and wire their dry contacts to GATE IN1 / IN2. K1 NO/COM → OPEN + COM, K2 NO/COM → CLOSE + COM.

## Build and flash

Needs `arduino-cli` with the SAMD core and five libraries:

```sh
arduino-cli core install arduino:samd
arduino-cli lib install "LoRa" "Crypto" "FlashStorage" "ArduinoJson" "Adafruit SleepyDog Library"
arduino-cli compile --fqbn arduino:samd:mkrwan1310 firmware/GateLink
arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM5 firmware/GateLink   # your port
```

Flash the same firmware to both boards. **Uploading erases the saved config and key** (they live in
program flash) — export config from the web page first and re-apply after.

LoRa runs point-to-point with the `LoRa` library, not LoRaWAN. If `radio_ok` shows false, update the
Murata module firmware with the `MKRWANFWUpdate_standalone` example from the MKRWAN library and retry.

## Web console

Hosted at **https://ccirone2.github.io/LoRa-GateLink/** (deployed from `web/` by GitHub Actions on push to `main`).
Or open `web/index.html` via `http://localhost` (e.g. `python -m http.server -d web 8000`) or host the
`web/` folder on GitHub Pages, in desktop Chrome or Edge. Click **Connect board** and pick the board's COM port.

The console uses generic names so it isn't tied to one product: the house-side device is the **controller**
(here the Shelly: *controller input* = IN1, *controller sync* = K1, `ctrl_sync`), and K2 drives the **contact sensor**.

Header: **Identify** strobes the connected board's LED for 6 s, to tell boards apart on the bench. After a
reboot from the console, or if the board drops off USB, the page reconnects to it automatically for 30 s
(no re-pairing); **Disconnect** releases the port so `arduino-cli upload` can use it. The browser tab
title shows the board's role.

Tabs: **Status** (gate, link quality, I/O incl. spare inputs, house bridge state, board uptime, last reset cause,
radio TX faults), **Config** (all parameters, apply/save,
export/import JSON), **Security** (generate and write the link key), **Tools** (relay tests, ping with
RSSI chart, remote gate diagnostics and settings over LoRa from the house board, replay self-test),
**Log** (live events and the board's event ring buffer), **Install** (field wiring diagram, terminal
table and notes for each board; works without a board connected).

### First-time setup

1. Flash both boards.
2. Connect board A → Config → `role = house` → Apply → Save → Reboot. Board B → `role = gate`, same.
3. Security → **Generate** → write the key to board A, then write the **same** key to board B.
   The radio link stays off until a key is set, so a fresh or reset board can never be commanded.
4. With both powered, Status on either board should show *Peer verified: yes* within a few seconds.
5. Tools → Ping to check RSSI/SNR. At the install site aim for ≥10 dB margin above the SF's sensitivity;
   raise `sf` (and/or `tx_power`) on **both** boards if the link is marginal.

### Status LED

| Pattern | Meaning |
|---|---|
| Solid, full brightness | No role set |
| Dim breathing (2.5 s) | Link up |
| Very dim, fast lub-dub heartbeat | No link (nothing heard for `link_timeout_s`) |
| Fast bright strobe (6 s) | **Identify** requested from the web console |

The breathing and heartbeat patterns never go fully dark between pulses.

## Radio defaults

915.0 MHz, 500 kHz bandwidth, SF9, CR 4/5, 17 dBm. 500 kHz keeps a fixed-channel LoRa link in the
FCC 15.247 digital-modulation category in the US. For EU use 868.x MHz and stay within the band's duty-cycle limits.

## Protocol and security

Frame: `ver | type | net_id | src | dst | session | seq | payload | tag`, tag = HMAC-SHA256 (shared
128-bit key) truncated to 8 bytes. Each board picks a random session id at boot; a peer's session is
accepted only after it echoes a fresh challenge (HELLO / HELLO_ACK), and seq numbers must increase
within a session, so recorded frames can't be replayed — even across reboots, with no counters in flash.
A 32-frame sliding window tolerates reordering between retried messages. Commands and status are
acknowledged and retried; duplicate commands are detected and not re-pulsed. Role changes take
effect after a reboot.

## Bench test checklist

Use LEDs or a meter on the relay outputs and jumper wires on the inputs. On USB power, set `tx_power`
to ~5 dBm on both boards: a full-power transmit while a relay is energized can crash the board (watchdog
reset, shown as `reset_cause` in Status). Use **Identify** in the web console to strobe a board's LED
and tell the two apart.

- House IN1 to GND (Shelly ON) → gate K1 pulses once; release → gate K2 pulses once.
- Gate IN1 to GND (open limit) → house K1 energizes, K2 releases; gate IN2 to GND → K1 releases, K2 energizes.
- External move: with no command sent, ground gate IN1 → house status shows `cause external`, **no command sent**
  (house log shows `sync`, not `cmd_sent`, if the Shelly or a jumper follows K1).
- Override: ground gate IN1 (open), turn house IN1 off (CLOSE) → gate reports `timeout` after `travel_timeout_s`,
  house log shows `resync`.
- Unpower the gate board → after `link_timeout_s` house K2 releases (sensor open), log `link_down`.
- Tools → Send replay on one board → the other board's replay counter increases (or it re-ACKs).
- Different key on one board → *Peer verified* stays no and `mac_fail` climbs.
- Reboot the house board with IN1 grounded → the gate does not move.

## Troubleshooting

- **Relays click at random / a board keeps restarting.** Check Status → *Last reset* and the log's `boot`
  entries. `watchdog` means the firmware froze for 8 s; each restart drops and re-energizes the house relays.
  On the bench this was caused by full-power TX with a relay energized on USB power — lower `tx_power`,
  improve the supply, and check the antenna (see Hardware notes). `brownout` points straight at power.
- **Radio TX faults** (Board card, `radio_fail` log entries): the radio stopped mid-transmit and was
  re-initialised. Occasional ones are recovered automatically; frequent ones mean power or RF trouble.
- **The gate never pulses on its own.** It only pulses K1/K2 for an OPEN/CLOSE command from the house or a
  relay test from the console. The house log's `cmd_sent` entries show what it sent and why.
