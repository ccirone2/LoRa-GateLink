---
name: flash
description: Flash GateLink firmware to the bench boards (and optionally the GateSim Uno), then check each board kept its role, config and link key (restoring them if not) and confirm the link. Use whenever firmware needs uploading to the MKR boards on the bench, e.g. before a bench test of a firmware change.
---

# Flash the bench boards

From 0.5.0 the saved config and key live in the board's SPI flash chip and survive uploads. Uploading over
older firmware, or onto a board whose chip doesn't answer (`cfg internal` in `ports`), still erases them, so
this procedure always snapshots first and restores when needed. Bench facts: the MKR boards are usually COM5/COM6 (which is which can change; boards
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
   shows a test profile left behind by an interrupted e2e run (short `heartbeat_s` 5 / `link_timeout_s` 15),
   reboot it (`python tools/gatelink.py <role> reboot`) before the snapshot so the saved config is captured.
3. **Compile once, upload to each board.** Clean compile means no warnings in project files:
   ```sh
   arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all --output-dir <scratchpad>/build firmware/GateLink 2>&1 | grep -E "GateLink[\\/].*warning|Sketch uses"
   arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM5 --input-dir <scratchpad>/build
   arduino-cli upload  --fqbn arduino:samd:mkrwan1310 -p COM6 --input-dir <scratchpad>/build
   ```
   A board in the bootloader can re-enumerate on another COM number; `arduino-cli board list` shows it. If an
   upload fails with the port busy, double-tap the board's reset button and retry on the port it shows.
4. **Check, then restore if needed.** Run `python tools/gatelink.py ports`. If both boards show their role,
   `key set`, `tx_power` 5 and `cfg spi`, the config survived: skip to the link check below. Otherwise restore
   role, config and key, reboot, and wait for the link:
   ```sh
   python tools/gatelink.py restore
   ```
   Link check without a restore: `ports` again after ~15 s should show `verified True` on both.
   It matches boards by port, so if a board came back on a different COM number, edit the port keys in
   `~/.gatelink_config.json` first. It ends with `link verified on gate, house`; anything else is a failure to
   report, with `python tools/gatelink.py ports` output.
5. **GateSim** (only if `tools/GateSim` changed): `arduino-cli compile --fqbn arduino:avr:uno --warnings all
   tools/GateSim` then `arduino-cli upload --fqbn arduino:avr:uno -p COM10 tools/GateSim`. Its settings live in
   EEPROM and survive. After boot the closed-limit (D3) and power (D4) relays are on.
6. Report the firmware version now on each board (`ports`). Don't raise `tx_power` above 5 on USB power.

If the key file is missing or wrong, stop and ask the user: boards can't read the key back, and setting a new
one means writing it to both boards and saving it somewhere safe.
