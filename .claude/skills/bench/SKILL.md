---
name: bench
description: Run the GateLink end-to-end bench suite (tests/e2e) on the real boards, GateSim and controller, pick the runs a change needs, watch them in the background, and report the results from summary.json. Use to bench-verify a firmware, console-contract or suite change before its pull request, for the release runs (/release), or when asked to run the bench, a soak or the power tests.
---

# Run the bench

The suite drives both boards over USB, the GateSim Uno and the controller through Home Assistant, and checks
the behavioural invariants after every scenario (`tests/e2e/README.md`). It needs the bench PC: it can't run in a
cloud session. Bench facts: the boards are found by role (usually COM5/COM21), the GateSim is on COM10, the key is
in `~/.gatelink_key`, the HA token in `~/.ha_token` (never print, copy or commit either), and the MKR board whose
serial ends `191117` is never used.

1. **Free the ports and check the boards.** The web console or another pytest holding a port fails everything; ask
   the user to click Disconnect if so.
   ```sh
   python tools/gatelink.py ports
   ```
   Both boards must show their role, the firmware under test, `key set`, `cfg spi`, `tx_power` 5 (USB power) and a
   verified link. Wrong firmware: `/flash` first. A board left on the test profile by an interrupted run
   (`heartbeat_s` 5): `python tools/gatelink.py <role> reboot`. If `tools/GateSim` changed, re-upload the Uno
   (`/flash` step 5).
2. **Pick the runs.** Every firmware change: the full suite. Then by what changed:

   | Change | Also run |
   |---|---|
   | link, radio, LBT, retries | `-m soak --cycles 20` |
   | key handling, `link.cpp` sessions | the full run with `GATELINK_KEY` set (adds the wrong-key test) |
   | supply, boot, config saves, power senses | `-m power` (ask the user whether the LiPos are in; one run per state they agree to) |
   | radio parameters, TX power | `-m rf --rf-cycles 5`, only with the attenuator in line or antennas off (ask) |
   | soak-sensitive timing, history | `-m longsoak --soak-minutes 60` |
   | only some suite files | those files (`pytest tests/e2e/test_<area>.py`); such a run doesn't count for a release |

   Release runs: `/release` and `docs/release-criteria.md` decide, with no `-k`, `--lf`, `--sw` or `--ignore`.
   Never `--tx-power` above 5 unless the user confirms the boards are on their install supplies.
3. **Start it in the background**, from the checkout under test, with the HA address from `.env`:
   ```sh
   set -a; . ./.env; set +a
   GATELINK_KEY=$(cat ~/.gatelink_key) python -m pytest tests/e2e -v 2>&1 | tee <scratchpad>/bench.log
   ```
   (`run_in_background`; the full suite takes about 30 min.) Wait for the completion notification instead of
   polling; to follow progress, `tail -5` the log now and then.
4. **Read the results:** the newest `tests/e2e/results/<run>/summary.md` (and `summary.json`: per test outcome,
   duration and reason; firmware, git commit, options). Report pass, skip and fail counts, every failure with its
   reason, and the anomalies the summary lists (status retries, `lbt_forced`, resets, radio faults, latencies far
   from earlier runs). For a failure, read that test's timeline (`results/<run>/<test>.jsonl`) before judging: a
   bench fault (USB port lost, HA unreachable, controller stale) or a firmware or suite bug. One failure in an
   otherwise clean run may be rerun once alone (`-k <name>`); a flaky test gets a TODO.md item naming it. More than
   one failure, or a failed rerun, means finding the cause.
5. **Leave the bench as found.** The suite restores the saved config at the end. `ports` again: both boards on
   `tx_power` 5 with a verified link. If a wrong-key run was interrupted, put the key back:
   `GATELINK_KEY=$(cat ~/.gatelink_key) python -m pytest tests/e2e --restore-key -k boards_and_link`.
6. **Record it** in the pull request body: per run the command, the counts and anything notable, and the results
   directory name. Release evidence is collected from `summary.json` by `tools/release_evidence.py` (`/release`).
