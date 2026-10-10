# Release criteria

What a firmware version must pass on the bench before it is released, and how that is recorded. Each release has
an evidence file, `docs/releases/vX.Y.Z.md`, written by `tools/release_evidence.py` from the bench runs' results
and merged before the tag. The `release` workflow checks it before building the binary, so a version can't be
released without it. The procedure is in [development.md](development.md#releasing) and the `/release` skill.

## Release kinds

The kind comes from the version change since the previous release tag ([Versioning](development.md#versioning)):

| Kind | Version change | Typical change |
|---|---|---|
| PATCH | 0.13.5 → 0.13.6 | A fix that keeps the config layout and the radio wire format |
| MINOR | 0.13.5 → 0.14.0 | A new feature or input, a `CFG_VERSION` bump, a change both boards need together |
| MAJOR | 0.x → 1.0.0 | 1.0 at the install, or a protocol break with no upgrade path |

Each kind needs everything the smaller kinds need.

## Criteria

| Criterion | Required for | How to meet it |
|---|---|---|
| `ci` | every release | CI (`ci.yml`, all jobs) passed on the release commit |
| `full_suite` | every release | `GATELINK_KEY=<key> pytest tests/e2e -v`: the default selection with the key set, so `test_wrong_key_rejected` runs, at the boards' saved `tx_power` (5 dBm or less) |
| `soak` | every release | `pytest tests/e2e -m soak --cycles 20` (20 cycles or more) |
| `power_lipo_out` | MINOR, MAJOR | `pytest tests/e2e -m power` with both LiPos unplugged |
| `power_lipo_in` | MINOR, MAJOR | `pytest tests/e2e -m power` with both LiPos plugged in |
| `longsoak_60m` | MINOR, MAJOR | `pytest tests/e2e -m longsoak --soak-minutes 60` (60 minutes or more) |
| `full_suite_17dbm` | MINOR, MAJOR | `GATELINK_KEY=<key> pytest tests/e2e -v --tx-power 17`: the full suite at full power on the install-like supplies |
| `longsoak_24h` | MAJOR | `pytest tests/e2e -m longsoak --soak-minutes 1440` (24 hours or more) |
| `install_rf` | MAJOR | By hand: TODO.md "Real RF" done at the install site (`-m rf` there, ping and RSSI from the web console) |
| `install_full_power` | MAJOR | By hand: TODO.md "Full power at the install" done (`tx_power` 17 saved on both boards for days, no resets or `radio_faults`) |

The power runs find out by themselves whether each LiPo is in (summary fact "LiPos") and run the tests for that
state; a run with one LiPo in and one out counts for neither. The 17 dBm run doesn't stand in for `full_suite`,
nor the reverse. A 24-hour long soak also meets `longsoak_60m`.

Every run needs the bench set up as in [tests/e2e/README.md](../tests/e2e/README.md) (`GATELINK_HA_URL`, the
web console disconnected); `GATELINK_KEY` is the key in `~/.gatelink_key`.

## Bench hardware matrix

| Part | Release bench | Notes |
|---|---|---|
| House board | MKR WAN 1310 …183013 | USB serial number tail; `python tools/gatelink.py ports` shows it |
| Gate board | MKR WAN 1310 …0C301C | Never …191117: weak receiver, CRC errors and lost pongs ([bench-testing.md](bench-testing.md#new-board-preflight)) |
| Opener | GateSim Uno, running the current `tools/GateSim` | Wired to the gate board; carries the power rig (CH1 gate 24 V feed, CH3 house 12 V rail) |
| Controller | The real Shelly Wave 1, driven through Home Assistant | SW input in follow mode (preflight checks it) |
| Supplies | Install-like: gate 24 V → 5 V buck into VIN, house 12 V → 5 V buck into VIN, shared with the Shelly and the IN2 opto | USB only through power-blocked cables, for the console |
| LiPos | One per board, plugged in or out by hand | Only the `-m power` runs care; the other runs may have them either way |
| UART taps | FTDI adapters on both boards' Serial1 | Optional, but the power runs need them to see boots while USB is down |

The tool checks the boards: a run counts only if the house and gate boards are the ones above (by USB serial
number, recorded in the run's `summary.json`). When a bench board is replaced, change this table and
`BENCH_BOARDS` in `tools/release_evidence.py` in the same pull request (a unit test checks they agree), after
the new board passes the [new board preflight](bench-testing.md#new-board-preflight). The rest of the matrix is
checked by the suite's preflight (simulator wiring, controller follow mode, bench-safe settings) or not at all
(the supplies).

## Which runs count

Every bench run writes `tests/e2e/results/<run>/summary.json` next to `summary.md`. `collect` looks at the runs
with both boards on the release version, and a run counts only if:

- both boards report the release version, and they are the bench boards above;
- it ran from a checkout whose `firmware/GateLink` tree is the release commit's, with no local changes under
  `firmware/`, `tests/e2e/`, `tools/gatelink_client/` or `tools/GateSim/` (other untracked or changed files are
  listed but don't matter). The boards report only `FW_VERSION`, so flash them from that same checkout (`/flash`);
- pytest wasn't interrupted, and every test file collected.

A run meets a criterion only if it ran every test of that criterion's group (the default selection, or one opt-in
marker): run on the whole `tests/e2e`, nothing left out by `-k`, `--deselect`, `--ignore`, `--lf` or `--sw`, and
every selected test reported. `-m "soak or power"` counts for both groups.

## What counts as a pass

- No test of the group failed or errored. The behavioural invariants are checked after every test, so a broken
  invariant is a failure.
- No test of the group was skipped, except in `-m power` the tests for the other LiPo state. A skip for
  `GATELINK_KEY` or a failed preflight fails the criterion.
- `xfail` tests (known open bugs) may fail; they are listed.
- For each criterion the **latest** run that matches it decides. Earlier attempts that failed are named in the
  evidence, so a run repeated until it passes is visible.
- Anomalies in the summary (USB lines cut, `lbt_forced`, ...) don't fail a run; they are listed for the reviewer.

## Rerunning a flaky scenario

If the deciding run failed exactly one scenario, that scenario may be rerun once, on its own and under the same
conditions (same `--tx-power`; for `-m power`, the same LiPo state; for a soak, `--cycles` or `--soak-minutes` that
still meet the criterion):

```sh
pytest tests/e2e -k test_jammed_gate             # default group
pytest tests/e2e -m power -k test_gate_power_bounce   # an opt-in group: give its marker too
```

The first later run that includes the scenario is its rerun; if it passes, the criterion is met, and the evidence
file records the failure, its reason and the rerun. A rerun that fails too can't be retried: fix the cause or make
a new full run. A rerun counts only once **TODO.md names the scenario** as flaky (what failed, on which version), so
the flake gets looked at. Two or more failed scenarios mean a new run.

## The evidence file

`python tools/release_evidence.py collect vX.Y.Z` (run on the release commit, normally `main` once the version's
pull request is merged) writes `docs/releases/vX.Y.Z.md`:

- the release kind and the previous release, the commit and its firmware tree;
- the criteria table: each criterion, the kinds that need it, whether it's met, and the evidence (run names,
  pass/skip/fail counts, reruns, the CI run);
- one section per counting run: when, the command and options, commit, boards, counts, failures and skips,
  facts (link quality, LiPos, ...) and anomalies; and a list of the runs on that version that don't count, with why;
- **Install notes**, hand-written and kept when the file is collected again. For a MAJOR release, tick
  `- [x] install_rf: ...` and `- [x] install_full_power: ...` there with what was done, when and the numbers;
- a JSON block in an HTML comment with the same data, which `check` reads.

`collect` exits 1 if a required criterion isn't met, after writing the file; `--dry-run` only prints the verdict.
`python tools/release_evidence.py check vX.Y.Z` (the workflow adds `--ci`) passes only if the file exists, is for
`FW_VERSION`, was collected as at least the right kind, tested the firmware at `HEAD` (same `firmware/GateLink`
tree), and records every required criterion as met (the install checks from the ticked notes). It needs `gh`
for the CI status.
