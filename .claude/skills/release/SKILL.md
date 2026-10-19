---
name: release
description: Release a GateLink firmware version - run the bench per docs/release-criteria.md on the merged version, collect the release evidence with tools/release_evidence.py and merge it, then create a draft release on the merge commit with notes from the pull requests since the last tag, and have the release workflow attach the binary and evidence and publish it. Use after a pull request that bumps FW_VERSION is merged, or when asked to cut, tag or publish a release or update the changelog.
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
   The version in `config.h` must not have a published release or a tag yet. If it does, the firmware change
   didn't bump `FW_VERSION`: stop and say so. A draft for it is a release in progress from an earlier run: keep it,
   and pick it up at step 10 or 11 rather than creating another. The dry run prints the release kind (from the previous
   tag), whether CI passed on this commit, and which criteria are still open: that is the bench plan. If CI
   didn't pass on `main`, stop: fix it first.
2. **Flash both boards with this `main`** (`/flash`, from this checkout, no local changes under `firmware/`).
   `uv run tools/gatelink.py ports` must show both boards on X.Y.Z with the same key id (`key <8 hex>`), and the bench boards from the matrix
   in `docs/release-criteria.md` (house `usb ...183013`, gate `usb ...0C301C`; never ...191117). Re-upload the
   GateSim if `tools/GateSim` changed since the last release.
3. **Run the bench** for the release kind, from this checkout, with the web console disconnected
   (`GATELINK_HA_URL` set as in `tests/e2e/README.md`; `GATELINK_KEY` from `~/.gatelink_key`). Every release:
   ```sh
   GATELINK_KEY=$(cat ~/.gatelink_key) uv run pytest tests/e2e -v    # ~30 min
   uv run pytest tests/e2e -m soak --cycles 20
   ```
   MINOR and MAJOR also (ask the user to plug the LiPos in or out between the power runs; the run reports the
   state it found in its "LiPos" fact). Before the 17 dBm run, have the user confirm both boards are fed from the
   install-like supplies, not USB power: full-power TX on USB power crashes a board into watchdog resets, and the
   tool can't tell the supplies apart. Never pass `-k`, `--ignore`, `--lf` or `--sw` to a criterion run (only to a
   rerun): such a run doesn't count.
   ```sh
   uv run pytest tests/e2e -m power                                  # both LiPos out
   uv run pytest tests/e2e -m power                                  # both LiPos in
   uv run pytest tests/e2e -m longsoak --soak-minutes 60
   GATELINK_KEY=$(cat ~/.gatelink_key) uv run pytest tests/e2e -v --tx-power 17
   ```
   MAJOR also `uv run pytest tests/e2e -m longsoak --soak-minutes 1440`, and the install checks (real RF and full power
   at the site, TODO.md), which the user records by hand in the evidence file's install notes.
   Long runs go in the background; check on them rather than waiting. If one scenario fails in an otherwise
   clean run, it may be rerun once on its own with the same options (`uv run pytest tests/e2e -k <name>`, plus `-m
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
   The release workflow runs the same check on the draft's commit and refuses to build or publish until it passes.
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
   before publishing: a release on this public repo is visible to everyone, and it is immutable once published
   (its tag and files are locked, and its version can never be used again).
10. **Create the draft and run the release workflow.** Never publish a release yourself while its files aren't
    attached: `gh release create` without `--draft` publishes at once, with nothing attached. If step 1 found a
    draft, update it instead (`gh release edit vX.Y.Z --target <merge-commit-sha> --notes-file <notes.md>`).
    ```sh
    gh release create vX.Y.Z --draft --target <merge-commit-sha> --title "vX.Y.Z — <one-line summary>" --notes-file <notes.md>
    gh workflow run release.yml -f tag=vX.Y.Z
    gh run list --workflow release.yml --limit 1   # then: gh run watch <id>
    ```
    A draft has no tag. The workflow finds the draft and checks it: its target is a full SHA on `main`, the tag is
    free, `FW_VERSION` matches and the evidence passes (`check --ci`). It then builds that commit, attaches
    `GateLink-vX.Y.Z.bin`, its `.sha256` and `GateLink-vX.Y.Z-evidence.md`, and publishes, which creates the tag
    there. With `-f publish=false` it stops once the files are attached, to check them on the draft first; then
    run it again without the flag to publish (it rebuilds the same commit with the same toolchain, so the `.sha256`
    it attaches should match the one checked).
11. **Check the release.**
    - `gh release view vX.Y.Z --json isDraft,isImmutable,tagName,assets` shows it published, immutable, and with
      the three files.
    - `git ls-remote origin refs/tags/vX.Y.Z` shows the tag on the target commit.
    - `gh release verify vX.Y.Z` checks GitHub's attestation, and `gh release verify-asset vX.Y.Z <downloaded
      .bin>` the binary against it.
    - The Pages run after the release workflow passed, and the page's `firmware/latest.json` names the version.

    If the workflow failed, check `gh release view vX.Y.Z --json isDraft,assets` and show the log (`gh run view <id>
    --log-failed`).
    - **Still a draft** (it failed before publishing): fix the cause and start a new run (`gh workflow run
      release.yml -f tag=vX.Y.Z`; not "Re-run failed jobs", which reuses the first job's reading of the draft). If
      the evidence was merged again, point the draft at the new merge commit first (`gh release edit vX.Y.Z --target
      <sha>`). If the build refused because `release.yml` differs between the branch it ran from and the target,
      point the draft at `main`'s head when its firmware tree and `FW_VERSION` are the same.
    - **Publish was refused by GitHub** with the files attached: GitHub doesn't let the workflow's token release a
      commit that adds or changes a workflow file relative to `main`. Publish it yourself (`gh release edit vX.Y.Z
      --draft=false`) once `gh release view` lists all three files, then run the checks above and `gh workflow run
      pages.yml`.
    - **Published** (it failed after Publish printed `published <url>`): don't run it again. Do the checks above: if
      the three files are there and the tag is on the target, only `gh workflow run pages.yml` is left. If not, stop
      and tell the user: the version is used up, and a fix needs a new one.
12. **Tidy the trackers.** Remove items shipped in this release from `TODO.md` and `ROADMAP.md` if the PR didn't
    already, on a branch with a PR.
