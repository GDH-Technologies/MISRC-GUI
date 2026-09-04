---
name: warn-git-add-all
enabled: true
event: bash
action: warn
pattern: (^|[;&|]|\n)\s*git\s+add\s+(-A|--all|\.\s|\.\s*$)
---

**Blanket `git add` detected - stage explicit paths instead.**

`.claude/CLAUDE.md` -> *Worktrees and git hygiene*: "Stage explicit paths, never
`git add -A`. Reece edits and commits in these same trees during a session."

**Why it matters here:** a worktree carries an untracked `.deps/` of symlinks, per-worktree
build directories, and whatever Reece has open in the same tree. A blanket add sweeps all of
that into the commit — and an upstream-bound branch must contain nothing but the change it
proposes (`.claude/rules/upstream.md`).

**Do this instead:** name the files, then verify.

- Stage only what you touched, path by path.
- `git diff --cached --stat` and confirm the list is exactly what you intended.
- On an `upstream/*` branch, also `git diff --name-only upstream/main...HEAD` and check
  that no fork-only path is present.
