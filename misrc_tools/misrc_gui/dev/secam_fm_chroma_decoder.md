# DEV NOTE: SECAM FM-chroma decoder (backlog item 4)

Status: shipped 2026-10-20 (colour decoding user-verified on a real capture);
one open follow-up - vertical rolling on that same real capture (see OPEN).

The tbc-tools `secamdecoder` pipeline (libchromadec v0.1.0 parameters) ported
to the GUI's streaming real-time per-line decode in
`misrc_tools/misrc_gui/signal/gui_cvbs.c` (the SECAM section, ~lines 771-1750)
+ `cvbs_secam_state_t` in gui_cvbs.h. Everything the reference does per FIELD
in the block-FFT domain runs here per LINE in the time domain inside the
real-time budget. Read the block comment at the top of the SECAM section
before touching any of it.

## Pipeline (and the measured reasons it looks like this)

- NCO mix-down of the FM block (locked centre + converter offset) -> band
  LPF + rate-relative decimation (~2.21 MHz) keeping BOTH the bell-free
  analytic path (porch calibration, ident, and THE PICTURE DEMOD) and the
  belled path (numeric contract only).
- THE PICTURE DEMOD RUNS BELL-FREE (g_secam_fref). Measured: the bilinear
  cloche's asymmetric phase response biased the belled steady carriers by
  -19.5 kHz at foR / +10 kHz at foB, and its amplitude tilt dropped
  strongly-deviated sidebands below the concealment's 12 dB dip floor -
  whole bars squashed to ~+0.25 D'x. The bell stays built + pinned by the
  numeric contract; carrying it into the demod needs pre-warp correction
  or magnitude-only post-shaping (future work).
- Discriminator: the reference's 9-tap maximally-flat central difference on
  the analytic signal (exact phase derivative). Measured on the synthetic
  bars: the demod reads every plateau within ~3%.
- De-emphasis: the reference's maskDeemphasis is a COMPLEX mask (the exact
  causal lead-lag (1 + j f/(3 f1))/(1 + j f/f1) WITH PHASE, overlap-save so
  the state runs continuously). The port is the same network as a CAUSAL
  first-order bilinear IIR at the decimated rate, state carried across
  lines/fields (reset only on a filter rebuild). A common pre-warp factor
  matched at 220 kHz keeps DC gain 1 and the 1/3 HF shelf exact and the
  deviation band at ~1.5% worst (numbers in the build comment). DO NOT
  reintroduce a zero-phase/symmetric-FIR or per-line-reset realization: the
  first port's FIR bled neighbouring bars into each bar centre by up to 26%
  and could never cancel the encoder pre-correction's phase.
- Rails BEFORE de-emphasis (BT.470 Table 4 asymmetric limits, verbatim),
  concealment (envelope dip + noise-adaptive overshoot, row-2 borrow) on
  the belled-FREE envelope (the belled envelope's tilt false-flagged every
  strongly-deviated bar), then the causal IIR, scale (1.505/1.902 verbatim)
  into u01/v01, causal one-line hold for the missing component.
- Field commit ONE FIELD BEHIND (porch medians -> cluster split + the EXACT
  10*fH pair lock + gates + last-good reuse), per-line porch zero, ident
  ladder (porch decisive -> bottles -> content median) + the frame-continuous
  parity flip at odd-line-count fields.

## Harness (misrc_tools/test/gui_cvbs_test.c)

- `gen_secam` models a REAL encoder: LF pre-correction biquad THEN the
  deviation limiter (the Table-4 maxima applied to the PRE-CORRECTED
  waveform). Without the limiter the pre-correction's x3 transition spikes
  sweep the carrier +-1.9 MHz outside any receiver's band - the decoder's
  envelope collapses and the colour killer gates 7-8 decimated samples at
  bar heads to zero (measured), which the causal de-emphasis integrates as
  real deviation. No real encoder emits that.
- Fixtures: "SECAM studio" and "SECAM offset +105 kHz" are PRE-CORRECTED
  (the de-emphasis must invert the pre-correction end to end; real signals
  always are); "SECAM flat deviation" is the robustness case (bar-7 chroma
  tolerance relaxed to 0.30 for its legitimate causal tail).
- Numeric contracts: bell tiers (0.651%/9.012%/22.461%) and the de-emphasis
  IIR response tiers (<2% deviation band / <8% above; measured 1.507%/2.911%).
- Bars asserts (hue order, black bar, calibration within tens of Hz, ident
  conf, VBI) all pass on all three fixtures. The >0.14 RGB prints on
  saturated bars are the ENCODER LIMITER's transition clamps (real SECAM
  behaviour on 100% bars; cyan's D'R plateau itself clips at the +350 kHz
  Table-4 limit) - diagnostics, not contract failures.

## UI

- CVBS-mode System dropdown: PAL/NTSC/SECAM (0/1/2 = gui_cvbs_set_format's
  space; the button label tracks the decoder's ACTUAL format).
- OSD Stats: SECAM-only 4th line (fB/fR, offset, ident conf/source,
  cal/reuse/nom) via `gui_cvbs_get_secam_diag`.
- `video_system` setting: the dropdown persists the pick (app settings +
  live session default); panels are created in it (gui_cvbs_set_default_system).
- The panel framework's ONE-SLOT view-state cache (gui_panel.c) keeps
  decoders alive across view switches (this note's OPEN item was originally
  mis-seen as "switching panel modes resets to PAL / loses sync").

## Verification record (2026-10-20)

43/43 static guards (`SECAM FM-chroma mode contract` pins the parameters
verbatim + the dropdown/OSD wiring + system_options_rect[3]); build-local
70/70 + smoke 0; `meson test` 34/34; harness output in
`prompt_logs/2026-10-20_secam-fm-chroma-port_README.md`. User real-GUI:
colour decodes on a real SECAM capture; System dropdown + width + view
switching + persistence confirmed through two feedback rounds.

## The baseband rolling (2026-10-21 continuation) - FIXED, root-caused

The user's real baseband SECAM capture (HackDAC V2.5, EBU bars) initially
reproduced the rolling headlessly (locked H, 0 accepted fields). Root
causes, ALL measured (the classifier diag + per-chunk resampler checksums):

1. HARNESS: `--panel` mode never applied `--secam` to the plugin decoder
   (created in the persisted default = PAL) - every earlier panel run was
   PAL-format (the LIVE secam diag line never printed). Fixed: the panel
   branch now calls gui_cvbs_set_format from the flags.
2. VBI CLASSIFIER: the AC-coupling baseline sag through the vertical
   blanking (+19-26 IRE over the train lines, same on PAL/SECAM) lifts
   the DC-clamped slicing threshold 9-16 IRE above the pulse midpoint:
   - the 2.35 us train EQs read 3.4-3.7 us, crossing the 3.5 EQ/H band
     edge - eq_run reset on EVERY train EQ - zero field candidates;
   - the 3rd+ broads of the 5-broad train read 0.8-0.9 us (shallow) or
     never cross at all (the recovering baseline lifts their floors above
     the per-line threshold; the next crossing is a picture H six lines
     later) - the 3-broad handshake never completed on those fields.
   The SPACING is threshold-invariant, so two cadence-assisted
   reclassification rules recover the true classification (sag-inflated
   EQ; sag-shallow broad), and the handshake fires on the SECOND broad
   (the anchor math - first_eq_pos + parity - uses the pre-EQ train, so
   firing one broad earlier changes nothing). See the code comments at
   the classifier.
3. RESAMPLER NONDETERMINISM: the system soxr's INT16 output path dithers
   with a run-varying seed - identical input produced different output
   runs (per-chunk checksums), and the outcome flipped 0 vs 282 accepted
   fields between identical runs. Fixed: the panel resampler outputs
   FLOAT32 (normalized +-1.0 - rescale x32768) and the int16 conversion
   is deterministic lrintf; the decode is now bit-identical run to run.

The PAL capture survives the same sag by margin (its trains completed
499/499 before and after - the rules only tighten the gates).

## Verification record (2026-10-21, the real baseband capture)

- SECAM (EBU bars, HackDAC V2.5, 40 msps 12-bit, 10 s): locked=1,
  accepted=364 rejected=5 (cadence, acquisition period), steady ~97%
  after ~2 s; calibration fob/for within 206 Hz of nominal (490/491
  fields, porch source), ident conf 0.96; bars: red 103.4 deg (PAL EBU
  ref 103.3), yellow 163.4, magenta 62.2, cyan 284.2, green 238.2,
  blue 350.0, coherence 0.83-1.00, black-bar chroma ~0.
- PAL control (same device): 499/499 accepted, yellow 167.1 deg / red
  103.3 deg coherence 1.00 - unchanged.
- Budget (SECAM stage ENGAGED): 19.0 us/line, 0.51x real time total
  (PAL reference 13 us/0.40x - the SECAM stage adds ~6 us/line ≈ 1.9
  ms/field, under the 4 ms/field plan line; the total is marginally
  above the 0.5x target - record, optimize if it matters).
- Determinism: identical per-chunk checksums + identical end-to-end
  results across runs.
- Static guards 43/43; full rebuild (see below) + smoke 0; meson test
  34/34; the synthetic suite unchanged.

## OPEN (resumable)

- Real-GUI confirmation on this capture (the user plays it in the GUI:
  bars in correct colours, the OSD SECAM line's calibration stable, no
  rolling after the ~2 s acquisition).
- The early-acquisition period (~2 s, only 8 candidates by frame 125)
  and the 5 cadence rejects are acceptable but could be tightened later.

## NOTE: the meson 1.10->1.11 reconfigure trap (2026-10-21)

A manual `meson setup --reconfigure` after a meson version bump LOSES
the environment `scripts/build-local.ps1` sets (PKG_CONFIG_PATH with
.deps/install first) - `dependency('raylib')` then resolves to the SYSTEM
MSYS2 raylib (built against the glfw DLL) and every raylib-linking target
fails with `__imp_glfw*` undefined under -static. ALWAYS re-run
`pwsh -File scripts\build-local.ps1` after a meson version change (it
wipes and regenerates with the correct environment). Also: the machine
was under memory pressure during the from-scratch rebuild (page-file
exhaustion at default parallelism; transient CreateProcess failures) -
`ninja -j1/-j2` completes fine.

## HARD rules

1. The picture demod stays on g_secam_fref (bell-free). Bell changes need
   the numeric contract re-run.
2. The de-emphasis stays CAUSAL with carried state. Any per-line reset or
   symmetric FIR regresses the bars contracts.
3. The concealment envelope source stays bell-free (lpre/lpim).
4. The generator's limiter is part of the contract - removing it recreates
   the killer-gating failure mode.
5. Parameters are VERBATIM from the reference; the guard pins them. Changes
   need a measured reason in the prompt log.
