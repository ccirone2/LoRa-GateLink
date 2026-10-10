# Routines

Scheduled Claude Code cloud sessions that keep GateLink tidy between working sessions. Each one is a file here;
the routine itself (on claude.ai, created with `/schedule` from the CLI) holds only a short prompt that points at
its file, so the instructions are versioned, reviewed and tested with the code
(`tests/tools/test_agent_tooling.py` checks every file's front matter, skills and paths).

| Routine | Schedule | Does |
|---|---|---|
| [doc-audit](doc-audit.md) | nightly 02:00 | Docs, CLAUDE.md, skills and routines against the code (`/doc-audit`) |
| [bug-sweep](bug-sweep.md) | nightly 03:00 | One area a night: bugs and simplifications, each proven by a test |
| [deps-check](deps-check.md) | nightly 04:00 | Pinned toolchain, libraries and tooling against what's released |
| [tracker-tidy](tracker-tidy.md) | nightly 05:00 | TODO.md and ROADMAP.md against what's merged |
| [pr-followup](pr-followup.md) | nightly 06:00 | Rebase and fix CI on routine pull requests (`/pr-followup`) |

Times are the account's time zone; the `schedule` field in each file is the cron expression the routine was
created with.

## The prompt each routine holds

```
You are GateLink's nightly <name> routine. Read .claude/routines/README.md and .claude/routines/<name>.md in
this repository and follow them exactly.
```

Changing a file here changes the routine from its next run; only a schedule change needs `/schedule update`.
Retiring one: delete its file and its row above in the same pull request, then delete the routine on claude.ai.

## Rules for every routine

- **Small and proven.** At most one pull request per run, on one subject. Every code change comes with a test that
  fails without it (`make -C tests/native`, `npm test`, `python -m pytest tests/tools -q`); every doc change cites
  the code that proves it. Verify as `docs/development.md` says for the areas touched, as far as the cloud allows.
- **Never merge, release, tag, or push to `main`.** Branch `claude/routine-<name>-<yyyymmdd>`; the pull request
  title starts `[routine]` and its body says what was checked, what changed and how it was verified. The user
  (or a working session they clear) merges.
- **Firmware changes** bump `FW_VERSION`, open as a draft, and say "needs bench" in the body: a cloud session
  can't run the bench (`/bench`).
- **Nothing to do is a fine result.** Then open no pull request; end with a one-paragraph summary of what was
  checked.
- **Don't duplicate.** Before starting, list open pull requests (`gh api repos/{owner}/{repo}/pulls`; `gh pr`
  isn't available in cloud sessions) and skip a subject one of them already covers.
- **Cloud setup.** The SessionStart hook installs the toolchain in the background; wait for it before compiling
  or testing (its message says how). If a step failed (no network to `downloads.arduino.cc`), say which checks
  couldn't run rather than skipping them silently.
- **Secrets and naming.** Never print or commit keys or tokens. No "Shelly" or "Alarm.com" under `web/`.
- Findings that need a person (a design decision, a bench test, a suspected bug without a host test) go in
  `TODO.md` in the pull request, or in the summary if there is no pull request.
