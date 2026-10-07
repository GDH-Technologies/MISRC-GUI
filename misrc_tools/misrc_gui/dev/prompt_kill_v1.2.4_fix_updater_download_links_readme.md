# Prompt log: kill v1.2.4 + fix updater release download links + CI link test

- Date: 2026-10-03
- Input: user report — "Apps are not linking to the correct file links for
  releases" (against the just-published v1.2.4); also: kill the latest
  release, fix the update path so apps can download the new version, and
  add a CI test for it.

## Root cause (hard data)

- The in-app updater (gui_ui.c `gui_ui_build_release_asset_filename_for_
  platform`) built asset filenames as `<Platform>_MISRC_%s_<arch>.<ext>` —
  missing the `_GUI` infix — while the workflow (and android/build-apk.sh)
  publish `<Platform>_MISRC_GUI_<tag>_<arch>.<ext>` (pinned by
  check_release_artifact_naming_contract on the WORKFLOW side only).
  Result: every platform's update-check Download button opened a 404 URL
  for every release (v1.2.2/v1.2.3/v1.2.4 all use the `_MISRC_GUI_` naming;
  verified via `gh release view --json assets`).
- Verified live: `curl -I -L` on
  `.../releases/download/v1.2.3/Windows_MISRC_GUI_v1.2.3_x86.zip` → 200,
  while the app-built `Windows_MISRC_v1.2.3_x86.zip` → 404. The tag
  resolution itself (releases/latest redirect → Location → tag) works;
  only the asset names were wrong.

## Actions

- Killed release v1.2.4 including its tag
  (`gh release delete v1.2.4 --cleanup-tag`) so it can be re-cut from the
  fixed commit. (Note: `releases/latest` stayed edge-cached to the deleted
  v1.2.4 for ~5 min before re-resolving to v1.2.3 — expected GitHub
  behavior, worth knowing when timing a re-cut.)
- Fixed the mapping: all 6 patterns in gui_ui.c now carry the `_GUI` infix
  (Android_MISRC_GUI_%s_arm64.apk, macOS_MISRC_GUI_%s_universal.dmg,
  Windows_MISRC_GUI_%s_arm64.zip / _x86.zip, Linux_MISRC_GUI_%s_arm64.zip /
  _x86.zip).
- New CI test `misrc_tools/test/release_link_check.py`:
  - extracts the asset filename patterns from gui_ui.c (source of truth:
    the strings actually compiled into the app),
  - resolves the tag exactly the way the app does (releases/latest
    redirect → Location header),
  - HEAD-checks every URL the app's Download button would open (follows
    redirects, asserts HTTP 200), and
  - supports `--tag vX.Y.Z` for the release job to verify the tag it just
    published.
- Workflow wiring (.github/workflows/build.yml):
  - new `release-link-check` job (ubuntu, every trigger) — verifies the
    app's URLs against the latest PUBLISHED release,
  - new post-publish step in the `release` job — runs the check with
    `--tag <released tag>` in the same run that publishes, so a broken
    mapping cannot ship 404 links again.
- New guard `release download link contract` in ci_guard_tests.py (38th
  check): pins the exact `_GUI` patterns in gui_ui.c, forbids the broken
  `MISRC_%s_` forms, and requires the script + both workflow wirings, so
  the app side can never drift from the workflow side again.

## Verification (hard data)

- A/B on the new CI test: pre-fix run → 6/6 FAIL (HTTP 404, exactly the
  user's report); post-fix run (`--tag v1.2.3`) → 6/6 OK (HTTP 200).
- Exact CI-job command run locally (latest mode) → resolves v1.2.3, 6/6 OK.
- `ci_guard_tests.py --static-only` → 37/37 PASS (incl. the new contract).
- meson rebuild (gui_ui.c changed): 0 errors; `meson test` 12/12 OK.
- Workflow YAML parses.

## Commands run

- gh release list/view/delete v1.2.4 (--cleanup-tag), gh release view
  v1.2.3/v1.2.2 --json assets
- curl -sSI/-sSIL probes of releases/latest + real/broken asset URLs
- python3 misrc_tools/test/release_link_check.py [--tag v1.2.3]
- python3 misrc_tools/test/ci_guard_tests.py --static-only
- PKG_CONFIG_PATH=.deps/install-appimage-local/lib/pkgconfig meson
  compile/test -C build-ci-check
- git add/commit/push; gh workflow run dispatches (see below)

## Changed files

- misrc_tools/misrc_gui/ui/gui_ui.c (6 asset filename patterns + `_GUI`)
- misrc_tools/test/release_link_check.py (new CI test)
- misrc_tools/test/ci_guard_tests.py (new guard + registration)
- .github/workflows/build.yml (release-link-check job + release-job
  post-publish verification step)
