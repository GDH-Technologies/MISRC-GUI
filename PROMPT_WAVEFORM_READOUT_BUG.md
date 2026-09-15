- Also pending: user re-testing the reverted baseline to see if the fast (<10k frame) trigger was caused by the ringbuffer change or is independent.

## 2026-09-06 — debug run analysis + targeted instrumentation added
- Ran `MISRC_DEBUG=1 build-local/misrc_gui 2> ~/misrc_debug.log`. Log analysis (321 lines):
  - Capture + extraction + buffers HEALTHY the entire run: `[BUFMGR] RF:0%/0/0 AUD:0%/0/0 DISP:0%/0/0` throughout (0% fill, 0 waits, 0 drops). Display thread processed 324736 frames. Capture stopped clean: `22735 frames, 0 missed, 0 errors, 0 waits, 0 drops`.
  - => corruption is STRICTLY in the panel path, downstream of BUF_DISPLAY. Ringbuffer/feed ruled out (consistent with the revert).
  - Thread priority could NOT be elevated (EPERM, no root): `Thread priority request 3 could not be elevated (rt_err=1, nice_err=13)` on capture/extract/display threads. Under high CPU load these run at normal priority and can be starved.
  - `[CB] Missed at least one frame` at fcnt 42637, 47471, 57188 (starvation-induced). `[FP]` lines show tolerated CRC errors (errors total=6, crc=4) NOT counted as errors (tolerate_crc_only=no but mixed-low tolerated).
  - NO `[DIAG]` lines present (existing logging does not cover the panel/resample path) — the corruption moment is invisible in current logs.
- User clue: "CPU being under high load during the viewing test." Corruption appeared during this run under load (~9 min). High CPU load accelerates the trigger → strongly suggests a RACE (fires more under scheduling pressure), not deterministic logic. c93c52b already fixed one such race in this exact path (display/render torn read on state->display_samples) via atomic_flag spinlock + render snapshot; a REMAINING race or a resampler-state corruption path is likely.
- Prior art: c93c52b "Fix random waveform shift and false clipping display" (added data_lock + waveform_render_snapshot_t). bbe9539 "DC offset helpers" was REVERTED by 08d533d. app->display_samples_a/b are legacy (only written, never read by panel render) — not the culprit.
- Instrumentation added (readout-only, no behavior change): `waveform_debug_log_bad_output()` in gui_oscilloscope.c, gated on MISRC_DEBUG, throttled 1/sec per panel. Emits `[WFDBG]` when panel output is rail-pinned (|v|>0.98 on ~all samples), non-finite (NaN/Inf), or trigger-held. Logs: state ptr, held flag, out count, nonfinite count, rail count, value range, decimation, display_width, trigger_display_pos, resampler ratio, INPUT sample range (first 256), trigger enable/mode/source.
  - Disambiguator: if `input256=[min,max]` is a normal spread but `vrange` is rail-pinned -> per-panel soxr resampler is the culprit. If input is also rail-pinned -> upstream frame corruption. `held=1` -> trigger-hold is keeping garbage on screen (explains persistence until app restart, since waveform_clear() does not reset the resampler).
- Build: BUILD_EXIT=0, smoke OK, tests 11/11 OK. Binary: build-local/misrc_gui.
- Next: user re-runs `MISRC_DEBUG=1 build-local/misrc_gui 2> ~/misrc_debug.log` UNDER HIGH CPU LOAD until the waveform corrupts, then stops and provides the log. The `[WFDBG]` lines around the corruption will identify the real mechanism.

## 2026-09-06 — Heisenbug confirmed + replaced expensive detector with cheap WFTRACE
- User ran the WFDBG-instrumented build under MISRC_DEBUG=1: 4418-line log, display thread processed 5,303,461 frames (~16x longer than the 321-line baseline run that corrupted), buffers 0% throughout, capture stopped clean (371270 frames, 0 missed/errors/waits/drops). **Zero `[WFDBG]` lines.** User confirms: "Clean the whole time — it did not corrupt this run."
- => HEISENBUG: the expensive WFDBG detector (full per-call loop over up to 4096 output samples at ~600 calls/sec/panel) added enough work to the display-thread hot path to shift the race window and mask the corruption. This is itself a strong clue: the bug is a RACE sensitive to display-thread timing (consistent with the c93c52b race-fix class), NOT deterministic logic. Note: MISRC_DEBUG=1 alone does NOT prevent corruption (the 321-line baseline run had MISRC_DEBUG=1 and DID corrupt) — only the added per-call output loop masked it.
- Replaced `waveform_debug_log_bad_output` (expensive, per-call full loop) with `waveform_debug_snapshot` (cheap periodic): common path is just a counter increment+compare (no clock call); a full output min/max/mean scan runs only every ~2s per panel. Emits `[WFTRACE]` lines with: state ptr, held flag, out count, nonfinite count, rail count, value range, value MEAN, decimation, display_width, trigger_display_pos, resampler ratio, INPUT sample range (first 256), trigger enable/mode/source, and a `<TRANSITION>` marker when mean/range jump materially from the previous snapshot.
  - Captures the healthy->corrupt transition regardless of type: rail-pinning (vrange->rails), DC offset (vmean jumps), channel flip (vrange/vmean no longer match this panel's input256 range), non-finite (nonfinite>0). The MEAN is the key addition over WFDBG — DC offset and flip show up as a mean shift even when values stay in range.
- Build: BUILD_EXIT=0, smoke OK, tests 10/10 OK (ringbuffer_spsc_stress removed in revert, so 11->10 expected). Binary: build-local/misrc_gui.
- Next: user re-runs `MISRC_DEBUG=1 build-local/misrc_gui 2> ~/misrc_debug.log` UNDER HIGH CPU LOAD (same conditions as the run that corrupted) until the waveform corrupts, then stops. The `[WFTRACE]` timeline will show the transition and identify the mechanism.

## NOT yet validated
- Real long-run (4+ h) live MISRC/HSDAOH capture: confirm the waveform/display no longer freezes/garbles after hours while recording stays clean. User-confirmation step before declaring the fix working.
- Restore-point zip + log note will be created once the user confirms the fix on hardware.

## 2026-09-06 UPDATE — ringbuffer fix was WRONG (reverted)
- User tested the ringbuffer fix build: "Not fixed, still triggers the bug. Whatever you changed made it more twitchy — not even 10k frames before it occurred."
- => the ringbuffer race was NOT the root cause. Reverted: `git checkout HEAD -- misrc_tools/common/ringbuffer.c`; removed `test/ringbuffer_spsc_stress_harness.c` and its meson registration. Tree is back to baseline (only this prompt log is untracked). Clean rebuild OK (BUILD_EXIT=0, smoke OK).
- The ringbuffer stress harness DID prove a real SPSC race exists in the old code (old ringbuffer.c corrupts at 20s, fixed code clean at 20s), but fixing it did NOT fix the visible bug and made it trigger faster. So the visible bug is a SEPARATE issue that the ringbuffer stall was apparently not gating.

## New hard clues from the user (2026-09-06)
- Symptom is display-only: ChA/ChB appear flipped, ChA has a massive UPWARDS DC offset, ChB is stuck in constant clipping. Recording stays clean (Tape_02 log: waits=0 drops=0, rawA=rawB match).
- Both CH A and CH B waveform panels were visible, split with FFT/other. => corruption is in a path feeding BOTH waveform panels (shared upstream of per-panel render), or in both panels' shared mechanism.
- **Stays corrupt until a FULL APP RESTART** — re-starting capture (without quitting the app) does NOT clear it.
  - This RULES OUT: capture ringbuffer, BUF_DISPLAY (both repopulate on new capture), and the display/extraction threads (gui_app_stop_capture stops both at lines 2525/2532 and they restart on new capture).
  - This POINTS AT: persistent state that is only re-initialized at app startup. Confirmed persistent across capture restart:
    - per-panel `waveform_panel_state_t` (created at panel registry init, destroyed at app cleanup).
    - per-panel soxr resampler `state->resampler` + `state->resampler_ratio` — `waveform_clear()` does NOT reset these (only clears display_samples_available, trigger_display_pos, phosphor).
    - static extraction buffers s_buf_a/s_buf_b/s_extract_fn (overwritten each frame, so unlikely to hold stale corruption).
- Trigger timing: was ~4h on baseline; with the (reverted) ringbuffer fix it triggered in <10k frames. User has not yet re-tested the reverted baseline to confirm whether the fast trigger was caused by the ringbuffer change or is independent.

## Revised hypothesis (UNCONFIRMED — pending debug log)
Corruption is in persistent per-panel state downstream of BUF_DISPLAY, most likely the per-panel soxr resampler path (`waveform_resample_to_buffer` / `waveform_ensure_resampler` in gui_oscilloscope.c): once it produces garbage (values pinned/offset), `waveform_process_display` only overwrites `state->display_samples` when it returns updated=true; with trigger enabled and no trigger found it HOLDS the previous (garbage) display, so the panel keeps showing corruption. Because the resampler is not reset on capture stop, re-starting capture does not clear it; only app restart (which destroys/recreates panel state) does.

## Pending hard data
- User will run a capture with MISRC_DEBUG=1 and save stderr to a file, stopping soon after the waveform corrupts. Need the [DIAG]/[CB]/[BUFMGR]/[EXTRACT]/[DISPLAY] lines around the break to confirm:
  - whether capture/extraction/buffers stay healthy when the display corrupts (confirms display-only),
  - whether trigger hold is involved (no-trigger => hold previous garbage),
  - any error/warning lines at the break moment.
- Also pending: user re-testing the reverted baseline to see if the fast (<10k frame) trigger was caused by the ringbuffer change or is independent.

# PROMPT — Broken waveform readout bug (long-run capture)

Session started 2026-09-06. Tracking all input/output/commands per project rule.

## 2026-09-07 — Two issues to fix (user request)
- Issue 1: "device disconnect can be used when recording (breaks file)" — the Disconnect button (toolbar ConnectButton) can be clicked while recording, and it breaks the capture file.
- Issue 2: "this readout bug breaks recording" — clarified: when the readout corrupts, the recording STOPS capturing data (counters freeze, file stops growing) even though the user did not stop it. => the extraction thread (gui_extract.c extraction_thread) WEDGES when the readout corrupts. The extraction thread is the shared producer for BOTH the display (BUF_DISPLAY) and the record path (BUF_RECORD_A/B) and updates total_samples/samples_a/samples_b. If it wedges, the readout freezes AND recording freezes (file stops growing, sample counters freeze). This is a major clue narrowing the readout Heisenbug: it is an extraction-thread wedge, not a display-only issue. Previously the Tape_02 log showed recording completing clean (16148s, waits=0 drops=0) — that run apparently did not wedge the extraction thread (or the user stopped before it affected recording). The wedge is intermittent/Heisenberg.

## Issue 1 root cause (confirmed from code)
- `gui_record_cleanup()` (gui_record.c:1551) waits for the async finalize thread (`while (s_record_stop_finalizing) sleep; thrd_join(s_finalize_thread)`). It is declared in gui_record.h and defined, but NEVER CALLED — neither `gui_app_cleanup` (gui_capture.c:1235) nor misrc_gui.c's exit path calls it.
- `gui_record_stop()` (gui_record.c:2972) spawns finalization on a background thread (to keep UI responsive) and returns immediately. `gui_app_stop_capture()` calls `gui_app_stop_recording()` (async) then immediately tears down capture/extraction/display and eventually `bufmgr_cleanup` frees BUF_RECORD_A/B — while the finalize thread is still joining the writer threads that drain those ringbuffers. => use-after-free on the record ringbuffers during finalize => truncated/corrupted file. Same race hits app exit while finalizing (the readout-bug-forces-restart path).

## Issue 1 fix applied (2026-09-07)
- gui_capture.c `gui_app_cleanup`: call `gui_record_cleanup()` BEFORE `bufmgr_cleanup()` so the finalize thread (and writer threads) complete and release the record ringbuffers before they are freed. (gui_capture.c:1285)
- gui_ui.c ConnectButton click handler: refuse to disconnect while `app->is_recording` is true — show status "Stop recording before disconnecting (protects the capture file)" and consume the click. Matches the existing "settings locked while recording" pattern (gui_ui_settings_locked). (gui_ui.c:8315)
- misrc_gui.c KEY_SPACE handler: same guard — refuse space-bar stop-capture while recording. (misrc_gui.c:749)
- User chose: "Disable Disconnect while recording: force me to stop recording first, then disconnect (matches the 'settings locked while recording' pattern)."
- Build: BUILD_EXIT=0, smoke OK, tests 10/10 OK. Binary: build-local/misrc_gui.
- NOT yet validated: real-hardware confirmation that (a) Disconnect is refused while recording, (b) stopping recording first then disconnecting yields a clean file, (c) quitting the app while a recording is finalizing no longer truncates the file.

## Issue 2 status (still under investigation)
- The readout bug is an extraction-thread wedge (counters freeze, file stops growing). The cheap WFTRACE diagnostic remains in gui_oscilloscope.c (waveform_debug_snapshot, gated on MISRC_DEBUG, ~2s/panel). Need a WFTRACE run that captures the wedge: user runs `MISRC_DEBUG=1 build-local/misrc_gui 2> ~/misrc_debug.log` under high CPU load until the waveform corrupts AND counters freeze, then stops. The [WFTRACE] timeline + whether [BUFMGR] fill changes (extraction stopped draining BUF_CAPTURE_RF) will identify the wedge point.
- The ringbuffer SPSC race fix was reverted (wrong direction). The real cause is still open.

## 2026-09-07 — Second instrumented run also clean: extreme Heisenbug confirmed
- User ran the cheap-WFTRACE build for a 1h30m real-world capture + sustained high CPU load: "stable ... only a mild stutter not a full glitch/swap break like before."
- Log: 6746 lines, 3610 WFTRACE lines, 9 missed frames (the stutter, scattered not clustered), 0 errors/waits/drops. Recording stopped clean (4920s, A=366.5GB, B=0 — capture_b off this run). All WFTRACE output healthy: rail values 0-16/576 (normal signal peaks, NOT rail-pinned), nonfinite=0, held=0, 117 TRANSITION markers (natural drift). No corruption, no wedge.
- => SECOND instrumented run that stayed clean (first: expensive WFDBG ran 16x longer clean; now cheap WFTRACE ran 1.5h clean). Even the cheap instrumentation (counter inc+cmp per call, full scan every ~2s) shifts the race window enough to prevent reproduction. The bug is an EXTREMELY timing-sensitive Heisenbug.
- STRATEGY CHANGE: cannot rely on reproducing under instrumentation. Must find the race from code using the accumulated clues:
  1. Display-only corruption (capture+record clean) => race is in the panel/extraction path, downstream of BUF_CAPTURE_RF.
  2. Extraction thread WEDGES when it corrupts (counters freeze, file stops growing) => the wedge is in extraction_thread (gui_extract.c) or what it calls (s_extract_fn / bufmgr_read_begin on BUF_CAPTURE_RF / bufmgr_write_begin on BUF_DISPLAY / bufmgr_write_begin on BUF_RECORD).
  3. Stays corrupt until app restart (not capture restart) => persistent state: per-panel waveform_panel_state_t + soxr resampler (not reset by waveform_clear), OR a wedged thread (extraction/display).
  4. High CPU load accelerates it => race, fires more under scheduling pressure. Thread priority can't elevate (EPERM) so capture/extract/display run at normal priority and get starved.
  5. Both CH A and CH B panels corrupt together => shared upstream of per-panel render: extraction_thread, s_extract_fn, or the display-thread dispatch.
  6. Instrumentation in the display-thread vtable process path masks it => the race window is in/near the display thread's per-panel process call (waveform_vtable_process) OR the shared path it shares timing with.
- NEXT (code-driven, no repro needed): audit extraction_thread + waveform_vtable_process + the per-panel soxr resampler for the race that fits all 6 clues. The ringbuffer fix made it WORSE, so the wedge is likely downstream of the ringbuffer read (in s_extract_fn or the display dispatch), not the ringbuffer itself.

## User input
- Goal: fix the broken waveform readout bug (again).
- Reproduced on freshly-pulled v1.1.9 (commit `3672aef`, tag v1.1.9) against live hardware.
- Feed: MISRC / HSDAOH USB capture (NOT DdD).
- Symptom: the waveform trace itself is wrong — flat / no signal / shifted / garbled / not updating.
- Timing: broke at around 4 hours into capture.

## Commands run
- `git pull` → fast-forward `9997eca..3672aef`, 33 files changed (DdD FIFO telemetry + waveform overlay harness + UI scale work).
- `git --no-pager log --oneline -30` → reviewed recent history (recent waveform commits: `705a42b` stable checkpoint after waveform validation, `c93c52b` Fix random waveform shift and false clipping display).

## Files inspected (hard data)
- `misrc_tools/misrc_gui/visualization/gui_oscilloscope.c` / `.h` — waveform panel vtable, per-panel resampler + display buffer, trigger, overlay.
- `misrc_tools/misrc_gui/processing/gui_display_thread.c` / `.h` — display thread reads `DISPLAY_FRAME_SIZE` (65536*2*2 B) frames from BUF_DISPLAY, dispatches `panel_process_all`.
- `misrc_tools/misrc_gui/visualization/panel_registry.c` / `panel_interface.h` — `panel_process_all` → per-panel `process`.
- `misrc_tools/misrc_gui/input/gui_capture.c` — hsdaoh raw/parser callback + upstream callback write into BUF_CAPTURE_RF.
- `misrc_tools/misrc_gui/processing/gui_extract.c` — extraction thread: BUF_CAPTURE_RF -> extract -> BUF_DISPLAY (every `ui_stride` frames), updates total_samples.
- `misrc_tools/common/buffer_manager.c` / `.h` — bufmgr_read_begin/end, write_begin/end over ringbuffer; default BUF_DISPLAY policy (3 waits, drop).
- `misrc_tools/common/ringbuffer.c` / `.h` — **double-mapped SPSC ringbuffer, head/tail are `atomic_size_t`.**
- `misrc_tools/common/rb_event.h` — POSIX mutex+cond auto-reset event.
- `misrc_tools/common/threading.h` — `get_time_ms`/`get_time_us` are uint64 on Linux (CLOCK_MONOTONIC). No 32-bit ms wrap on Linux.
- `misrc_tools/misrc_gui/visualization/gui_phosphor_rt.c` — phosphor render textures (UnloadRenderTexture before reload; looks leak-free).
- `misrc_tools/misrc_gui/core/gui_app.h` — counter types: `total_samples`/`samples_a/b` are `atomic_uint_fast64_t`; `frame_count` etc are `atomic_uint_fast32_t`. None wrap at ~4h on this feed.

## Leading hypothesis (NOT yet confirmed — pending hard data)
`ringbuffer.c` has a data race on `rb->tail` and `rb->head`:
- `rb_write_finished`: `rb->tail += size` is a **non-atomic** RMW (writer thread).
- `rb_read_finished`: `rb->head += size` is a **non-atomic** RMW, and on wrap does `rb->head -= buffer_size; rb->tail -= buffer_size` — a **non-atomic** RMW on `tail` from the **reader** thread.

So `tail` has two non-atomic writers (writer `+= size`, reader `-= buffer_size` on wrap). A lost update between them drifts the index. Sustained drift can make `tail < head`, after which BOTH `rb_read_ptr` and `rb_write_ptr` return NULL permanently (`if(tail < head) return NULL`) → BUF_DISPLAY feed deadlocks: display thread gets NULL forever (waveform freezes/flatlines), while the capture + record paths keep running. Probabilistic, accumulates over many wraps → "around 4 hours."

This would affect ALL panels fed from the display thread (waveform, FFT, histogram, CVBS), not only the waveform — needs confirmation.

## Pending (hard data to confirm before fixing)
- stderr log from the failing run (MISRC_DEBUG=1?) around the 4h mark: [DIAG]/[BUFMGR] drops/timeouts, [CB] lines.
- Whether capture/recording kept running when the waveform broke (display-feed deadlock vs capture stall).
- Whether FFT/histogram/CVBS panels also froze/garbled at the same time (BUF_DISPLAY feed vs per-panel).
- Whether recording was on (ui_stride / record-buffer path involvement).

## Next
- Confirm root cause from log + disambiguators, then propose a minimal fix to the ringbuffer index updates (atomic RMW / eliminate the reader's tail write) without destabilizing the CLI capture path that shares `ringbuffer.c`.

## Hard-data confirmation (Tape_02 capture log + code + stress reproduction)
- Log: `/media/harry/20TB_Scott_M/Tape_02/VHS_NTSC_LP_Tape_02_2026.09.06_06.17.57_misrc_capture.log`
  - Version that broke: `dev-2026-09-06-74e67b1-dirty` (capture-server-client-modes). `ringbuffer.c` is IDENTICAL on main/v1.1.9 (`git diff origin/main origin/capture-server-client-modes -- ringbuffer.c buffer_manager.c` = empty).
  - Feed: MS2130 hsdaoh, misrc_mode=on, A=on B=on -> raw/parser backend (`gui_capture_callback`), not the upstream callback.
  - Recording ran 16148 s (04:29:08) with waits=0 drops=0, rawA=rawB=1203.084 GB each -> capture+record path clean the whole run.
  - Waveform/display broke at ~4h while capture+record kept running -> failure isolated to the BUF_DISPLAY feed.
- Root cause (confirmed): `misrc_tools/common/ringbuffer.c` `rb_read_finished` did a non-atomic rewind `head -= buffer_size; tail -= buffer_size` while `rb_write_finished` did non-atomic `tail += size` -> `tail` had TWO non-atomic RMW writers. A lost update drifts `tail` and corrupts/stalls the ring. Latent since initial import (9f592e5, 2026-08-09); surfaces on long high-throughput runs. BUF_DISPLAY (smallest buffer, most wraps, tightest coupling) is hit first; larger record buffers with spill fallback survived.
- Reproduction (hard data): new harness `misrc_tools/test/ringbuffer_spsc_stress_harness.c` (two threads hammer a 16 KiB real ringbuffer, 256 B blocks -> frequent wraps; asserts no tail<head, no data mismatch, produced==consumed, progress).
  - Original ringbuffer.c (HEAD), 20 s: FAIL - consumed block did not match producer pattern (lost-update race corrupts blocks).
  - Fixed ringbuffer.c, 20 s: PASS - 8,112,812 blocks, no inversion, no mismatch. (2 s: 7,044,534 blocks clean.)
  - Note: ThreadSanitizer does NOT flag this (fields are _Atomic-typed; TSan treats each plain access as atomic). The bug is a logical lost-update RMW race; the harness catches its observable consequence (corruption/inversion), not via TSan.

## Fix applied
- `misrc_tools/common/ringbuffer.c`: rewrote rb_put/rb_write_ptr/rb_write_finished/rb_read_ptr/rb_read_finished to a monotonic-counter + modulo-indexing SPSC design. Only the producer mutates tail (atomic_fetch_add); only the consumer mutates head (atomic_fetch_add); indices never decremented/rewound; buffer addressed via head%buffer_size / tail%buffer_size (valid across the wrap thanks to the double-mapped backing store). Eliminates the two-writer tail race and fixes weak-memory (ARM/Apple Silicon) ordering (old code used plain +=/-= on _Atomic fields). Backward compatible: every direct head/tail reader (ringbuffer_writer.c, misrc_capture.c, gui_ui.c, buffer_manager.c) computes fill = tail - head, invariant under both schemes.
- `misrc_tools/test/ringbuffer_spsc_stress_harness.c`: new regression harness.
- `misrc_tools/meson.build`: register ringbuffer_spsc_stress test.

## Validation (local, Linux Mint)
- Standalone: old ringbuffer.c -> harness FAIL (corruption); fixed ringbuffer.c -> harness PASS (8.1M blocks clean / 20 s).
- `meson compile -C build-local` -> BUILD_EXIT=0 (GUI + CLI capture lib + tests; only pre-existing warnings).
- `build-local/misrc_gui --smoke-test` -> SMOKE_EXIT=0.
- `meson test -C build-local` -> 11/11 OK (including new ringbuffer_spsc_stress 2.01s OK).
- `python3 misrc_tools/test/ci_guard_tests.py --static-only` -> 30/30 PASS, CIGUARD_EXIT=0.

## NOT yet validated
- Real long-run (4+ h) live MISRC/HSDAOH capture: confirm the waveform/display no longer freezes/garbles after hours while recording stays clean. User-confirmation step before declaring the fix working.
- Restore-point zip + log note will be created once the user confirms the fix on hardware.
