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
   arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM5 --input-dir <scratchpad>/build
   arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM21 --input-dir <scratchpad>/build
   ```
   If `arduino-cli upload` fails with `No device found` (its port discovery sees nothing on the bench PC; `board
   list` comes back empty too), upload by hand: open the board's port at 1200 baud and close it with DTR off,
   wait for the bootloader port to appear (USB PID 0x0059, `serial.tools.list_ports`; a new COM number), then
   `~/AppData/Local/Arduino15/packages/arduino/tools/bossac/1.7.0-arduino3/bossac.exe -i -d --port=<bootloader port> -U true -i -e -w -v <scratchpad>/build/GateLink.ino.bin -R`
   and look for `Verify successful`.
   A board in the bootloader can re-enumerate on another COM number (`python -m serial.tools.list_ports -v` shows it). If an
   upload fails with the port busy, double-tap the board's reset button and retry on the port it shows.
   Upload one board at a time and check `ports` between. If ports report `no answer within 8 s` (every USB
   serial port stuck, seen after uploads), ask the user to replug the USB hub; the boards keep running on their
   bench supplies.
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
