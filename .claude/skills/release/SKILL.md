---
name: release
description: Publish a GateLink firmware release on GitHub - tag the merged version, write release notes from the pull requests since the last tag, and check the release workflow attached the firmware binary. Use after a pull request that bumps FW_VERSION is merged, or when asked to cut, tag or publish a release or update the changelog.
---

# Publish a firmware release

The changelog is the list of GitHub releases, one per `FW_VERSION`. Process and versioning rules:
`docs/development.md` ("Versioning", "Releasing").

1. **Check the state.**
   ```sh
   git fetch origin --tags && git checkout main && git pull --ff-only
   grep FW_VERSION firmware/GateLink/config.h
   gh release list --limit 5
   ```
   The version in `config.h` must not have a release yet. If it does, the firmware change didn't bump
   `FW_VERSION`: stop and say so rather than retagging.
2. **Gather what changed** since the previous tag:
   ```sh
   git log --oneline <prev-tag>..main
   gh pr list --state merged --search "merged:>=<prev-tag-date>" --json number,title,body
   ```
   Look for: `CFG_VERSION` changes (config layout), `roles.h` STATUS/DIAG or `link.cpp` frame changes (both
   boards must be updated together), GateSim changes (re-upload the Uno), console/log changes (web console and
   e2e suite move with them).
3. **Write the notes** to a file in the scratchpad, using the template in `docs/development.md`. Lead with the
   upgrade line (which boards to flash, config re-apply, GateSim re-upload). Keep bench results (suite pass
   count, notable measurements) from the PR bodies. Plain, specific sentences; no marketing.
4. **Show the user the version, target commit and notes, and get a yes** before publishing: a release on this
   public repo is visible to everyone.
5. **Publish:**
   ```sh
   gh release create vX.Y.Z --target main --title "vX.Y.Z — <one-line summary>" --notes-file <notes.md>
   ```
6. **Check the binary.** The `release` workflow builds the tag and attaches `GateLink-vX.Y.Z.bin`:
   `gh run list --workflow release.yml --limit 1`, then `gh release view vX.Y.Z` should list the asset. If the
   workflow failed, show its log (`gh run view <id> --log-failed`).
7. **Tidy the trackers.** Remove items shipped in this release from `TODO.md` and `ROADMAP.md` if the PR didn't
   already, on a branch with a PR.
