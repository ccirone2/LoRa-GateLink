# GateLink — LoRa gate bridge for Alarm.com

Two Arduino MKR WAN 1310 boards on MKR Relay Proto Shields link an Alarm.com / 2GIG
system at the house to a LiftMaster CSW24UL swing-gate opener over point-to-point LoRa.

```
Alarm.com ── Z-Wave ── Shelly Wave 1

HOUSE board
  IN1 <── Shelly relay contact (O/I)
          switch state → OPEN/CLOSE
  K1  ──> Shelly SW input
          mirrors the real gate state
  K2  ──> 2GIG contact sensor
          closed = gate closed
   │
   │  LoRa 915 MHz, HMAC-signed
   │
GATE board
  K1  ──> CSW24UL OPEN + COM    (pulse)
  K2  ──> CSW24UL CLOSE + COM   (pulse)
  IN1 <── AUX "open limit"      (opto)
  IN2 <── AUX "closed limit"    (opto)
  IN3 <── 24 V accessory        (opto)
          = opener powered
```

The opener's OPEN/CLOSE inputs are shared with an AES Prime Edge cellular controller and a
siren sensor.

## Repository

| Path | What |
|---|---|
| `firmware/GateLink/` | One Arduino sketch for both boards; the role (house or gate) is set in config |
| `web/` | Static configuration and diagnostics page (Web Serial over USB, no server), hosted on GitHub Pages |
| `tests/e2e/` | Bench end-to-end suite (pytest; needs the hardware) |
| `tools/GateSim/` | Bench-only Uno sketch that simulates the opener |
| `tools/gatelink.py` | Command-line access to a board's USB console; config snapshot/restore around a flash; link history as CSV |
| `docs/` | Hardware, protocol, console and bench documentation |

## Documentation

- [docs/hardware.md](docs/hardware.md) — wiring and device settings for each board, power and antenna
- [docs/protocol.md](docs/protocol.md) — radio defaults, framing, sessions and replay protection
- [docs/console.md](docs/console.md) — USB JSON console: commands, status fields, events, log codes
- [docs/bench-testing.md](docs/bench-testing.md) — bench rules and the test checklist
- [tests/e2e/README.md](tests/e2e/README.md) — the end-to-end suite
- [tools/GateSim/README.md](tools/GateSim/README.md) — the opener simulator
- [docs/development.md](docs/development.md) — toolchain, workflow, versioning and releases

## Project tracking

- **[Releases](https://github.com/ccirone2/LoRa-GateLink/releases)** — the firmware changelog, one release per
  firmware version (from v0.3.5 on with a prebuilt `.bin`)
- **[TODO.md](TODO.md)** — open bugs, investigations and bench/field tasks
- **[ROADMAP.md](ROADMAP.md)** — desired features and ideas

## How it behaves

| Event | Result |
|---|---|
| Alarm.com switch turned **ON** | House sends `OPEN`; gate pulses the opener OPEN input |
| Alarm.com switch turned **OFF** | House sends `CLOSE`; gate pulses CLOSE |
| Gate moved by AES Prime Edge / siren / keypad | Gate reports it (`cause: external`, even right after one of our commands); house flips K1, the Shelly follows, **no command is sent back**. `cause: lora` means the gate is moving toward the limit our last command asked for |
| Gate travelling (`between`) | K1 (and so the Shelly) keeps showing the limit it left and flips only when the other limit is reached; if the gate stays `between` longer than `travel_timeout_s` it shows open. The contact sensor reads open as soon as the gate leaves closed |
| Command ignored by opener (e.g. siren holding gate open) | Gate reports `timeout`; house re-syncs the Shelly to the real state |
| Command already satisfied (OPEN while open) | Suppressed at the house, or acknowledged as `already` at the gate — unless the opposite command is still in flight (switch flipped and straight back), which is sent and pulsed to reverse it |
| Link lost | Contact sensor reads **open** (fail-safe, configurable); commands expire after `cmd_ttl_s` rather than firing late |
| Opener loses power (gate IN3 off) | Gate reports `no_power` instead of `between`. The gate doesn't move, but its position can't be verified (and it may be moved by hand), so the house shows not-closed: contact sensor open, K1 energized and the Shelly shows on. Commands are refused (gate log `cmd_refused`; house shows *refused: opener unpowered*). When power returns everything follows the limits again |
| Shelly loses power (house IN2 off) | Its relay drops, but that edge is never sent as a command (house log `ctrl_power 0`, then `ctrl` with b=1); an edge seen up to `ctrl_confirm_ms` before the power sense drops is discarded too. When power returns the Shelly comes back at the K1 level and that edge is logged `sync` |
| House board reboots | Never commands the gate from the Shelly's level at power-up; waits for gate status first |

Gate relays are **only ever pulsed** (default 500 ms), never held, so the other devices on the
opener's inputs keep working. Gate state always comes from the opener's limit outputs (and its power sense),
never from the last command sent.

## Build and flash

Needs `arduino-cli` with the SAMD core and five libraries (versions CI builds with are in
[docs/development.md](docs/development.md)):

```sh
arduino-cli core install arduino:samd
arduino-cli lib install "LoRa" "Crypto" "FlashStorage" "ArduinoJson" "Adafruit SleepyDog Library"
arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all firmware/GateLink
arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM5 firmware/GateLink   # your port
```

Or flash a release binary: `arduino-cli upload --fqbn arduino:samd:mkrwan1310 -p COM5 --input-file GateLink-v0.3.5.bin`.
Without any tools, use the web console's **Tools → Firmware update** (below).

Flash the same firmware to both boards. From 0.5.0 the config and key are kept in the board's SPI flash chip
and **survive firmware uploads**; settings a new firmware doesn't know are dropped and new ones take their
defaults. Upgrading from an older version erases them once (they lived in program flash), so export config from
the web page first and re-apply after, or use `python tools/gatelink.py snapshot` / `restore`. **Reset to
defaults** in the web console (`config.reset`) erases the saved config and key.

## Web console

Hosted at **https://ccirone2.github.io/LoRa-GateLink/** (deployed from `web/` by GitHub Actions on push to `main`).
To run it locally, serve `web/` over `http://localhost` (`python -m http.server 8000 -d web`). It needs desktop
Chrome or Edge (Web Serial). Click **Connect board** and pick the board's COM port.

The console uses generic names so it isn't tied to one product: the house-side device is the **controller**
(here the Shelly: *controller input* = IN1, *controller sync* = K1, `ctrl_sync`), and K2 drives the **contact sensor**.

Header: **Identify** strobes the connected board's LED for 6 s, to tell boards apart on the bench. After a
reboot from the console, or if the board drops off USB, the page reconnects to it automatically for 30 s
(no re-pairing); **Disconnect** releases the port so `arduino-cli upload` can use it. The browser tab
title shows the board's role.

**Tools → Firmware update** flashes a board from the browser: **Install** the latest release (the page
carries its `.bin`) or pick a `.bin` file. The page restarts the board into its bootloader and writes, verifies
and restarts it, then reconnects and checks it kept its role and key. The first time on a computer the browser
asks for the bootloader's port (a separate USB device) once. A board already in its bootloader (reset pressed
twice; the LED fades in and out), for example after an interrupted update, can be flashed without connecting
first. The page refuses files that aren't GateLink firmware for this board; files from before 0.5.1 (no version
marker) need a confirm.

**Tools → Link history** charts the board's hourly link record (firmware 0.4.0+, up to four days since its
boot):
- received signal and SNR margin above the spreading factor's limit, with the house board also showing what the
  gate received;
- the noise floor;
- a strip of resends, give-ups, CRC errors and link-down time.

Summary figures above the chart give link-up %, worst SNR margin, resend rate and messages lost. Hover the
chart or use the arrow keys to read an hour. There is also a table view and CSV download.

Tabs: **Status** (gate, link quality, I/O incl. opener power and spare inputs, house bridge state, board uptime,
last reset cause, radio TX faults), **Config** (all parameters, with toggle switches for on/off settings;
apply/save, export/import JSON), **Security** (generate and write the link key), **Tools** (relay tests, ping with
RSSI chart, remote gate diagnostics and settings over LoRa from the house board, replay self-test),
**Log** (live events and the board's event ring buffer), **Install** (field wiring diagram, terminal
table and notes for each board; works without a board connected).

### First-time setup

1. Flash both boards.
2. Connect board A → Config → `role = house` → Apply → Save → Reboot. Board B → `role = gate`, same.
   If the gate's IN3 power sense isn't wired yet, turn its `power_sense` toggle off too, or the gate reads
   `no_power` and refuses commands. Likewise turn the house's `ctrl_power_sense` off until IN2 is wired, or
   Alarm.com commands are ignored (Status: *Controller power: off*).
3. Security → **Generate** → write the key to board A, then write the **same** key to board B.
   The radio link stays off until a key is set, so a fresh or reset board can never be commanded.
   Store the key somewhere safe: boards can't read it back, and config export doesn't include it.
4. With both powered, Status on either board should show *Peer verified: yes* within a few seconds.
5. Tools → Ping to check RSSI/SNR. At the install site aim for ≥10 dB margin above the SF's sensitivity;
   raise `sf` (and/or `tx_power`) on **both** boards if the link is marginal.

### Status LED

| Pattern | Meaning |
|---|---|
| Solid, full brightness | No role set |
| Dim breathing (2.5 s) | Link up |
| Very dim, fast lub-dub heartbeat | No link (nothing heard for `link_timeout_s`; on the house at least 2.5 gate heartbeats) |
| Fast bright strobe (6 s) | **Identify** requested from the web console |

The breathing and heartbeat patterns never go fully dark between pulses.

## Troubleshooting

- **Relays click at random / a board keeps restarting.** Check Status → *Last reset* and the log's `boot`
  entries. `watchdog` means the firmware froze for 8 s; each restart drops and re-energizes the house relays.
  On the bench this was caused by full-power TX with a relay energized on USB power — lower `tx_power`,
  improve the supply, and check the antenna (see [docs/hardware.md](docs/hardware.md)). `brownout` points
  straight at power.
- **Radio faults** (Board card, `radio_fail` log entries): the radio stopped mid-transmit or reset and was
  re-initialised. Occasional ones are recovered automatically; frequent ones mean power or RF trouble.
- **`radio_ok` is false.** Update the Murata module firmware with the `MKRWANFWUpdate_standalone` example from
  the MKRWAN library and retry.
- **The gate never pulses on its own.** It only pulses K1/K2 for an OPEN/CLOSE command from the house or a
  relay test from the console. The house log's `cmd_sent` entries show what it sent and why.
- **The Alarm.com switch flips whenever the gate moves**, with `sync` then repeated `resync` in the house log:
  the Shelly's SW input is set to toggle on every edge. Set it to follow the switch (see
  [docs/hardware.md](docs/hardware.md)).
