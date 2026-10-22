# Prompt log — CI guard break on main + local build gate + CI-on-push (2026-10-19)

Session follow-up of the 2026-10-19 CVBS cleanup session (`80f7c18` was HEAD and
pushed). User input this session:

1. "Make some hard dev notes about all these fixes I am happy with this, commit
   and push with logs." (backlog: player drag-and-drop; Windows move/resize
   freeze; sync robustness on real 4:3/Philips media; separate SECAM CVBS mode
   — already recorded in `cvbs_decoder_realtime_sync_rules.md` at `80f7c18`.)
2. "Something broke the GH actions building when I went to test — fix that and
   update the CI test and guardrails for local building testing."

## Diagnosis (commands + findings)

- `git status --short; git log --oneline -8`: clean tree (only untracked debug
  litter + the untracked `misrc_tools/misrc_gui/dev/rp2350_firmware_timing_fix.md`);
  HEAD `80f7c18` = origin/main.
- `gh run list --repo harrypm/MISRC-GUI --limit 6`: the manual dispatch
  (run 39312753593, head `80f7c18`) FAILED in 37 s; the last green runs were PRs.
- `gh run view 39312753593 --log-failed`: ALL FOUR guard jobs failed at the
  static guard step — `optional-dep guard consistency`:
  `gui_playback.c:4608/4609` `decoder_a/b`, `io_a/b`; `4699/4753/4755/4762`
  `duration_seconds`, `has_tags`, `tags` — members declared inside
  `#if LIBFLAC_ENABLED` but referenced outside. Every build job skipped.
- Guard-region map of `gui_playback.c` (python scan of `#if LIBFLAC_ENABLED`
  / `#else` / `#endif`): the three channel-preparation helpers
  (`gui_playback_open_native_flac` 4604, `gui_playback_prepare_raw` 4679,
  `gui_playback_prepare_ogg` 4711) sat at file scope OUTSIDE any guard, while
  their callers (in `gui_playback_prepare`'s `#else` region) are enabled-only.
  `gui_playback_raw_open` (1348) and `playback_ogg_header_t` (1810) live inside
  the big guard (272-3416) — so `prepare_raw` broke the dep-less compile via
  the function call too (invisible to the member checker).
- `.github/workflows/build.yml` triggers: `workflow_dispatch`, `pull_request`,
  `push: tags: ['v*']` ONLY — direct pushes to main never ran CI. `e34dce9`
  (the FLAC-Chop playback reader, direct push) sat latent for a day until the
  manual dispatch. The release job is safely gated
  (`startsWith(github.ref, 'refs/tags/') || workflow_dispatch create_release`),
  so adding a main-branch push trigger builds but never publishes.
- `scripts/build-local.ps1` / `build-local.sh`: never ran the guard tests
  (the dev-notes bootstrap contract only TOLD the user to run them manually).
- `misrc_tools/meson.build`: `LIBFLAC_ENABLED=0` is a real config (line 208,
  flac absent) — the guard break was a genuine dep-less-platform compile break.
- Root `INSTALLATION.md` HAS the "Vendored deps caching model" section (53-60);
  `misrc_tools/README.md` is a pointer since `e346b59` (2026-08-21).
  `check_local_deps_cache_contract`'s fail message says "INSTALLATION.md", and
  the call passed `installation_md_path` in ALL history (verified with
  `git show` on c1b231b / 87183e8 / db13ba1^1 / db13ba1^2 / db13ba1 / HEAD) —
  but the check's 4th PARAMETER was misleadingly named `tools_readme_path`
  with a dead `tools_readme_path` variable in `main()`: a wiring trap.

## Fixes (edits made)

1. `misrc_tools/misrc_gui/input/gui_playback.c`:
   - The three channel-preparation helpers are now ONE `#if LIBFLAC_ENABLED == 1`
     block (nothing there is reachable without libFLAC).
   - The `PLAYBACK_CONV16_*` constants section (state + constants are
     dep-independent: the unguarded `conv16_state` member is reset by the
     unguarded `gui_playback_release_context`) moved OUTSIDE the guard.
2. `misrc_tools/test/ci_guard_tests.py`:
   - Checker refactor: `_scan_struct_member_source` (one stripped pass;
     declarations and/or refs) + `_struct_member_decl_baseline` (member
     declarations from ALL repo headers — a member declared unguarded in any
     header is not guard-only; kills the `duration_seconds` false-positive
     class) + `_find_unguarded_struct_member_refs(source, macro, preseed_members=...)`.
   - `check_optional_dep_guard_consistency`: per-macro lazy header baseline;
     regression doc updated (e34dce9 + 64b2171 kept).
   - New `local CI guard gate contract` (`check_local_guard_gate_contract`):
     both build-local scripts must run `ci_guard_tests.py --static-only` and
     FAIL on guard errors; `build.yml` must have `push: branches: [main]`.
   - `check_build_workflow_entrypoint_contract`: requires the exact
     `push:\n branches:\n - main\n tags:\n - 'v*'` block.
   - `check_local_deps_cache_contract`: 4th parameter renamed
     `tools_readme_path` -> `installation_md_path` (matches what it receives
     and the fail message); dead `tools_readme_path` variable removed.
     NOTE (honest record): an intermediate edit batch in this session flipped
     the call argument to `tools_readme_path`; the guard run caught it
     immediately (exactly the gate this session adds) and it was fixed to
     match HEAD/history wiring before commit.
3. `scripts/build-local.ps1`: static-guard gate after `-BootstrapOnly`,
   before `pkg-config`/deps/meson: runs `ci_guard_tests.py --static-only`,
   fails the build on nonzero exit.
4. `scripts/build-local.sh`: same gate right after `require_tools`
   (`python3` added to the required tools).
5. `.github/workflows/build.yml`: `push: branches: [main]` + tags `v*`
   (release job still tag/dispatch-gated — branch pushes never publish).
6. `misrc_tools/meson.build`: the GUI rtlsdr link now adds
   `-L<libdir-from-rtlsdr_pc.get_variable(pkgconfig: 'libdir')>` next to
   `-l:librtlsdr_static.a` (rtl-sdr is installed only under mingw64 here
   — `mingw-w64-x86_64-rtl-sdr 2.0.2-1` — while build-local.ps1 builds with
   the ucrt64 toolchain for CI parity; ucrt64 ld never searches mingw64\lib).
   CI installs no rtlsdr (RTLSDR stays disabled there). Purity caveat noted:
   the clean local fix is `pacman -S mingw-w64-ucrt-x86_64-rtl-sdr`
   (available, 2.0.2-1).

## Validation (commands + results)

1. `python misrc_tools/test/ci_guard_tests.py --static-only`:
   first run after the playback fix exposed the deps-cache wiring trap
   (see above); final run: **40/40 PASS** (incl. the new
   `local CI guard gate contract`).
2. Macro-off translation proof (the real dep-less compiler check):
   flags taken from `build-local/compile_commands.json` for `gui_playback.c`,
   `-DLIBFLAC_ENABLED=1` flipped to `0`, `gcc -fsyntax-only`:
   - first run: ERROR `PLAYBACK_CONV16_PENDING` undeclared at 4556
     (the second break — found ONLY by this proof, not the member checker);
   - after the constants move: **EXIT 0** (only pre-existing mingw header
     warnings, identical to the normal build).
3. `pwsh -File scripts/build-local.ps1`:
   - first run: guard gate PASS, 69/69 compiled, LINK FAILED
     `cannot find -l:librtlsdr_static.a` (the rtlsdr libdir mix);
   - after the meson fix: guards PASS, deps stamp reused, reconfigure with
     librtlsdr found (RTLSDR enabled), 69/69, link OK, `--smoke-test` OK,
     "OK: build path is connected and local build succeeded."
     (Non-fatal pre-existing noise: meson's "Static library 'rtlsdr' not
     found ... may not be statically linked" — expected, the .pc Libs point at
     the import lib, hence the by-filename archive link; and the python
     `-c "import ninja"` probe's ModuleNotFoundError before ninja.exe is
     found on the MSYS2 path.)
4. `meson test -C build-local`: **34/34 PASS** (incl. `gui_cvbs_baseband`).

## Restore point

`C:\Users\Harry\MISRC-GUI-restore-points\ci-guard-local-build-gate-2026-10-19\`
contains a copy of the dev note and a zip of every changed file
(gui_playback.c, ci_guard_tests.py, build-local.ps1, build-local.sh,
build.yml, meson.build, the dev notes, this log) — per the user's
restore-point rule for accepted fixes.

## Commits

- `ci-guard-local-build-gate-2026-10-19` content: the playback guard fix +
  conv16 constants move + checker/header baseline + local guard gate +
  CI-on-main-push + rtlsdr libdir -L + dev notes + README index + this log.
- rp2350 note tracked with its README index entry (from the 2026-09-11 session,
  previously untracked).

Backlog (unchanged, in `cvbs_decoder_realtime_sync_rules.md`): player-mode
drag-and-drop; Windows move/resize freeze; sync robustness on real 4:3/Philips
media; separate SECAM CVBS mode.
