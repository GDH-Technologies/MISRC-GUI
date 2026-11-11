# Prompt log — git pull + build with new updates (2026-10-21)

Host: Linux Mint, `/home/harry/MISRC-GUI` (local native build, not the Windows box).

## User input this session

1. "git pull and build with the new updates"

## Commands run + results

1. `git -C /home/harry/MISRC-GUI pull`
   - Fast-forward `main`: `0cfbc19` -> `dd3bdf4`
   - 83 files changed, 15,519 insertions(+), 1,691 deletions(-)
2. `git --no-pager log --oneline 0cfbc19..dd3bdf4` — 16 new commits:
   - dd3bdf4 SECAM FM-chroma decoder + System selector + panel view-state cache
   - 090c9a4 Windows: keep rendering during move/resize (modal-loop freeze fix)
   - a8c69cf Playback drag-and-drop: per-channel panel zones + audio monitor source
   - 1929c57 CI guards: playback reader dep-less fix, local static-guard gate, CI on main pushes
   - 7bfa4e9 dev: RP2350 dual_external_adc video timing hard note
   - 80f7c18 CVBS decoder: real-time throughput, stream-jump hardening, per-line DC clamp
   - f1ce105 FFT panel: Linear/Log frequency-axis toggle, full-word labels
   - bd311e0 CVBS: panel-rate hue fixes, render-path R/B swap, GUI output verification rules
   - 83a97e8 CVBS baseband decoder: real-signal rate/threshold fixes + full-frame hybrid
   - 27f95b5 RF rate provisions to 200 MSPS + output stdio buffer dev note
   - db13ba1 Merge PR #9 (output stdio buffer fix)
   - e34dce9 Viewer chrome hide, full-frame VHS FM hybrid CRT levels, FLAC-Chop playback reader
   - 5d72391 Windows MS2130 hsdaoh video via Media Foundation
   - 69e5386 Merge PR #8 (FLAC unicode paths)
   - 0994d3f 8 MB stdio buffer for capture output files
   - 87183e8 Unicode file I/O + UI text on Windows and Linux
3. `bash scripts/build-local.sh --all` (CI guards -> deps prefix detect -> meson setup
   build-local --wipe -> compile misrc_gui + misrc_capture + misrc_extract -> smoke):
   - CI guard tests (static): PASS
   - meson setup + compile: SUCCESS, no compile errors
   - Build warnings (pre-existing style): repeated `-Wcast-function-type` on the
     `pthread_create` cast in threading.h, unused functions/vars/params, `/*` inside
     a comment in gui_capture.c, shadowed var in gui_audio.c, ignored region pragmas,
     serial LTO compile. Meson notes: `win_subsystem` needs Meson 0.56 (min is 0.54);
     RTL-SDR support omitted (`librtlsdr` not found on this machine).
   - Smoke test `misrc_gui --smoke-test`: PASS (script)
   - `misrc_capture -h` / `misrc_extract -h` Usage checks: PASS

## Post-build verification (hard data, not assumed)

1. Binary timestamps (`ls -la --time-style=full-iso`):
   - `build-local/misrc_gui`    3,984,040 bytes  2026-10-21 12:55:50
   - `build-local/misrc_capture` 101,168 bytes  2026-10-21 12:55:02
   - `build-local/misrc_extract`  41,272 bytes  2026-10-21 12:55:01
   All fresh from this build.
2. Independent smoke re-run: `build-local/misrc_gui --smoke-test` -> `SMOKE_EXIT=0`.
3. hsdaoh source check (`meson-logs/meson-log.txt`):
   - Link line: `-L/home/harry/MISRC-GUI/.deps/install-appimage-local/lib -lhsdaoh`
     (i.e. the VENDORED appimage-local prefix, not system), `hsdaoh found: YES 0.1.0`.
   - The `misrc_tools/meson.build:172` WARNING ("Vendored hsdaoh not found in
     .deps/install; falling back to system hsdaoh") is cosmetic here: that check
     looks only at `.deps/install`, while build-local.sh resolved hsdaoh from
     `.deps/install-appimage-local` via PKG_CONFIG_PATH. Actual link is vendored.

## Not done / notes

- No code changed this session (pull + build only), so no restore-point zip was made.
- New GUI features in this build (SECAM FM-chroma decoder, System selector, panel
  view-state cache, FFT Linear/Log axis, playback drag-and-drop zones + audio
  monitor, Unicode paths) are compiled in and smoke-tested, but NOT yet exercised
  in a real GUI session on this machine — real-signal verification still pending.
- `build-local` was reconfigured with `--wipe` by the script (existing dir had
  coredata.dat); full rebuild, not incremental.
