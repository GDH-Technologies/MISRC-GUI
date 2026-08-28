# MISRC GUI development notes

Recent capture regressions showed that small callback-gating changes can silently break GUI feeds. Keep the following constraints in mind when touching capture/parser/audio paths:

- Preserve tolerated-frame behavior in MISRC frame mode: only drop frames when `result.error_count > 0 && result.report_errors`.
  Do not reject tolerated CRC-only frames, or GUI RF feed can stall while CLI still works.
- Keep capture heartbeat updates early in the callback (after buffer/null checks), before width/height early returns.
  This prevents false timeout/reconnect loops when callback activity exists.
- After any `capture_handler_init(&s_capture_handler)` during GUI capture start, explicitly restore audio capture state:
  `atomic_store(&s_capture_handler.capture_audio, true);`
  Without this, audio monitor path (`stream1 -> BUF_CAPTURE_AUDIO -> gui_audio`) remains empty.
- Validate RF and monitor audio as separate end-to-end checks after capture-path edits:
  - RF: waveform/scope feed present and stable.
  - Audio monitor: `Audio Mon` audible and `BUF_CAPTURE_AUDIO` no longer pinned at 0%.
- Prefer minimal, isolated fixes in `frame_parser`, `gui_capture`, `gui_extract`, and `gui_audio`; avoid unrelated UI/settings churn during capture debugging.

## 2026-08-21 Windows WASAPI clockgen audio — PARKED (firmware-side issue)

- Problem: clockgen audio (PCM1802 2ch + headswitch CH3) captured via WASAPI shared mode is distorted on Windows. Ch3/Ch4 showed red/clipping (now fixed: CH3/Ch4 peaks disabled for MISRC Clockgen, CH4 always zero). Recorded audio is still unusable due to resampling distortion.
- Root cause: the MISRC Clockgen hardware runs at 46875 Hz (stock crystal rate). Windows WASAPI shared mode resamples the 46875 Hz stream to the system mix rate (48000 Hz), corrupting the headswitch signal. Exclusive mode at 46875 Hz fails with `AUDCLNT_E_UNSUPPORTED_FORMAT` (0x8889000e) — the device does not expose a native format Windows will accept exclusively. Exclusive at 48000 Hz also fails (`AUDCLNT_E_UNSUPPORTED_FORMAT`) because the hardware is not at 48000. The endpoint exposes NO `PKEY_AudioEngine_DeviceFormat` property.
- What was tried and reverted:
  - GUI: exclusive mode at 46875 Hz (WAVEFORMATEXTENSIBLE, S24_3LE, 3ch) — failed `AUDCLNT_E_UNSUPPORTED_FORMAT`.
  - GUI: exclusive mode at 48000 Hz — failed `AUDCLNT_E_UNSUPPORTED_FORMAT`.
  - GUI: `PKEY_AudioEngine_DeviceFormat` approach (cxadc-win capture-server pattern) — the endpoint has no such property.
  - Firmware: resample 46875->48000 in `fill_buffer_normal()` with a 128/125 phase accumulator (zero-order hold, per-buffer) — produced a variable/wobbly rate because the USB endpoint pulls at a fixed 48000 Hz and the buffer fill rate doesn't match exactly. Reverted.
  - Firmware: present 48000 to USB (descriptor + `CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE`) — Windows still reports 48000 shared-mode resampling; the device format isn't exposed so Windows resamples regardless.
- Conclusion: this is a firmware-side issue that needs a proper approach not yet found. The GUI-side WASAPI path is reverted to the original working shared-mode capture (distorted but functional). The firmware repo is reverted to the working `5f98d2f` baseline. Both repos are clean.
- Open question for future work: the firmware needs to present the clockgen as a real 48000 Hz device to USB with the PCM1802 data accurately resampled in firmware before USB transmit. The per-buffer 128/125 phase accumulator approach was time-base accurate in theory but the USB endpoint's fixed 64-sample buffer size makes the output rate wobble. A variable-output-rate approach (not fixed 64) or a proper fractional resampler (linear interpolation / windowed sinc) may be needed. This is parked until a correct firmware resample design is worked out.
- GUI-side fixes that ARE committed and working: CH3/Ch4 peaks disabled for MISRC Clockgen (no red), CH4 always zero (no mirror), audio monitor forced to CH1/2 for MISRC Clockgen. Audio monitoring works (CH1/2, distorted by Windows resampling but functional).

## 2026-08-21 Vendored deps caching + CI parity (any terminal just works)

- Problem: building the Windows exe locally snagged because `.deps/install` (compiled hsdaoh + libuvc + raylib) never existed locally. CI rebuilt these from scratch every run with no cache, and the local scripts bailed at the deps gate pointing at a Linux-only script. The dev notes only covered the Meson PATH snag, not the deps-build step.
- Root cause: hsdaoh/libuvc/raylib are not system packages; they must be compiled from `third_party/hsdaoh` + upstream clones into `.deps/install`. This is what CI does every run (`windows-exe` job, `.github/workflows/build.yml`), but it was never scripted or documented for local Windows use.
- Fix: `scripts/build-deps-windows.sh` mirrors the CI `windows-exe` deps block (static libuvc v0.0.7 + vendored hsdaoh + vendored raylib with internal GLFW) into `.deps/install`. It is stamp-gated: a content-addressed hash of the hsdaoh source + raylib tag + libuvc ref + pacman dep versions skips the rebuild when unchanged. `scripts/build-local.ps1` auto-invokes it via MSYS2 MINGW64 on first run or when inputs change — one command builds everything, no manual deps step, no shell restart.
- Cross-platform parity: `scripts/build-deps-unix.sh` mirrors the `linux-appimage`/`macos` CI deps blocks with the same stamp gate. `scripts/build-local.sh` auto-invokes it.
- CI caching: `actions/cache@v4` on `.deps/install` (keyed on `hashFiles(third_party/hsdaoh/**)` + raylib/libuvc versions) added to all four build jobs, with a cache-hit guard that skips the rebuild when the install is already present.
- Prebuilt publishing: `scripts/publish-deps-cache.sh <platform> <arch>` packages `.deps/install` into a tar.xz + sha256 for upload to `harrypm/MISRC-ci-cache` (same flow as libFLAC 1.5.0). Not auto-uploaded; prints the `gh release upload` command for manual review.
- Local CI/dev guard: `python misrc_tools/test/ci_guard_tests.py --static-only` now checks the deps scripts exist, the stamp gate is present, `build-local.ps1` invokes the deps script (not bails), and docs snippets exist — so the local==CI path can't be silently removed.
- Validated locally: `misrc_gui.exe` (10.7 MB) builds clean, `--smoke-test` exits 0, deps stamp cache skips on second run.

## 2026-08-21 Windows local build bootstrap note (Meson PATH snag)

- Symptom:
  - `meson: The term 'meson' is not recognized...` in PowerShell.
- Root cause:
  - Local shell does not have Meson/Ninja on `PATH`, even when Python is installed.
- Standard recovery (now the expected flow):
  - Bootstrap and verify build-tool connection:
    - `pwsh -File scripts/build-local.ps1 -BootstrapOnly`
  - Build locally:
    - `pwsh -File scripts/build-local.ps1`
- Why this avoids repeat breakage:
  - `scripts/build-local.ps1` auto-installs user-level `meson` + `ninja` when missing and adds the Python user `Scripts` directory to session `PATH` before invoking Meson.
- Local CI/dev guard:
  - `python misrc_tools/test/ci_guard_tests.py --static-only` now checks this contract (script + note snippets), so future edits don’t accidentally remove the bootstrap path.

## 2026-04-16 capture/runtime snapshot

- Timestamp (UTC): `2026-04-16T03:38:18Z`
- OS: `Linux Mint 21.3`
- System: `Linux 5.15.0-173-generic x86_64 GNU/Linux`
- Branch: `heads/misrc_gui_dev`
- Commit: `48054ea`
- Stability note: current version is running stable for 8+ hours.

## 2026-04-19 local AppImage build note

- Local AppImage builds are now reproducible and passing smoke tests using:
  - `./scripts/build-appimage-local.sh`
- Default mode runs in an `ubuntu:22.04` container (`docker`/`podman`) to keep a portable glibc baseline.
- Script output location:
  - `.ci-artifacts/linux-appimage/`
- Verified locally:
  - AppImage artifact builds successfully.
  - `APPIMAGE_EXTRACT_AND_RUN=1 <AppImage> --smoke-test` passes.
  - Direct run `<AppImage> --smoke-test` passes on host.
## 2026-04-22 macOS Apple Silicon capture scheduling fix

- Symptom: on M-series Macs, capture/recording workloads could remain on efficiency cores, causing immediate drops/errors under GUI load.
- Root cause pattern: process-priority/QoS promotion was happening too late (after stream startup), so transport/ingest workers did not reliably inherit elevated scheduling class.
- Changes made:
  - `misrc_tools/common/threading.h`
    - Apple Silicon maps `THRD_PRIORITY_ABOVE+` to `QOS_CLASS_USER_INTERACTIVE`.
    - Added macOS QoS hinting in `proc_set_priority(...)`.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before `sc_start_capture(...)` / `hsdaoh_start_stream(...)`.
    - Roll back to `PROC_PRIORITY_NORMAL` on startup failure and on capture stop.
  - `misrc_tools/misrc_gui/output/gui_record.c`
    - Move FLAC-recording priority promotion to before encoder/worker creation.
    - Restore normal priority on FLAC init failure paths.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - Apply `proc_set_priority(PROC_PRIORITY_ABOVE)` before stream startup.
    - Restore normal priority on shutdown/startup-failure exit paths.
- Validation:
  - GUI soak run (`--debug-view`) for ~331.9s showed:
    - `waits=0`, `drops=0`
    - record buffers: A waits/drops `0/0`, B waits/drops `0/0`
    - no capture/dropout instability lines during soak window.

## 2026-04-22 macOS scheduling regression repair (post-rebase)

- Issue: after rebasing to `main`, a conflict-resolution mistake in `misrc_tools/common/threading.h` weakened macOS QoS escalation for capture-critical threads and reduced the effectiveness of Apple Silicon core placement.
- Corrective changes:
  - `misrc_tools/common/threading.h`
    - restored clean separation between `thrd_set_priority(...)` and `proc_set_priority(...)` QoS logic.
    - strengthened macOS QoS calls by adding non-zero relative priority for `ABOVE/HIGH/CRITICAL` levels.
    - added Mach thread precedence (`THREAD_PRECEDENCE_POLICY`) alongside QoS for capture-critical caller threads, avoiding blanket process-wide escalation of unrelated threads.
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - move `proc_set_priority(PROC_PRIORITY_ABOVE)` earlier in HSDAOH startup (before `hsdaoh_open`/`hsdaoh_alloc`/`hsdaoh_open2`) so early transport/open threads inherit elevated scheduling.
    - rollback to `PROC_PRIORITY_NORMAL` on all HSDAOH open/alloc failure exits.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirror earlier process-priority elevation for CLI HSDAOH path before `hsdaoh_alloc/open2`, with rollback on failure exits.
    - removed accidental early option-parse priority side effect so elevation only happens at real capture startup intent.

## 2026-04-22 macOS callback-thread scheduling follow-up

- Issue: callback-priority promotion was previously guarded by a single process-wide one-shot flag, so only the first callback thread was guaranteed to be elevated.
- Corrective changes:
  - `misrc_tools/misrc_gui/input/gui_capture.c`
    - changed callback promotion to thread-local one-shot (`MISRC_THREAD_LOCAL`) so each callback worker thread self-promotes once to `THRD_PRIORITY_CRITICAL`.
  - `misrc_tools/misrc_capture/misrc_capture.c`
    - mirrored the same thread-local callback-promotion behavior in CLI capture callback path.
  - `misrc_tools/common/threading.h`
    - in macOS `proc_set_priority(...)`, return early when `pthread_set_qos_class_self_np(...)` succeeds (after setting caller-thread precedence), avoiding unnecessary fallthrough into process `nice` fallback that can fail with EPERM on non-root runs.
- Runtime check (privileged CLI path):
  - successful `hsdaoh` capture runs with `waits=0`, `rf_drops=0`, `audio_drops=0`.
  - sampled `powermetrics --samplers cpu_power` during capture showed sustained higher P-cluster activity than E-cluster activity.
## 2026-05-03 parser CRC mismatch root-cause note
- Symptom: persistent parser error bursts with stable low counts (`13/14/15`) even when capture backpressure remained clean.
- Debug evidence: per-frame parser diagnostics showed `idle=0` and `total==crc` for all observed bursts, confirming CRC-only mismatches.
- Root cause: MISRC shared parser (`misrc_tools/common/frame_parser.c`) diverged from upstream hsdaoh CRC behavior by masking trailer/stream-id high nibbles before `crc16_ccitt(...)`.
- Upstream reference (`.deps-gha-local/hsdaoh/src/libhsdaoh.c`) computes CRC over raw line bytes without this masking.
- Corrective direction: compute CRC on raw line bytes in shared parser to align verifier input with upstream transport format, then revalidate mismatch rate with `MISRC_DEBUG=1` capture runs.

## 2026-08-13 Android arm64-v8a APK support (android-support branch)

- Target: basic, launchable Android 11+ (API 30) `arm64-v8a` APK via NativeActivity + raylib `rcore_android.c`, cross-compiled with NDK r25c on an x86_64 Linux host. Full plan in `android/ANDROID_SUPPORT_PLAN.md`.
- Toolchain: Android SDK cmdline-tools + build-tools;34.0.0 + platforms;android-30/34 + NDK 25.2.9519653 installed to `~/Android/Sdk`; `ANDROID_HOME`/`ANDROID_NDK_HOME`/`NDK_HOME` persisted to `~/.bashrc`.
- Build flow (all reproducible via scripts under `android/`):
  1. `android/build-deps-android.sh` cross-builds 7 static deps into `.deps/install-android-arm64` (libFLAC 1.5.0, FFTW 3.3.10 fftw3f, libsoxr 0.1.3, libusb 1.0.27, libuvc, vendored hsdaoh, raylib 5.5 PLATFORM=Android GLES 3.0). Versions + SHAs recorded in `android/deps-versions.txt`.
  2. `android/gen-cross-file.sh` emits the Meson cross-file `android/aarch64-linux-android.ini` from `$ANDROID_NDK_HOME` + `$DEPS_PREFIX`; `android/android-pkg-config` is a self-contained cross pkg-config wrapper (DEPS_PREFIX-only, no host .pc leak).
  3. `PKG_CONFIG=android/android-pkg-config meson setup --cross-file android/aarch64-linux-android.ini -Dbuildtype=release build-android misrc_tools` then `meson compile -C build-android misrc_gui` produces `build-android/libmisrc_gui.so`.
  4. `android/build-apk.sh` packages `libmisrc_gui.so` + `libhsdaoh.so` + manifest + icon into a debug-signed, zipaligned APK at `.ci-artifacts/android-apk/`.
- Code changes (see `git show` on the android-support commit for the full diff):
  - `misrc_tools/meson.build`: `android` host branch — GUI built as `shared_library('misrc_gui')` (NativeActivity loads .so), CLI executables gated off, link `-lEGL -lGLESv2 -lGLESv3 -landroid -llog -lOpenSLES -lm -ldl` (no -lpthread/-lX11/-lGL), `-DGRAPHICS_API_OPENGL_ES3` added to cflags.
  - `misrc_tools/misrc_capture/simple_capture/simple_capture.h` + new `simple_capture_android.c`: `__ANDROID__` branch checked BEFORE `__linux__` (NDK clang defines both) + no-device `sc_*` stub (CXADC/V4L2 out of scope for basic release).
  - `misrc_tools/misrc_gui/core/gui_settings.c`: `__ANDROID__` storage paths -> `/sdcard/Android/data/dev.misrc.gui/files/` (no HOME/Desktop on Android).
  - `misrc_tools/common/shm_anon.h`: `__ANDROID__` branch uses bionic's native `memfd_create` (API 30), not the glibc syscall shim.
  - `misrc_tools/common/flac_writer.c`: FLAC thread-affinity block gated to `__linux__ && !__ANDROID__` (bionic lacks `pthread_setaffinity_np`).
  - `.github/workflows/build.yml` + `misrc_tools/test/ci_guard_tests.py`: `android-apk` CI job + Android packaging-mirror guard.
- Entry-point contract (risk #2 resolved by reading `rcore_android.c:269-279`): raylib 5.5 defines `android_main()` which calls the app's standard `main()` — the existing `misrc_gui.c` main() is reused UNCHANGED. Do NOT add an `android_main` shim (would collide with raylib's).
- Bugs found & fixed during the cross-build (all verified against hard data, not assumed):
  - raylib 5.5 `rlgl.h:1908-1909` has inverted ES3/non-ES3 branches in `rlActiveDrawBuffers()`: under `GRAPHICS_API_OPENGL_ES3` it called `glDrawBuffersEXT` (an ES2 extension symbol absent on GLES3) instead of `glDrawBuffers` (GLES3 core). Patched via sed in `build-deps-android.sh` to use `glDrawBuffers`. Upstream raylib bug.
  - raylib CMake install emits a Desktop-style `raylib.pc` with `Requires.private: glfw3` (wrong for PLATFORM=Android — rcore_android.c uses EGL/ANativeWindow, no glfw). With the cross pkg-config wrapper forcing `--static`, this made `pkg-config --static raylib` fail and Meson fell back to CMake, picking up a HOST x86_64 `/usr/local/lib/libraylib.a` (arch-mismatch leak). Fixed by dropping the glfw3 require + setting Android `Libs.private` in the deps script.
  - hsdaoh hard-requires libuvc (`CMakeLists.txt:132` FATAL_ERROR) — libuvc was not in the original plan's 6 deps. Added a libuvc cross-build block.
  - hsdaoh `src/CMakeLists.txt:143` links `hsdaoh_test` with `-lrt`, which does not exist on Android (clock_gettime is in libc, like macOS). sed-patched the build copy to add `ANDROID` to the no-rt branch.
  - Meson 0.61.2 reads machine info from `[host_machine]`, NOT `[properties]` — the cross-file must set `system = 'android'` under `[host_machine]` or `host_system` is misreported as `linux` and the linux branch adds `simple_capture_v4l2.c` (no V4L2 headers on Android -> compile cascade failure).
  - Cross pkg-config wrapper must be self-contained (derive `DEPS_PREFIX` from its own location): Meson invokes the binary without the shell env, so requiring `DEPS_PREFIX` as an env var broke dep resolution.
- Verification (hard data):
  - `build-android/libmisrc_gui.so`: 16.2 MB, ELF 64-bit ARM aarch64 shared object.
  - `llvm-nm -D`: `ANativeActivity_onCreate` exported (T) — NativeActivity entry; `android_main` + `main` both exported.
  - `llvm-readelf -d` NEEDED: libEGL.so, libGLESv2.so, libGLESv3.so, libandroid.so, liblog.so, libm.so, libdl.so, libhsdaoh.so, libc.so — all Android sysroot. Zero host x86_64 lib leaks (`/usr/lib/x86_64`/`/usr/local/lib` grep count = 0).
  - APK `misrc_gui-v1.0.7-3-gaf76387-dirty-android-arm64.apk` (17 MB): `lib/arm64-v8a/libmisrc_gui.so` + `libhsdaoh.so` at correct path, both ARM aarch64; `aapt2 dump badging` -> `sdkVersion:'30'`, `targetSdkVersion:'34'`, `package: name='dev.misrc.gui'`; `apksigner verify` v3 scheme TRUE.
  - All 7 cross deps verified ARM aarch64 via `file` on extracted `.o` objects from each static archive.
  - `python3 misrc_tools/test/ci_guard_tests.py --static-only`: 22/22 PASS (including new Android packaging assertions).
- NOT yet validated (separate milestones, per plan out-of-scope):
  - Real-device install + GUI render + simulated-device selection (needs a physical Android 11+ arm64 device; cannot verify on this host).
  - USB host capture (hsdaoh/FX3/DdD) on Android — risk #1; libusb Android backend needs root or a JNI USB-Host bridge. Connect path is runtime-gated off in the basic release.
- Key constraints to preserve when touching Android build paths:
  - Keep `[host_machine] system = 'android'` in the cross-file; do not move it to `[properties]` (Meson 0.61.x ignores it there and misreports host_system as linux).
  - Keep the `__ANDROID__` branch in `simple_capture.h` BEFORE `__linux__` (NDK defines both).
  - Keep `-DGRAPHICS_API_OPENGL_ES3` in the android cflags — it must match the raylib cross-build's GRAPHICS or `rlActiveDrawBuffers` re-introduces the `glDrawBuffersEXT` link error.
  - Do not add `-lpthread` to android link flags (bionic pthread is in libc; -lpthread fails to link).
  - Keep the raylib.pc + rlgl.h sed-patches in `build-deps-android.sh` (run unconditionally so reruns stay correct).

## 2026-08-18 FLAC encode failure on clipped/resampled signal (issue #1)

- Issue: https://github.com/harrypm/MISRC-GUI/issues/1 ("Clipped, resampled signal causes FLAC encode to fail")
- Symptom: when capturing and downsampling on the fly (e.g. 40 MHz -> 20 MHz), clipped input lets the soxr resampler overshoot the source 12-bit range, and the FLAC encoder aborts with:
  - `FLAC process error: FLAC__STREAM_ENCODER_CLIENT_ERROR`
  - `FLAC encoder error on channel A` (or B)
- Root cause: `convert_i16_to_flac_i32()` in `misrc_tools/misrc_gui/output/gui_record.c` (at the function referenced in the issue) clamped the 8-bit path via `gui_record_sample_12bit_to_i8()`, but the 12-bit and 16-bit paths did not clamp:
  - 12-bit path: `dst[i] = (int32_t)src[i];` could write values outside the signed 12-bit range `[-2048, 2047]` declared to the FLAC encoder.
  - 16-bit path: `dst[i] = (int32_t)src[i] << 4;` could shift an overshooting 12-bit value outside the signed 16-bit range `[-32768, 32767]` declared to the FLAC encoder.
  - Out-of-range samples violate the declared bits-per-sample and trigger `FLAC__STREAM_ENCODER_CLIENT_ERROR`.
- Fix: clamp both unclamped paths to their declared bit-depth range, mirroring the existing 8-bit clamp:
  - 12-bit: clamp to `[-2048, 2047]`.
  - 16-bit: clamp the post-`<<4` value to `[-32768, 32767]`.
  - RAW 16-bit path was already safe (writes `int16_t` directly, inherently bounded).
- Files changed:
  - `misrc_tools/misrc_gui/output/gui_record.c` (`convert_i16_to_flac_i32`).
- Base commit: `1f6c54c` (prior to this fix).
- Validation (local, Linux Mint):
  - `meson compile -C build-local2 misrc_gui` -> builds clean (only pre-existing warnings).
  - `build-local2/misrc_gui --smoke-test` -> passes.
- Not yet validated: real on-the-fly resample capture (40 MHz -> 20 MHz) with clipped input against live hardware; confirm no `FLAC__STREAM_ENCODER_CLIENT_ERROR` under sustained clipped+resampled capture before shipping a release.
- Restore point: zip of the patched `gui_record.c` + this note preserved on the host at `~/MISRC-GUI-restore-points/fix-issue-1-flac-resample-clamp/`.
