---
name: sync-upstream
description: MISRC-GUI's repo profile for the generic sync-fork procedure. Merges harrypm/MISRC-GUI (a release tag or upstream/main) into the GDH fork on a dated chore branch, with this repo's preflight flags, invariants, build/test commands, and how the fork's version tag is minted and checked. Use weekly and before any upstream PR.
when_to_use: Use when asked to sync, merge, pull or update to an upstream release; when `git log origin/main..upstream/main` is non-empty; and always before /upstream-pr so the cherry-picks apply to a base the fork already carries.
allowed-tools:
  - Bash(git fetch *)
  - Bash(git log *)
  - Bash(git diff *)
  - Bash(git merge *)
  - Bash(git status *)
  - Bash(git show *)
  - Bash(git ls-remote *)
  - Bash(python3 ~/.claude/skills/sync-fork/preflight.py *)
  - Bash(scripts/build-local.sh *)
  - Bash(python3 misrc_tools/test/ci_guard_tests.py *)
  - Bash(meson test *)
  - Bash(bash misrc_tools/test/net_settings_e2e.sh *)
  - Bash(build-local/misrc_gui --smoke-test)
  - mcp__plugin_github_github__create_pull_request
---

# MISRC-GUI: syncing with harrypm/MISRC-GUI

The procedure is the generic **sync-fork** skill (`~/.claude/skills/sync-fork/SKILL.md`):
load it and follow it. This file is the repo profile it asks for.

## Profile

| Field | MISRC-GUI |
| --- | --- |
| Remotes | `origin` = `gdh` = GDH-Technologies/MISRC-GUI (the fork); `upstream` = harrypm/MISRC-GUI |
| Base | `origin/main` |
| Default target | harrypm's newest release (`git ls-remote --tags upstream`). Use `upstream/main` when asked. If `upstream/main` is past the tag, ask which |
| Worktree | `EnterWorktree` name `chore/merge-upstream-YYYY-MM-DD`: `.claude/hooks/worktree-deps.py` bases it on `origin/main` and links `.deps`. Check `git status --short --branch`, `git merge-base --is-ancestor origin/main HEAD`, and `ls .deps` |
| Upstream-owned | `.github/workflows/build.yml`, `PROMPT_*`, `third_party/`, `misrc_tools/git-version.sh` |
| Fork-only | `.claude/rules/upstream.md`. A conflict in one of these paths is a leak |
| PR | `/open-pr`, kind **fork-only**. Follow-on commits are labelled upstream-bound or fork-only one by one |

Preflight:

```bash
python3 ~/.claude/skills/sync-fork/preflight.py --base origin/main --target <ref> --upstream upstream \
  --watch 'PROMPT_*' --watch 'misrc_tools/misrc_gui/dev/*' --watch 'misrc_tools/meson.build' \
  --watch '.github/*' --watch 'misrc_tools/test/*' --watch 'scripts/*' \
  --invariant 'misrc_tools/misrc_gui/input/gui_capture.c' --invariant 'misrc_tools/misrc_gui/net/*' \
  --invariant 'misrc_tools/misrc_gui/core/gui_app.h' --invariant 'misrc_tools/misrc_gui/core/gui_settings*' \
  --invariant 'misrc_tools/misrc_gui/core/misrc_gui.c' --invariant 'misrc_tools/misrc_gui/output/gui_record.c' \
  --invariant 'misrc_tools/common/flac_writer.c' --invariant 'misrc_tools/test/gui_stats_layout_harness.c' \
  --fork-only '.claude/*' --fork-only '.clangd' --fork-only '.github/workflows/selfhosted-deploy.yml' \
  --fork-only 'docs/gdh-*' --fork-only 'docs/superpowers/*' --fork-only 'scripts/fetch-mediamtx.sh' --fork-only 'scripts/gdh-host/*' \
  --fork-only 'misrc_tools/misrc_gui/streaming/*' --fork-only 'misrc_tools/misrc_gui/input/gui_preview_v4l2.c' \
  --fork-only 'misrc_tools/misrc_gui/output/gui_video_record.*' --fork-only 'misrc_tools/misrc_gui/visualization/gui_preview_panel.*'
```

## Invariants: where the fork's side wins, or both sides are kept

- **Settings.** The fork's descriptor table replaced upstream's hand-written struct and
  load/save. On `gui_app.h` and `gui_settings.c`, take the fork's side, then port every new
  upstream field: add it to `gui_settings.h`, give it a default, and APPEND a row to
  `gui_settings_table.c`, since row order is the file's write order. Use `GS_LOCAL` if the
  field describes this machine, and add it to the guard's `SETTINGS_CLIENT_LOCAL_KEYS`.
  Legacy keys become `GS_LOAD_ONLY` alias rows placed before the new row; migrations
  become hooks. The v1.1.8 key list never loses a key.
- **Net record model.** A client records on the server unless `net_client_record_local` is
  on (`58bf460`, a follow-on commit to the v1.2.0 merge). Upstream changes to
  `gui_app_start/stop_recording` or `gui_app_effective_recording` do not replace this.
  `check_record_parity` guards it.
- **Net server.** The published snapshot goes out before the listener starts (`b407a5c`).
  HTTP threads read only published copies, and `/set` is applied on the main thread.
- **FLAC finalize.** Keep the fork's `gui_record` finalize, and resolve only
  `flac_writer.c` toward upstream (see `.claude/CLAUDE.md`).
- **`gui_stats_layout_harness.c`.** The panel-name stub keeps the fork's `"Preview"` entry
  and its table-size bound.
- **Recording locks.** Keep the ones from upstream v1.2.0; a fork change must not drop
  them: `gui_record_cleanup()` before `bufmgr_cleanup()`, and Disconnect, Space and the
  mode toggle refused while recording or finalizing.
- **`ci_guard_tests.py`.** Both sides append guards. Keep both, upstream's first.

## Verification (quote every number)

```bash
scripts/build-local.sh --clean                                   # build + --smoke-test
python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-local/misrc_gui   # 70 PASS at v1.2.0
meson test -C build-local                                         # 10 targets at v1.2.0
bash misrc_tools/test/net_settings_e2e.sh build-local/misrc_gui <free port>  # from the worktree root
```

Then run the `dev-notes-auditor` agent, then the `capture-path-verifier` agent
(`--rtsp-soak`, RF and monitor audio as separate checks). When a check fails, run it
against the pre-merge binary first:
- the installed `~/.local/bin/misrc_gui`; or
- the merge-base built in the scratchpad: `git archive <sha>`, unpack it, then
  `PKG_CONFIG_PATH=/home/rdodge/Repos/MISRC-GUI/.deps/install/lib/pkgconfig meson setup <dir> misrc_tools`
  and `ninja -C <dir> misrc_gui`.

Two e2e checks are timing-sensitive on a loaded wm; say so rather than retrying until green:
- the overwrite-prompt cancel;
- before `b407a5c`, the first `/settings`.

## Version

The `bump-tag` job in `selfhosted-deploy.yml` mints `v<X.Y.Z>-gdh.N` on the first code push
to `main`:
- X.Y.Z is the highest plain upstream release tag reachable from HEAD; the job fetches
  harrypm's tags itself.
- N is one past the highest `-gdh.N` already minted on that base.

So a release sync produces `-gdh.1` with no hand tagging, provided no other code PR reaches
`main` first. Check it after merge:
- `git ls-remote --tags origin 'v<X.Y.Z>-gdh.*'`;
- `~/.local/bin/misrc_gui --version` on wm, cs0 and air0 (launch from the Mac's Terminal);
- the Start Menu install on win0.

Never push a plain upstream `v*` tag to the fork: its tag trigger would build and install
upstream's tree on every machine.

## History

| Sync | Conflicts |
| --- | --- |
| 2026-09-14, v1.2.0 + `c051361` (`7853b35`) | `gui_app.h` and `gui_settings.c` (settings port); `gui_capture.c` (autostop helpers next to the fork's effective state; `a7d1511` merged silently); `misrc_gui.c` (Space lock); `gui_net.c` (idle probe); `gui_ui.c` (mode lock). Silent clash: `cxadc_hw_rate_khz` |
| 2026-09-06, v1.1.9 (`60f5d1d`) | `gui_ui.c` (toolbar); the stats harness `"Preview"` stub |
| 2026-09-02, v1.1.8 (`7b92f44`); `38814fe`; `e9ddaf6` | by hand, before this skill existed |

## Never

- Never rebase `main`, never merge locally into `main`, never push `--force`.
- Never resolve a conflict by taking the fork's side of an upstream-owned file.
- Never edit `.github/workflows/build.yml` to make a guard pass.
