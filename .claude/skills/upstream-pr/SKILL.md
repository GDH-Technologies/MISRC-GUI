---
name: upstream-pr
description: Prepare and open a pull request against harrypm/MISRC-GUI from a branch based on upstream/main, cherry-picking named fork commits, proving no fork-only path rides along, checking upstream's six-target CI contract, and attaching evidence harrypm can run. Use when a fix or feature on the fork is ready to go upstream.
when_to_use: Use when asked to upstream, contribute, or send a change to harrypm; when a fork PR was marked upstream-bound and has merged; and for any change to misrc_gui/net/, which is upstream's code.
allowed-tools:
  - Bash(git fetch *)
  - Bash(git log *)
  - Bash(git diff *)
  - Bash(git cherry-pick *)
  - Bash(git status *)
  - Bash(git show *)
  - Bash(git push origin upstream/*)
  - Bash(scripts/build-local.sh *)
  - Bash(python3 misrc_tools/test/ci_guard_tests.py *)
  - Bash(build-local/misrc_gui --smoke-test)
  - mcp__plugin_github_github__create_pull_request
  - mcp__plugin_github_github__pull_request_read
---

# Taking a change upstream

Upstream has one maintainer, no CONTRIBUTING.md and fast churn. What lands there is small,
self-contained, evidenced, and free of anything that only makes sense at GDH.

## Preconditions

- `/sync-upstream` is fresh — the fork already carries the `upstream/main` the PR will base on.
- The change is on fork `main` (or at least reviewed there) as its own commits, not mixed
  with fork-only work. If it is not, cherry-picking will be the moment that shows.
- **Reece has said this change should go upstream.** Opening a PR on someone else's repo is
  never a routine step: draft the body, show it, open on go.

## Procedure

1. **Worktree from upstream.** `EnterWorktree` with the name `upstream/<topic>`. The hook
   branches `upstream/<topic>` from **`upstream/main`**, not `origin/main`.
2. **Cherry-pick the fork commits** by SHA, with `-x` so each commit records its origin:
   ```bash
   git cherry-pick -x <sha> [<sha>...]
   ```
   A conflict here means the change depends on fork-only code — stop and say what.
3. **Prove the diff is clean.**
   ```bash
   git diff --name-only upstream/main...HEAD
   ```
   Every path must be an upstream path. None of `.claude/`, `docs/gdh-*`, `docs/superpowers/`,
   `.github/workflows/selfhosted-deploy.yml`, `scripts/fetch-mediamtx.sh`,
   `misrc_gui/streaming/`, `gui_preview_v4l2.c`, `gui_video_record.c`, `gui_preview_panel.c`
   may appear. Grep the diff for `gdh`, `GDH`, `cs0`, `air0`, `workflow-master`, `wm` as a
   host name, and `192.168.` — none may appear.
4. **Upstream's CI contract** (`.github/CI_RULES.md`): the change must build on ubuntu-22.04
   AppImage, windows-2022 x64, windows-11-arm, macos-14, macos-15-intel and the Android APK.
   No new `dependency()` without a matching install step in `build.yml` (which you cannot
   edit here — say so and propose it in the body). Any version string resolves through
   `misrc_tools/git-version.sh`. Windows needs `-lws2_32`-style explicit link lines; check
   the existing pattern in `meson.build`.
5. **Build and prove on what you have.**
   ```bash
   scripts/build-local.sh --clean
   python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-local/misrc_gui
   build-local/misrc_gui --smoke-test
   ```
   Fork guards that name fork-only surfaces will not run against this tree; that is
   expected — report upstream's guard count. State plainly which of the six targets were
   **not** built (Windows, Android, Intel macOS unless air0 or wm ran them).
6. **Evidence harrypm can run.** A defect PR carries a reproduction he can execute: a guard
   harness in `misrc_tools/test/` (the fanout harness pattern — extract the code under test
   verbatim, drive it, print measured numbers), a `--smoke-test` transcript, or a log line
   with the commit that produced it. "Verified locally" is not evidence.
7. **Push to the fork and open against upstream.**
   ```bash
   git push -u origin upstream/<topic>
   ```
   `create_pull_request` with `owner: harrypm`, `repo: MISRC-GUI`, `base: main`,
   `head: GDH-Technologies:upstream/<topic>`. Open as a **draft** unless Reece says otherwise.

## Body

Written for harrypm, not for GDH. No GDH workflow, no fleet, no "we".

```markdown
## Problem
## Change
## Evidence
## Not built here
```

- **Problem** — what breaks, quoted. For the net-mode fanout: bytes produced vs delivered,
  mallocs vs frees, the lost-wakeup run.
- **Change** — the mechanism, in the code's own terms; a before/after table for any
  behaviour that changed.
- **Evidence** — the harness or guard and its output, with the command line.
- **Not built here** — the targets you could not exercise, honestly.

The `github-conventions.py` hook appends a provenance footer; do not write one.

## After opening

Link the upstream PR from the fork PR or issue it came from, and note the sync obligation:
once it merges upstream, `/sync-upstream` brings it back and the fork's copy of the
commits becomes redundant — the merge resolves that automatically because they are the
same patch.
