# Prompt Log — 2026-10-11 — Playback data loading fix + in-line bit-depth/frequency readout

User request:
1. Fully fix the playback feature so it properly reads/loads data.
2. Readout of the ACTUAL bit-depth and frequency of input files.
3. Readout info to be in-line with the readout slider bar (playback timeline row).
4. Investigate the suspected overall issue with audio programs reading longer files and showing all the actual data.

## Investigation (hard data, no assumptions)

### Tools used
- ffprobe, mediainfo, metaflac (system, libFLAC 1.5.0)
- Custom parsers: /tmp/flac_meta_dump.py, /tmp/flac_seek_last.py (FLAC metadata/seektable ground truth)
- libFLAC 1.5.0 source review: .deps/src-android-arm64/flac-src/src/libFLAC/stream_decoder.c
- Standalone C test: /tmp/flac_probe_test.c (probe + STREAMINFO patch + seek verification)

### Commands run and key results

```
ffprobe -show_entries stream=sample_rate,bits_per_raw_sample,channels,duration_ts,duration ... (per file)
mediainfo --Inform=... (per file)
metaflac --export-tags-to=- ... (per file)
python3 /tmp/flac_seek_last.py <3 captures>
cc -O2 -o /tmp/flac_probe_test /tmp/flac_probe_test.c -lFLAC && /tmp/flac_probe_test <3 captures>
```

| File | Header rate | Header total | True stream total (seektable/find_total_samples) | Tags | True duration |
|---|---|---|---|---|---|
| VHS_PAL_SP_Tape_03_..._hifi_rf_8-bit_10msps.flac | 10000 | 2,165,571 (kHz-domain) | 2,165,570,800 | RF_TOTAL_SAMPLES=2165570800, RF_SAMPLE_RATE=10M | 216.557 s |
| VHS_NTSC_Spiderman_..._video_rf_8-bit_20msps.flac | 20000 | 253,755,070 (RF-domain, TRUE) | 253,755,070 | none | 12.688 s @ 20 MSPS |
| 630B-Tomas-Test_2_..._video__hifi_rf_12-bit.flac | 40000 | 154,140,672 (RF-domain, TRUE) | 154,140,672 | none | 3.854 s @ 40 MSPS |

### Root causes verified (libFLAC 1.5.0 source + test)

1. TWO on-disk conventions exist:
   - OLD (pre-finalize, e.g. 2026-07 captures): STREAMINFO total = raw RF sample count (TRUE), header rate = kHz-domain (e.g. 20000 = 20 MSPS). Header DURATION is 1000x too long (this is what external audio programs show wrong).
   - NEW (finalized, 2026-08+): STREAMINFO total = RF/1000 (kHz-domain), header rate = kHz-domain. Header duration is CORRECT, but the sample count claim is 1000x too small.
2. libFLAC `FLAC__stream_decoder_seek_absolute` REJECTS any seek where target >= STREAMINFO total_samples (stream_decoder.c:1305) and ignores seektable points with sample_number >= STREAMINFO total (line 3731). For NEW-convention files, seeking past the first 0.1% of the file fails SILENTLY (gui_playback.c ignored the return value) — timeline scrubbing desyncs from audio.
3. libFLAC 1.5.0 `FLAC__stream_decoder_find_total_samples()` decodes backward from EOF and returns the TRUE stream sample count cheaply (verified: matches RF tags and seektable exactly).
4. Read-callback STREAMINFO total patch (file offsets 18..25, low 36 bits) makes libFLAC seek correctly past the kHz-domain header lie WITHOUT modifying the file on disk (verified with /tmp/flac_probe_test on Tape_03: seeks to 2,165,571+ and to 1,082,785,400 succeed after patch; FAIL without).

### The "overall arching issue" (user's suspicion) — CONFIRMED
FLAC headers lie by design in this ecosystem (rate and/or total). External audio programs (ffprobe/MediaInfo/players) trust STREAMINFO, so:
- OLD-convention files: duration shown 1000x too long (3h31m for a 12.7s capture).
- NEW-convention files: duration correct, but sample count wrong; players that trust total_samples stop early / cannot seek past it.
The GUI now resolves the truth itself (probe + tags) regardless of convention.

## Implementation (misrc_tools)

### gui_playback.h
- playback_file_info_t: + header_total_samples (raw STREAMINFO value), + real_sample_rate_hz (resolved true rate).
- New API: gui_playback_get_selected_file_info(app, channel, info) — live playback info when running, else path-cached validation of the settings-selected file (for idle readout + timecodes).

### gui_playback.c
- New per-channel IO layer (playback_io_t) with libFLAC init_stream callbacks (read/seek/tell/length/eof) replacing init_FILE (GUI now owns the FILE*; stop() fcloses).
- Read callback patches STREAMINFO total_samples bytes in-flight (one-shot, before any seek) with the probe-resolved TRUE total when the header total disagrees.
- New probe helper: throwaway decoder per file — reads header STREAMINFO + FLAC__stream_decoder_find_total_samples() for the true stream total (cheap: metadata + tail decode only).
- Total resolution order: probe true total -> RF_TOTAL_SAMPLES tag -> legacy wrap/companion/frame-scan fallbacks (kept for probe failure).
- seek_absolute return values checked; failed seeks no longer desync the position tracker or drop buffered audio.
- validate_file uses the probe; compatible with both conventions.

### gui_ui.c
- Timeline slider row gains an in-line readout per channel: "8-bit 20.0 MSPS" style (compact tier "8b 20.0M" on narrow screens, hidden if no room; own row when stacked).
- gui_ui_playback_channel_timeline_info now uses gui_playback_get_selected_file_info so timecodes + readout show BEFORE playback starts (idle), from the actual selected files.
- Layout: readout slot budgeted with fixed measured widths (no reflow on value change); stacked rows grew to fit the readout line.

## Verification
- /tmp/flac_probe_test results above (mechanism-level, real files).
- Build: ninja -C build-local (see end of file for build log summary).
- GUI confirmation requested from user per rule (user interactable element changed).

## Files changed
- misrc_tools/misrc_gui/input/gui_playback.h
- misrc_tools/misrc_gui/input/gui_playback.c
- misrc_tools/misrc_gui/ui/gui_ui.c

## Implementation results (2026-10-11)

### Build
- `ninja -C build-local misrc_gui` — links clean; no new warnings from gui_playback.c / gui_ui.c (remaining warnings pre-exist in other files).
- New meson test target `gui_playback_resolve_test` (misrc_tools/test/gui_playback_resolve_harness.c) registered in misrc_tools/meson.build alongside the other CPU-only harnesses.

### Harness results (synthetic both-convention coverage)
```
$ build-local/gui_playback_resolve_test
[PLAYBACK] Validate: header total=3000000 resolved total=3000000 (source=TAIL_FRAME_DECODE, real_rate=20000000Hz, 0.15 sec)
[PLAYBACK] Validate: header total=3000 resolved total=3000000 (source=TAIL_FRAME_DECODE, real_rate=20000000Hz, 0.15 sec)
gui_playback_resolve: 30 checks, 0 failures
```
Covers: OLD convention (header total = raw count), NEW convention (header total = count/1000 + RF tags), and the IO-layer STREAMINFO patch (decoder-visible total patched to the true count; seeks to mid-file / near-end / 0 all succeed past the lying header total).

### Real-file verification (validate_file on actual captures)
```
$ /tmp/pb_real <3 captures>   # one-off build of the harness validate path
Tape_03 (tagged, finalized):   header_total=2165571   -> resolved_total=2165570800, rate=10 MHz, duration=216.557 s  (matches RF_TOTAL_SAMPLES tag + seektable)
Spiderman (tagless, old conv): header_total=253755070 -> resolved_total=253755070,   rate=20 MHz, duration=12.688 s  (matches seektable truth)
630B 12-bit (tagless, old):    header_total=154140672 -> resolved_total=154140672,   rate=40 MHz, duration=3.854 s   (matches seektable truth)
```
All three now resolve to the TRUE stream totals; before the fix the finalized convention would have been read as 2165571 samples (0.1% of the file) with seeks past that point silently failing.

### Additional latent bug fixed along the way
The old error_cleanup path fclosed FILE handles that `FLAC__stream_decoder_init_FILE` decoders owned (double-close on failed playback start). With init_stream the GUI owns the handles; stop/error paths close them exactly once.

## Session 2 additions (2026-10-12) — real set: VHS_Tape_08

Real set (user-provided): /media/harry/20TB HDD1/Hugo_UK_2025/VHS Tapes/VHS_Tape_08/
- video_rf_8-bit_20msps.flac: 130 GB, header total=225,155,481 (kHz-domain), tags RF_TOTAL_SAMPLES=225,155,481,120, RF_SAMPLE_RATE=20M. 3h07m38s.
- hifi_rf_8-bit_5msps.flac: 18.4 GB, header total=56,288,870, tags RF_TOTAL_SAMPLES=56,288,870,044, RF_SAMPLE_RATE=5M.

### Found + fixed (all verified with logs / real-file tests)
1. 36-bit overflow: 225G samples > STREAMINFO 36-bit field. Patch of the true total truncated to total % 2^36 (metadata cb saw 18,997,050,912) and libFLAC rejected every seek past it (250 seek failures in run 3, state=SEARCH_FOR_FRAME_SYNC). Fix: patch writes 0 ("unknown") when the true total exceeds 36 bits — no clamp, seektable/binary-search seeks over the real stream. Verified on the real files (/tmp/flac_seek_verify + /tmp/flac_loop_verify): all seeks OK incl. the exact failed targets; EOF loop-seek(0) resumes decode in both patch modes.
2. Waveform not 1:1: 8-bit playback stayed in ±128 while the display pipeline runs the 12-bit container (extract: (2047-field)<<4). Recorder convention (gui_record.c): 8-bit FLAC = container>>4. Fix: write callbacks restore the 12-bit container for every depth (8-bit <<4). Level-autostop full scale for playback now 2048 (normalized).
3. Pause "crash"/reset: pausing stopped feeding last_callback_time_ms → 2 s no-data watchdog (auto-reconnect) tore the session down. Fix: pause branch keeps the watchdog fed.
4. Spectrogram while paused: kept scrolling (streaking) with repeated last frames. Fix: DAW-style hold — frozen while PAUSED; a seek-while-paused pushes one preview batch (preview_pending) so the display steps to the new position, then holds. Live capture scroll unchanged.
5. Right-side file info next to each slider: basename + "8-bit 20.0 MSPS" + "RAW 838.8GiB FLAC 121.5GiB 6.9x" (RAW = total_samples x 4, matching capture-log raw accounting; FLAC = on-disk size; cached per path; tiered widths).
6. Mouse-wheel scrubbing on the timeline rows: 1-10 s steps with burst acceleration (notches within 400 ms ramp +0.75 s/notch, cap 10 s; idle resets to 1 s). Wheel-up=back, wheel-down=forward.
7. HARD DEV NOTE added: gui_ui.c (above the file-info helpers) + TECHNICAL.md "HARD DEV NOTE — Playback data pipeline".

### Still pending (user-requested, next session)
- Drag-and-drop file input onto the GUI.
- Side-car style loading: same-base-name companion files (e.g. select video_rf -> auto-load hifi_rf on CH B; baseband/HiFi_Decoded audio for the audio monitor).

## Session 3 final state (2026-10-13) — USER CONFIRMED WORKING

User confirmed "current works" after the last test run (run 19 log: independent
per-channel scrubbing verified — only "Paused scrub CH A" lines appear when
dragging the A slider; file info line visible next to both sliders).

Committed + pushed:
- 6be1adf — Playback: DAW-style file analysis (true totals via tail-decode probe,
  patched-STREAMINFO IO layer, 1:1 sample scaling, pause watchdog feed,
  paused backfill, full-resolution contiguous analysis windows via 8 parallel
  segment decoders with worker-side FFTs + heap frame buffers (stack-smash
  segfault fixed), waveform paused re-process, space-bar pause, in-line
  timeline file info (basename / bit-depth + real rate / RAW+FLAC sizes),
  1-10s accelerated wheel scrub, seek-sequence history clears, 512 slices,
  regression harness, TECHNICAL.md HARD DEV NOTE).
- 00650c3 — Spectrogram: heat-map palette dropdown (Phosphor/Heat, top-right),
  reload-on-palette-change (paused: instant same-spectra re-map + re-request;
  rolling: clear + refill), DAW time axis (Spectro: absolute timecodes bottom
  while paused / relative seconds rolling; Waterfall: time on the left),
  per-channel true-rate resolution.

Known container limitation (documented, verified): FLAC cannot represent
>655,350 Hz rates or >36-bit sample counts; external tools that trust the
header sample count (ocenaudio) stop at 0.1% of the data while the duration
looks correct — the full data IS in the file (verified end-to-end).

Restore point: prompt_logs/restore-point_2026-10-13_00650c3.zip (git archive
of 00650c3).

## Pending user confirmation (per rules)
- GUI run: select [Playback] FLAC Files device, pick a finalized capture (e.g. Tape_03) and an old one (e.g. Spiderman); confirm:
  1. In-line readout next to each slider bar shows e.g. "8-bit 20.0 MSPS" / "8-bit 10.0 MSPS" / "12-bit 40.0 MSPS" before AND during playback.
  2. Timecodes next to the slider show the true total duration at idle (00:00:00/00:03:36 for Tape_03).
  3. Scrub/drag the slider anywhere on a FINALIZED capture (Tape_03): audio position must actually jump (previously silently ignored past the first 0.1%).
  4. Playback of a long file runs the full length and loops.
- Once user confirms fully working: create log note + zip restore point per rule.
