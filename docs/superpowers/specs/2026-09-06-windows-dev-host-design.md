# Windows development host — design

**Date:** 2026-09-06 · **Kind:** fork-only · **Status:** approved, implemented on the same day

## Problem

The fork builds and deploys only on Linux (`wm`, `cs0`) and macOS (`air0`). Upstream's
`build.yml` is disabled here, so no fork change is ever compiled on Windows, and
`docs/gdh-selfhosted-ci.md` records a near-miss (an unused-variable warning that existed only
under `#if !defined(_WIN32)`). Reece's Windows 10 PC should build, run and validate the fork
before work is proposed upstream.

## Findings that shaped the design

- Upstream already ships the Windows path: `scripts/build-local.ps1` (PowerShell) drives MSYS2
  MINGW64 at the hardcoded `C:\msys64`, auto-runs `scripts/build-deps-windows.sh` (static
  libuvc v0.0.7 + vendored hsdaoh + raylib 5.5 into `.deps/install`, stamp-gated), then
  `meson setup`/`compile` and `--smoke-test`. It mirrors `build.yml`'s `windows-exe` job.
- The machine had no C toolchain (no MSYS2, meson, ninja, cmake, gcc, pkg-config). It had
  winget, Git for Windows 2.55, Python 3.14, pwsh 7, ffmpeg 8.1.
- Git for Windows' system config sets `core.autocrlf=true` and the repo has no
  `.gitattributes`, so every `.sh` (build scripts, hooks, `git-version.sh`) checked out CRLF
  and would fail under MSYS2 bash.
- Every fork-only C source (`streaming/*`, `input/gui_preview_v4l2.c`,
  `output/gui_video_record.c`) carries a complete `#else` stub, so the fork compiles on
  Windows; streaming, V4L2 preview, video record and mediamtx are compiled out there.
- The fork's Claude Code tooling was Linux-shaped: `build-on-stop.sh` ran
  `build-local/misrc_gui` (no `.exe`) via `python3`; `worktree-deps.py` used `os.symlink`
  (needs Developer Mode on Windows); `settings.json` allowed only the `.sh`/`python3` forms.
  `guards-on-edit.py` and `ci_guard_tests.py` were already cross-platform.

## Decisions

| Question | Decision | Why |
| --- | --- | --- |
| Scope | Toolchain + build + guards on this PC, plus a fork-only PR for `.claude/` | Proves Windows without touching CI; a self-hosted Windows runner is a later scope |
| CRLF | Repo-local `git config core.autocrlf false` + re-checkout; no `.gitattributes` | Nothing rides upstream; a `.gitattributes` is a separate upstream proposal |
| Worktrees | Directory junction (`mklink /J`) fallback when `os.symlink` fails on Windows | No Developer Mode or admin needed; junctions are unlinked with `rmdir` before `git worktree remove` so nothing recurses into the main checkout's real `.deps/install` |
| Toolchain | MSYS2 MINGW64, the exact `build.yml:582-593` package list plus `libjpeg-turbo` (CI has it) and `clang-tools-extra` (clangd for the `clangd-lsp` plugin) | local == CI |

## Components

- **Machine:** MSYS2 at `C:\msys64` via winget; pacman MINGW64 packages; `core.autocrlf=false`
  in the clone; `upstream` fetched; `.deps/install` built by `build-deps-windows.sh`.
- **`.claude/hooks/build-on-stop.sh`:** `python3`→`python` fallback, `.exe` resolution, and
  self-locating `C:\msys64\mingw64\bin` when `ninja` is off PATH under Git Bash. Linux path unchanged.
- **`.claude/hooks/worktree-deps.py`:** junction fallback in `link_deps`; `unlink_junctions`
  before removal; docstring updated. `worktree-deps.test.sh` made path-agnostic (native
  Windows paths compared through `cygpath`, backslashes JSON-escaped).
- **`.claude/settings.json`:** allow-list gains the `pwsh`, `python` and `.exe` forms.
- **`.claude/CLAUDE.md`, `.claude/skills/build/SKILL.md`:** Windows host section; the
  "mediamtx on PATH for Windows" claim corrected (it is compiled out on Windows).

## Out of scope

No capture hardware on the PC (no RF/audio-monitor/soak validation). Windows arm64 not
built. No Windows leg in `selfhosted-deploy.yml`. No `.gitattributes` upstream PR.
