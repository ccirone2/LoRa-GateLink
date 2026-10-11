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
     in `web/js/settings.js`, then `python tools/docgen.py --write` ([config.md](config.md)). Saved config is
     stored by param id, so a changed meaning or unit needs a new id; bump `CFG_VERSION` (it only guards the
     program-flash fallback) if the struct layout changes.
3. Bump `FW_VERSION` in `firmware/GateLink/config.h` for any firmware change (see Versioning).
4. Verify:
   ```sh
   arduino-cli compile --fqbn arduino:samd:mkrwan1310 --warnings all firmware/GateLink   # no warnings in GateLink/
   npm ci && npm test         # web console: lint, unit tests, browser tests (tests/web/README.md)
   ruff check tests tools     # pip install ruff==0.16.10 (the version CI pins); rules in ruff.toml
   python tools/check_contract.py   # firmware enums, log events and console commands vs the suite and docs
   python tools/docgen.py     # reference docs vs the source (--write regenerates the generated tables)
   make -C tests/native       # host tests: link/config, and both boards' firmware in a simulated site (tests/native/README.md)
   make -C tests/native cppcheck tidy coverage   # static analysis and coverage (in Docker on Windows, as the README says)
   python -m pytest tests/tools -q   # unit tests for the tools (release_evidence.py, docgen.py, the agent hooks, key backups)
   pytest tests/e2e -v        # on the bench; see tests/e2e/README.md
   ```
5. Open a pull request with a summary and the bench results (suite pass count, anything new it found). CI
   compiles the firmware and GateSim (failing on warnings in project files) and checks the firmware's size budget
   (`tools/fw_size.py`: 160 KB flash, 24 KB static RAM; raising it is a decision for the pull request), runs the host
   unit tests, lints and tests the web console (ESLint, `node --test`, Playwright), byte-compiles and lints the
   Python (`ruff check tests tools`), runs the tools' unit tests (`tests/tools`), collects the e2e suite (`pytest
   --collect-only`, which catches import and fixture errors without the bench), checks the docs against the source
   (`check_contract.py`, `docgen.py`), fails a pull request that changes the firmware without bumping `FW_VERSION`,
   lints the workflows (actionlint, with shellcheck), runs cppcheck and clang-tidy on the firmware and reports
   the host tests' coverage of it (failing below 85 % of lines; tests/native/README.md).
6. Merge to `main`. Once CI has passed on `main`, the web console is deployed to GitHub Pages.

CI's actions are pinned by commit SHA and its runners by image (`ubuntu-24.04`); Dependabot
(`.github/dependabot.yml`) proposes action, npm and Python updates weekly, and the nightly deps-check routine
covers the Arduino cores and libraries, arduino-cli and ruff.

## Reference docs from source

`tools/docgen.py` keeps the reference docs provably in step with the code (stdlib Python; it reads the web
console's tables by importing `web/js/settings.js`, `wiring.js` and `logdecode.js` in Node.js):

```sh
python tools/docgen.py           # check, as CI does: exit 1 listing every drift and how to fix it
python tools/docgen.py --write   # regenerate the generated sections in place, then check the rest
```

- **Generated** (between `<!-- docgen:NAME begin -->` and `<!-- docgen:NAME end -->`; edit the source, never the
  section): [config.md](config.md), every setting with its id, range, default, when it applies and its help text
  (`PARAMS`, `configDefaults()`, `paramValid()` in `config.cpp`; `GROUPS`, `HELP`, `MUST_MATCH`, `SELECTS` in
  `web/js/settings.js`; a setting missing from `GROUPS` or `HELP` fails, as the Config tab would hide it); and the
  message types, ACK results, STATUS and DIAG layouts in [protocol.md](protocol.md) "Wire formats" (`link.h`,
  `roles.h`). Descriptions `roles.h` lacks are in `ST_MEANING`/`STI_MEANING` in `docgen.py`.
- **Checked** (hand-written, compared with the source): the status fields `appFillStatus`, `houseStatus` and
  `gateStatus` write and the console events' fields, against [console.md](console.md), with the names an
  enumerated field can take; the link history fields (`FIELDS`); each command's arguments (`req["..."]`), the
  keys its reply carries and whether it is house or gate only, and the commands held while a relay pulses
  (`blocksLoop()`, also in CLAUDE.md); log events in `log.h` order, `log.cpp` and `logdecode.js` (and against
  console.md in `check_contract.py`); pins (`pins.h`) against every doc, the bench wiring record and
  `WIRING`/`IO_LABELS`; the frame layout; the radio settings that must match; the e2e profile's "firmware
  defaults". Every backticked snake_case name in the docs (`ctrl_confirm_ms`, `link_up`,
  `test_power_loss_at_rest`) must still be a setting, field, argument, log event, e2e test or release criterion,
  so a rename can't live on in the prose (`OTHER_NAMES` in `docgen.py` takes any other kind).
- **Facts:** `FACTS` in `docgen.py` lists the constants and setting defaults the docs quote (`INTERLOCK_MS`,
  `BOOT_SETTLE_MS`, the watchdog, the radio defaults, toolchain versions, ...). Each reads its value from the
  source with one regex and finds every quote of it with another, converting units (`s` turns ms into s). Two
  kinds of quote are checked without an entry: a setting's default ("`setting` (default X)", "`setting`, default
  on", "`setting` is X by default", "`setting` defaults to X", "the default `setting` of X") against
  `configDefaults()`, and a time constant next to its name ("`INTERLOCK_MS` (100 ms)", "100 ms (`INTERLOCK_MS`)").
  When a doc quotes a new value another way, add a fact next to the similar ones:

  ```python
  D("TXQ_LEN", f"{FW}/link.cpp", Q(PRO, r"queued (\w+) deep"), Q(C, r"wait in a (\d+)-deep queue")),
  ```

  A space in a doc pattern matches any whitespace, line breaks included; a quote's first group is the value
  (digits, `0x..`, or a word such as "four"). A reworded doc fails with "no quote of ...": fix the pattern.

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

1. Run the bench on the merged version as [release-criteria.md](release-criteria.md) asks for its kind (PATCH:
   the full suite and a soak; MINOR adds the power runs with LiPos out and in, a 60-minute long soak and a full
   suite at 17 dBm; MAJOR adds a 24-hour soak and the install checks).
2. `python tools/release_evidence.py collect vX.Y.Z` writes the evidence, `docs/releases/vX.Y.Z.md`, from the
   runs' `summary.json` (exit 1 while a criterion is unmet). Merge it in a pull request with the bench results in
   its body.
3. Once CI has passed on that merge commit, `python tools/release_evidence.py check vX.Y.Z --ci`, then tag the
   merge commit `vX.Y.Z` and publish a GitHub release with notes:
   ```sh
   gh release create vX.Y.Z --target <merge-commit-sha> --title "vX.Y.Z — <one-line summary>" --notes-file notes.md
   ```
4. The `release` workflow checks the evidence (`check --ci`; tags from before the criteria are built without it),
   builds the firmware at the tag and attaches `GateLink-vX.Y.Z.bin`, `GateLink-vX.Y.Z.bin.sha256` (from v0.13.0)
   and the evidence as `GateLink-vX.Y.Z-evidence.md`. It never replaces a binary already attached unless run by hand
   with `replace` ticked. When it succeeds the `pages` workflow checks the `.bin` against that `.sha256` and redeploys the
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
<KIND> release, all criteria met: [evidence](https://github.com/ccirone2/LoRa-GateLink/blob/vX.Y.Z/docs/releases/vX.Y.Z.md).
- Full suite: N passed. Soak: 20 cycles, <latencies>. <Power, long soak, 17 dBm runs for MINOR.> <Reruns.>
  <Anything notable.>

PRs: #n
```

## Claude Code skills

Project skills live in `.claude/skills/`:

- `/flash` — flash the bench boards (and optionally the GateSim) and restore their config and key.
- `/release` — run the bench per the release criteria, collect and merge the evidence, then tag and publish the
  release with notes from the merged pull requests.
