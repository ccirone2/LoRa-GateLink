---
name: pr-followup
description: Follow up GateLink's open pull requests - check CI, read review comments, fix failures and conflicts on the branch, rebase onto main, and merge only what the user has cleared to merge. Use when asked about the state of open pull requests, after pushing a branch, or from the nightly pr-followup routine.
---

# Follow up open pull requests

Landing rules: a pull request merges only when CI is green, review found nothing open, its body has the bench
results a firmware change needs (`/bench`), and the user has cleared it (said so for this one, or for the program
of work it belongs to). Routine pull requests (title starts `[routine]`) are never merged by a routine. Releases
are separate (`/release`) and always need the user's yes.

1. **List them.** `gh pr list --state open --json number,title,headRefName,isDraft,mergeable,reviewDecision`
   (in a cloud session `gh pr` is unavailable: use `gh api repos/{owner}/{repo}/pulls`).
2. **For each:**
   - CI: `gh pr checks <n>`. A failure: read the log (`gh run view <id> --log-failed`), reproduce it locally,
     fix it on the branch with a commit that says what broke, push. A flaky failure (passes on rerun, unrelated
     to the change) gets a TODO.md item rather than a silent rerun.
   - Conflicts or behind `main`: rebase onto `origin/main`, rerun the checks for the areas touched
     (`docs/development.md` "Verify"), push with `--force-with-lease`.
   - Review comments: `gh api repos/{owner}/{repo}/pulls/<n>/comments` and `.../issues/<n>/comments`. Address each
     in a commit or answer why not; never resolve a reviewer's thread for them.
   - Firmware changes need bench results in the body before merging (`/bench`); a cloud session can't run the
     bench, so it says so in a comment and leaves the merge.
3. **Merge** only cleared pull requests: `gh pr merge <n> --merge --delete-branch`, then remove the branch's
   worktree if it had one (`git worktree remove`). A merged pull request that bumps `FW_VERSION` is ready for
   `/release` (ask the user first).
4. **Report** per pull request: state, what you fixed, what's waiting (on CI, review, bench, or the user).
