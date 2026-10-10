---
name: flash
description: Flash GateLink firmware to the bench boards (and optionally the GateSim Uno), then check each board kept its role, config and link key (restoring them if not) and confirm the link. Use whenever firmware needs uploading to the MKR boards on the bench, e.g. before a bench test of a firmware change.
---

# Flash the bench boards

From 0.5.0 the saved config and key live in the board's SPI flash chip and survive uploads. Uploading over
older firmware, or onto a board whose chip doesn't answer (`cfg internal` in `ports`), still erases them, so
this procedure always snapshots first and restores when needed. Bench facts: the MKR boards are usually COM5/COM21 (which is which can change; boards
are identified by role), the GateSim Uno is on COM10, the key is in `~/.gatelink_key`.

1. **Free the ports.** The web console (or a running pytest) holding a port makes everything below fail with
   "access denied"/busy. If so, ask the user to click Disconnect in the web console.
2. **Snapshot** (skip if the boards are already blank, e.g. after a failed flash; then an older
   `~/.gatelink_config.json` is used):
   ```sh
   python tools/gatelink.py ports
   python tools/gatelink.py snapshot
   ```
   Check `ports` first: both boards should show their role, `key set` and the bench `tx_power` 5. If a board
   runs a test profile left behind by an interrupted e2e run (`python tools/gatelink.py <role> config.get`
   shows short `heartbeat_s` 5 / `link_timeout_s` 15), reboot it (`python tools/gatelink.py <role> reboot`) before the snapshot so the saved config is captured.
3. **Compile once, upload to each board.** Clean compile means no warnings in project files:
   ```sh
   arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all --output-dir <scratchpad>/build firmware/GateLink 2>&1 | grep -E "GateLink[\\/].*warning|Sketch uses"
   python tools/flash.py <scratchpad>/build/GateLink.ino.bin house gate
   ```
   `tools/flash.py` does what `arduino-cli upload` does with every wait bounded, one board at a time: the 1200-baud
   touch, the bootloader's own port (often a new COM number), bossac with a timeout, then the board back by its USB
   serial number, reporting the firmware in the .bin. Don't use `arduino-cli upload` on this bench: it has hung in
   bossac and left a board wedged in its bootloader. Never kill a bossac that is writing.
   - `didn't come back as a bootloader port`: double-tap the board's reset button, then
     `python tools/flash.py <bin> <the port it shows>` (a bootloader port, USB PID 0x0059, is written directly).
   - `bossac didn't finish` or `No device found` on a bootloader port: ask the user to replug that board's USB cable
     (or double-tap reset), then flash its bootloader port as above. The board stays in its bootloader until then.
   - If ports report `no answer within 8 s` (every USB serial port stuck, seen after uploads), ask the user to
     replug the USB hub; the boards keep running on their bench supplies.
4. **Check, then restore if needed.** Run `python tools/gatelink.py ports`. If both boards show their role,
   `key set`, `tx_power` 5 and `cfg spi`, the config survived: skip to the link check below. Otherwise restore
   role, config and key, reboot, and wait for the link:
   ```sh
   python tools/gatelink.py restore
   ```
   It finds each board by its USB serial number, so a board that came back on a different COM number is still
   matched (a snapshot taken before that change is keyed by port: then edit the port keys in
   `~/.gatelink_config.json` first). It ends with `link verified on ...` naming both roles; anything else is a
   failure to report, with `python tools/gatelink.py ports` output.
   Link check without a restore: `ports` again after ~15 s should show `verified True` on both.
5. **GateSim** (only if `tools/GateSim` changed): `arduino-cli compile --fqbn arduino:avr:uno --warnings all
   tools/GateSim` then `arduino-cli upload --fqbn arduino:avr:uno -p COM10 tools/GateSim`. Its settings live in
   EEPROM and survive. After boot only the power relay (D4) is energized; D3's coil is off, its NC contact giving the closed limit.
6. Report the firmware version now on each board (`ports`). Don't raise `tx_power` above 5 on USB power.

If the key file is missing or wrong, stop and ask the user: boards can't read the key back, and setting a new
one means writing it to both boards and saving it somewhere safe.
