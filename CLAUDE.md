# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository. It is loaded into every session, so keep it short (a test fails it past 16 KB): detail belongs in `docs/`.

## What this is

GateLink: two Arduino MKR WAN 1310 boards (on MKR Relay Proto Shields) bridge an Alarm.com / 2GIG system (via a Shelly Wave 1 Z-Wave relay) at the house to a LiftMaster CSW24UL gate opener over raw point-to-point LoRa. `firmware/GateLink/` is one Arduino sketch flashed to both boards; the role (house/gate) is stored in flash config. `web/` is a static page that configures/diagnoses a board over USB using the Web Serial API. README.md is the overview and indexes the docs: `docs/architecture.md` (firmware layers and web console in detail), `docs/hardware.md` (wiring, device settings), `docs/protocol.md`, `docs/threat-model.md` (threats by id; update it with link, console, key or release changes), `docs/console.md` (commands, status, log events), `docs/key-management.md`, `docs/config.md` (settings; generated), `docs/bench-testing.md` (bench rules, checklist), `docs/development.md` (workflow, CI, versions, releases).

Repo: https://github.com/ccirone2/LoRa-GateLink (public). Web console: https://ccirone2.github.io/LoRa-GateLink/, deployed from `web/` by `.github/workflows/pages.yml` (`gh workflow run pages.yml` redeploys).

## Commands

`arduino-cli` (not PlatformIO), from the repo root. The pinned core and libraries (`ArduinoJson` v7 among them) and the less routine commands are in `docs/development.md`.

```sh
arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all firmware/GateLink   # keep warning-free (grep GateLink[\\/])
python tools/flash.py <dir>/GateLink.ino.bin house gate   # bench upload, checks the version (/flash)
npm ci && npm test                             # web console: ESLint, unit and browser tests
make -C tests/native                           # host tests (tests/native/README.md)
make -C tests/native -f fuzz/Makefile fuzz-smoke   # libFuzzer (clang)
python -m pytest tests/tools -q; ruff check tests tools; python tools/check_contract.py; python tools/docgen.py
GATELINK_HA_URL=https://<ha>:8123 pytest tests/e2e -v   # bench suite, hardware required (/bench)
python tools/gatelink.py ports                 # boards on USB (also: <house|gate|COMx> <cmd> k=v)
python tools/gatelink.py snapshot | restore    # config before a flash / config + key after
```

Extend the host tests (both boards in a simulated site, every invariant below checked each simulated millisecond) with any behaviour change, especially paths the bench can't reach (replays, restarts, wraps, power cuts mid-save, races); `XFAIL_TEST` marks a known open bug. A firmware change that moves a mutation's text must update `tests/native/mutations.json`; `fuzz/crashes/` reproducers must keep crashing until fixed.

## Bench

Rules and checklist: `docs/bench-testing.md`. Never skip these:
- Only one program can hold a port: close/disconnect the web console before scripts or the suite.
- On USB power keep `tx_power` low (~5): full-power TX with a relay energized crashed the board into watchdog resets.
- Keep `inN_invert` at 0: toggle it only to exercise an input on a bare board, then restore it to 0.
- Uploads keep config and key, but snapshot anyway (`/flash`, or `snapshot`/`restore`). The key can't be read back: if `~/.gatelink_key` is lost, `key.set` a fresh one on both boards.
- Serve the bench wiring record (`tools/bench-wiring/wiring.json`) only with `python tools/bench-wiring/serve.py` (port 8001; stop whatever else holds it), never `python -m http.server`; update it in the same session whenever a wire moves or is confirmed.
- `tools/GateSim/` is a bench-only Uno sketch (`--fqbn arduino:avr:uno`) simulating the opener; open its port with DTR off, or the gate briefly sees `no_power`.

## Firmware architecture

Layers, bottom up. **Before changing a layer, read its section in `docs/architecture.md`** (design, measured numbers, approaches tried and abandoned) and update it with the code.
- `radio.cpp`: LoRa lib in polling mode, RX continuous, async TX with a deadline. The loop must never block (no lib `parsePacket()`, RX-single, blocking `endPacket()` or waiting for TX-done).
- `link.cpp`: HMAC-tagged frames; per-boot sessions verified by a HELLO challenge, then a replay window; reliable `Slot`s with retry/TTL; listen-before-talk. Session ids never repeat under one key; reliable messages must be idempotent (CMDs by `lastCmdId`). Off while `cfg.key_set == 0`.
- `role_gate.cpp` / `role_house.cpp`: the state machines (invariants below); STATUS/DIAG layouts in `roles.h`, kept in sync with their parsers.
- `history.cpp` / `histlog.cpp` (link history, in SPI flash) and `health.cpp` (fault output D5): diagnostics only; nothing may read them to decide.
- `console.cpp` / `console_io.cpp`: newline-delimited JSON over USB (and Serial1), one request per port per loop pass. The web console (and its fake board, `tests/web/fake-serial.js`), the e2e suite and `tools/gatelink_client/` parse it: change them with it, `log.cpp`, `roles.h` and `docs/console.md` (`tools/check_contract.py` checks).
- `config.cpp` / `extflash.cpp`: (param id, value) pairs in SPI flash, kept across uploads; every flash access via `FlashAccess` (radio off the air ~0.5 s; never hold `LORA_RESET` low permanently). PARAMS ids are permanent; a setting = `Config` field + default + `PARAMS` row with a new id + `web/js/settings.js`. Radio params and `inN_invert` are never remote-writable.
- `app.cpp` (+ `GateLink.ino`, `board.h`, `supply.cpp`, `io.cpp`, `log.cpp`): boot, loop, dispatch, LED, spare inputs. `activeRole` is latched at boot: use it, not `cfg.role`, for runtime behaviour.

## Web console

Plain ES modules, no build step (`web/app.js`, `web/js/`), tested by `npm test` against fake boards. Read `docs/architecture.md` "Web console" before changing it (reconnect, port close order, `fieldValue`/`setField`, SAM-BA flashing, the `WIRING` table).

**Naming rule:** don't use "Shelly" or "Alarm.com" anywhere in `web/` — the house-side device is the generic "controller" (it may be replaced). Firmware names that reach the page follow the same rule (`ctrl_sync`, `ctrl` status field / log event). README, `docs/` and firmware comments may name the actual install hardware.

## Tracking, versions and releases

- `TODO.md` — open bugs, investigations, bench/field tasks. `ROADMAP.md` — desired features. Add findings there as you go; remove items once their fix or feature is merged (the PR and release notes record them).
- Every firmware change bumps `FW_VERSION` (`config.h`); MINOR for a `CFG_VERSION` or config record format bump, a new feature, or a change both boards need together (STATUS/DIAG or frame format), PATCH otherwise. One branch + PR per change, with bench results in the PR body.
- The changelog is GitHub Releases (`vX.Y.Z` tag on the merge commit). A release first needs bench evidence per `docs/release-criteria.md` (`/release`), merged before the tag. CI (`docs/development.md`) must pass.
- Agent tooling (`docs/agent-tooling.md`): skills in `.claude/skills/` (`/flash`, `/bench`, `/release`, `/doc-audit`, `/pr-followup`), hooks in `.claude/settings.json` running `tools/agent/hooks.py` (checks after each edit; a checklist before finishing), nightly routines in `.claude/routines/`. Change them in the same PR as the code they describe; `tests/tools/test_agent_tooling.py` tests them.
- Keep docs in step with code: layer design → `docs/architecture.md`; console/log/status → `docs/console.md`; pins/wiring → `docs/hardware.md` and the `WIRING` table; suite options → `tests/e2e/README.md`. `docs/config.md` and the wire-format tables in `docs/protocol.md` are generated (`python tools/docgen.py --write`; never edit between its markers); `python tools/docgen.py` (CI) checks the rest, quoted values too (`FACTS`; add one when a doc quotes a new value).

## Behavioural invariants (don't break these)

- The CSW24UL OPEN/CLOSE inputs are shared with an AES Prime Edge cellular controller and a siren sensor. Gate relays are **only ever pulsed** (K1=OPEN, K2=CLOSE, interlocked: a pulse never starts within `INTERLOCK_MS` (100 ms) of the other relay releasing, whether it cuts that one short or it ended by itself (`Relay::gapLeft`), since a contact can make before the other breaks), never held, a running pulse is never restarted (a command for the relay already pulsing is the same press, ACKed without a pulse; a `relay.test` of it is refused `busy`), and gate state comes only from the opener's AUX limit inputs, never from our last command. A relay test sets a target only if the gate isn't already at that limit.
- Nothing that stops the loop (a flash save, a radio restart: up to ~1 s) may run while a pulse does (`appRelaysPulsing`), or `Relay::update` can't end it on time: the console holds `config.set`/`config.save`/`config.reset`/`hist.clear`/`key.set`/`reboot` and the gate queues remote config writes until it's over (four deep; a fifth isn't taken, `linkRefuse`); radio recovery after a fault or a failed start (`radioRecover`, in `appLoop`) and the history's flash writes (`histSave`) wait too.
- House K1 drives the Shelly SW input so the Alarm.com switch mirrors the real gate. While the gate is `between`, K1 holds the level of the limit it left (`holdingTravel`) until the other limit is reached or the gate's `travel_timeout_s` (carried in STATUS since 0.13.0; the house's own before that) passes; mismatch/resync is paused during that hold. Every K1 change opens a sync window (`driveK1`), and IN1 edges inside it must not become commands, even when the Shelly shouldn't have moved (a Shelly set to toggle on SW edges would otherwise turn sync into reverse commands); lingering mismatches are fixed by the resync path (cycle K1; no IN1 edge ends its window before K1 is back, `settleUntil`, or the Shelly following K1 away and back would read as the user).
- House never commands the gate from the Shelly level at boot — it waits for the first STATUS plus a sync window (`armed`). After boot or controller power return (`checkSoon`), a Shelly still out of step once the settle window closes is resynced at once instead of after `mismatch_timeout_s`; so is one left out of step by a refused (`no_power`) or overridden (`timeout`) command, even when the travel hold kept the mismatch from starting. A user command clears `checkSoon`. A command opposite to one ACKed but not yet reported in a STATUS is sent, never suppressed (the gate may already be heading away).
- Gate IN3 is AC power (`power_sense`, default on; the 24 V supply on mains, while the opener rides through on its battery). While it's off, commands are ACKed `RES_NO_POWER` without pulsing, a limit that reads is still trusted, and with none reading the gate state is `no_power` (not `between`: the opener may be dead); transitions into or out of `no_power` have cause none. A move into `between` is held `BETWEEN_HOLD_MS` (`BOOT_SETTLE_MS` out of `no_power`) so a failing input supply, whose limit opto drops before IN3, reads `no_power` and not a move. STATUS bit 6 = AC lost. `updateSpareInputs` must run before `readState()` in `gateLoop`.
- After boot the gate's first STATUS waits until its inputs have been steady for `BOOT_SETTLE_MS` (3 s, at most 10 s; status `settling`): the opener may restart with it and its limits come back later, and an early report made the house flash not-closed. Nothing is attributed to a boot (no cause).
- Gate `cause` is `lora` only while our pulse still has a `target` and the gate moves toward it (`between` or the target; leaving the target limit counts only when our pulse is what moves it off, `leaving`, as after a reversal before the gate left its limit, and it lapses once both relays have been released for `debounce_ms` with the gate still at that limit); reaching the opposite limit ends the target with `timeout` (the house resyncs at once); every other movement is `external`, even right after one of our commands. Relay tests set a target too. Don't go back to a time window after the last pulse: it mislabels local/AES moves.
- While the gate is `no_power` or `fault`, the house shows not-closed (K1 energized, K2 open): the position can't be verified and the gate may be moved by hand. Nothing is commanded. Without AC but with a limit reading, the house follows that limit as usual.
- House controller power = IN2 (`ctrl_power_sense`) and the board's own VIN power good (`ctrl_power_pmic`, `supply.cpp`: the BQ24195L charger over I2C, read-only), both default on: while either is off, IN1 edges are never commands and resync pauses. The supply drops ~0.2 s before the Shelly's relay (the board shares its 12 V); the opto lags the relay by ~1.65 s and misses 300 ms dips. OFF edges (CLOSE) wait `ctrl_confirm_ms` (default 0.5 s; `pendingAction`) and are discarded if power drops meanwhile; ON edges (OPEN) go at once, since a power loss can't produce them. Power return and house boot open a sync window of `ctrl_settle_ms` (`openSettleWindow`), which a matching IN1 edge can't end before `settleUntil` (the Shelly may restore its own state or chatter); `openSyncWindow` never shortens an open window.
- House K2 (alarm contact sensor) reads closed only when the gate is known closed (from its latest STATUS); on link loss it fails open (`linkloss_open`). The link is up only while STATUS keeps coming: a STATUS older than the last one taken (withheld and delivered late) changes nothing and keeps nothing alive.
- Timing comparisons must be wrap-safe and signed (`elapsed(now, t, ms)` in `link.h`, i.e. `(int32_t)(now - t) >= ms`): handlers invoked from `linkPoll` stamp times with `millis()`, which can be slightly later than the loop's `now`. A stamp that can sit unused for 2^31 ms (~24.9 days: a steady link, a long silence) then reads as in the future: cap its age (`RX_AGE_CAP_MS`, `capStamps`) or compare it unsigned as an age (`now - t < ms`). Host tests run such paths past 25 days.
- All inputs use the internal **pull-down** (`INPUT_PULLDOWN`, active = HIGH) to suit the gate's PNP-output opto board: a dead opto or cut wire must read inactive. Keep `inN_invert` at 0; don't fix polarity with invert (it makes faults read active). Bench jumpers go to 3.3 V, not GND.
- D5 (`fault_out`) is only a "needs attention" signal: off, it stays an input with the pull-down; on, HIGH while healthy, LOW from boot until the role has decided, LOW once a problem has lasted `fault_hold_s`, HIGH again at once on recovery. It commands nothing and no role decision may depend on it.
- Use `Serial.dtr()` rather than `if (Serial)` — the SAMD core's bool operator has a 10 ms `delay`.
- Hardware watchdog is 8 s, enabled in `setup()` right after the relays are de-energised (so a hang during init resets too); keep blocking work well under that.
