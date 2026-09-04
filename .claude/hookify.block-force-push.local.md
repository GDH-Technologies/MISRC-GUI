---
name: block-force-push
enabled: true
event: bash
action: block
pattern: (^|[;&|]|\n)\s*git\s+push\b[^;&|\n]*(\s--force(-with-lease|-if-includes)?\b|\s-f\b|\s\+[A-Za-z0-9._/-]+(:|$))
---

**Blocked: force-pushing is banned in this repo.**

`.claude/CLAUDE.md` -> *Worktrees and git hygiene*: "never push `--force`". Fork `main` is
what three self-hosted runners install from on every push, `bump-tag` mints a release tag per
merge, and `upstream/*` branches may be open as PRs on harrypm's repo — a rewritten history
breaks all three and Reece's own checkouts.

**Do this instead:**

- Wrong commit on a branch -> add a fixing commit, or open a fresh branch and a fresh PR.
- Need to update an open upstream PR after a rebase onto a newer `upstream/main` -> say so
  and let Reece decide; a squash-merge on his side does not need it.
- `+refs/...` refspecs are the same thing spelled differently and are blocked too.
