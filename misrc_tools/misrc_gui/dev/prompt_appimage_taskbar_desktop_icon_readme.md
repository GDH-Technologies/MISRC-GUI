# PROMPT — AppImage: stop the non-removable Desktop icon, fix taskbar identity

Session 2026-09-30. Tracking input / output / commands per project rule.

## User input
"cleanup misrc GUI always making a non-removable desktop icon file, take note from
the flac chop and tape-decode rust appimage work for taskbar"

## Reference work (read, not assumed)
- `FLAC-Chop/docs/DEV_NOTE_linux_appimage_taskbar_integration.md` (same note in
  `tape-decode-rust/docs/`), plus each project's `AppRun` and `.desktop`.
- Recipe: real `.desktop` + `AppRun` files (not YAML heredocs); window identity
  (`WM_CLASS`) must match `StartupWMClass`; AppRun installs only a user-level
  launcher + icon, only for GUI launches, only when `APPIMAGE` is a real file,
  rewrite only on change, opt-out env var, never `set -e`.

## Root cause (hard data)
1. The CI-embedded AppRun (`.github/workflows/build.yml`, heredoc) ran
   `install_shortcuts()` on EVERY GUI launch. It did, each time:
   - rewrote `~/.local/share/applications/misrc_gui.desktop`
   - created `~/.local/bin/misrc_gui.AppImage` -> AppImage symlink
   - `cp -f` the launcher to `~/Desktop/MISRC GUI.desktop`  <- the "non-removable"
     icon: delete it and the next launch copies it back.
   Evidence on the user's machine: `~/Desktop/MISRC GUI.desktop` and
   `~/.local/share/applications/misrc_gui.desktop` share mtime Sep 30 13:50
   (written together), plus an older copy `~/Desktop/Data Dumping/MISRC GUI.desktop`
   (v1.1.7) and `~/.local/bin/misrc_gui.AppImage -> ~/appimages/Linux_MISRC_GUI_v1.2.2_x86.AppImage`.
2. Window identity was version-dependent. Measured on the user's running v1.2.2
   window (by PID): `WM_CLASS = "MISRC Capture v1.2.2", "MISRC Capture v1.2.2"`.
   GLFW takes class (and instance, if `RESOURCE_NAME` is unset) from the window
   title at creation. So `StartupWMClass` had to be re-derived from
   `misrc_gui --version` at every launch and went stale every release.
   That is also why the old AppRun needed to run on every launch.

## Changes
- NEW `assets/appimage/AppRun` (chmod +x): exports `RESOURCE_NAME=misrc_gui`;
  installs launcher + 512px icon on plain GUI launches only; idempotent; no
  `~/.local/bin` symlink; no `%U` in Exec (a dropped file would have routed the
  process into CLI capture mode); `TryExec`; opt-out `MISRC_GUI_NO_INTEGRATION=1`;
  Desktop icon ONLY via explicit `--create-shortcut` (falls back to `~/Desktop`
  when `xdg-user-dir` answers `$HOME`).
- NEW `assets/appimage/misrc_gui.desktop`: `StartupWMClass=MISRC Capture`
  (version independent), `Icon=misrc`.
- `misrc_tools/misrc_gui/core/misrc_gui.c`: Linux-only `setenv("RESOURCE_NAME",
  "misrc_gui", 0)`; window created with fixed title `MISRC_WINDOW_CLASS_TITLE`
  ("MISRC Capture"), then `SetWindowTitle(versioned title)`.
- `.github/workflows/build.yml` and `scripts/build-appimage-local.sh`: inline
  AppRun/desktop heredocs replaced with `install -m` from `assets/appimage/*`;
  linuxdeploy now gets `-d AppDir/misrc_gui.desktop`.
- `misrc_tools/test/ci_guard_tests.py`: removed heredoc extraction; rewrote
  `linux desktop metadata`, `AppRun static contract`, `AppRun runtime behavior`;
  added `window identity contract`.
- `TECHNICAL.md`: new section "Linux AppImage desktop integration".

## Verification (hard data)
- `bash -n` on AppRun and build-appimage-local.sh: OK. `desktop-file-validate`
  on the template and on the generated launcher: OK. `build.yml` YAML loads
  (9 jobs intact).
- `ci_guard_tests.py` full run (incl. sandboxed AppRun simulation): all PASS.
  Sandbox env is built from scratch (HOME only) so nothing can touch the real
  Desktop. Covers: plain launch installs launcher+icon only; idempotent (mtime);
  re-point to another AppImage; GUI flags integrate; capture/extract/--smoke-test/
  --version/--help/CLI args/APPIMAGE unset/opt-out/`$` and `"` in path create zero
  files and dispatch correctly; `--create-shortcut` creates the Desktop icon and
  does not start the GUI; and the regression: a deleted Desktop icon stays deleted.
- Mutation check (guards must be able to fail): injecting the old bugs into the new
  AppRun is rejected — M1 auto-copy to ~/Desktop (runtime + static), M2 integrate
  on every launch incl. `--smoke-test`, M3 ignore the opt-out, M4 rewrite every
  launch. The OLD AppRun from `git HEAD` is also rejected.
- `meson compile` OK (only pre-existing warnings), `--smoke-test` exit 0,
  `meson test` 10/10.
- End-to-end on the user's X11 session: real built `misrc_gui` inside a temp AppDir
  with the new AppRun, sandboxed HOME, matched by PID, killed only own process group.
  Result: `WM_CLASS = "misrc_gui", "MISRC Capture"` (also when the binary is launched
  directly), `_NET_WM_NAME` still versioned, installed launcher
  `StartupWMClass=MISRC Capture`, Desktop folder empty, no `~/.local/bin`.
  The user's own running instance (PID 4012209) was not touched.

## NOT yet validated (needs the user)
- A real CI-built AppImage (this change is not committed/pushed; no CI run yet) on
  Linux Mint / Cinnamon: correct icon in the taskbar, pinning sticks, no new
  `*.cinnamon-generated.desktop`.
- The currently installed v1.2.2 AppImage still contains the OLD AppRun and will
  keep recreating `~/Desktop/MISRC GUI.desktop` and the `~/.local/bin` symlink
  until it is replaced by a build containing this change.
- Wayland: GLFW's Wayland app_id is not addressed here (this session is X11).
- linuxdeploy keeping the custom AppRun is inherited from the existing workflow
  (the v1.2.2 AppImage shows the custom AppRun ran), not re-measured.
- No restore point created: per project rule, only after the user confirms it works.

## Stale files on the user's machine (NOT deleted by this session)
- `~/Desktop/MISRC GUI.desktop` (the icon that kept coming back)
- `~/Desktop/Data Dumping/MISRC GUI.desktop` (old v1.1.7 copy)
- `~/.local/share/applications/Misrc_Gui.cinnamon-generated.desktop`
  (Exec=/home/harry/MISRC/misrc_tools/build/misrc_gui, a stale dev path)
- `~/.local/bin/misrc_gui.AppImage` (symlink from the old AppRun)
- `~/.local/share/applications/misrc_gui.desktop` self-heals: the new AppRun
  rewrites it on the next launch because its content differs.
Deleting the first two before running a build with this change is pointless if the
old AppImage is launched again (it recreates them).

## Unrelated finding (not changed)
`misrc_tools/misrc_gui/ui/gui_ui.c:364` and `:369` format `ch` (a `const char *`,
"A"/"B") with `%c`, so the CXADC rate status message prints a garbage character
(compiler warning `-Wformat`). Should be `%s`.

## 2026-10-02 follow-up: "clean up stale files and make a new appimage for me to locally test"

### Stale-file cleanup (explicit paths, nothing running)
Checked first: no MISRC process running; no Cinnamon panel/favorites/dconf entry
references any MISRC launcher. State on 2026-10-02: `~/Desktop/MISRC GUI.desktop`
was already gone (user deleted it); the old v1.2.2 AppImage had recreated the
`~/.local/bin` symlink and `misrc_gui.desktop` at 11:13 that day.
Removed (`rm -v`, exact paths):
- `~/Desktop/Data Dumping/MISRC GUI.desktop` (v1.1.7-era copy)
- `~/.local/share/applications/Misrc_Gui.cinnamon-generated.desktop` (stale dev path)
- `~/.local/bin/misrc_gui.AppImage` (symlink only; target AppImage untouched)
Kept: `~/.local/share/applications/misrc_gui.desktop` (its Exec still points at the
removed symlink; the new AppImage rewrites it on first launch), the hicolor icon,
the v1.2.2 AppImage, and the unrelated `Data Dumping/CXADC-Reload.desktop`.

### Local AppImage build
- Route: `bash scripts/build-appimage-local.sh --native` (the script is not
  executable in git; invoked via bash, mode unchanged). docker is unusable here;
  podman exists, but the default container route would fail: it installs Ubuntu
  22.04's apt libflac (1.3.3) while `misrc_tools/meson.build` hard-fails below
  libFLAC 1.5.0 (CI fetches a prebuilt 1.5.0 from harrypm/MISRC-ci-cache; the local
  script does not). The host (Mint 21.3, glibc 2.35) has libFLAC 1.5.0 in
  /usr/local, so native works. TECHNICAL.md documents the container route as the
  default; that gap is NOT fixed here.
- Result: `.ci-artifacts/linux-appimage/Linux_MISRC_GUI_dev-2026-10-02-f693070-dirty_x86.AppImage`
  (4,246,008 bytes, sha256 4479dcb359baafd62e6a2727ea2cff3be47025aaf19fa97dbf610a40de8c5af7),
  copied (not overwriting) to `~/appimages/` with identical checksum. Version string
  is `-dirty` because the change is uncommitted.
- Build log checkpoints: libFLAC 1.5.0 found, multithreading enabled; glibc floor
  (2.35) assertions passed; script's own AppImage `--smoke-test` passed.
  Pre-existing: the cached raylib checkout in `.deps/src-appimage-local` carries the
  Android-only `rlActiveDrawBuffers` patch (inside `#if GRAPHICS_API_OPENGL_ES3`,
  not compiled for desktop GL 3.3).

### Artifact verification (hard data)
- `--appimage-extract`: `AppRun` byte-identical to `assets/appimage/AppRun`
  (linuxdeploy kept the custom AppRun - this closes the earlier "not re-measured"
  item); desktop entry has `StartupWMClass=MISRC Capture`; icon 512x512 RGBA;
  bundled libs include `libFLAC.so.14` = libFLAC 1.5.0; no unresolved libs (ldd).
- Real AppImage under a sandboxed HOME (matched by session/PID, only own process
  group killed):
  - `--version`, `--smoke-test`, `--help`, `capture -h`, `extract -h`: exit codes
    0/0/0/1/1 as expected, zero files created, Desktop untouched.
  - Plain GUI launch: `WM_CLASS = "misrc_gui", "MISRC Capture"`,
    `_NET_WM_NAME = "MISRC Capture dev-2026-10-02-f693070-dirty"`; launcher
    `Exec="<AppImage path>"` (no symlink), `StartupWMClass=MISRC Capture`; icon
    installed; Desktop folder empty; no `~/.local/bin`; second launch did not
    rewrite the launcher.

### User confirmation (2026-10-02)
After testing the locally built AppImage the user reported: "fixed commit and push".
The user did not itemize what was checked, so the individual taskbar points (icon,
pinning, no new `*.cinnamon-generated.desktop`, Desktop stays clean) are recorded
as confirmed only in that one-line statement.
Restore point (host, outside the repo): `~/MISRC-GUI-restore-points/appimage-taskbar-desktop-icon-2026-10-02/`
(source zip + patch + RESTORE_NOTE.md + the tested AppImage with checksum).

### Caveats that remain
- The tested AppImage is a host-native build, not the CI container/prebuilt-libFLAC
  artifact; it bundles shared libFLAC 1.5.0 instead of CI's static one. The first
  CI-built release containing this change has not been run yet.
- `v1.2.3` already points at the commit BEFORE this change, so that release still
  ships the old AppRun (Desktop icon recreated on every launch).
- `scripts/build-appimage-local.sh`'s default container route still installs apt
  libFLAC 1.3.3 and would fail the libFLAC >= 1.5.0 check; `--native` is used.
- Pushing to main does not trigger the build workflow (tags / PRs / manual dispatch
  only).
