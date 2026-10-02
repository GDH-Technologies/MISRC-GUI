# Prompt log: fix + reinforce Actions workflows (failed run 37035393223)

- Date: 2026-10-02
- Input: https://github.com/harrypm/MISRC-GUI/actions/runs/37035393223
  ("Build and release binary", workflow_dispatch on main @ 3bf7ff7)
- Task: fix the failed jobs and reinforce the workflows so the failure
  class cannot silently pass on the platforms that did not catch it.

## Run 37035393223 findings (hard data from `gh run view` logs)

- Failed jobs (all other jobs green):
  - `Windows EXE build (x86_64)` — "Build and package EXE":
    `misrc_tools/misrc_gui/output/gui_record_direct.c:146:33: error: implicit
    declaration of function 'malloc'` and `:284:5 ... 'free'`.
  - `Windows EXE build (arm64)` — same file, same errors (clang wording:
    "call to undeclared library function").
- Annotations:
  - `[msystem-mingw64] MINGW64 is deprecated. Migrate to UCRT64 or CLANG64.`
  - arm64 meson setup logged `libsoxr not found, building without resample
    support` (x86_64 had libsoxr) — silent per-arch feature gap.
- Root cause of the asymmetry: `common/threading.h` includes `<stdlib.h>`
  only in its POSIX branch (Windows branch gets `<process.h>`), so glibc
  builds (Linux CI, local gcc 11) get malloc declared transitively while
  MSYS2 builds do not. ubuntu-22.04 gcc 11 / Apple clang / NDK clang only
  WARN on implicit declarations; MSYS2 GCC/clang treat them as hard errors.
  The bug class is invisible on every platform except the one it breaks.
- `gui_record_direct.c` was introduced in 3bf7ff7 (the only commit since the
  last green Windows run, 36542940058 @ 2026-09-29), so it was the only new
  Windows-compiled TU that could carry the regression.

## Fixes

- `misrc_tools/misrc_gui/output/gui_record_direct.c`: added
  `#include <stdlib.h>` (the CI failure).
- `misrc_tools/common/ringbuffer.c`: hoisted `#include <string.h>` out of
  the POSIX `#else` branch to top level — `memcpy` at rb_put() is top-level
  (all-platform) code that previously relied on POSIX-branch includes plus
  windows.h transitive leaks. Same landmine class, found by the new guard.
- `misrc_tools/misrc_gui/ui/gui_ui.c`: `%c` -> `%s` in the CXADC cycle-rate
  status text (366/371) — `ch` is `const char *` ("A"/"B"); `%c` printed a
  garbage byte in the status bar. Surfaced by the harness builds during
  validation.

## Reinforcements

- `misrc_tools/meson.build`: promoted the GCC-14/clang-16 default-error set
  to `-Werror=` on every platform: `implicit-function-declaration`,
  `implicit-int`, `int-conversion`, `incompatible-pointer-types`. Old
  toolchains (gcc 11, Apple clang, NDK clang 14) now fail at the first
  compile instead of only the MSYS2 jobs.
- `misrc_tools/test/ci_guard_tests.py`: new `libc direct-include contract`
  preflight guard (include-what-you-use, libc subset). Top-level (all-
  platform) code calling a libc function must include the header itself or
  get it from a project header that includes it OUTSIDE any platform-
  conditional `#if`. Platform-conditional provision (threading.h's
  POSIX-only `<stdlib.h>`) does not count — that is exactly what compiles
  on glibc and breaks on MinGW. Runs in the 10-second preflight on every
  platform, before any deps build. A/B verified: pre-fix it flagged exactly
  gui_record_direct.c (stdlib) and ringbuffer.c (string); post-fix clean.
- `.github/workflows/build.yml`:
  - `windows-exe` (x86_64) migrated MINGW64 -> UCRT64 (deprecation
    annotation): msystem + `mingw-w64-ucrt-x86_64-*` package lists in both
    the cached and retry steps. Keeps the gcc code path (meson.build's
    -lgomp/getgetopt selection is compiler-based, not msystem-based).
  - Deps cache key now names the toolchain
    (`deps-windows-ucrt64-x86_64-...`) so a stale MINGW64-built (msvcrt)
    .deps/install can never be reused by the UCRT64 job.
  - `windows-exe-arm64`: added `mingw-w64-clang-aarch64-libsoxr` to both
    install lists (resample parity with x86_64; package verified on
    packages.msys2.org — clangarm64 repo, ships soxr.pc + libsoxr.a).
- `misrc_tools/test/ci_guard_tests.py`: new `MSYS2 toolchain policy` guard:
  forbids `msystem: MINGW64`, requires `UCRT64`/`CLANGARM64` + libsoxr in
  both Windows install lists + the toolchain-named cache key. Also updated
  `workflow FFT dependency policy` to the ucrt64 fftw package.
- Local==CI parity chain migrated: `scripts/build-deps-windows.sh`
  (ucrt64-first MINGW_PREFIX detection, stamp now records the toolchain
  prefix so a MINGW64<->UCRT64 switch busts the local deps stamp too),
  `scripts/build-local.ps1` (MSYSTEM=UCRT64, ucrt64-first PATH/pkgconfig
  ordering), `INSTALLATION.md` + `TECHNICAL.md` (UCRT64 prerequisites).

## Verification performed locally (hard data)

- Full local meson build (gcc 11.4 — same compiler family as the ubuntu-22.04
  runners) with the new -Werror flags and the vendored deps prefix
  (raylib 5.5 / hsdaoh / flac 1.5.0): `meson compile` rc=0.
- `meson test`: 12/12 OK including `gui_record_direct` (byte-exact writer
  harness, 2.09s).
- `python3 misrc_tools/test/ci_guard_tests.py --static-only`: 37/37 PASS
  (including the two new guards).
- `python3 misrc_tools/test/ci_guard_tests.py --post-build --gui-path
  build-ci-check/misrc_gui`: 44/44 PASS (incl. runtime + binary
  introspection guards).
- Android NDK sweep: android-unique TUs (android_usb_jni.c,
  simple_capture_android.c) syntax-checked clean with the exact CI NDK
  (25.2.9519653) clang + the new -Werror flags.
- Workflow YAML parses (PyYAML) for build.yml + flac-cache.yml.
- 3bf7ff7 Windows-branch review: the only new `_WIN32` guards (gui_cxadc.c
  rate probe) use HANDLE/INVALID_HANDLE_VALUE with windows.h included in
  the Windows block; top-level `free` is covered by the direct stdlib.h.

## Verification run 37053411644 (dispatched on main after the fix commit)

- Windows EXE build (x86_64): GREEN in 3m0s — the original failure is fixed
  and the UCRT64 migration links + smoke-tests clean (subsystem/DLL
  assertions passed under UCRT64).
- All preflight guard jobs (ubuntu/windows/macos), Linux x86_64+arm64,
  macOS x86_64+arm64+universal, Android: GREEN (the new -Werror flags and
  the libc direct-include guard pass on every platform).
- Windows EXE build (arm64): FAILED at link — NEW finding, caused by the
  libsoxr addition itself: MSYS2's clang-built static libsoxr.a is compiled
  with OpenMP enabled, so the -static link needs the LLVM OpenMP runtime:
  ld.lld: undefined symbol: __kmpc_fork_call / omp_init_lock / ... >>>
  referenced by libsoxr.a(soxr.c.obj) / libsoxr.a(filter.c.obj).
  The gcc jobs already cover this (-lgomp in meson.build); the clang
  environment needed -lomp + the mingw-w64-clang-aarch64-llvm-openmp
  package (verified on packages.msys2.org: ships /clangarm64/lib/libomp.a,
  version matched to the CI clang 22.1.8). Follow-up commit adds:
  - meson.build: `elif soxr_dep.found()` -> `-lomp` in the Windows static
    link block (gcc keeps -lgomp)
  - build.yml: mingw-w64-clang-aarch64-llvm-openmp in both arm64 install lists
  - ci_guard_tests.py: MSYS2 toolchain policy now requires the llvm-openmp
    package so it cannot be silently dropped

## Final verification run 37054722490 (after the follow-up commit a3c9b65)

- Run conclusion: SUCCESS — all 12 jobs green:
  - Preflight + cross-platform guard tests (ubuntu-22.04/windows-2022/
    macos-14) — the new libc direct-include contract + MSYS2 toolchain
    policy guards pass on every platform.
  - Windows EXE build (x86_64) in 2m19s — UCRT64 migration verified
    end-to-end (compile, -static link, subsystem/DLL assertions, smoke
    test, post-build guards).
  - Windows EXE build (arm64) in 4m42s — link fixed; meson now reports
    'Message: libsoxr found, building with resample support' (feature
    gap closed) and no msystem-mingw64 deprecation annotation remains.
  - Linux AppImage (x86_64 + arm64), macOS (arm64/x86_64/universal),
    Android APK — all green with the new -Werror flags in cflags.
- Commits: 24ecc14 (fix + reinforcement) + a3c9b65 (arm64 OpenMP link fix)
  on main; deps caches saved for the ucrt64/arm64 Windows jobs.

## Commands run (investigation + validation)

- gh run view 37035393223 --repo harrypm/MISRC-GUI [--log-failed] [--job ...]
- gh run list --repo harrypm/MISRC-GUI --workflow "Build and release binary" --limit 12 --json ...
- git show/log/status/diff on 3bf7ff7 and the working tree
- PKG_CONFIG_PATH=.deps/install-appimage-local/lib/pkgconfig meson setup
  build-ci-check misrc_tools --buildtype release; meson compile; meson test
- python3 misrc_tools/test/ci_guard_tests.py [--static-only | --post-build
  --gui-path build-ci-check/misrc_gui]
- NDK clang --target=aarch64-unknown-linux-android30 -fsyntax-only on the
  android-unique TUs
- gcc -H include-tree probes (identified threading.h's POSIX-only stdlib.h
  as the masking mechanism)
- packages.msys2.org package lookups (clang-aarch64-libsoxr, ucrt64 set)

## Changed files

- .github/workflows/build.yml
- INSTALLATION.md, TECHNICAL.md
- misrc_tools/meson.build
- misrc_tools/common/ringbuffer.c
- misrc_tools/misrc_gui/output/gui_record_direct.c
- misrc_tools/misrc_gui/ui/gui_ui.c
- misrc_tools/test/ci_guard_tests.py
- scripts/build-deps-windows.sh, scripts/build-local.ps1
