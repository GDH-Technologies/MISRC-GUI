# Prompt Readme — CVBS decoder colour phase (real-capture fit), full-frame hybrid (tbc-tools analyse model), FFT wording/chrome scope

Date: 2026-10-19 (resumes the hard-crashed 2026-10-18 session)
Repo: `C:\Users\Harry\MISRC-GUI` (branch `main` at `27f95b5`; the crashed session's uncommitted work was preserved and this session builds on it — still uncommitted, per rule)
Plan: `0903354e-6b33-4fa1-beab-1a9236e1263f` (approved; A live colour-phase fit -> B hybrid -> C FFT -> D validation+log)

## Reconstructed crashed-session state (2026-10-18, workstation hard-crash)
- Survived uncommitted: `misrc_tools/misrc_gui/signal/gui_cvbs.c` +1511/−351, `gui_cvbs.h` +97/−, the new `misrc_tools/test/gui_cvbs_test.c` (synthetic baseband contracts + a TEMP live diagnostic), the `gui_cvbs_baseband` meson test target; the build had linked at 20:39 before the crash; all 13 synthetic contract runs were green at this session's start; the centering fix (the profile-derived active render crop) was already in.
- Lost with the crash: the session's todo list and its readme (none had been written — this file replaces it). The last to-dos per the user: colour phase + hybrid mode.
- Also uncommitted in the tree from earlier sessions (untouched today): the FFT Linear/Log feature (`gui_fft.c` +234, the label harness +59) and `misrc_tools/misrc_gui/dev/rp2350_firmware_timing_fix.md`.

## Session inputs (user-provided)
- To-dos: (1) colour phase — hues wrong/rotated on a real capture; (2) hybrid mode — NOT implemented (the crashed session's per-line VBI contrast-stretch stood in): wanted the active area correct + the VBI offset like tbc-tools analyse (local repo `C:\Users\Harry\tbc-tools`, `src/tbc-analyse/tbcsource.cpp` HYBRID_CHROMA_MODE / `applyFullFrameDecodeBounds`, commit 063d83e offsets); (3) FFT — full words "Scale: Linear", the ORIGINAL mode kept as stock, chrome-hide hides only the UI not the stats info.
- Live ground truth (captured for this session at 10:48): `C:\Users\Harry\Desktop\EBU_PAL_HackDac2_MISRC_V2.5\10sec-40msps\cvbs-pal-ebu_2026.10.19_10.48.13_Video_RF_12-bit.flac` — FFprobe: FLAC, s16, 40000 Hz header (Msps-in-Hz convention = 40 MSPS), mono, bits_per_raw_sample 12, duration 10.0139; capture log: MS2130/MISRC, channel A 12-bit = the CVBS baseband signal, channel B 8-bit HiFi (unused here).

## Commands + key outputs (chronological; pwsh 7.5)
```
# Excerpt (the Msps-in-Hz trap: -ss/-t are in 40 kHz "seconds"; 1000 = 1 real s, 2000 = 2 real s)
& 'C:\Program Files\DecodeTools\ffmpeg.exe' -y -v error -ss 1000 -i <flac> -t 2000 -f s16le -ac 1 .ci-artifacts\ebu_pal_40msps_excerpt.raw
# -> 160,000,000 bytes = 80,000,000 samples = 2 real s @ 40 MSPS. Sample convention: 0.00% non-mod-16
#    = the MISRC recorder 12-bit<<4 domain (the decoder is level-adaptive/amplitude-invariant; fed as-is).
#    ("ffmpeg" bare resolves to a ps1 wrapper that eats -i — always use ffmpeg.exe.)
```

### A. Colour phase — investigation
- Extended the TEMP live diag (`run_live_diag`) to `gui_cvbs_test.exe [--pal|--ntsc] [--rate=<MHz>] [--u8|--s16] [--fixed] [--full] [--ppm=<path>] [--declared] <raw>`; the per-bar U/V is now a multi-row VECTOR mean over picture rows with a coherence ratio (|mean|/mean-amp; 1.0 = every row agrees, ~0 = rows disagree).
- Run 1 (self-calibrated, AUTO sync): locked, 92 frames, meas_rate **39.98 MHz**, every field boundary rejected (`vertical EQ lacks recent H reference`), bars rotated (yellow 178.1 vs 167.1, red 108.7 vs 103.5, blue 7.5 vs 347.1 — mean ~+14°).
- Run 2 (`--declared`, exact 40.000 MHz): ALL bars within ±3.6° of the EBU vectors → the burst-locked demod convention is CORRECT; the jitter-biased H-spacing median (edge quantization ±1 sample) is the hue bug: at 40 MSPS a 0.05% rate error sweeps the line-local carrier ~0.02°/sample and rotates the late-active hues by up to ~10°.
- Python pulse-structure analysis of the raw excerpt (the decoder's LPF/threshold models reproduced standalone): the real PAL VBI is TEXTBOOK (last H exactly 1.00 line before the first EQ; then 5 EQ / 5 broad / 5 EQ at half-line cadence; H 4.70 µs, EQ 2.35) at the `(tip+blank)/2` threshold (-16240) — and the two-peak picker's threshold landed ABOVE true blanking (-9134 vs blank -10675): the classification degenerated into 36,928 sub-µs fragments per 0.1 s and rejected every train.
- FIXED sync (the GUI panel default) was additionally broken end-to-end by the same threshold: the first-lock freeze (10 syncs) latched a ~1.3-sample period residual → wraps late → porch windows misaligned → committed black ~2 IRE high → threshold ~300 counts too shallow → false fragments → 125 cadence rejects + scrambled hues.

### Fixes (misrc_tools/misrc_gui/signal/gui_cvbs.c + gui_cvbs.h)
1. **Rate refinement** (pll_process_hsync + the header's documented-but-never-implemented intent): the PLL's converged period commits as the measured rate — AUTO: `line_period+freq_adjust`; FIXED lock: the frozen period + a slow EMA of good-sync `phase_error` (a pure MEASUREMENT — an EMA fed back into the wrap period was measured UNSTABLE and is explicitly rejected in cvbs_effective_line_period's comment). Gated on 1000 good syncs since lock, bounded ±10% of the running rate. New state: `pll.lines_since_lock`, `fixed_period_ema`.
2. **Sync threshold**: `commit_adaptive_levels` now ALWAYS derives `thresh_ref = (tip+blank)/2` (tip = the raw min — chroma never reaches the tip; blank = the porch once valid, the picker's second peak before) — it previously trusted the picker's `sync_threshold` until the porch committed (a chicken-and-egg: the lock that gates the porch needs a working threshold). The one-buffer bootstrap is now the FILTERED min + 25% of the filtered min..max span (no two-peak picker — its "tip" peak mispicked onto mid-level content), with a 64-sample filter warm-up skip (a chunk starting on a chroma peak — measured mx=12306, the cyan bar's raw peak — otherwise carried that peak as the "filtered" max and lifted the threshold above blanking, fusing the black bar + porches + sync tip into one ~15 µs blob whose fragments spawned false first-EQ candidates; the synthetic 100% bars exposed the same content-dependence via the yellow bar's +44.7 IRE chroma peak).
3. **FIXED sync mode**: the freeze is delayed until the acquisition integral has CONVERGED (1000 good syncs since lock — leaves <0.01 samples of residual; the first-lock freeze latched ~1.3), the acquisition branch mirrors AUTO (wild-sync guard, per-line snap once locked, integral converging the period), and the locked branch guards the snap against wild syncs (never snap to a noise/garbage classification).

### Verified result (live, both sync modes)
| bar | AUTO angle | FIXED angle | EBU truth |
|---|---|---|---|
| yellow | 166.4° | 166.5° | 167.1° |
| cyan | 284.0° | 283.9° | 283.7° |
| green | 241.0° | 241.0° | 240.6° |
| magenta | 60.5° | 60.5° | 60.7° |
| red | 103.7° | 103.6° | 103.5° |
| blue | 346.8° | 346.9° | 347.1° |

All within ±0.7°, coherence 0.98–1.00, accepted 99/99, rejected 0, meas_rate 40.00 MHz, both AUTO and FIXED (the GUI default) — FIXED now equals AUTO. (Before the fixes: +10–22° rotation, 247/352 rejects.)

### B. Hybrid (tbc-tools analyse HYBRID mode)
- `gui_rf_preview.h`: new shared `RF_HYBRID_BLACK_IRE_PAL/NTSC` + `RF_HYBRID_SPAN_IRE_PAL/NTSC` (-38.7543/174.147 PAL, -44.1843/197.680 NTSC — the derivation comment moved here); the RF preview's `rf_profiles` initializers now use the macros (the two decoders cannot drift).
- `decode_line_to_pixels`: the per-line VBI contrast-stretch is REPLACED by the positional hybrid split (like tbc's activeVideoStart/End check and the RF preview's verified active_x0/x1, from the same profile active window the render crop uses, sync-edge-delay compensated): the active horizontal window of picture lines keeps `rf_display_level` (studio-swing, now on the gray paths too — chroma-free behaviour unchanged, only the level transfer differs from the legacy raw 0..255); everything else (VBI lines + the blanking/sync/burst regions of picture lines) uses the fixed IRE-domain hybrid background mapping; the chroma demod window widened from the active-only clamp to the full line so the background KEEPS the decoded chroma (tbc's hybrid chroma mode).
- New synthetic contracts `run_full_view_hybrid(PAL/NTSC)` in gui_cvbs_test.c (the generator's lines 8..21 carry normal content while classified VBI = the exact chroma-keeping case):
  `porch dy=0.224 (expected 0.223/0.224), sync tip dy=0.000 (floor), picture black 0.039 (lifted studio floor), VBI-row yellow r=0.92 b=0.00 (chroma kept), VBI-row black 0.224 (hybrid blanking) — all pass.`
- Live full-view verification (`cvbs_live_full.ppm`, 1135x625, Python pixel profile + ffmpeg signalstats: U/V 29-238, SATMAX 118 — full chroma): the EBU bar ORDER + hues correct, the black bar at the 10/255 lifted studio floor, sync tips clamped at the hybrid floor, porch ≈ 52-57/255 (expected ~57), the VBI EQ/broad trains readable at hybrid contrast, the burst-tinted blanking lines keep decoded chroma.
- The stale "contrast offset baked in above" render comment replaced with the decode-side hybrid description.

### C. FFT
- "Scale: Lin" → "Scale: Linear" at both label sites (render + the click-rect measurement; the button auto-sizes). Stock default documented: Linear (the original pre-feature mode; `log_scale` starts false).
- Chrome-hide now hides ONLY the UI: the Scale button + the "FFT" corner title. Ungated (always visible): the dB axis labels, the bottom frequency labels, the /div + log-span + zoom readouts, and the peak-hover marker + readout.

### D. Validation
```
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts\build-local.ps1   # build + --smoke-test PASS
python -m mesonbuild.mesonmain test -C build-local                     # 34/34 OK, 0 Fail
.\build-local\gui_cvbs_test.exe                                        # all contracts incl. the new hybrid pair
.\build-local\gui_cvbs_test.exe --pal --rate=40 --s16 [--full] [--fixed] [--ppm=...] .ci-artifacts\ebu_pal_40msps_excerpt.raw
```

## Known observations (recorded, non-blocking)
- The real bars decode ~10–20% under-saturated vs the EBU 75% amplitudes (hue exact): the 20 IRE burst-reference constant vs this signal's actual burst + the RGB-clamp distortion in the diagnostic inversion. A possible follow-up calibration item.
- The real pattern's white bar decodes ~83% display (pure gray, correct hue): pattern/level-model amplitude scaling, cosmetic.
- The NTSC 20 MHz synthetic red-bar 0.192 blue-leak print persists (a tolerance REPORT, not an abort; pre-existing from the crashed session).

## Not done / pending
- GUI confirmation checklist: `prompt_logs/2026-10-19_cvbs-colour-phase-hybrid_README.md` (the user).
- NO commit, NO zip restore point — only after the user confirms "fully working" (per rule).
