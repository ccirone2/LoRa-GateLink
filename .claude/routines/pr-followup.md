---
name: pr-followup
schedule: 0 6 * * *
description: Keep routine pull requests mergeable - rebase them and fix their CI - and summarise every open pull request's state
---

# Nightly pull request follow-up

1. Run `/pr-followup` for the open pull requests whose title starts `[routine]`: fix CI failures and conflicts on
   their branches and answer review comments, as the skill says. Never merge (routines don't), and close none;
   if one is obsolete (its subject merged another way), say so in a comment for the user to close.
2. For the other open pull requests, change nothing: list their state (CI, conflicts, waiting on review, bench or
   the user).
3. No pull request of its own. End with the summary: one line per open pull request.
