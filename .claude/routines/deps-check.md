---
name: deps-check
schedule: 0 4 * * *
description: Compare the pins Dependabot can't read with what's released; bump development tooling, record firmware library updates in TODO.md
---

# Nightly dependency check

The pins: `docs/development.md` (cores and libraries, also in `.github/workflows/ci.yml`, `release.yml` and
`tools/agent/cloud_setup.sh`), `package.json` (web tooling), the ruff version in `ci.yml`, the actions in
`.github/workflows/`, and arduino-cli in `tools/agent/cloud_setup.sh`.

Dependabot (`.github/dependabot.yml`) opens weekly pull requests for the actions, the npm tooling and the bench
suite's Python requirements; this routine covers what it can't read, and checks on its pull requests.

1. **Find what's newer**: `arduino-cli core search arduino:samd` / `arduino:avr`, `arduino-cli lib search <name>`
   for each pinned library, arduino-cli's latest release (`gh api repos/arduino/arduino-cli/releases/latest`),
   `pip index versions ruff`, and `npm audit`. Read the changelog of anything newer.
2. **Security first:** an advisory against something we use (npm audit, a library's release notes, GitHub
   advisories) is tonight's subject, whatever else is pending.
3. **Development tooling Dependabot doesn't cover** (ruff and gcovr in `ci.yml`, arduino-cli in
   `tools/agent/cloud_setup.sh`): bump one per pull request when its checks still pass (`ruff check tests tools`,
   fixing new findings; for arduino-cli, the new release's SHA-256 from its checksums file). Keep exact pins and
   versions at least two weeks old; update `docs/development.md` with them. An open Dependabot pull request with
   failing CI: say what fails and why in the summary (a fix belongs on its branch, by a working session).
4. **Firmware cores and libraries are never bumped by a routine**: they change the binary on the gate and need
   the bench and a release. Add or update one `TODO.md` item per available update (pinned and available versions,
   what the changelog says matters to us, e.g. SAMD USB or SX127x fixes), in a pull request titled
   `[routine] Deps: ...`, only when something changed since the item was last written.
5. Branch `claude/routine-deps-check-<yyyymmdd>`. Nothing newer: no pull request.
