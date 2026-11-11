# Prompt log: releases download fix (releases page) + auto-patch update manifest + confirmed self-update + CI download test

- Date: 2026-10-22
- Repo: /home/harry/MISRC-GUI (branch main, HEAD dd3bdf4 at start)
- Plan artifact: "Releases page link + auto-patch update manifest + confirmed self-update + CI download test" (created this session)

## Input (user requests, in order)

1. "Releases download is broken again... just send users to
   https://github.com/harrypm/MISRC-GUI/releases/.... also add a mechnanisum to
   push a auto patch out for looking/pulling new releses"
2. "Add a CI test for downloads checking"
3. Clarification answers (ask_user_question):
   - What was broken: "Download button opened a 404 / wrong asset URL"
   - Auto-patch design: "1 & 3 but users have to confirm and trigger (old binary
     is renamed/archived not deleted) (user-provided)" - i.e. BOTH the remote
     manifest (option 1) AND full self-update with user confirmation + archived
     old binary (option 3).

## Research findings (hard data, pre-change)

- Current source's Download button (gui_ui.c gui_ui_build_release_asset_*)
  built `<Platform>_MISRC_GUI_<tag>_<arch>.<ext>` URLs; live check
  (release_link_check.py) against latest release v1.2.6: 6/6 HTTP 200; GitHub
  API asset names match exactly. So the wild 404s came from binaries whose
  compiled-in mapping drifted - the failure class is the DESIGN (compiled-in
  asset naming), not a specific bad pattern.
- Prior history (misrc_tools/misrc_gui/dev/prompt_kill_v1.2.4_fix_updater_
  download_links_readme.md): v1.2.4 shipped missing `_GUI` infix -> every
  platform 404; fixed by pinning patterns + CI test. The class recurred
  because installed binaries can never be fixed retroactively.
- Android's update check never actually worked (no curl binary on Android;
  popen("curl") fails gracefully). The new design gives Android the guided
  browser-download path (am VIEW intent), which works.

## What was implemented

1. NEW module `misrc_tools/misrc_gui/core/gui_updater.c/.h`:
   - flat-JSON manifest parser (single-level "key": "value" strings, schema int;
     no escapes/nesting by contract), manifest fetch (curl popen),
   - tag resolution via the manifest's tag_lookup_url (releases/latest
     redirect -> Location -> tag; compiled default fallback),
   - semver compare + dev-build text fallback, tag safety, {tag} asset
     filename/URL building from the manifest (NO compiled-in patterns),
   - confirmed self-update pipeline (desktop): resolve own path ($APPIMAGE
     else /proc/self/exe; GetModuleFileNameA; _NSGetExecutablePath + .app
     root), download to the per-platform updates dir
     (~/.local/share/misrc/updates, %LOCALAPPDATA%\misrc\updates,
     ~/Library/Application Support/misrc/updates), extract (unzip / tar.exe /
     hdiutil+cp), archive old by RENAME to `<path>.archived-v<oldver>`
     (never deleted; restore-on-failed-move), chmod 0755.
2. NEW repo-root `update_manifest.json` (the auto patch): releases_page,
   tag_lookup_url, download_base_url + 6 asset patterns with {tag}. Apps fetch
   it from the fixed raw URL on main
   (https://raw.githubusercontent.com/harrypm/MISRC-GUI/main/update_manifest.json,
   compiled into gui_updater.h). Fixing asset naming / page / lookup for every
   installed binary = edit this file + push to main, no release needed.
3. gui_ui.c rewired: "Download Latest" (per-platform asset URL) is GONE;
   "Releases Page" button opens the releases PAGE (manifest value or compiled
   default - cannot 404). New "Install" row + two-click confirm
   (s_update_install_armed: first click arms "Confirm install", second
   triggers), capture/record gate, install runs on the existing worker
   thread (task.install), result surfaces the installer status (archive path
   on success / kept download path on failure). Android: guided browser
   download of the manifest-named APK. Arm state resets on window close, ESC,
   backdrop, toggles, check start, and result processing.
4. NEW unit test `misrc_tools/test/gui_updater_test.c` (+ meson wiring,
   test('gui_updater')): manifest parse (valid/missing key/wrong schema/
   non-https/missing {tag}/multiline/unquoted), helpers, semver + dev
   fallback, tag extraction/safety, archive path sanitization, updates dir
   creation (isolated HOME), install download-failure path (reserved-invalid
   host, offline-safe in CI).
5. REWRITTEN `misrc_tools/test/release_link_check.py` (the CI downloads
   checking): extracts the manifest URL from the app source (gui_updater.h +
   gui_ui.c include check), fetches the LIVE raw-main manifest exactly as
   the app does (3 retries for raw CDN lag), validates structure, HEAD-checks
   the manifest's releases page, resolves the latest tag via the manifest's
   tag_lookup_url, HEAD-checks all 6 manifest-built asset URLs + the repo
   manifest against the latest release. --tag mode = repo manifest vs the
   just-published tag (release job post-publish step).
6. REWRITTEN guard `check_release_download_link_contract` in
   ci_guard_tests.py: pins the manifest file + contract, app-side wiring
   (include, page button, confirm gate, capture gate, archive rename +
   restore), FORBIDS any compiled-in asset filename pattern anywhere in the
   app (the v1.2.4-class 404 bug is now impossible by construction), requires
   the unit test + meson wiring + the live link check + both workflow
   wirings.
7. build.yml comments updated for both wirings (commands unchanged);
   README.md "In-app updates" subsection added.

## Verification (hard data, all run locally)

- meson rebuild: 172/172 linked, no errors (one new warning fixed:
  gui_updater_macos_app_root now #if __APPLE__-guarded).
- `meson test -C build-ci-check`: **34/34 OK** including the new
  `gui_updater` test (0.18s).
- `ci_guard_tests.py --static-only`: **43/43 PASS** including the rewritten
  `release download link contract`.
- `release_link_check.py --tag v1.2.6` (repo manifest vs published release):
  **6/6 OK HTTP 200** (Android/macOS/Windows x86+arm64/Linux x86+arm64).
- Full live-pipeline driver (localhost-served manifest since raw main does
  not have the manifest until this lands; everything else real):
  live manifest fetch+validate OK, releases page HTTP 200, latest tag
  resolved v1.2.6 via manifest tag_lookup_url, 6/6 live-manifest + 6/6
  repo-manifest asset URLs HTTP 200, rc=0.
- REAL-DATA install pipeline test (throwaway harness in /tmp, isolated HOME,
  the exact gui_updater_install_asset path against the real v1.2.6 Linux
  x86 asset):
  - download: real Linux_MISRC_GUI_v1.2.6_x86.zip (4,005,347 bytes) into the
    updates dir; kept there.
  - unzip -> payload staged; old "binary" archived by rename
    (fake_misrc.archived-dev-selftest, 30,824 bytes, still present).
  - replacement moved into place at the original path: static-pie ELF,
    4,475,384 bytes (the real v1.2.6 AppImage), chmod rwxr-xr-x.
  - `APPIMAGE_EXTRACT_AND_RUN=1 <replaced path> --smoke-test` -> **exit 0**
    (the installed replacement actually runs).
  - Cleanup: /tmp tree removed.

## Push + post-rebase re-verification (hard data)

- Commit created (05432fc, 11 files, +2307/-541) but the first push was
  REJECTED: remote main had moved while this session ran - PR #10
  (NTSC Betamax/SuperBeta RF preview, "JunliangRen/ntsc-betamax-preview-
  upstream") was merged as 9c3894c and TAGGED v1.2.6 (the release this
  session validated against all along; only meson.build overlapped).
- `git fetch origin` + `git rebase origin/main` -> clean, no conflicts
  (both sides' meson.build additions merged: gui_updater + beta_hifi).
- Re-verification on the merged tree: compile 0 errors; meson test
  **38/38 OK** (34 prior + 4 new from PR #10, incl. gui_updater);
  `ci_guard_tests.py --static-only` **43/43 PASS**, no FAILED/ERROR;
  `release_link_check.py --tag v1.2.6` **6/6 OK**.
- Pushed: **4b756b6** on origin/main.
- LIVE check (exact CI command) after the push, against the real raw URL:
  live manifest fetched + valid from
  https://raw.githubusercontent.com/harrypm/MISRC-GUI/main/update_manifest.json,
  releases page HTTP 200, latest tag v1.2.6 via manifest tag_lookup_url,
  6/6 live-manifest + 6/6 repo-manifest asset URLs HTTP 200 -> **the
  auto-patch mechanism is live and every URL it serves resolves.**

## 2026-10-22 follow-up: CI on the push was RED (Windows), fixed + new hard rule

- User report: "the test build failed" + request to add a GH Actions check
  to the hard dev rules.
- Hard data: `gh run list` -> runs 39789335139 + 39789161172 (both pushes)
  X; failed jobs: Windows EXE build (x86_64) + (arm64), step "Build and
  package EXE"; `gh run view --log-failed`: gui_updater.c lines 526-538:
  `struct _stat` incomplete type, `_stat` implicit declaration,
  `_S_IFDIR`/`_S_IFREG` undeclared (both MinGW toolchains; Linux/macOS/
  Android jobs green).
- Root cause: MSVC-only `_stat` API spellings in the Windows stat helpers
  (MinGW does not expose them) AND `<sys/stat.h>` included only on the
  POSIX branch.
- Fix: shared unconditional `#include <sys/stat.h>` + portable
  `stat`/`S_ISDIR`/`S_ISREG` forms in gui_updater.c (single impl, all
  platforms). Local re-verify: compile 0 errors, 38/38 meson tests,
  43/43 static guards. No MinGW cross-compiler on this machine — the CI
  matrix is the compiler of record for Windows (which is the point of the
  new rule below).
- New hard rules (dev/ci_guard_local_build_gate.md, incident 5 + rules
  6-7 + sharpened verification step 5): every push is NOT done until the
  triggered GH Actions run is verified green (gh run list -> gh run watch
  --exit-status -> gh run view --log-failed on red, fix, repeat);
  cross-platform `#if` code uses the portable API forms; never report a
  push done from local results alone, never start work on a red main.
- Fix pushed as **b3e326c** (gui_updater.c stat fix + hard rules + this
  log); its GH Actions run **39793372186 watched to completion per the new
  rule 6: CONCLUSION success, ALL 13 jobs green** - Preflight guards,
  Cross-platform guards (ubuntu/windows/macos), Windows EXE x86_64 AND
  arm64 (the broken pair; arm64's job record lagged in_progress with every
  step already success until it finalized completed/success on re-poll),
  Linux AppImage x86_64+arm64, macOS APP builds, Android APK, and the
  Release download link check (the manifest pipeline in CI).

## Not yet verified / follow-ups

- Windows/macOS install code paths are implemented + guarded but only
  statically verified here (build + guards); the real-data install proof
  above covers the shared pipeline on Linux.
- GUI behaviour (Releases Page button, Install two-click confirm, capture
  gate) still needs a real-world confirmation run by the user (per rule:
  user-interactable elements need real-world confirmation).
- The fix push's own CI run was watched to green (run 39793372186:
  success, all jobs) - recorded above.

## 2026-10-22 follow-up 2: update stuff in the CLI + automatically testable

- User request: "update stuff should also be in the CLI this should be
  automatically testable".
- Implemented:
  - gui_updater.c/.h: `gui_updater_load_manifest_from(source, ...)` - manifest
    from an https URL, an http URL on a LOOPBACK host only (127.0.0.1 /
    localhost / [::1]; the automated test's local server; any other http is
    rejected), or a local file; `gui_updater_fetch_manifest` now wraps it with
    the compiled-in URL.
  - misrc_gui.c: headless update CLI (the same pipeline as the GUI buttons,
    no window, runs before settings/elevation):
    - `--check-update`: manifest + tag fetched, prints `key: value` lines
      (update_mode/manifest_source/manifest/latest/current/update_available),
      exit 0/1;
    - `--update-dry-run`: also the platform asset URL + exe path + updates
      dir + archive path; no download, nothing replaced;
    - `--install-update`: the full confirmed install (requires `--yes`);
      prints install/install_status/archive;
    - `--update-manifest <url-or-path>`: manifest source override for
      diagnostics/automated tests; `--yes`: confirmation. Usage text updated.
  - misrc_tools/test/update_cli_test.py (new, meson `test('update_cli')`,
    drives the REAL built misrc_gui): local http server serves the manifest,
    a 302 releases/latest -> tag redirect, and a crafted asset zip; isolated
    HOME; a COPY of the binary is the "old version". Asserts: check output,
    dry-run output (asset URL per platform, exe path, archive path), the
    install REFUSAL without --yes (binary untouched), and the FULL confirmed
    install: download kept, old binary archived by rename byte-for-byte,
    payload replaced at the original path, install: ok. macOS runs
    check/dry-run/refusal only (real install needs a real DMG).
  - meson.build: misrc_gui executable captured into misrc_gui_exe + the
    update_cli test wiring (path_io-style python test).
  - ci_guard_tests.py: new `update CLI contract` guard (flags, runner,
    confirmation gate, manifest-source load + loopback carve-out, test
    assertions, meson wiring, README docs).
  - README.md: "Headless update mode" section (flags table + example).
- Verification (hard data):
  - compile 0 errors; meson test **39/39 OK** incl. `update_cli` (0.74 s:
    check + dry-run + refusal + full confirmed install against the local
    server, old binary archived, payload replaced, download kept);
  - `ci_guard_tests.py --static-only` **44/44 PASS** incl. the new
    `update CLI contract`;
  - LIVE CLI demo (no override - the compiled-in raw URL):
    `--check-update` -> manifest ok, latest v1.2.6, current
    dev-2026-10-22-aac5820-dirty, update_available true;
    `--update-dry-run` -> the real
    https://github.com/harrypm/MISRC-GUI/releases/download/v1.2.6/Linux_MISRC_GUI_v1.2.6_x86.zip,
    exe path, updates dir, archive path.

## Commands run (full list)

- ls -la /home/harry/MISRC-GUI; ls prompt_logs misrc_tools .github/workflows scripts
- python3 misrc_tools/test/release_link_check.py (pre-change: 6/6 OK v1.2.6)
- gh release view (cancelled; replaced with curl to the public API)
- git --no-pager log --oneline (updater/workflow history + recent HEAD)
- curl -sS https://api.github.com/repos/harrypm/MISRC-GUI/releases/latest
  (tag v1.2.6, 6 assets, names match the app mapping)
- grep/usages scan of gui_ui.c helpers slated for removal
- sed -i '729,953d' gui_ui.c (removed the moved helpers)
- edit_files: gui_ui.c (include, statics, thread main, task start, result
  processing, status block, buttons + install row, click handlers, arm
  resets), meson.build (source + test), ci_guard_tests.py (json import +
  rewritten guard), build.yml (two comment blocks), README.md
- PKG_CONFIG_PATH=.deps/install-appimage-local/lib/pkgconfig meson compile
  -C build-ci-check (172/172; warning fix pass)
- PKG_CONFIG_PATH=... meson test -C build-ci-check (34/34 OK)
- python3 misrc_tools/test/ci_guard_tests.py --static-only (43/43 PASS)
- python3 misrc_tools/test/release_link_check.py --tag v1.2.6 (6/6 OK)
- python3 /tmp/rlc_driver.py (full pipeline, localhost manifest, all real
  GitHub URL checks OK)
- gcc -O2 -o /tmp/.../fake_misrc /tmp/.../install_harness.c -lm; HOME=/tmp/...
  fake_misrc (real install: install_ok=1); ls/file/APPIMAGE_EXTRACT_AND_RUN=1
  --smoke-test (exit 0); rm -rf /tmp/misrc_selfupdate_test /tmp/rlc_driver.py

## Changed files

- misrc_tools/misrc_gui/core/gui_updater.h (new)
- misrc_tools/misrc_gui/core/gui_updater.c (new)
- update_manifest.json (new, repo root - the auto-patch source)
- misrc_tools/misrc_gui/ui/gui_ui.c (updater rewired to gui_updater; page
  button; confirmed self-update install button/flow/thread/result)
- misrc_tools/meson.build (gui_updater.c source + gui_updater test)
- misrc_tools/test/gui_updater_test.c (new)
- misrc_tools/test/release_link_check.py (rewritten for the manifest pipeline)
- misrc_tools/test/ci_guard_tests.py (json import; rewritten
  check_release_download_link_contract)
- .github/workflows/build.yml (comments for both link-check wirings)
- README.md (In-app updates subsection)
- prompt_logs/2026-10-22_releases-download-autopatch_README.md (this log)

## Output

Pushed as 4b756b6 on origin/main (rebased onto the merged PR #10 / v1.2.6
release commit). All verification green on the final tree: 38/38 meson tests,
43/43 guards, full live manifest pipeline 6/6+6/6 HTTP 200 vs v1.2.6, and the
real-data install pipeline proof. Remaining: user real-world GUI
confirmation; per the fix-restore-point rule a log note + zip restore point
will be created once the user confirms it is fully working.
