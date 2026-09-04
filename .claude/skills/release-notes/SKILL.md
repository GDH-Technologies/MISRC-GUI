---
name: release-notes
description: Use when a GDH release tag (v<upstream>-gdh.N) needs release notes, when asked what changed between two fork tags or since the upstream version the fork tracks, or when a GitHub Release is to be published on the fork.
when_to_use: Use after a merge to main has minted a new -gdh.N tag and before announcing or relying on it, and when comparing the fork against its upstream base.
disable-model-invocation: true
allowed-tools:
  - Bash(git tag *)
  - Bash(git log *)
  - Bash(git describe *)
  - Bash(git show --stat *)
  - Bash(git diff --stat *)
  - Bash(gh release list *)
  - Bash(gh release view *)
  - Bash(gh run list *)
---

# Release notes for a `-gdh.N` tag

## How tags work here

`selfhosted-deploy.yml`'s `bump-tag` job mints `v<upstream>-gdh.N` on every push to `main`
(it skips when HEAD already carries a `v*` tag), so N counts fork releases on top of the
upstream version the fork last synced — `v1.1.8-gdh.9` today. When `/sync-upstream` lands a
new upstream `vX.Y.Z`, the next tag restarts at `-gdh.1`. The fork publishes **no GitHub
Releases**; only upstream does ("MISRC GUI vX.Y.Z"). Tags exist for the install step:
`misrc_gui --version` on wm/cs0/air0 names the tag that is running.

## Procedure

1. **Range.** `git tag -l 'v*-gdh.*' | sort -V | tail -2` gives previous and new. For the
   first tag after a sync, the previous is the upstream tag:
   `git describe --tags --abbrev=0 --exclude '*-gdh.*' <new>`.
2. **Commits.** `git log --no-merges --format='%h %s' <prev>..<new>`. PR numbers come from
   the merge commits: `git log --merges --format='%s' <prev>..<new> | grep -oE '#[0-9]+'`.
3. **Group by area**, from the conventional-commit scope and the paths touched:
   capture path (`input/`, `processing/`, `common/`) · cxadc / clockgen · streaming /
   preview (fork-only) · UI · CI / deploy · upstream sync (the
   `Merge upstream/main (N commits, vX.Y.Z)` subject — name the upstream version) ·
   dev environment (`.claude/`).
4. **One line per change, for an operator**: what they will notice, not the diff. `git show
   --stat` anything whose subject is not self-explanatory. Link the PR. Mark upstream-bound
   items — they are `/upstream-pr` candidates.
5. **Header**: tag · date · upstream base · hosts that received it (`gh run list
   --workflow selfhosted-deploy.yml --limit 3` shows the install legs) · commit and PR counts.
6. **Output the Markdown.** Creating a GitHub Release from it (`gh release create <tag>
   --notes-file …`) is a separate, explicit request — never on your own.

## Template

```markdown
## v1.1.8-gdh.10 — 2026-09-05
Upstream base v1.1.8 · installed on wm, cs0, air0 · 6 commits · 3 PRs

### Capture path
- Record path now spills to disk instead of dropping when the ring is full for >1 s (#41) — upstream-bound

### Streaming / preview (fork)
- Stream bitrate readout replaces the frame counter (#29)

### CI / dev environment
- Worktrees link .deps from the main checkout; clangd reads build-local (#40, #43)
```

## Never

- Never describe a change from its subject line alone — open it.
- Never list the `bump-tag` commit or the merge commits themselves as changes.
