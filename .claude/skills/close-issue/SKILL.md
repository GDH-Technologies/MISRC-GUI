---
name: close-issue
description: Close a GitHub issue on the MISRC-GUI fork only after proving the fix is on origin/main, covers the whole issue, is installed where it matters, and has no open sub-issues. Use when an issue looks resolved and you are about to close it.
when_to_use: Use before closing any issue, and when asked whether an issue can be closed or is still live. Also use when auditing stale-looking issues.
allowed-tools:
  - mcp__plugin_github_github__issue_read
  - mcp__plugin_github_github__list_issues
  - mcp__plugin_github_github__search_issues
  - mcp__plugin_github_github__get_commit
  - Bash(git fetch origin --quiet)
  - Bash(git fetch upstream --quiet)
  - Bash(git merge-base --is-ancestor *)
  - Bash(git log *)
---

# Closing an issue

Close an issue when — and only when — **every** defect it describes is fixed and the fix is
**present on `origin/main`**. Both halves are load-bearing, and each is a separate check.

Never leave a fixed-and-verified issue open silently. Equally, never close one on inference,
on a green PR, or because it "looks handled" — say which state it is in instead.

## The four gates

### 1. Is it on `origin/main`?

`main` is the landed bar. Merged ≠ on main until you have proved it, and a branch is not
main at all:

```bash
git fetch origin --quiet
git merge-base --is-ancestor <fix-sha> origin/main && echo "on main" || echo "NOT on main"
```

A fix living on an unpushed branch or an open PR is **progress**, not closure. Comment the
branch name and tip SHA and leave the issue open.

**Upstream-bound fixes have a second bar.** If the issue is about upstream code, "fixed on
the fork" is the first half; the issue stays open (with the upstream PR linked) until the
fix is also on `upstream/main` — `git merge-base --is-ancestor <sha> upstream/main` — or
Reece says the fork will carry it.

### 2. Does the fix cover the whole issue?

Map every numbered defect and fix-direction bullet in the body to a state before closing,
and write the mapping into the comment as a table. Partial delivery is the most common
false close — "the leak, bullet 2" leaves bullets 1 and 3 live.

### 3. Is the operational half done?

Code on main is not the finish line when the issue also implies an action on real hardware
or a real host:

| Owed action | Looks like |
| --- | --- |
| Installed build on the rigs | the merge triggered `selfhosted-deploy.yml`; `~/.local/bin/misrc_gui --version` on wm/cs0 and `~/Applications/MISRC.app` on air0 name the new `-gdh.N` tag |
| Live-rig smoke | any capture-path, cxadc/clockgen, streaming or net change — a real capture, RF and audio monitor checked separately |
| Guard added | a regression that had no guard now has one, and it fails on the old code |
| Upstream PR opened | an upstream-bound fix has its `/upstream-pr` linked from the issue |

**Verify the deployed state, don't assume it.**

### 4. Are all sub-issues closed?

A parent closes last. Sub-issues can be cross-repo (`digitization-toolkit`, `capture-node`
during the transition); read each with `issue_read` and check its state.

## Things that are not closure

- **A guard is not a fix.** A check that makes a defect *loud* leaves it in place. Downgrade
  the severity in a comment, keep it open, list the remaining options.
- **A stale-looking issue is not a resolved one.** If it cannot be verified, say so and let
  Reece decide; do not close as `not planned` on your own.
- **A prior comment claiming "fixed" is not proof.** Comments predate merges and syncs.
  Re-verify against the current tree every time.

## Closing procedure

1. **Re-read the issue body top to bottom**, including any stated closing condition.
2. **Re-check the issue is still open right before closing.** Sessions run concurrently.
3. **Re-run the verification now, on the current tree** — the guard, the harness, the
   `--version` on the host. Paste the real command and its real output into the comment.
4. **Post a closing comment before closing:** what was fixed, where it landed (SHA + PR,
   plus the upstream PR if any), the evidence, and anything carved off into a follow-up.
5. **Close with an explicit reason** — `issue_write` with `state: "closed"` and
   `state_reason: "completed"`, or `"not_planned"` for won't-fix / obsolete (never for
   "probably fine"), or `"duplicate"` with `duplicate_of`.
6. **Prefer `Fixes #N` in the PR body** when the fix rides a fork PR — it closes on merge.

> [!IMPORTANT]
> **Never reopen or re-close against a human's decision.** Say so in a comment and leave the
> state alone.
