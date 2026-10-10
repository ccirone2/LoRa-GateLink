# Development

How changes are made, verified and released. Architecture notes and the behavioural invariants that every
change must keep are in [CLAUDE.md](../CLAUDE.md); read those before touching the firmware.

## Toolchain

`arduino-cli` (not PlatformIO). CI builds with these versions; match them locally when chasing a build
difference:

| Package | Version |
|---|---|
| `arduino:samd` core | 1.8.14 |
| `arduino:avr` core (GateSim) | 1.8.8 |
| `LoRa` (sandeepmistry) | 0.8.0 |
| `Crypto` (rweather) | 0.4.0 |
| `FlashStorage` (cmaglie) | 1.0.0 |
| `ArduinoJson` | 7.4.3 |
| `Adafruit SleepyDog Library` | 1.8.4 |

When updating one, change it here and in `.github/workflows/ci.yml` and `release.yml`.

The web console is plain JS (ES modules) with no build step; its lint and tests need Node.js 22 and `npm ci`
(`package.json`). The bench tools need Python 3 with
`pip install -r tests/e2e/requirements.txt`.

## Where things are tracked

| What | Where |
|---|---|
| Released changes (changelog) | [GitHub Releases](https://github.com/ccirone2/LoRa-GateLink/releases), one per firmware version |
| Open bugs, investigations, bench and field tasks | [TODO.md](../TODO.md) |
| Desired features and ideas | [ROADMAP.md](../ROADMAP.md) |
| Work in progress | A branch and pull request per change |

When an item is done, remove it from TODO.md / ROADMAP.md and describe it in the pull request; the release notes
are built from the pull requests since the last tag.

## Making a change

1. Branch from `main`.
2. Make the change. Things that must change together:
   - Console protocol (`console.cpp`) ↔ the web console (`web/js/`, and the fake board in `tests/web/`) ↔
     `tools/gatelink_client/` and `tests/e2e/` ↔ [docs/console.md](console.md).
   - Log events (`log.h`/`log.cpp`) ↔ the suite ↔ docs/console.md.
   - STATUS/DIAG wire format (`roles.h`) ↔ `role_house.cpp` / `console.cpp`; both boards need the new firmware.
   - Pins or role behaviour (`pins.h`) ↔ the `WIRING` table in `web/js/wiring.js` ↔ [docs/hardware.md](hardware.md).
   - A new setting: field in `Config` + default + `PARAMS` row with a new, never reused id + group/help text
     in `web/js/settings.js`. Saved config is stored by param id, so a changed meaning or unit needs a new id; bump
     `CFG_VERSION` (it only guards the program-flash fallback) if the struct layout changes.
3. Bump `FW_VERSION` in `firmware/GateLink/config.h` for any firmware change (see Versioning).
4. Verify:
   ```sh
   arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all firmware/GateLink   # no warnings in GateLink/
   npm ci && npm test         # web console: lint, unit tests, browser tests (tests/web/README.md)
   ruff check tests tools     # pip install ruff==0.16.10 (the version CI pins); rules in ruff.toml
   python tools/check_contract.py   # firmware enums, log events and console commands vs the suite and docs
   make -C tests/native       # host unit tests for link.cpp and config.cpp; see tests/native/README.md
   pytest tests/e2e -v        # on the bench; see tests/e2e/README.md
   ```
5. Open a pull request with a summary and the bench results (suite pass count, anything new it found). CI
   compiles the firmware and GateSim (failing on warnings in project files), runs the host unit tests, lints and tests the web console (ESLint, `node --test`, Playwright),
   byte-compiles and lints the Python (`ruff check tests tools`) and collects the e2e suite (`pytest
   --collect-only`, which catches import and fixture errors without the bench).
6. Merge to `main`. A change under `web/` deploys the web console to GitHub Pages.

## Versioning

`FW_VERSION` is `MAJOR.MINOR.PATCH`, reported by `info`, `status` and the remote diagnostics.

- **PATCH:** fixes and small additions that keep the config layout and the radio wire format.
- **MINOR:** a new feature or input, a `CFG_VERSION` bump, or a change that needs both boards updated together
  (STATUS/DIAG layout, frame format).
- **MAJOR:** reserved for a protocol break with no upgrade path, or 1.0 at the install.

Saved config survives uploads (from 0.5.0) and is matched by param id, so an added setting takes its default
and a removed one is ignored (log event `cfg`, b; from 0.13.3 an older firmware's saves keep it in the record,
so it's back after the next upgrade). Say so in the release notes when that happens, and when a
config exported from the old version may not import cleanly.

## Releasing

After the pull request carrying a new `FW_VERSION` is merged (`/release` in Claude Code walks through this):

1. Tag the merge commit `vX.Y.Z` and publish a GitHub release with notes:
   ```sh
   gh release create vX.Y.Z --target main --title "vX.Y.Z — <one-line summary>" --notes-file notes.md
   ```
2. The `release` workflow builds the firmware at the tag and attaches `GateLink-vX.Y.Z.bin` and
   `GateLink-vX.Y.Z.bin.sha256` (from v0.13.0). It never replaces a binary already attached unless run by hand with
   `replace` ticked. When it succeeds the `pages` workflow checks the `.bin` against that `.sha256` and redeploys the
   web console with it and a `firmware/latest.json` (version, file, sha256, size) bundled in (the page can't fetch release assets from github.com: no CORS), so **Tools →
   Firmware update** offers it. They are never committed (`web/firmware/` is ignored).

The web console flashes over Web Serial by speaking the Arduino SAM-BA bootloader protocol itself (`SamBa` in
`web/js/samba.js`, driven by `web/js/firmware.js`): a 1200-baud open/close resets the board into the bootloader (USB PID 0x0059, a separate port
grant), then erase from 0x2000 (`X`), 4 KB chunks staged in RAM and written (`S` + `Y`), CRC-16 check (`Z`)
and a reset. It recognises GateLink images by the `GATELINK_FW=x.y.z` marker (`FW_MARKER`, `config.cpp`; the
version reported by `info`/`status` is read from it so the linker keeps it).

Release notes template:

```markdown
**Upgrade:** <flash both boards | gate only | ...>. <Config layout changed (CFG_VERSION n): re-apply config.>

## Firmware
- ...

## Web console
- ...

## Tests and tools
- ...

## Bench results
- Full suite: N passed. <Anything notable.>

PRs: #n
```

## Claude Code skills

Project skills live in `.claude/skills/`:

- `/flash` — flash the bench boards (and optionally the GateSim) and restore their config and key.
- `/release` — tag and publish a firmware release with notes from the merged pull requests.
