# Agent tooling

GateLink is developed with Claude Code, and the repo carries what an agent needs to follow the project's rules
without being reminded: [CLAUDE.md](../CLAUDE.md) (architecture and the behavioural invariants), skills for the
recurring workflows, hooks that check work as it happens, and nightly routines. They are code: they change in the
same pull request as what they describe, and [tests/tools/test_agent_tooling.py](../tests/tools/test_agent_tooling.py)
tests them in CI.

## Skills

`.claude/skills/<name>/SKILL.md`, run as `/<name>`:

| Skill | For |
|---|---|
| `/flash` | Upload firmware to the bench boards; check role, config and key survived, restore them if not |
| `/bench` | Pick, run and report the end-to-end bench suite (`tests/e2e`) for a change |
| `/release` | Bench evidence per [release-criteria.md](release-criteria.md), then tag and publish with notes |
| `/doc-audit` | Check the docs, CLAUDE.md, skills and routines against the code; fix in a pull request |
| `/pr-followup` | Open pull requests: CI, review comments, rebases, and merging only what's cleared |

A skill's front matter `name` is its directory name, and its `description` says when to use it. The tests check
that, and that every repo path a skill names exists, so a renamed tool or doc fails CI until its skills follow.

## Hooks

`.claude/settings.json` runs `tools/agent/hooks.py` through `tools/agent/hook.sh` (which finds a Python 3: `python3`
on Linux, `python` on Windows, where Claude Code runs hooks in Git Bash). A hook never breaks a session: an error
of its own is reported and ignored, and without Python the checks are skipped.

| Event | Does |
|---|---|
| SessionStart | Notes what the branch already lacked (the stop checklist won't ask about it in this session). In a cloud session (`CLAUDE_CODE_REMOTE=true`), starts `tools/agent/cloud_setup.sh` in the background and tells Claude how to wait for it |
| PostToolUse (Edit, Write, NotebookEdit) | Fast checks for the file just written; a failure goes back to Claude (exit 2) |
| Stop | The change checklist below; asks once per item, then lets Claude finish |

**After each edit**, by file:

| File | Check |
|---|---|
| `web/**/*.js`, `tests/web/**/*.js`, `eslint.config.js` | `node --check`, then ESLint on that file (when `npm ci` has been run) |
| `*.py` | Compiles; `ruff check` under `tools/` and `tests/` (when ruff is installed) |
| `*.json` | Parses |
| `.github/workflows/*.yml` | Parses (when PyYAML is installed) |
| `*.md` | Relative links resolve; `tools/docgen.py` still agrees (the docs against the source) |
| `.claude/settings.json` | Parses; every hook's script exists |
| `.claude/skills/*/SKILL.md`, `.claude/routines/*.md` | Front matter; skills used and paths named exist |
| The console contract (`console.cpp`, `log.cpp`, `log.h`, `roles.h`, `docs/console.md`, `tools/gatelink_client/`, `tests/e2e/gatelink/`) | `tools/check_contract.py` |

Firmware edits get no per-edit check: a compile takes too long for every keystroke; the checklist and CI cover it.

**Before finishing**, for everything the branch changed since it left `main` (committed or not, and untracked
files), the stop hook asks about what usually changes together. These are reminders: Claude does what applies and
says why the rest doesn't, and the hook doesn't ask the same item again in that session.

| Item | Asked when | Settled by a change to |
|---|---|---|
| `fw-version` | anything under `firmware/GateLink/` | `FW_VERSION` in `config.h` |
| `native-tests` | `link.cpp`/`.h`, `config.cpp`/`.h`, `extflash.cpp` | anything under `tests/native/` |
| `console-docs` | `console.cpp`, `log.cpp`, `log.h`, `roles.h` | `docs/console.md` |
| `contract` | a console contract file | `tools/check_contract.py` passing |
| `docgen` | a doc, or a source `tools/docgen.py` reads (firmware, `web/js/`, `tests/e2e/`, ...) | `tools/docgen.py` passing |
| `wiring` | `pins.h` | `web/js/wiring.js` and `docs/hardware.md` |
| `params` | a `PARAMS[]` row in `config.cpp` | `web/js/settings.js` |
| `web-tests` | `web/` scripts, pages or styles | anything under `tests/web/` |
| `hook-tests` | `tools/agent/`, `.claude/settings.json` | `tests/tools/test_agent_tooling.py` |
| `agent-docs` | a tool or test script a skill or routine names | that skill or routine |
| `trackers` | any code | `TODO.md` or `ROADMAP.md` (or "neither applies") |

`python tools/agent/hooks.py check` prints the same list for the current branch, without asking-once state. CI runs
it as `check --only fw-version` on every pull request, so that one item is enforced, not just asked.

**Turning them off** for a session or machine: `"disableAllHooks": true` in `.claude/settings.local.json` (not
committed). `/hooks` lists what's loaded; `claude --debug` logs each run.

## Cloud sessions

Claude Code on the web clones the repo into a fresh Linux VM. The SessionStart hook installs the toolchain there
in the background (`tools/agent/cloud_setup.sh`: arduino-cli by checksum, the cores and libraries CI pins, `npm ci`,
ruff and pytest in a venv), logging to `~/.cache/gatelink/setup.log` and writing `~/.cache/gatelink/setup.done`
with what succeeded. The environment's network access must reach `downloads.arduino.cc` and `github.com` (the
default "Trusted" list covers npm and PyPI only; add them, or use full access). To have the toolchain cached
instead of installed on every start, set the cloud environment's setup script to `bash tools/agent/cloud_setup.sh`;
the hook then finds it done. The bench can't be reached from the cloud: `/bench` and `/flash` are local only.

The pinned versions in `cloud_setup.sh` must match `ci.yml` and [development.md](development.md); a test checks.

## Routines

Nightly cloud sessions, each defined by a file in [.claude/routines/](../.claude/routines/README.md) (which also
lists their shared rules): doc audit, bug sweep, dependency check, tracker tidy and pull request follow-up. They
open at most one small pull request each, titled `[routine] ...`, never merge or release, and leave firmware
changes as drafts that need the bench. The routines themselves are created on claude.ai with `/schedule`, each
holding a two-line prompt that points at its file, so editing the file changes the routine.

## Changing the tooling

- A new hook check: add it to `post_edit_checks` or `stop_findings` in `tools/agent/hooks.py`, a test in
  `tests/tools/test_agent_tooling.py` (the `repo` fixture builds a throwaway repo to judge), and its row above.
- A new skill or routine: its file, its row here (and in the routines index), and nothing else: the tests pick it
  up.
- Retiring one: delete its file and rows in the same pull request; for a routine, delete it on claude.ai too.
