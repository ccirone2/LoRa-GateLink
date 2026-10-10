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
  IN2 <── AUX "closed limit" NC (opto)
  IN3 <── 24 V supply on AC     (opto)
          = AC power present
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
| `tools/bench-wiring/` | Bench wiring diagram (`wiring.json`, edited in the page served by `serve.py`) |
| `tools/gatelink.py` | Command-line access to a board's USB console; config snapshot/restore around a flash; link history as CSV; radio preflight for a new board (`rftest`) |
| `tools/gatelink_client/` | Python client for the boards' USB console, shared by `tools/gatelink.py` and the e2e suite |
| `docs/` | Hardware, protocol, console and bench documentation |

## Documentation

- [docs/hardware.md](docs/hardware.md) — wiring and device settings for each board, power and antenna
- [docs/protocol.md](docs/protocol.md) — radio defaults, framing, sessions and replay protection, wire formats
- [docs/console.md](docs/console.md) — USB JSON console: commands, status fields, events, log codes
- [docs/config.md](docs/config.md) — every setting: range, default, when a change applies, what it does
- [docs/bench-testing.md](docs/bench-testing.md) — bench rules and the test checklist
- [tests/e2e/README.md](tests/e2e/README.md) — the end-to-end suite
- [tools/GateSim/README.md](tools/GateSim/README.md) — the opener simulator
- [tools/bench-wiring/README.md](tools/bench-wiring/README.md) — the bench wiring diagram
- [docs/development.md](docs/development.md) — toolchain, workflow, versioning and releases
- [docs/release-criteria.md](docs/release-criteria.md) — what a release must pass on the bench, and its evidence
- [docs/agent-tooling.md](docs/agent-tooling.md) — Claude Code hooks, skills and nightly routines

## Project tracking

- **[Releases](https://github.com/ccirone2/LoRa-GateLink/releases)** — the firmware changelog, one release per
  firmware version (from v0.3.5 on with a prebuilt `.bin`; from the release criteria on with its bench evidence,
  [docs/releases](docs/releases))
- **[TODO.md](TODO.md)** — open bugs, investigations and bench/field tasks
- **[ROADMAP.md](ROADMAP.md)** — desired features and ideas

## How it behaves

| Event | Result |
|---|---|
| Alarm.com switch turned **ON** | House sends `OPEN`; gate pulses the opener OPEN input |
| Alarm.com switch turned **OFF** | House sends `CLOSE`; gate pulses CLOSE |
| Gate moved by AES Prime Edge / siren / keypad | Gate reports it (`cause: external`, even right after one of our commands); house flips K1, the Shelly follows, **no command is sent back**. `cause: lora` means the gate is moving toward the limit our last command asked for |
| Gate travelling (`between`) | K1 (and so the Shelly) keeps showing the limit it left and flips only when the other limit is reached; if the gate stays `between` longer than the gate's `travel_timeout_s` it shows open. The contact sensor reads open as soon as the gate leaves closed |
| Command ignored by opener (e.g. siren holding gate open) | Gate reports `timeout`; house re-syncs the Shelly to the real state |
| Command already satisfied (OPEN while open) | Suppressed at the house, or acknowledged as `already` at the gate — unless the opposite command is still in flight (switch flipped and straight back), which is sent and pulsed to reverse it |
| Link lost | Contact sensor reads **open** (fail-safe, configurable); commands expire after `cmd_ttl_s` rather than firing late |
| AC power lost (gate IN3 off) | The opener runs on its battery, so a limit that still reads is trusted and the house shows the real state. Commands are refused (gate log `cmd_refused`; house shows *refused: no AC power*) and the Shelly is resynced to the gate at once. With no limit reading (moving, or the opener's battery dead too) the gate reports `no_power` instead of `between`: its position can't be verified, so the house shows not-closed (contact sensor open, K1 energized, the Shelly on). When AC returns everything follows the limits again |
| Shelly loses power (house IN2 off) | Its relay drops, but that edge is never sent as a command (house log `ctrl_power 0`, then `ctrl` with b=1); the house board's own supply (`ctrl_power_pmic`, the same 12 V through its buck) drops ~0.2 s before the relay, where the IN2 opto lags it by ~1.7 s and misses short dips; a switch-OFF still waits `ctrl_confirm_ms` (0.5 s) before it becomes a CLOSE, and is discarded if power drops meanwhile; a switch-ON (OPEN) is sent at once, since a power loss can't cause it. When power returns the Shelly comes back at the K1 level and that edge is logged `sync` |
| House board reboots | Never commands the gate from the Shelly's level at power-up; waits for gate status first |

Gate relays are **only ever pulsed** (default 500 ms), never held, so the other devices on the
opener's inputs keep working. Gate state always comes from the opener's limit outputs (and its power sense),
never from the last command sent.

## What you need

Per install (one house end, one gate end); wiring and settings for each part are in
[docs/hardware.md](docs/hardware.md).

| Part | Qty | Where | Notes |
|---|---|---|---|
| Arduino MKR WAN 1310 | 2 | both | One firmware for both; the role is set in config |
| Arduino MKR Relay Proto Shield | 2 | both | K1/K2 relays (3 V coils on the 3.3 V rail) |
| 868/915 MHz antenna (U.FL → SMA pigtail) | 2 | both | Mount it outside the enclosure, away from the relays |
| 4-channel PNP-output opto isolator (NOYITO MT-301R4P-P or similar) | 1–2 | gate (IN1–IN3), house (IN2) | Outputs from the board's 3.3 V only; a 12 V-rated channel for the house |
| 24 V → 5 V buck converter | 1 | gate | From the opener's 24 V accessory output (MKR VIN is 5 V max) |
| 12 V → 5 V buck converter | 1 | house | From the Shelly's 12 V supply |
| 3.7 V LiPo (JST-PH) | 0–2 | optional | Rides through supply cuts; the house needs one for `ctrl_power_pmic` to see its supply drop |
| Shelly Wave 1 on a 12 V DC supply | 1 | house | The Z-Wave "controller" Alarm.com switches |
| 2GIG-compatible contact sensor with terminal input | 1 | house | Driven by K2: closed = gate closed |
| LiftMaster CSW24UL (AUX relays set to open / closed limit) | 1 | gate | The opener; its OPEN/CLOSE inputs may be shared with other controllers |

## Build and flash

Needs `arduino-cli` with the SAMD core and five libraries, at the versions CI builds with (also in
[docs/development.md](docs/development.md)):

```sh
arduino-cli core install arduino:samd@1.8.14
arduino-cli lib install "LoRa@0.8.0" "Crypto@0.4.0" "FlashStorage@1.0.0" "ArduinoJson@7.4.3"   "Adafruit SleepyDog Library@1.8.4"
arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all firmware/GateLink
arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM5 firmware/GateLink   # your port
```

Or flash a release binary (check it first: `sha256sum -c GateLink-vX.Y.Z.bin.sha256`): `arduino-cli upload --fqbn arduino:samd:mkrwan1310 -p COM5 --input-file GateLink-vX.Y.Z.bin`
(from the [latest release](https://github.com/ccirone2/LoRa-GateLink/releases/latest)).
`python tools/flash.py GateLink-vX.Y.Z.bin COM5` (needs pyserial) does the same with every wait bounded, and checks the board
comes back reporting that version.
Without any tools, use the web console's **Tools → Firmware update** (below).

Flash the same firmware to both boards. From 0.5.0 the config and key are kept in the board's SPI flash chip
and **survive firmware uploads**; settings a new firmware doesn't know are dropped and new ones take their
defaults. Upgrading from an older version erases them once (they lived in program flash), so export config from
the web page first and re-apply after, or use `python tools/gatelink.py snapshot` / `restore`. **Factory
reset** in the web console (`config.reset`) erases the saved config and key.

## Web console

Hosted at **https://ccirone2.github.io/LoRa-GateLink/** (deployed from `web/` by GitHub Actions on push to `main`).
To run it locally, serve `web/` over `http://localhost` (`python -m http.server 8000 -d web`); opened as a file it
won't run (it is ES modules), and says so. It needs desktop
Chrome or Edge (Web Serial). Click **Connect board** and pick the board's COM port. Once a
board has been granted, **Connect board** lists the granted boards by role ("LoRa GateLink – House"), with an
**Identify** button to strobe a board's LED; **Add board…** opens Chrome's port chooser for a new one.

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

Tabs: **Status** (gate, link quality, I/O incl. AC power and spare inputs, house bridge state, board uptime,
last reset cause, radio TX faults), **Config** (all parameters, with toggle switches for on/off settings;
apply/save, export/import JSON), **Security** (generate and write the link key), **Tools** (relay tests, ping with
RSSI chart, remote gate diagnostics and settings over LoRa from the house board, replay self-test),
**Log** (live events and the board's event ring buffer), **Install** (field wiring diagram, terminal
table and notes for each board; works without a board connected).

### First-time setup

1. Flash both boards.
2. Connect board A → Config → `role = house` → Apply → Save → Reboot. Board B → `role = gate`, same.
3. Power senses: on the gate, turn `power_sense` off if IN3 (AC power) isn't wired yet, or the gate reads
   `no_power` and refuses commands. On the house, turn `ctrl_power_sense` off until IN2 is wired, or Alarm.com
   commands are ignored (Status: *Controller power: off*). Apply and Save.
4. Security → **Generate** → write the key to board A, then write the **same** key to board B.
   The radio link stays off until a key is set, so a fresh or reset board can never be commanded.
   Store the key somewhere safe: boards can't read it back, and config export doesn't include it.
5. With both powered, Status on either board should show *Peer verified: yes* within a few seconds.
6. Tools → Ping to check RSSI/SNR. At the install site aim for ≥10 dB margin above the SF's sensitivity;
   raise `sf` (and/or `tx_power`) on **both** boards if the link is marginal.
7. Check end to end, with someone watching the gate: turn the Alarm.com switch on, and the gate should open, the
   house Status should follow (*between*, then *open*) and the contact sensor should report open; turn it off and
   it should close again, with the sensor closed. Then move the gate another way (keypad or remote): the switch
   should follow without the gate being commanded back. Export each board's config (Config → Export) as a record.

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

## License

[MIT](LICENSE). Third-party libraries keep their own licenses: the Arduino SAMD core and FlashStorage are LGPL-2.1,
so the release binaries include LGPL code; its source is available from those libraries, and ours is here.
