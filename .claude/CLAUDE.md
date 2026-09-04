# MISRC-GUI — GDH fork, project instructions

This is `GDH-Technologies/MISRC-GUI`, a fork of `harrypm/MISRC-GUI` (remote `upstream`;
`origin` and `gdh` are the same fork URL). The goal is upstream contribution and a thin fork:
every change is either bound for harrypm's repo or a GDH-only surface that must never ride
an upstream PR. `.claude/rules/upstream.md` is the policy. When this file and a script
disagree, the scripts and `.github/workflows/selfhosted-deploy.yml` are ground truth.

## Direction

GDH is consolidating on MISRC-GUI as its capture engine and operator seat, retiring the
Python `capture-node` service in stages (dev environment → capture-node's fate → decouple
`digitization-toolkit` → adapt the toolkit to MISRC-GUI). `../capture-node/docs/
misrc-gui-comparison.md` is the prior analysis. Keep GDH specifics (hostnames, fleet,
archival policy) out of any code that could go upstream.

Fork-only surfaces: `misrc_tools/misrc_gui/streaming/`, `misrc_tools/misrc_gui/input/
gui_preview_v4l2.c`, `misrc_tools/misrc_gui/output/gui_video_record.c`,
`misrc_tools/misrc_gui/visualization/gui_preview_panel.c`,
`.github/workflows/selfhosted-deploy.yml`, `docs/gdh-*`, `docs/superpowers/`,
`scripts/fetch-mediamtx.sh`, `.claude/`, `.clangd`.

## Commands

```bash
scripts/build-appimage-local.sh --native   # once: build vendored hsdaoh/raylib into .deps/install-appimage-local
scripts/build-local.sh [--all] [--clean]   # native meson build + smoke test -> build-local/misrc_gui
build-local/misrc_gui --smoke-test         # exit 0 = init + teardown OK, no window needed
python3 misrc_tools/test/ci_guard_tests.py --static-only                                  # the guard suite
python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path build-local/misrc_gui  # + binary introspection
build-local/misrc_gui --rtsp-soak          # capture-path acceptance: RF throughput stream-off vs stream-on
```

In a worktree the `WorktreeCreate` hook symlinks `.deps` from the main checkout, so the same
commands work. Without the symlink:
`PKG_CONFIG_PATH=/home/rdodge/Repos/MISRC-GUI/.deps/install/lib/pkgconfig meson setup <scratch> misrc_tools`.
Meson then warns it "fell back to system hsdaoh" — that is its literal `.deps/install`-under-
source check; the vendored library is what links.

## Layout

- Meson root is **`misrc_tools/`**, not the repo root. Root `build/` is tracked source
  (`build-static.sh`, `install-udev-rules.sh`) — never configure into it. Build dirs are
  `build-local/`, `build-fedora/` (gitignored).
- `misrc_tools/misrc_gui/` — `core` (app, settings, main loop) · `input` (hsdaoh, cxadc +
  clockgen, DdD, v4l2 preview) · `processing` (extraction) · `output` (FLAC/raw/WAV record,
  video record) · `signal` (headswitch lock, demod) · `streaming` (mediamtx supervisor, RTSP
  publisher, tap mux — fork) · `net` (server/client mode — upstream, unproven) · `ui`
  (raylib + Clay) · `visualization` · `dev/dev_notes_README.md` (upstream's constraints).
- `misrc_tools/common/` — shared capture core: `buffer_manager`, `flac_writer`, `wave`.
- `third_party/{hsdaoh,tape-decode-rs}` — vendored; changes go to their own upstreams.
- `.deps/` exists only in the main checkout. `librtlsdr` is optional (not installed here;
  the RTL-SDR backend compiles out, the Demod panel still builds).

## Upstream relationship

- `git fetch upstream`. Sync with `/sync-upstream` (branch `chore/merge-upstream-YYYY-MM-DD`,
  merge `upstream/main`, guards + smoke, PR). Upstream moves fast (~80 commits/month, one
  maintainer): sync weekly and before any upstream PR.
- Upstream PRs go through `/upstream-pr`: branch from `upstream/main`, cherry-pick, no
  fork-only paths in the diff, honour `.github/CI_RULES.md` (six targets — ubuntu AppImage,
  Windows x64 + arm64, macOS arm64 + Intel, Android APK; version strings only via
  `misrc_tools/git-version.sh`; actions pinned `@vN`, no retired runner images).
- **Never edit `.github/workflows/build.yml`.** It is upstream's, kept byte-identical and
  disabled as a repo setting (`gh workflow disable`), and `ci_guard_tests.py`
  substring-matches it. Fork CI lives in `selfhosted-deploy.yml`.
- `PROMPT_*_README.md` are harrypm's agent working logs: read, never edit.

## CI and deploy (fork-only)

`docs/gdh-selfhosted-ci.md`. `selfhosted-deploy.yml` runs on the org's own runners: `wm`
(Linux, every event), `cs0` (Linux, `main` + tags only), `air0` (macOS ARM64, `main` + tags
only, builds `~/Applications/MISRC.app`). PRs build and guard-test on wm only. `bump-tag`
mints `v<upstream>-gdh.N` before the build legs so both resolve the same version. Installs
are atomic into `~/.local/bin`; the GNOME launcher's `StartupWMClass` must equal
`GUI_WINDOW_CLASS_NAME` (guarded).

## Testing

- The guard suite is the real test surface: `misrc_tools/test/ci_guard_tests.py`, 55
  registered checks, several of which compile and run C harnesses in
  `misrc_tools/test/*_harness.c`. Meson has only three `test()` targets (DdD).
- `--smoke-test` after every build. `--rtsp-soak` is the acceptance test for anything that
  touches the capture path; run it and quote the numbers.
- Headless modes for automation (`misrc_gui --help`): `--auto-record`, `--config <path>`
  (settings from a file; how the net-mode server/client tests are driven), `--device-list`,
  `--mediamtx-test`, `--preview-{probe,probe-stream,selftest,dump-frame,only,format,parent-pid}`,
  `--rtsp-{stream-test,fault-test,soak}`, `--video-{probe,tap-test,record-test,name-test,settings-test}`.
- No formatter — there is no `.clang-format` upstream or here. Match the surrounding code.
- Two hooks enforce this automatically (`.claude/settings.json`): `guards-on-edit.py` runs
  `--static-only` after any edit to a guard-matched file (the deploy workflow, `meson.build`,
  `scripts/build-*`, `git-version.sh`, the suite and its harnesses); `build-on-stop.sh` refuses
  to end a turn while C sources differ from `origin/main` until an incremental build and
  `--smoke-test` pass. `MISRC_GUARDS_ON_EDIT=0` / `MISRC_BUILD_ON_STOP=0` disable them for a
  session.
- Agents: `capture-path-verifier` (runtime checks), `dev-notes-auditor` (static check of the
  five constraints below), `upstream-diff-curator` (the gate before `/upstream-pr`). Skills:
  `/build`, `/guard`.

## Gotchas

Upstream's standing constraints (`misrc_tools/misrc_gui/dev/dev_notes_README.md`; small
callback-gating edits have silently broken GUI feeds before):

- MISRC frame mode: only drop frames when `result.error_count > 0 && result.report_errors`.
  Rejecting tolerated CRC-only frames stalls the GUI RF feed while the CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null checks, before
  width/height early returns), or you get false timeout/reconnect loops.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start, restore
  `atomic_store(&s_capture_handler.capture_audio, true);` or the audio-monitor path is empty.
- Validate RF and monitor audio as separate checks after capture-path edits.
- Minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`, `gui_audio`.

Fork-side:

- FLAC ≥ 1.5.0 is a hard floor in `misrc_tools/meson.build`. `flac_writer` reserves a 4096-byte
  PADDING block so post-capture tags are an in-place write (a 143 GB rewrite died at 71 GB
  once), and sets STREAMINFO `total_samples` to 0 past 2^36 (libFLAC wraps; ~28.6 min at 40 MSps).
- Record path: `BUF_RECORD_A/B` wait up to 1 s, then spill to a disk temp file (sticky per
  channel, ordered). Only a failed spill is a real drop, and it stops the capture only when
  `stop_on_dropout` is on (default off). Tape-end is `level_autostop_enabled`, a separate switch.
- The net server/client mode (`misrc_gui/net/gui_net.c`, upstream 752182c) is unproven: the
  `/rf` fanout replays, leaks and loses wakeups, and `server_stop()` frees state under live
  client threads. Do not build on it until rewritten (#42).
- AppImage container build: `APPIMAGE_BUILD_IMAGE=misrc-appimage-build:22.04
  scripts/build-appimage-local.sh` — stock `ubuntu:22.04` fails at `meson setup` (apt libFLAC
  1.3.3). The image is in `~/misrc-appimage-build/`, outside the repo. The container runs as
  root on a host-owned mount; a failed run can leave `.deps`, `.tmp`, `.ci-artifacts` root-owned.
- mediamtx ports are canonical+100 (RTSP 8654) so the fork never collides with capture-node's
  `tier×10000+8554` instances on the same hosts.

## Worktrees and git hygiene

- Worktrees live under `<main>/.claude/worktrees/<topic>/` and are created only by
  `EnterWorktree` / `claude --worktree`; the global location guard refuses anywhere else. The
  `WorktreeCreate` hook (`.claude/hooks/worktree-deps.py`) branches from `origin/main` and
  symlinks `.deps`. `.claude/rules/worktrees.md` is the rule.
- Remove through `ExitWorktree`; the hook refuses a dirty tree or commits not on `origin/main`.
- Never `git stash` — the stack is shared with Reece's live work; make a WIP commit. Stage
  explicit paths, never `git add -A`. Reece edits and commits in these same trees during a
  session: re-check `git status` before touching a file you have not edited recently.
- PRs target `main`. Never merge locally into `main`; never push `--force`.

## GitHub

Conventions in `.claude/rules/github.md`; procedures in `/file-issue`, `/close-issue`,
`/open-pr`, `/sync-upstream`, `/upstream-pr`, `/release-notes` (user-invoked). Hooks append
provenance footers and warn on Actions outages; never hand-write either.

## External tools expected on PATH

`meson`, `ninja`, `pkg-config`, `gcc`, `python3`, `ffmpeg`, `ffprobe`, `v4l2-ctl`,
`arecord`/`amixer`, `mediamtx` (bundled into the AppImage by `scripts/fetch-mediamtx.sh`; on
PATH for macOS/Windows), `clangd` (the `clangd-lsp` plugin; the root `.clangd` points it at
`build-local/compile_commands.json`, so build once before expecting diagnostics).
