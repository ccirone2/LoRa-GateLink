---
name: doc-audit
description: Audit GateLink's documentation and agent instructions against the code - CLAUDE.md, README.md, docs/, the skills and routines - fix what's stale or wrong on a branch, and open a small pull request. Use when asked to check or update the docs, after a large change, or from the nightly doc-audit routine.
---

# Audit the docs against the code

The code is the truth; a doc that disagrees is fixed, never the other way round (a doc describing behaviour the
code lacks is a finding to report, not a reason to change firmware here). Work on a branch from `main`.

1. **Mechanical checks first.** Each prints what's wrong:
   ```sh
   python tools/check_contract.py          # enums, log events, console commands: firmware vs suite vs docs/console.md
   python tools/agent/hooks.py paths CLAUDE.md README.md docs/*.md .claude/skills/*/SKILL.md .claude/routines/*.md
   python -m pytest tests/tools -q         # includes the agent tooling checks (skills, routines, hooks, links)
   ```
2. **Read against the code**, one area per pass, checking every concrete claim (a name, number, default, pin,
   timing, file, command or option) with grep or by reading the code it describes:
   - `CLAUDE.md`: every function, variable, status field, log event and file it names still exists and does what
     it says; the behavioural invariants match the code (a mismatch there is a bug report for the user, never a
     doc edit to fit the code).
   - `docs/console.md` (commands, status fields, log events) against `console.cpp`, `log.cpp`, `roles.h`.
   - `docs/hardware.md` and the web `WIRING` table (`web/js/wiring.js`) against `pins.h` and the roles.
   - `docs/protocol.md` against `link.cpp` and `roles.h` (frame layout, timings, retries).
   - Parameter defaults and ranges anywhere in the docs against `PARAMS[]` in `config.cpp`.
   - `docs/development.md` versions against `.github/workflows/ci.yml`, `release.yml` and `tools/agent/cloud_setup.sh`.
   - `README.md` indexes every doc, and each `tests/*/README.md` matches its suite's options.
   - Skills (`.claude/skills/*/SKILL.md`) and routines (`.claude/routines/*.md`): commands and options they use
     still exist (`--help`), and nothing describes a retired workflow. Retire stale ones.
3. **Fix** what's wrong in the plainest words that are true. Keep `CLAUDE.md` short: detail belongs in `docs/`.
   No "Shelly" or "Alarm.com" under `web/`. Anything you can't settle from the code (intent unclear, behaviour that
   looks like a bug) goes to `TODO.md` or the pull request body as a question, not into the docs as a guess.
4. **Verify:** the step 1 commands pass, and `node --check` / `ruff check tests tools` if you touched code.
5. **Open one pull request** titled `Docs: ...`, listing each correction with the code that proves it
   (file:line). Nothing changed: no pull request; say what was checked.
