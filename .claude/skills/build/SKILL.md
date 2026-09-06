---
name: build
description: Build MISRC-GUI the right way for where you are (main checkout or worktree, native or AppImage) and run exactly the checks a change demands — smoke test, guard suite, RTSP soak, separate RF and audio-monitor validation. Use before claiming any build or test result.
when_to_use: Use when asked to build, compile, test, or verify; after editing anything under misrc_tools/; and before writing "builds clean" or "tests pass" anywhere.
allowed-tools:
  - Bash(scripts/build-local.sh *)
  - Bash(scripts/build-appimage-local.sh *)
  - Bash(meson *)
  - Bash(ninja *)
  - Bash(python3 misrc_tools/test/ci_guard_tests.py *)
  - Bash(build-local/misrc_gui *)
  - Bash(pwsh -File scripts/build-local.ps1 *)
  - Bash(python misrc_tools/test/ci_guard_tests.py *)
  - Bash(build-local/misrc_gui.exe *)
  - Bash(ls *)
  - Bash(git diff --name-only *)
---

# Building and proving a change

## Where am I?

| Checkout | Deps | Build command |
| --- | --- | --- |
| Main checkout `~/Repos/MISRC-GUI` | `.deps/install*` is real | `scripts/build-local.sh` |
| Hook-built worktree | `.deps/install*` are symlinks to the main checkout's | `scripts/build-local.sh` (unchanged) |
| Worktree without `.deps/` | none | `PKG_CONFIG_PATH=/home/rdodge/Repos/MISRC-GUI/.deps/install/lib/pkgconfig meson setup build-local misrc_tools && ninja -C build-local` |
| Windows host (MSYS2 MINGW64 at `C:\msys64`) | `.deps/install` built by `scripts/build-deps-windows.sh` | `pwsh -File scripts/build-local.ps1 [-All] [-Clean]` → `build-local/misrc_gui.exe` |

If `.deps/` has no `install*` in the main checkout at all, run
`scripts/build-appimage-local.sh --native` there once (it populates
`.deps/install-appimage-local`). Meson's "fell back to system hsdaoh" warning is its literal
`.deps/install`-under-source check — the vendored library is what links.

Never configure into the repo root's `build/` — it is tracked source. Meson root is
`misrc_tools/`.

## Windows host

`pwsh -File scripts/build-local.ps1` is upstream's entry point and mirrors `build.yml`'s
`windows-exe` job: it pip-installs user-level meson/ninja if missing, runs
`scripts/build-deps-windows.sh` under MSYS2 MINGW64 (static libuvc + hsdaoh + raylib into
`.deps/install`, stamp-gated), then `meson setup`/`compile` and `--smoke-test`. Use `python`,
not `python3`, and the `.exe` suffix. The checkout must be LF (`git config core.autocrlf
false` in the clone) or every `.sh` breaks under MSYS2 bash. Streaming, V4L2 preview, video
record and mediamtx supervision are compiled out on Windows (`#else` stubs), so
`--rtsp-*`, `--preview-*`, `--video-*` and `--mediamtx-test` prove nothing there; the
Windows checks are `--smoke-test`, `--static-only`, `--post-build`, the CLI `-h` exit-1
checks and a manual GUI launch. `objdump` for the post-build hsdaoh-link guard is in
`C:\msys64\mingw64\bin`.

## Native vs AppImage

- **Native** (the everyday path): `scripts/build-local.sh [--all] [--clean]` → `build-local/misrc_gui`,
  then runs `--smoke-test`. `--all` also builds `misrc_capture` and `misrc_extract`.
- **AppImage** (release parity): `APPIMAGE_BUILD_IMAGE=misrc-appimage-build:22.04
  scripts/build-appimage-local.sh`. Container, runs as root on a host-owned mount, can leave
  `.deps/.tmp/.ci-artifacts` root-owned on failure. Ask before running it.

## Which checks, by what changed

`git diff --name-only origin/main...HEAD` decides:

| Changed | Run | Report |
| --- | --- | --- |
| anything | `build-local/misrc_gui --smoke-test`; `ci_guard_tests.py --static-only` | exit code; `N/N` guards |
| a built target | `ci_guard_tests.py --post-build --gui-path build-local/misrc_gui` | guard count incl. binary-introspection |
| `input/`, `processing/`, `common/buffer_manager*`, `output/gui_record*` | + `build-local/misrc_gui --rtsp-soak`; then validate **RF** (waveform present, stable) and **audio monitor** (`Audio Mon` audible, `BUF_CAPTURE_AUDIO` not pinned at 0%) as two separate checks | soak's RF throughput and recorder frame counters stream-off vs stream-on |
| `streaming/`, `gui_video_record*`, `gui_preview_v4l2*` | + `--rtsp-soak`, `--mediamtx-test`, `--rtsp-fault-test`, `--video-tap-test` | each exit code |
| `net/` | + `--config <server.json>` / `--config <client.json>` pair on localhost per `PROMPT_SERVER_CLIENT_README.md` | `[NET]` log lines quoted |
| `ui/`, `gui_ui_scale*` | + the UI-scale harness runs inside the guard suite | guard names that cover it |
| `.github/workflows/selfhosted-deploy.yml` | push the branch; watch the wm run (`gh run watch`) | run URL and conclusion |
| `misrc_tools/meson.build` | `scripts/build-local.sh --clean` (reconfigure from scratch) | that it reconfigured |

## Reporting

Real commands, real exit codes, real numbers. Say what was **not** run and why. PR CI builds
on wm only; if a change is upstream-bound, say which of upstream's six targets you could not
exercise. A build that only did `-fsyntax-only` is not a build.
