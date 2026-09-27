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

Dependencies: `arduino:samd` core; libraries `LoRa` (sandeepmistry), `Crypto` (rweather), `FlashStorage` (cmaglie), `ArduinoJson` v7, `Adafruit SleepyDog Library`. There are no automated tests; verification is a clean compile (keep project files warning-free — filter output with `grep GateLink[\\/]`) plus the hardware bench checklist in README.md.

## Firmware architecture

Layers, bottom up:
- `radio.cpp` — wraps the LoRa lib in **polling** mode (`parsePacket()` re-arms RX-single each loop; `endPacket()` blocks). The loop must stay non-blocking. `radioRandom32()` samples wideband RSSI in continuous RX and hashes it; don't replace it with `LoRa.random()` alone (near-constant in standby).
- `link.cpp` — authenticated framing: `ver|type|net_id|src|dst|session|seq|payload|tag`, tag = HMAC-SHA256(key) truncated to 8 bytes. Replay protection has no persisted counters: each boot picks a random session id; a peer session is accepted only after a HELLO challenge is echoed in HELLO_ACK; seqs pass a 32-frame sliding window. Reliable messages use per-type `Slot`s (CMD/STATUS/CFG) with retry/TTL; a new send replaces the slot. Already-accepted retransmits are re-ACKed from an ACK memo, not re-processed. The link is disabled entirely while `cfg.key_set == 0`.
- `role_gate.cpp` / `role_house.cpp` — application state machines (see below). Wire formats for STATUS/DIAG payloads are defined in `roles.h` and parsed in `role_house.cpp` / `console.cpp`; keep them in sync.
- `console.cpp` — newline-delimited JSON request/response over USB serial (`{"id","cmd",...}` → `{"id","ok",...}`) plus unsolicited `{"event":...}` lines. This is the contract `web/app.js` depends on; change both together.
- `config.cpp` — `Config` struct in program flash (FlashStorage) with CRC; **erased on every firmware upload**. The `PARAMS[]` table drives console get/set, the web form (via `meta`), and remote-over-LoRa writes (`P_REMOTE` flag; radio params are never remote-writable). Adding a setting = field in `Config` + default + `PARAMS` row (+ group/help text in `web/app.js`). Bump `CFG_VERSION` when the struct layout changes.
- `GateLink.ino` — setup/loop, PING/PONG, dispatch by `activeRole`. `activeRole` is latched at boot; `cfg.role` changes only take effect after reboot. Use `activeRole`, not `cfg.role`, for runtime behaviour.

## Behavioural invariants (don't break these)

- The CSW24UL OPEN/CLOSE inputs are shared with an AES Prime Edge cellular controller and a siren sensor. Gate relays are **only ever pulsed** (K1=OPEN, K2=CLOSE, interlocked), never held, and gate state comes only from the opener's AUX limit inputs, never from our last command.
- House K1 drives the Shelly SW input so the Alarm.com switch mirrors the real gate. Shelly edges caused by K1 fall inside a sync window and must not become commands; lingering mismatches are fixed by the resync path (cycle K1). House never commands the gate from the Shelly level at boot — it waits for the first STATUS plus a sync window (`armed`).
- House K2 (alarm contact sensor) reads closed only when the gate is known closed; on link loss it fails open (`linkloss_open`).
- Timing comparisons must be wrap-safe and signed (`(int32_t)(now - t) >= ms`): handlers invoked from `linkPoll` stamp times with `millis()`, which can be slightly later than the loop's `now`.
- Use `Serial.dtr()` rather than `if (Serial)` — the SAMD core's bool operator has a 10 ms `delay`.
- Hardware watchdog is 8 s; keep blocking work well under that.
