# DEV NOTE (HARD): CI guard break on main — optional-dep guards, local build gate, CI-on-push

Status: incident-driven (2026-10-19: a manual workflow_dispatch run launched to
test failed in ~40 s at the preflight guards and every build job was skipped).
Read this before touching `gui_playback.c`'s optional-dep guard regions,
`misrc_tools/test/ci_guard_tests.py`, `scripts/build-local.ps1`/`build-local.sh`,
`.github/workflows/build.yml` triggers, or the `meson.build` static link flags.

Companion notes: `cvbs_decoder_realtime_sync_rules.md` (the playback reader this
note builds on), session log `prompt_logs/2026-10-19_ci-guard-local-build-gate_README.md`.

## The incidents (why this note exists)

1. THE TEST CI RUN WAS RED ON ALL FOUR GUARD JOBS. e34dce9 (the FLAC-Chop
   playback reader) was pushed straight to main with the three channel-preparation
   helpers (`gui_playback_open_native_flac`, `gui_playback_prepare_raw`,
   `gui_playback_prepare_ogg`) OUTSIDE `#if LIBFLAC_ENABLED == 1`: they touch the
   guarded ctx members (`decoder_a/b`, `io_a/b`) and call functions defined inside
   the guard (`gui_playback_raw_open`, the Ogg header model). The optional-dep
   guard check failed on all four guard jobs and every build job was skipped. The
   break sat latent for a day because the push trigger was tags-only: direct
   pushes to main ran NO CI at all. Fixed: the three helpers are one
   `#if LIBFLAC_ENABLED == 1` block — nothing there is reachable without libFLAC
   (`gui_playback_prepare` rejects startup entirely when LIBFLAC_ENABLED != 1).
2. A SECOND BREAK THE MEMBER CHECKER CANNOT SEE. The optional-dep checker only
   models struct-member references. The LIBFLAC_ENABLED=0 translation proof
   (rule 3 below) found `PLAYBACK_CONV16_*` defined INSIDE the guard while
   `gui_playback_release_context` (unguarded) resets `ctx->conv16_state` — a
   plain identifier, invisible to the member checker. Fixed: the constants
   section moved OUTSIDE the guard (the `conv16_state` member and the constants
   are dep-independent by design: FLAC frames and raw s16/WAV16 reads share the
   sticky conversion detector).
3. THE LOCAL LINK: `cannot find -l:librtlsdr_static.a`. build-local.ps1 builds
   with the ucrt64 toolchain (CI parity), but this machine has rtl-sdr installed
   only under mingw64 (`mingw-w64-x86_64-rtl-sdr`): pkg-config finds the mingw64
   .pc through the appended pkgconfig dirs, ucrt64 ld's default search path never
   contains mingw64\lib, and the by-filename archive link broke. Fixed: the GUI
   link adds the .pc's OWN libdir (`rtlsdr_pc.get_variable(pkgconfig: 'libdir')`)
   next to `-l:librtlsdr_static.a`. CI is unaffected (CI installs no rtlsdr:
   RTLSDR stays disabled there). Purity caveat: that links mingw64's
   msvcrt-based archive into a ucrt64 build; the clean local fix is installing
   `mingw-w64-ucrt-x86_64-rtl-sdr` (available, 2.0.2-1) so pkg-config resolves
   the ucrt64 .pc first.
4. CHECKER FALSE POSITIVES. File-local-only member analysis flagged
   `info->duration_seconds` (`playback_file_info_t`, declared UNGUARDED in
   gui_playback.h) because `playback_vorbis_rf_tags_t` (guarded, in the .c) also
   declares that member name. The checker now resolves member declarations
   across the .c AND the repo headers: a member declared unguarded in any
   header is not guard-only. (Same session: the deps-cache guard's 4th
   parameter was misleadingly named `tools_readme_path` while receiving the
   repo-root INSTALLATION.md; renamed to `installation_md_path` and the dead
   `tools_readme_path` variable in `main()` removed — the exact wiring trap.)

## HARD RULES

1. DIRECT PUSHES TO MAIN RUN CI. `push: branches: [main]` (plus the v* tags) is
   a contract (`build workflow entrypoint contract` asserts the exact block): a
   tags-only push trigger let a guard break sit on main for a day. The release
   job stays tag/workflow_dispatch-gated, so branch pushes build but never
   publish.
2. LOCAL BUILDS RUN THE STATIC GUARDS FIRST. `scripts/build-local.ps1` and
   `scripts/build-local.sh` run `python misrc_tools/test/ci_guard_tests.py
   --static-only` before any deps/meson work and FAIL the build on guard errors
   (`local CI guard gate contract` asserts both scripts + the fail path). No
   guard break reaches a push from a local build again.
3. A MEMBER-REF CHECKER IS NOT A DEP-LESS COMPILER. Any change touching
   optional-dep guard regions gets the real proof: translate the touched TU
   with the macro off (gcc -fsyntax-only -DLIBFLAC_ENABLED=0 — or the macro in
   question = 0 — with the exact flags from build-local's compile_commands.json).
   The conv16 break (a plain identifier) was invisible to the member checker;
   only the translation proof caught it.
4. OPTIONAL-DEP MEMBER DECLARATIONS RESOLVE ACROSS .c + HEADERS. Struct types
   legitimately live in headers regardless of whether the header mentions the
   macro; a member declared unguarded in a header
   (`playback_file_info_t.duration_seconds`) is not guard-only. Keep the header
   baseline (`_struct_member_decl_baseline`) when touching the checker.
5. BY-FILENAME STATIC ARCHIVE LINKS CARRY THEIR OWN -L. `-l:libX_static.a` link
   steps must add the pc's libdir (`dep.get_variable(pkgconfig: 'libdir')`);
   never rely on the compiler environment's default search path — the .pc can
   resolve from a different MSYS2 environment than the toolchain (rtlsdr:
   mingw64 .pc, ucrt64 gcc).

## Verification procedure (any guard/checker/trigger/local-script change)

1. `python misrc_tools/test/ci_guard_tests.py --static-only` — all PASS
   (40/40 after 2026-10-19, including `local CI guard gate contract`).
2. The macro-off translation proof for touched TUs (rule 3) — EXIT 0.
3. `pwsh -File scripts/build-local.ps1` — guards PASS, deps stamp reuse, meson
   compile + link + `--smoke-test` OK.
4. `meson test -C build-local` — 34/34.
5. Push; the CI run the push itself now triggers must be green (guards + all
   platform builds) before the change is considered closed.

## Backlog

The four user-prioritised playback/decoder items live in
`cvbs_decoder_realtime_sync_rules.md` (player-mode drag-and-drop; Windows
move/resize freeze; sync robustness on real 4:3/Philips-pattern media; separate
SECAM CVBS mode) — unchanged by this note.
