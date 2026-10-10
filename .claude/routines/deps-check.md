---
name: deps-check
schedule: 0 4 * * *
description: Compare every pinned dependency with what's released; bump development tooling, record firmware library updates in TODO.md
---

# Nightly dependency check

The pins: `docs/development.md` (cores and libraries, also in `.github/workflows/ci.yml`, `release.yml` and
`tools/agent/cloud_setup.sh`), `package.json` (web tooling), the ruff version in `ci.yml`, the actions in
`.github/workflows/`, and arduino-cli in `tools/agent/cloud_setup.sh`.

1. **Find what's newer**: `arduino-cli core search arduino:samd` / `arduino:avr`, `arduino-cli lib search <name>`
   for each pinned library, `npm outdated`, `npm audit`, `pip index versions ruff`, and each action's latest
   release (`gh api repos/<owner>/<action>/releases/latest`). Read the changelog of anything newer.
2. **Security first:** an advisory against something we use (npm audit, a library's release notes, GitHub
   advisories) is tonight's subject, whatever else is pending.
3. **Development tooling** (npm devDependencies, ruff, actions, arduino-cli for cloud sessions): bump one group
   per pull request when its tests still pass (`npm ci && npm test` for npm; `ruff check tests tools` and fixing
   new findings for ruff; the workflow running for actions, so say "CI is the test" in the body). Keep exact pins;
   versions at least two weeks old; update `docs/development.md` and the arduino-cli SHA-256 with them.
4. **Firmware cores and libraries are never bumped by a routine**: they change the binary on the gate and need
   the bench and a release. Add or update one `TODO.md` item per available update (pinned and available versions,
   what the changelog says matters to us, e.g. SAMD USB or SX127x fixes), in a pull request titled
   `[routine] Deps: ...`, only when something changed since the item was last written.
5. Branch `claude/routine-deps-check-<yyyymmdd>`. Nothing newer: no pull request.
