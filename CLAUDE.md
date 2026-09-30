# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

GateLink: two Arduino MKR WAN 1310 boards (on MKR Relay Proto Shields) bridge an Alarm.com / 2GIG system (via a Shelly Wave 1 Z-Wave relay) at the house to a LiftMaster CSW24UL gate opener over raw point-to-point LoRa. `firmware/GateLink/` is one Arduino sketch flashed to both boards; the role (house/gate) is stored in flash config. `web/` is a static page that configures/diagnoses a board over USB using the Web Serial API. See README.md for wiring, device settings, and the bench-test checklist.

Repo: https://github.com/ccirone2/LoRa-GateLink (public). The web console is hosted at https://ccirone2.github.io/LoRa-GateLink/, deployed from `web/` by `.github/workflows/pages.yml` on pushes to `main` that touch `web/**` (or via `gh workflow run pages.yml`).

## Commands

Toolchain is `arduino-cli` (not PlatformIO). Run from the repo root:

```sh
arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all firmware/GateLink
arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COMx firmware/GateLink
node --check web/app.js                        # syntax check for the web UI
python -m http.server 8000 -d web              # serve UI at http://localhost:8000 (Chrome/Edge)
```

`tools/GateSim/` is a separate bench-only Uno sketch (`--fqbn arduino:avr:uno`) that simulates the CSW24UL's limits, power and OPEN/CLOSE inputs for the gate board; see README "Bench opener simulator". It is not GateLink firmware.

Dependencies: `arduino:samd` core; libraries `LoRa` (sandeepmistry), `Crypto` (rweather), `FlashStorage` (cmaglie), `ArduinoJson` v7, `Adafruit SleepyDog Library`. There are no automated tests; verification is a clean compile (keep project files warning-free — filter output with `grep GateLink[\\/]`) plus the hardware bench checklist in README.md.

## Firmware architecture

Layers, bottom up:
- `radio.cpp` — wraps the LoRa lib in **polling** mode (`parsePacket()` re-arms RX-single each loop). `radioSend()` uses async `endPacket(true)` and polls TX-done with a deadline (airtime + 200 ms) via its own SX127x register reads (the lib's are private); a stuck or reset radio is logged (`radio_fail`, a=1), counted (`radio_faults`) and re-initialised instead of hanging until the watchdog. Don't go back to the lib's blocking `endPacket()` — it spins forever if the radio resets mid-TX. The loop must stay non-blocking. `radioRandom32()` samples wideband RSSI in continuous RX and hashes it; don't replace it with `LoRa.random()` alone (near-constant in standby).
- `link.cpp` — authenticated framing: `ver|type|net_id|src|dst|session|seq|payload|tag`, tag = HMAC-SHA256(key) truncated to 8 bytes. Replay protection has no persisted counters: each boot picks a random session id; a peer session is accepted only after a HELLO challenge is echoed in HELLO_ACK; seqs pass a 32-frame sliding window. Reliable messages use per-type `Slot`s (CMD/STATUS/CFG) with retry/TTL; a new send replaces the slot. Already-accepted retransmits are re-ACKed from an ACK memo, not re-processed. The link is disabled entirely while `cfg.key_set == 0`.
- `role_gate.cpp` / `role_house.cpp` — application state machines (see below). Wire formats for STATUS/DIAG payloads are defined in `roles.h` and parsed in `role_house.cpp` / `console.cpp`; keep them in sync.
- `console.cpp` — newline-delimited JSON request/response over USB serial (`{"id","cmd",...}` → `{"id","ok",...}`) plus unsolicited `{"event":...}` lines. This is the contract `web/app.js` depends on; change both together.
- `config.cpp` — `Config` struct in program flash (FlashStorage) with CRC; **erased on every firmware upload**. The `PARAMS[]` table drives console get/set, the web form (via `meta`), and remote-over-LoRa writes (`P_REMOTE` flag; radio params are never remote-writable). Adding a setting = field in `Config` + default + `PARAMS` row (+ group/help text in `web/app.js`). Bump `CFG_VERSION` when the struct layout changes.
- `GateLink.ino` — setup/loop, PING/PONG, dispatch by `activeRole`, status LED (`updateLed`: PWM on `LED_BUILTIN`; identify strobe / solid = no role / breathing = link up / heartbeat = no link), spare inputs IN3/IN4 (`updateSpareInputs`, called by both roles; reported in STATUS bits 4–5; on the gate IN3 is the opener power sense, IN4 has no behaviour yet); house IN2 is the controller power sense (`ctrl_power_sense`, handled in `role_house.cpp`, not reported to the gate), and the reset cause (`PM->RCAUSE`, logged in `boot` a= and reported as `reset_cause`). `activeRole` is latched at boot; `cfg.role` changes only take effect after reboot. Use `activeRole`, not `cfg.role`, for runtime behaviour.

## Web console

`web/app.js` is plain JS, no build step. Notable pieces: auto-reconnect (`startReconnect`/`tryReconnect`: after a reboot or unexpected drop, reopen the already-granted port for 30 s; the Web Serial `connect` event identifies the returning board since both boards share VID/PID); `disconnect()` must await both stream pipes before `port.close()` or the port stays open and blocks uploads; the Install tab is driven by the `WIRING` table (keep it in sync with `pins.h` and the role behaviour); 0/1 params without a `SELECTS` entry render as toggle checkboxes, so read/write form fields through `fieldValue`/`setField`, not `.value`.

**Naming rule:** don't use "Shelly" or "Alarm.com" anywhere in `web/` — the house-side device is the generic "controller" (it may be replaced). Firmware names that reach the page follow the same rule (`ctrl_sync`, `ctrl` status field / log event). README and firmware comments may name the actual install hardware.

## Bench testing

The boards can be driven from scripts over USB serial with the same JSON console the web page uses (e.g. pyserial: `{"id":1,"cmd":"status"}`, `log.get`, `config.set`, `key.set`, `identify`, `reboot`). Only one program can hold a port — close/disconnect the web console first. Every upload wipes config (role, key, `tx_power`; `power_sense` and `ctrl_power_sense` return to on), so re-apply them afterwards. Without jumpers, toggling an input's `inN_invert` flips what the firmware sees, which lets scripts exercise input paths on bare boards (restore to 0 afterwards). On USB power keep `tx_power` low (~5): full-power TX with a relay energized crashed the board into watchdog resets. The key can't be read back from a board, so save `config.get` and know the key before flashing; if the key is lost, `key.set` a fresh one on both boards. With the GateSim Uno wired to the gate board, the full loop (controller → house → gate → simulated opener) can be scripted; open its port with DTR off, or the Uno resets and the gate briefly sees `no_power`.

## Behavioural invariants (don't break these)

- The CSW24UL OPEN/CLOSE inputs are shared with an AES Prime Edge cellular controller and a siren sensor. Gate relays are **only ever pulsed** (K1=OPEN, K2=CLOSE, interlocked), never held, and gate state comes only from the opener's AUX limit inputs, never from our last command.
- House K1 drives the Shelly SW input so the Alarm.com switch mirrors the real gate. While the gate is `between`, K1 holds the level of the limit it left (`holdingTravel`) until the other limit is reached or `travel_timeout_s` passes; mismatch/resync is paused during that hold. Every K1 change opens a sync window (`driveK1`), and IN1 edges inside it must not become commands, even when the Shelly shouldn't have moved (a Shelly set to toggle on SW edges would otherwise turn sync into reverse commands); lingering mismatches are fixed by the resync path (cycle K1). House never commands the gate from the Shelly level at boot — it waits for the first STATUS plus a sync window (`armed`).
- Gate IN3 is opener power (`power_sense`, default on): while it's off the gate state is `no_power` (overrides the limits, cause none) and commands are ACKed `RES_NO_POWER` without pulsing. `updateSpareInputs` must run before `readState()` in `gateLoop`.
- Gate `cause` is `lora` only while our pulse still has a `target` and the gate moves toward it (`between` or the target); every other movement is `external`, even right after one of our commands. Relay tests set a target too. Don't go back to a time window after the last pulse: it mislabels local/AES moves.
- While the gate is `no_power` or `fault`, the house shows not-closed (K1 energized, K2 open): the position can't be verified and the gate may be moved by hand. Nothing is commanded.
- House IN2 is controller power (`ctrl_power_sense`, default on): while it's off, IN1 edges are never commands and resync pauses. IN1 edges wait `ctrl_confirm_ms` (`pendingAction`) and are discarded if power drops meanwhile, since the relay can drop before the opto. Power return and house boot open a sync window of `ctrl_settle_ms`; `openSyncWindow` never shortens an open window.
- House K2 (alarm contact sensor) reads closed only when the gate is known closed; on link loss it fails open (`linkloss_open`).
- Timing comparisons must be wrap-safe and signed (`(int32_t)(now - t) >= ms`): handlers invoked from `linkPoll` stamp times with `millis()`, which can be slightly later than the loop's `now`.
- All inputs use the internal **pull-down** (`INPUT_PULLDOWN`, active = HIGH) to suit the gate's PNP-output opto board: a dead opto or cut wire must read inactive. Keep `inN_invert` at 0; don't fix polarity with invert (it makes faults read active). Bench jumpers go to 3.3 V, not GND.
- Use `Serial.dtr()` rather than `if (Serial)` — the SAMD core's bool operator has a 10 ms `delay`.
- Hardware watchdog is 8 s; keep blocking work well under that.
