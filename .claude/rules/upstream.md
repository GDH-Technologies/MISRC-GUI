# Upstream policy — GDH fork ↔ `harrypm/MISRC-GUI`

## Every change is one of two kinds

- **Upstream-bound** — generic, builds on all six CI targets, carries no GDH host, fleet or
  archival policy. Lands on fork `main` first, then reaches harrypm through `/upstream-pr`
  as a cherry-pick onto `upstream/main`.
- **Fork-only** — GDH's CI, deploy, docs, `.claude/`, and anything harrypm has declined or
  that encodes GDH policy. Lives on `main` only.

Say which in the PR body. A change that mixes both is split before it is opened.

## Fork-only paths — never in an upstream PR

`.claude/` · `.github/workflows/selfhosted-deploy.yml` · `docs/gdh-*` · `docs/superpowers/` ·
`scripts/fetch-mediamtx.sh` · `misrc_tools/misrc_gui/streaming/` ·
`misrc_tools/misrc_gui/input/gui_preview_v4l2.c` (+ `gui_preview_tap.h`) ·
`misrc_tools/misrc_gui/output/gui_video_record.{c,h}` ·
`misrc_tools/misrc_gui/visualization/gui_preview_panel.{c,h}` · the fork's guards in
`misrc_tools/test/ci_guard_tests.py` and `misrc_tools/test/*_harness.c` that name them.

Check with `git diff --name-only upstream/main...<branch>` before opening an upstream PR.
The streaming stack is upstream-bound *in intent* but goes as its own proposal, not as a
ride-along — ask harrypm first.

## Upstream owns these — do not edit

- `.github/workflows/build.yml` — byte-identical to upstream, disabled by repo setting
  (`gh workflow disable`), never by editing. `ci_guard_tests.py` substring-matches it, so an
  edit both breaks guards and conflicts on every sync.
- `PROMPT_*_README.md` — harrypm's agent working logs.
- `third_party/` — vendored; fixes go to hsdaoh / tape-decode-rs directly.
- `misrc_tools/git-version.sh` — the single version source `.github/CI_RULES.md` pins.

## Upstream's CI contract (`.github/CI_RULES.md`)

Six targets: ubuntu-22.04 AppImage (glibc baseline), windows-2022 x64, windows-11-arm,
macos-14 arm64, macos-15-intel, Android APK. Every version string resolves through
`git-version.sh`. Actions pinned to a `@vN` major, never `@main` or a SHA; never a retired
runner image. The fork's self-hosted CI proves only Linux x64 and macOS ARM64 — an upstream
PR says so in its body and names what was not built.

## Syncing

`/sync-upstream` weekly and before every upstream PR. Merge `upstream/main` into a
`chore/merge-upstream-YYYY-MM-DD` branch; never rebase `main`. In shared files resolve
toward upstream; fork-only files cannot conflict by construction — if one does, a fork-only
path leaked into upstream and the leak is the bug.

## Contribution posture

Upstream has no CONTRIBUTING.md, no issue templates, one maintainer and fast churn. What
lands: small, self-contained PRs with evidence — a harness, a guard, a quoted log line.
Lead with fixes to upstream's own newest code, then hardening (cxadc / clockgen), then
features (streaming), and ask before anything large. When a problem is GDH-only, prefer a
container, CI or config fix over a source edit.
