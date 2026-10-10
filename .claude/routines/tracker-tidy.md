---
name: tracker-tidy
schedule: 0 5 * * *
description: Keep TODO.md and ROADMAP.md true - drop what's done, merge duplicates, flag what's stale - in one small pull request
---

# Nightly tracker tidy

`TODO.md` holds open bugs, investigations and bench or field tasks; `ROADMAP.md` desired features. An item leaves
when its fix or feature is merged (`docs/development.md`).

1. **What merged lately:** `git log --merges --since="8 days ago" origin/main` and the pull requests behind them
   (`gh api "repos/{owner}/{repo}/pulls?state=closed&per_page=30"`, merged ones). For each item they claim to
   close or that matches their subject, check the code: is it really done? Remove it only with that evidence (the
   pull request number in the commit message).
2. **Duplicates and overlap** between items, or between `TODO.md` and `ROADMAP.md`: merge them, keeping every
   distinct fact.
3. **Stale items:** an item naming code that no longer exists (grep it) or a finding the code now contradicts.
   Fix the item's wording, or remove it if it no longer applies, saying why in the pull request body.
4. **Don't decide for the user.** Never drop a feature idea because it looks unlikely, and never reprioritise;
   items that need a decision get a one-line question in the pull request body instead.
5. One pull request titled `[routine] Trackers: ...` on `claude/routine-tracker-tidy-<yyyymmdd>`, each change
   with its evidence. Nothing to tidy: no pull request.
