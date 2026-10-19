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

## Round 2 (same day): the GUI panel path — the actual user symptom, root-caused and fixed

User GUI check: hybrid GOOD (committed as `83a97e8`), the colours STILL wrong in the GUI. The GUI panel
path differs from every headless path verified in round 1: the panel plugin RESAMPLES the input to a
fixed 4x-fsc-classic target (PAL 17.9 / NTSC 14.3 MHz) via streaming soxr (`cvbs_panel_target_rate` /
`video_cvbs_plugin_process`) - the GUI decoder never sees the 40 MSPS stream, so the round-1 live
verification never exercised the GUI's decode regime. New diag mode `--panel` drives the EXACT plugin
path headlessly (the plugin struct zeroed first - `video_cvbs_plugin_init` initializes only the decoder,
so the soxr fields must start NULL/0 or the resampler ensure dereferences garbage; `TraceLog` stubbed
for the link). Reproduced the symptom immediately: green 280.8-338.1 deg (truth 240.6), coherence
0.25-0.31, every other line wrong.

Root causes - three stacked, found by A/B runs plus per-line burst/PLL traces:
1. The per-line k=0 carrier-table frame carries every quantized wrap/snap line-origin offset into the
   measured burst phase. At the panel's 4x-fsc decode rate ONE SAMPLE of origin wobble is ~89 degrees,
   so the per-line burst-phase steps lost the true +-90 PAL alternation (they landed on a
   +-90-scattered grid), the V-switch flipped on every other line and half the frame's hues scrambled.
   At 40 MSPS the same wobble is only ~40 degrees, which the +-90 steps survive - why the round-1
   direct-rate verification looked correct while the GUI did not.
2. The obvious fix for (1) - an ABSOLUTE sample-stream frame - demands an EXACT dphase: the
   self-calibrated rate (median + PLL refinement, ~0.01% residual) slips the absolute table tens of
   degrees per line against the real carrier. Measured: with the always-absolute frame the direct-40
   FIXED path regressed to green 281.6 deg / coherence 0.28.
3. The panel resampler's OUTPUT rate is exact by construction (the soxr ratio is exact) - but the
   self-calibration kept overriding that exact declared rate with its own ~0.01-0.06% error. Proven by
   forcing the declared rate in `cvbs_rate` for one A/B build: with the exact 17.9 MHz the panel path
   decoded every bar within +-0.2 deg with coherence 1.00.

Fixes (all in the working tree, uncommitted):
- `rate_trusted` (gui_cvbs.h + the plugin): the plugin marks the declared rate trusted after a
  successful resample (and clears it on the passthrough, so a stale flag cannot outlive a resampler
  failure); `cvbs_measure_hh_spacing` (the median) and the PLL refinement's EMA + commit are skipped
  when trusted. `lines_since_lock` still counts under trust (the FIXED freeze gate depends on it).
- Hybrid carrier frame in `decode_line_to_pixels`: ABSOLUTE sample-stream frame when the rate is
  trusted (the exact-rate panel path), per-line k=0 reset when the rate is self-calibrated (the
  direct paths). The anchor source: `decoder->global_sample_pos` written back at the wrap.
- Sub-sample sync edges: linear threshold-crossing interpolation on the falling/rising edges
  (`prev_filtered` read from the Q8 LPF state) - the pulse width and the PLL snap carry sub-sample
  resolution instead of the integer crossing's +-0.5 samples/pulse quantization.
- The synthetic generator's carrier is now CONTINUOUS across lines (the real PAL 283.75-cycles/line,
  NTSC 227.5 convention - it previously reset per line), matching the absolute-frame decoder.

Verified (all headless, live EBU capture):
- The EXACT GUI plugin path (`--panel`): every bar within +-0.2 deg of the EBU vectors (yellow
  167.1/167.1, green 240.7/240.6, red 103.5/103.5, blue 347.1/347.1), coherence 1.00 on every bar,
  99/99 fields accepted, 0 rejected.
- The direct paths (AUTO + FIXED at 40 MSPS, self-calibrated, per-line frame): every bar within
  +-1 deg, coherence 0.97-1.00 (restored from the absolute-frame regression).
- The synthetic suite (incl. the new full-view hybrid contracts) and the full meson suite 34/34;
  canonical build + smoke test PASS.

## Round 3 (same day): the render-path R/B channel swap — the last visible symptom

User GUI report: White/Cyan/Yellow/Green/Magenta/Red/Black ("bust is Blue") instead of the standard
White/Yellow/Cyan/... — while the user VISUALLY CONFIRMED the decoded PNGs correct. Decoder right,
GUI render wrong. Root cause: the RGB24->RGBA conversion packed raw uint32s
(`0xFF000000 | r<<16 | g<<8 | b`) whose LITTLE-ENDIAN byte sequence is b,g,r,a - but the image is
raylib `Color` memory (r,g,b,a bytes; the init allocates `Color*` and sets
PIXELFORMAT_UNCOMPRESSED_R8G8B8A8), so the display read the decoded BLUE as red and RED as blue:
every hue mirrored across the gray axis (yellow<->cyan, red<->blue; white/green/magenta/black
unchanged) - exactly the reported bar pattern. The PPM/PNG dumps bypass the conversion (they read
the RGB24 buffers directly), which is why they were correct. The user-verified RF preview path
(`rf_render_rows`, gui_rf_preview.c) packs byte-wise [r,g,b,a] - the working convention.
FIX: the conversion writes through raylib's own `Color` struct fields (type-safe, matching the
allocation and the RF preview convention). Rebuilt: misrc_gui.exe 14:44, smoke PASS, the CVBS
contract suite green. This render bug predates today (it appeared with the crashed session's
colour decode; invisible in grayscale) - the earlier GUI colour reports were the composite of
the real decoder bugs (rounds 1-2, all measured) AND this swap.

## Not done / pending
- GUI confirmation checklist: `prompt_logs/2026-10-19_cvbs-colour-phase-hybrid_README.md` (the user) -
  the colour items are now verified headlessly on the EXACT panel plugin path AND the render-path
  swap is fixed (the decoded PNG the user confirmed = what the GUI should now display).
- Rounds 2+3 are UNCOMMITTED (the user's flow: GUI confirmation first). `83a97e8` (round 1 + the
  hybrid) is committed locally, not pushed.
- NO zip restore point — only after the user confirms "fully working" (per rule).
