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

## OPEN (do not close without data)

Vertical rolling on the user's real SECAM capture while the H-PLL holds
(colour keeps decoding). The sync machinery is untouched by this work and
the synthetic locks 7/7 fields, so this is capture-specific. Needs: the
Stats OSD `VBI rej (cad/par)` + reason line while rolling, and a 20 s raw
excerpt (`ffmpeg.exe -t 20 -i <capture.flac> -f s16le -acodec pcm_s16le
secam_excerpt.raw`) through `gui_cvbs_test.exe --secam --s16 --rate=<MHz>
[--panel]` - the same ask as backlog item 3.

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
