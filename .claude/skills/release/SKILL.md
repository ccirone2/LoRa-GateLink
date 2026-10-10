---
name: release
description: Release a GateLink firmware version - run the bench per docs/release-criteria.md on the merged version, collect the release evidence with tools/release_evidence.py and merge it, then tag the merge commit, publish notes from the pull requests since the last tag, and check the release workflow attached the binary and evidence. Use after a pull request that bumps FW_VERSION is merged, or when asked to cut, tag or publish a release or update the changelog.
---

# Release a firmware version

The changelog is the list of GitHub releases, one per `FW_VERSION`. A release needs bench evidence first: the
criteria (by release kind) are in `docs/release-criteria.md`, the evidence file `docs/releases/vX.Y.Z.md` is
written by `tools/release_evidence.py collect` and checked by `check`, and the `release` workflow refuses to build
without it. Versioning and the notes template: `docs/development.md` ("Versioning", "Releasing").

1. **Check the state.**
   ```sh
   git fetch origin --tags && git checkout main && git pull --ff-only
   grep FW_VERSION firmware/GateLink/config.h
   gh release list --limit 5
   python tools/release_evidence.py collect vX.Y.Z --dry-run
   ```
   The version in `config.h` must not have a release or tag yet. If it does, the firmware change didn't bump
   `FW_VERSION`: stop and say so rather than retagging. The dry run prints the release kind (from the previous
   tag), whether CI passed on this commit, and which criteria are still open: that is the bench plan. If CI
   didn't pass on `main`, stop: fix it first.
2. **Flash both boards with this `main`** (`/flash`, from this checkout, no local changes under `firmware/`).
   `python tools/gatelink.py ports` must show both boards on X.Y.Z, key set, and the bench boards from the matrix
   in `docs/release-criteria.md` (house `usb ...183013`, gate `usb ...0C301C`; never ...191117). Re-upload the
   GateSim if `tools/GateSim` changed since the last release.
3. **Run the bench** for the release kind, from this checkout, with the web console disconnected
   (`GATELINK_HA_URL` set as in `tests/e2e/README.md`; `GATELINK_KEY` from `~/.gatelink_key`). Every release:
   ```sh
   GATELINK_KEY=$(cat ~/.gatelink_key) pytest tests/e2e -v    # ~30 min
   pytest tests/e2e -m soak --cycles 20
   ```
   MINOR and MAJOR also (ask the user to plug the LiPos in or out between the power runs; the run reports the
   state it found in its "LiPos" fact). Before the 17 dBm run, have the user confirm both boards are fed from the
   install-like supplies, not USB power: full-power TX on USB power crashes a board into watchdog resets, and the
   tool can't tell the supplies apart. Never pass `-k`, `--ignore`, `--lf` or `--sw` to a criterion run (only to a
   rerun): such a run doesn't count.
   ```sh
   pytest tests/e2e -m power                                  # both LiPos out
   pytest tests/e2e -m power                                  # both LiPos in
   pytest tests/e2e -m longsoak --soak-minutes 60
   GATELINK_KEY=$(cat ~/.gatelink_key) pytest tests/e2e -v --tx-power 17
   ```
   MAJOR also `pytest tests/e2e -m longsoak --soak-minutes 1440`, and the install checks (real RF and full power
   at the site, TODO.md), which the user records by hand in the evidence file's install notes.
   Long runs go in the background; check on them rather than waiting. If one scenario fails in an otherwise
   clean run, it may be rerun once on its own with the same options (`pytest tests/e2e -k <name>`, plus `-m
   <marker>` for an opt-in group) after adding a TODO.md item that names it as flaky; more than one failure, or a
   failed rerun, means finding the cause (and likely a new version).
4. **Collect the evidence** on a branch:
   ```sh
   git checkout -b release-vX.Y.Z
   python tools/release_evidence.py collect vX.Y.Z
   ```
   It writes `docs/releases/vX.Y.Z.md` and exits 1 while a required criterion is unmet; go back to step 3 for
   what it lists (runs that don't count say why). Read the file: anomalies, earlier failed attempts, reruns.
5. **Open a pull request** adding `docs/releases/vX.Y.Z.md` (and any TODO.md flake items), with the bench results
   in its body: per run, the command, pass/skip/fail counts and anything notable from the evidence file. CI must
   pass on it. The user merges it.
6. **After it merges,** check the evidence on the merge commit, once CI has passed there:
   ```sh
   git checkout main && git pull --ff-only
   gh run list --workflow ci.yml --commit $(git rev-parse HEAD) --limit 1
   python tools/release_evidence.py check vX.Y.Z --ci
   ```
   The release workflow runs the same check on the tag; publishing before it passes leaves a release without a
   binary.
7. **Gather what changed** since the previous tag:
   ```sh
   git log --oneline <prev-tag>..main
   gh pr list --state merged --search "merged:>=<prev-tag-date>" --json number,title,body
   ```
   Look for: `CFG_VERSION` changes (config layout), `roles.h` STATUS/DIAG or `link.cpp` frame changes (both
   boards must be updated together), GateSim changes (re-upload the Uno), console/log changes (web console and
   e2e suite move with them).
8. **Write the notes** to a file in the scratchpad, using the template in `docs/development.md`. Lead with the
   upgrade line (which boards to flash, config re-apply, GateSim re-upload). Take the bench results from the
   evidence file (kind, runs and counts, reruns, notable measurements) and link it. Plain, specific sentences; no
   marketing.
9. **Show the user the version, target commit (the evidence merge commit, full SHA) and notes, and get a yes**
   before publishing: a release on this public repo is visible to everyone.
10. **Publish** on that exact commit:
    ```sh
    gh release create vX.Y.Z --target <merge-commit-sha> --title "vX.Y.Z — <one-line summary>" --notes-file <notes.md>
    ```
11. **Check the assets.** The `release` workflow checks the evidence, builds the tag and attaches
    `GateLink-vX.Y.Z.bin`, its `.sha256` and `GateLink-vX.Y.Z-evidence.md`: `gh run list --workflow release.yml
    --limit 1`, then `gh release view vX.Y.Z` should list all three. It refuses to replace a binary already attached
    (re-run by hand with `replace` only if one must be rebuilt). If the workflow failed, show its log (`gh run view
    <id> --log-failed`); an evidence failure there names what's missing. Tell the user the release is now public
    with no binary, and the next Pages deploy would drop the web console's bundled firmware (it takes the latest
    release). Offer to make it a draft again (`gh release edit vX.Y.Z --draft`) until the evidence is fixed; if
    CI just hadn't finished, re-run the workflow once it has.
12. **Tidy the trackers.** Remove items shipped in this release from `TODO.md` and `ROADMAP.md` if the PR didn't
    already, on a branch with a PR.
