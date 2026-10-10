---
name: doc-audit
schedule: 0 2 * * *
description: Audit the docs, CLAUDE.md, skills and routines against the code; one pull request with the fixes
---

# Nightly doc audit

1. Skip if an open pull request titled `[routine] Docs` exists: report it and stop.
2. Run `/doc-audit`. Cover the whole of step 1 (the mechanical checks) every night; for step 2, take the area
   for tonight from the day of the month (`date +%d`), in this order, wrapping round:
   1. `CLAUDE.md` firmware architecture
   2. `CLAUDE.md` invariants and web console
   3. `docs/console.md`
   4. `docs/hardware.md` and `web/js/wiring.js`
   5. `docs/protocol.md`
   6. `docs/development.md`, `docs/release-criteria.md`
   7. `README.md` and each `tests/*/README.md` and `tools/*/README.md`
   8. `.claude/skills/` and `.claude/routines/`
3. Open the pull request as `/doc-audit` says, titled `[routine] Docs: <area>`, on branch
   `claude/routine-doc-audit-<yyyymmdd>`.
