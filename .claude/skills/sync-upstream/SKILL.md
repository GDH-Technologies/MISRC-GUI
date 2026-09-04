---
name: sync-upstream
description: Merge harrypm/MISRC-GUI's main into the GDH fork on a dated chore branch, resolve conflicts toward upstream, rebuild and run the guard suite, mirror any new upstream constraints into CLAUDE.md, and open the PR. Use weekly and before any upstream PR.
when_to_use: Use when asked to sync, merge, or pull upstream; when `git log main..upstream/main` is non-empty; and always before /upstream-pr so the cherry-picks apply to a base the fork already carries.
allowed-tools:
  - Bash(git fetch *)
  - Bash(git log *)
  - Bash(git diff *)
  - Bash(git merge *)
  - Bash(git status *)
  - Bash(git show *)
  - Bash(scripts/build-local.sh *)
  - Bash(python3 misrc_tools/test/ci_guard_tests.py *)
  - Bash(build-local/misrc_gui --smoke-test)
  - mcp__plugin_github_github__create_pull_request
---

# Syncing the fork with upstream

The fork is ~110 commits ahead of upstream and upstream moves at ~80 commits/month. The
three merges so far (`e9ddaf6`, `38814fe`, `7b92f44`) were done by hand; this is that
procedure, written down.

## Procedure

1. **Worktree.** `EnterWorktree` with the name `chore/merge-upstream-YYYY-MM-DD` (today's
   date). The hook branches it from `origin/main`.
2. **See what is coming.**
   ```bash
   git fetch upstream
   git log --oneline main..upstream/main | wc -l          # count for the commit message
   git log --oneline main..upstream/main                  # read every subject
   git diff --stat main...upstream/main | tail -20         # where the mass is
   git describe --tags --abbrev=0 upstream/main            # the upstream version, if tagged
   ```
   Note any commit touching `misrc_tools/test/ci_guard_tests.py`, `misrc_tools/meson.build`,
   `.github/workflows/build.yml`, `misrc_gui/dev/dev_notes_README.md` or a `PROMPT_*` file —
   those are the ones that carry new constraints or conflict with fork guards.
3. **Merge, never rebase.**
   ```bash
   git merge --no-ff upstream/main -m "Merge upstream/main (<N> commits, <version>) into the GDH fork"
   ```
4. **Conflicts.** In shared files resolve **toward upstream** unless a fork feature depends
   on the fork's side — then keep both and say so in the PR. A conflict in a fork-only path
   (`.claude/rules/upstream.md` lists them) means a fork-only path leaked upstream: stop and
   report, that leak is the bug. `git diff --name-only --diff-filter=U` lists what is left.
5. **Build and prove.**
   ```bash
   scripts/build-local.sh --clean
   python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-local/misrc_gui
   build-local/misrc_gui --smoke-test
   ```
   A guard that fails because upstream changed what it matches is fixed in the guard, in
   this same merge, with the reason in the PR body. Never by editing `build.yml`.
6. **New constraints.** Read the diff of `dev_notes_README.md` and any new `PROMPT_*` file.
   Anything that reads as "do not …" or "always …" about the capture path goes into
   `.claude/CLAUDE.md` *Gotchas* in the same PR.
7. **PR** via `/open-pr`, kind **fork-only**. Body: commit count and upstream version, the
   list of conflicted files with how each was resolved, guard pass count and smoke result,
   new constraints mirrored, and anything upstream changed that affects the streaming or
   net modules (fork surfaces that sit next to upstream code).

## Never

- Never rebase `main`, never merge locally into `main`, never push `--force`.
- Never resolve a conflict by taking the fork's side of an upstream-owned file
  (`build.yml`, `PROMPT_*`, `third_party/`, `git-version.sh`).
