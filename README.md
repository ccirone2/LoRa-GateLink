# GateLink — LoRa gate bridge for Alarm.com

Two Arduino MKR WAN 1310 boards on MKR Relay Proto Shields link an Alarm.com / 2GIG
system at the house to a LiftMaster CSW24UL swing-gate opener over point-to-point LoRa.

```
 Alarm.com ─Z-Wave─ Shelly Wave 1 ──relay contact (O/I)──▶ HOUSE IN1   (switch state → OPEN/CLOSE)
                    Shelly SW input ◀──────────────────── HOUSE K1    (mirror real gate state)
                    2GIG contact sensor ◀──────────────── HOUSE K2    (closed = gate closed)
                                  ~~~~ LoRa 915 MHz, HMAC-signed ~~~~
 CSW24UL OPEN  + COM ◀── GATE K1 (pulse)      CSW24UL AUX "open limit"   ─opto─▶ GATE IN1
 CSW24UL CLOSE + COM ◀── GATE K2 (pulse)      CSW24UL AUX "closed limit" ─opto─▶ GATE IN2
  (OPEN/CLOSE shared with AES Prime Edge      CSW24UL 24 V accessory     ─opto─▶ GATE IN3 (opener powered)
   + siren sensor)
```

- `firmware/GateLink/` — one Arduino sketch for both boards; the role is set in config.
- `web/` — static configuration and diagnostics page (Web Serial over USB, no server).

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

## Hardware notes

- Relays: K1 = D1, K2 = D2 on the Relay Proto Shield. Inputs: IN1–IN4 = A1–A4, all with the SAMD21's **internal
  pull-down**: an input is active when driven to 3.3 V (a PNP opto output or a contact to the board's 3.3 V) and
  reads off when open. **Never put more than 3.3 V on an input.** Change pins in `firmware/GateLink/pins.h` if your
  wiring differs; check the shield silkscreen.
- **Gate IN3 = opener power sense** (`power_sense`, default on). Without power the CSW24UL's AUX limit relays
  drop and the gate would read `between`, the same as a gate stopped mid-travel. An opto channel across the
  opener's 24 V accessory output drives IN3 to tell them apart: IN3 off → gate state `no_power`, which overrides
  the limits, and OPEN/CLOSE commands are acknowledged as refused without pulsing. Turn the `power_sense` toggle
  off (also possible remotely over LoRa) if IN3 isn't wired.
- **House IN2 = Shelly power sense** (`ctrl_power_sense`, default on). When the Shelly loses power its relay drops,
  which looks exactly like the Alarm.com switch being turned off; without this a power blip would close the gate
  and reopen it when the Shelly came back. A PNP opto channel across the Shelly's 12 V supply drives IN2 (use a
  channel rated for 12 V input; output side from the board's 3.3 V, as on the gate). While IN2 is off, IN1 edges are
  logged but never sent and resync pauses. Because the relay can drop before the opto does, each IN1 edge is held
  for `ctrl_confirm_ms` (default 500 ms) and discarded if IN2 drops meanwhile. After power returns (and after a house
  boot) IN1 edges count as sync for up to `ctrl_settle_ms` (default 10 s), ending as soon as the Shelly matches K1.
  A Shelly that reboots internally without losing its supply isn't covered. Turn `ctrl_power_sense` off if IN2 isn't
  wired.
- **Spare inputs** IN4 = A4 (and IN3 on the house board): pull-down inputs (contact to 3.3 V) reserved for
  future use such as a beam-break sensor or alarm status. They're debounced, logged (`input` events), shown in Status
  (the gate's are also sent to the house), with `inN_invert` toggles, but don't affect behaviour yet. Leave
  unwired if unused.
- **Opto board (gate):** a 4-channel PNP-output opto isolator (NOYITO MT-301R4P-P), OUT1–OUT4 → IN1–IN4, output
  GND → board GND, output VCC → the board's **3.3 V only** (a PNP output passes VCC straight to the pin). On the
  24 V side, wet each AUX limit contact from the opener's 24 V (24 V → AUX C, AUX NO → opto input) and put channel 3
  across the 24 V; OUT4 is spare. A lit opto reads active; power loss, a dead opto or a cut wire reads off — "not
  at a limit", never closed. Leave the gate's `inN_invert` toggles off: inverting would make those faults read active.
- **MKR VIN is 5 V max.** Power the gate board from the opener's 24 V accessory supply through a 24 V→5 V buck.
- **Power and antenna:** always attach the antenna before the radio transmits, and keep it away from the relay
  shield and field wiring (a U.FL→SMA pigtail lets it sit outside the enclosure). A full-power transmit while a
  relay is energized can crash a weakly powered board (seen on PC USB power); use a solid 5 V supply, and consider
  a bulk capacitor (~470 µF) across 5 V/GND or a LiPo on the MKR battery connector.
- Use relay NO/COM contacts for everything. Add TVS/RC suppression on long input runs.
- **Shelly Wave 1**: power from a low-voltage supply per Shelly's wiring (12 V DC here) so the SW input that K1 drives
  is low voltage — do not switch mains with the shield. Wire the Shelly relay output **I→3.3 V, O→HOUSE IN1**.
  Set the SW input to *toggle switch, contact closed = ON / open = OFF* (see the "SW1 switch type"
  parameter in the Shelly Wave 1 manual). Don't use detached mode. If it is left on "changes status when
  switch changes status", the Alarm.com switch flips whenever K1 moves (house log: `sync` right after
  `gate_state`, then repeated `resync`); the house ignores those flips, so no gate commands result.
- **2GIG contact sensor**: any 2GIG-compatible door/window sensor with external terminal input. Wire K2 NO/COM
  to its terminals; name it "Gate" in Alarm.com. (`sensor_invert` flips the sense if needed.)
- **CSW24UL**: set AUX relay A to *open limit* and AUX relay B to *closed limit* (per the LiftMaster manual)
  and wire their contacts to GATE IN1 / IN2 through the opto board (above).
  K1 NO/COM → OPEN + COM, K2 NO/COM → CLOSE + COM.

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
acknowledged and retried: the `retries` resends are spread over the message's lifetime with doubling gaps
(`cmd_ttl_s` for commands: at 10 s and 5 retries about 0.3, 0.9, 2.2, 4.7 and 9.7 s), so a command survives an
outage of nearly `cmd_ttl_s` and is dropped, never fired late, after it. Duplicate commands are detected and not re-pulsed. Role changes take
effect after a reboot.

## Bench test checklist

Use LEDs or a meter on the relay outputs and jumper wires on the inputs: an input is active when jumpered to
**3.3 V** (not GND). With the `power_sense` toggle on (the default after every flash), keep gate IN3 jumpered to
3.3 V for the other tests, or the gate reads `no_power`. On/off settings are toggle switches on the Config tab:
flip one, click **Apply** (it takes effect immediately), then **Save** to keep it across reboots. Without jumpers,
turning on an input's `inN_invert` toggle makes an open input read active, which is handy for testing on a
bare board; turn it back off afterwards. On USB power, set `tx_power` to ~5 dBm on both boards: a full-power
transmit while a relay is energized can crash the board (watchdog reset, shown as `reset_cause` in Status). Use **Identify** in the web console to strobe a board's LED
and tell the two apart.

Most of this list is automated by the end-to-end suite (`tests/e2e`, below), which runs it through the opener
simulator and the real Shelly; the test covering each item is named after it. By hand, only what needs eyes on
the hardware or the web page:

- **Still manual:** status LED patterns and **Identify**; the web console itself (Config toggles: flip one → its
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
- Power sense: release gate IN3 → gate `no_power` (`cause none`), house K2 releases and K1 energizes;
  toggle house IN1 → gate log `cmd_refused`, no `pulse`, house *Last command* shows *refused: opener unpowered*. Re-jumper IN3 → state
  follows the limits again. *e2e: `test_power_loss_at_rest`, `test_power_loss_mid_travel`.* Manual: turn the
  gate's `power_sense` toggle off and Apply (or from the house: Tools → remote setting `power_sense` = 0) → IN3 is
  ignored.
- Shelly power sense: with the gate open and the Shelly on, remove the Shelly's 12 V → house log `ctrl_power 0`
  (plus `ctrl` b=1, or `ctrl_power` b=2 if the relay dropped first), no `cmd_sent`, gate no `pulse`; restore it →
  `ctrl_power 1`, then `sync 1` when the Shelly comes back on. Compare the `ctrl` and `ctrl_power` times to check
  `ctrl_confirm_ms` covers the gap. *e2e: `test_controller_faults.py` (simulated unless
  `GATELINK_HA_POWER_ENTITY` is set)*
- Unpower the gate board → after `link_timeout_s` house K2 releases (sensor open), log `link_down`.
  *e2e: `test_link_loss_at_rest` (simulated outage)*
- Tools → Send replay on one board → the other board's replay counter increases (or it re-ACKs).
  *e2e: `test_replayed_frames_rejected`*
- Different key on one board → *Peer verified* stays no and `mac_fail` climbs. *e2e: `test_wrong_key_rejected`*
- Reboot the house board with IN1 jumpered to 3.3 V → the gate does not move.
  *e2e: `test_house_reboot_controller_on_gate_closed`*

### Bench opener simulator (`tools/GateSim`)

Bench only: an Arduino Uno with a relay module stands in for the CSW24UL so the gate board sees real limit and
power signals through its opto board, and the whole loop (controller → house → LoRa → gate → "opener" →
limits → house) runs without the opener. Relay COMs are wetted with 24 V and the NO contacts feed the opto
channels, as the opener's AUX relays would.

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

Serial 115200, one command per line: `status`, `open`, `close` (a local button, reported as an external
move), `stop` (strand it between), `power on|off`, `travel <s>` (default 15), `fault none|stuck|both|flicker|deaf`,
`polarity low|high`, `relay <1-3> on|off|auto` (force D2/D3/D4 for wiring checks, not saved), `help`. It prints `evt ...` lines for pulses and state changes. OPEN heads for open and
CLOSE for closed, reversing mid-travel. Without power the limits drop, motion freezes and pulses are ignored.
Faults: `stuck` leaves the limit and jams (exercises `travel_timeout_s`), `both` asserts both limits (gate
`fault`), `flicker` chatters a limit on arrival (debounce), `deaf` ignores the gate's pulses. Opening the port
resets the Uno, which restarts it closed and powered (the gate briefly sees `no_power`); scripts can avoid that
by opening it with DTR off (pyserial: set `dtr = False` before `open()`).

Polarity: the bench relay module switches on when the pin is HIGH (`polarity high`, the default); many
modules are the other way round. After boot, the closed-limit (D3) and power (D4) relays should be on and D2
off. If it's the other way round, send `polarity low` (saved in EEPROM). Travel time is saved too.

## End-to-end tests (`tests/e2e`)

A pytest suite drives the real bench end to end: both boards over their USB JSON console, the opener simulator
over serial, and the controller (the bench Shelly) through Home Assistant. It runs the bench checklist above,
plus failure modes that are hard to test by hand, and asserts the outcome at every hop. Bench only; there are no
unit tests.

Prerequisites:
- Both boards are flashed and configured with role, shared key and `tx_power` of about 5.
- The GateSim is wired to the gate board, and the web console is disconnected.
- `pip install -r tests/e2e/requirements.txt`.

```sh
export GATELINK_HA_URL=https://<home-assistant>:8123    # token read from ~/.ha_token (GATELINK_HA_TOKEN_FILE)
pytest tests/e2e -v                       # about 30 min; boards found by role, simulator on COM10 (--sim-port)
pytest tests/e2e -m soak --cycles 20      # repeated open/close cycles with latency stats
GATELINK_KEY=<32 hex> pytest tests/e2e -k wrong_key   # wrong-key test, opt-in (rewrites the gate's saved key)
GATELINK_KEY=<32 hex> pytest tests/e2e --restore-key -k boards_and_link   # put the shared key back on both boards
pytest tests/e2e -m longsoak --soak-minutes 120   # hours-long run, outages and opener faults mixed in
pytest tests/e2e -m rf --rf-cycles 5      # marginal link (2 dBm, SF12); see "Real RF" below
```

`--restore-key` reboots both boards (dropping any unsaved test profile an interrupted run left behind), then
`key.set`s `GATELINK_KEY` on both and waits for the link. Use it if a wrong-key run was killed mid-test and left the
gate with a random key. Keep the key itself somewhere safe (e.g. a password manager): boards can't read it back,
and the web console's config export doesn't include it.

Optional hardware:
- **Real controller power.** Wire house IN2's opto to the Shelly's 12 V and put that supply on an HA smart plug,
  then set `GATELINK_HA_POWER_ENTITY=switch.<plug>`. `test_controller_faults.py` then cuts real power, so the real
  relay-drops-before-opto race is tested (either order passes as long as nothing is commanded). Without it,
  controller power is simulated with house `in2_invert`.
- **Real RF.** `-m rf` runs pings and open/close cycles at `tx_power` 2 and SF12 and reports pings, RSSI/SNR,
  retries and giveups in the summary. Run it with an attenuator in line or the antennas off at the bench, and at
  the install site; also check ping and RSSI from the web console there.

Without the bench connected, every test is skipped. If `test_00_preflight` fails, the scenarios are skipped.

| File | Covers |
|---|---|
| `test_00_preflight.py` | Boards, firmware, key and link. Bench-safe settings. Ping. Simulator wiring. The controller reachable and following K1 (its SW input in follow mode, not edge-toggle) |
| `test_normal.py` | Open and close from the controller, with timing at every hop. Reversal mid-travel. Flip back before the gate leaves its limit. External moves, including one right after our command |
| `test_opener_faults.py` | Opener power loss at rest and mid-travel. Jammed gate. Opener ignoring the command (siren/override). Both limits active. Limit chatter |
| `test_link_faults.py` | Link loss and recovery. A command into a dead link (expires, never fires late). Short and 4 s outages covered by retries within `cmd_ttl_s`. A gate move missed during an outage. Replayed frames. Wrong key |
| `test_reboots.py` | Gate reset at rest and mid-travel. House reset with the controller wrong, and with the gate open |
| `test_controller_faults.py` | Controller toggled while unpowered. Relay dropping before the power sense. Controller coming back at the wrong level. Rapid toggling |
| `test_soak.py` | `-m longsoak`: open/close cycles, outages, opener power blips, external moves and jams in rotation; no resets or radio faults; counters to `soak_counters.csv` |
| `test_rf.py` | `-m rf`: the full loop over a marginal link (minimum power, SF12) |

How it works:
- **Faster timings.** The suite applies shorter timings to both boards for the run, unsaved: `heartbeat_s` 5,
  `link_timeout_s` 15, `travel_timeout_s` 15, `mismatch_timeout_s` 20, `cmd_ttl_s` 10, and simulator travel 8 s.
  At the end it restores every param from `results/<run>/config_backup.json`. Saved config is never written,
  except by the opt-in wrong-key test.
- **Simulated faults.** A radio outage is the gate moved to another `net_id`. Controller power is house
  `in2_invert` (IN2 isn't wired on the bench) unless `GATELINK_HA_POWER_ENTITY` is set.
- **Baseline.** Each test starts from the same point: opener powered, gate closed, controller off, house armed
  and in sync.
- **Invariant checks after every test.** OPEN and CLOSE are never pulsed together. Every gate pulse is `pulse_ms`
  long and answers a received command. No board resets or radio faults. No MAC failures or replays. The house sent
  exactly the number of commands the scenario expects.
- **Results.** `tests/e2e/results/<run>/` holds a time-ordered timeline per test (all four devices, JSONL) and
  `summary.md` (results, latencies, link quality, anomalies such as a gate → house status that needed a retry).

## Troubleshooting

- **Relays click at random / a board keeps restarting.** Check Status → *Last reset* and the log's `boot`
  entries. `watchdog` means the firmware froze for 8 s; each restart drops and re-energizes the house relays.
  On the bench this was caused by full-power TX with a relay energized on USB power — lower `tx_power`,
  improve the supply, and check the antenna (see Hardware notes). `brownout` points straight at power.
- **Radio TX faults** (Board card, `radio_fail` log entries): the radio stopped mid-transmit and was
  re-initialised. Occasional ones are recovered automatically; frequent ones mean power or RF trouble.
- **The gate never pulses on its own.** It only pulses K1/K2 for an OPEN/CLOSE command from the house or a
  relay test from the console. The house log's `cmd_sent` entries show what it sent and why.
