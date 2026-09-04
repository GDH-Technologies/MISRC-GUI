---
name: open-pr
description: Open a pull request on the MISRC-GUI fork (target main) with a body that says which kind of change it is, why it exists, what changed, what was verified on which platform, and what is owed after merge. Use when work on a branch is ready to be proposed to the fork; use /upstream-pr for harrypm's repo.
when_to_use: Use when asked to open, create, or raise a PR on the fork, and when a branch's work is complete and ready for review. Not for PRs against harrypm/MISRC-GUI — that is /upstream-pr.
allowed-tools:
  - mcp__plugin_github_github__create_pull_request
  - mcp__plugin_github_github__pull_request_read
  - mcp__plugin_github_github__list_pull_requests
  - Bash(gh label list *)
  - Bash(git diff --name-only *)
  - Bash(git log *)
  - Bash(git status *)
---

# Opening a PR on the fork

## Before you open it

- **Read the live label set** — `gh label list -R GDH-Technologies/MISRC-GUI`.
  `create_pull_request` takes no `labels` parameter; labels are a separate step afterwards.
- **Know your own diff** — `git diff --name-only origin/main...HEAD`. Do not describe changes
  you have not looked at. PRs target `main`.
- **Classify it** — upstream-bound or fork-only (`.claude/rules/upstream.md`). If the diff
  mixes both, split it first.
- **Push the branch** from the worktree (`git push -u origin <branch>`); never force.

## Body

Omit any section with nothing to say. No checkboxes anywhere — a ticked box proves nothing.

```markdown
**Kind:** upstream-bound | fork-only

## The problem
## What changed
## Verification
## Owed after merge
```

- **Kind** — one line. For upstream-bound, name the `/upstream-pr` that will follow (or
  link it if it already exists).
- **The problem** — evidence first. Quote the decisive log line, harness output or guard
  failure inline so the PR survives loss of the artifact. Someone who never opens the diff
  should understand why this exists.
- **What changed** — a before/after table beats prose. Do **not** restate the diff.
- **Verification** — real commands and real counts: the guard pass count
  (`ci_guard_tests.py --static-only` / `--post-build`), `--smoke-test` exit code, and for a
  capture-path change the `--rtsp-soak` numbers. Say which platforms built: PR CI builds on
  **wm only**; cs0 and air0 join at merge. Say explicitly what was **not** run.
- **Owed after merge** — the merge *is* the deploy: `selfhosted-deploy.yml` installs on
  wm, cs0 and air0. List what that implies — GUI relaunch, `--version` check on each host,
  rig smoke for a capture-path change — or write `nothing`. Never leave it blank.

Add `Fixes #N` when the PR closes an issue. The gates in `/close-issue` still apply.

## Labels

At least one component/area label as it exists in the live set, plus workflow flags where
they apply — `regression`, `upstream-bound`, `documentation`. Never `bug` or `enhancement`.

## Do not write a provenance footer

The `github-conventions.py` hook appends the `🤖 Generated with` line and the session table
automatically. Writing your own is how the footer ends up duplicated.
