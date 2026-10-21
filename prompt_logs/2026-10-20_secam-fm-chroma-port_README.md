# Prompt log — SECAM FM-chroma mode, backlog item 4 (2026-10-20)

User input this session (the item-4 arc, WIP until real-GUI confirmation):

1. "rebuild with fixes" — continue the SECAM port (one defect located: the
   belled-path discriminator bias).
2. "finish the implimentation then and dedicated mode box option for SEACAM
   so I can test in the GUI" — finish the decoder AND add the GUI selector.
3. "I am wondering why tape-decode comes up as CVBS and VHS FM decoders are
   the only active ones we are working on.." — answered (dormant plugin
   behind VIDEO_TAPE_DECODE_UI_ENABLED 0, commit 83a97e8).
4. "okay keep going and add the UI elements" — the System dropdown/OSD work.
5. "also the VHS FM decoder UI boxes should be vertically stacked like the
   CVBS decoder as this is better compaible with more scaling modes" — the
   VHS FM overlay rework.

## Root causes found and fixed (all measured, harness dumps)

### 1. The bell's phase bias in the picture demod (pre-existing WIP defect)

The bilinear cloche's asymmetric phase response (its centre sits 42 kHz below
the block centre) biased the BELLED discriminator's steady carriers by
-19.5 kHz at foR / +10 kHz at foB, and its amplitude tilt dropped the
strongly-deviated sidebands below the concealment's 12 dB dip floor — whole
bars squashed to ~+0.25 D'x. FIX: the picture demod now runs on the BELL-FREE
discriminator (g_secam_fref — the reference's fInstRef path the porch/ident
already used); the bell stays built + pinned by the numeric contract.

### 2. The de-emphasis realization was unfaithful to the reference

The reference (secamdecoder.cpp:343/347,605-631) applies inverseLf as a
COMPLEX FFT mask — the exact causal lead-lag (1 + j f/(3f1))/(1 + j f/f1)
WITH ITS PHASE — per field with overlap-save, so its state runs continuously.
The port had realized it as a ZERO-PHASE symmetric FIR reset per line:
wrong on both counts. Measured on the bars fixture: the symmetrised kernel
put half its energy into the FUTURE, bleeding each bar into its neighbours
by up to 26% at the bar centres, and it could never cancel the encoder
pre-correction's phase. FIX: a CAUSAL first-order bilinear IIR at the
decimated rate with the state carried across lines/fields (reset only on a
filter rebuild). The common pre-warp factor is matched at 220 kHz (the
measured error-curve balance): DC gain 1 and the 1/3 HF shelf ratio are
exact by construction, the deviation band holds 1.5% worst (a plain bilinear
measured 2.4% mid-shelf; the geometric-centre 147 kHz match 2.0% at the
350 kHz edge; independent corner pre-warping 3.7% mid-shelf + broken shelf
ratio).

### 3. The synthetic generator emitted an unphysical signal

The pre-correction's x3 transition spikes swept the carrier up to +-1.9 MHz —
outside any receiver's +-580 kHz band: the decoder's envelope collapsed and
the colour killer gated 7-8 decimated samples at the cyan/magenta bar-heads
to zero (SECAMWAVE dump, i=49-56/78-84 read d=+0.000), which the causal
de-emphasis then integrated as real deviation. A real SECAM encoder clamps
the PRE-CORRECTED deviation (its deviation limiter, the BT.470 Table-4
maxima — the same limits the decoder's rails enforce). FIX: gen_secam models
the limiter. Fixture semantics corrected to match reality: a real encoder
ALWAYS pre-corrects, so "SECAM studio" and "SECAM offset +105 kHz" are
PRE-CORRECTED (the de-emphasis must invert the pre-correction end to end);
"SECAM flat deviation" is the robustness case (no real encoder emits it; its
causal de-emphasis legitimately tails the previous bar's colour into black —
bar-7 tolerance relaxed to 0.30 for that fixture only).

## UI elements (all user-interactive, awaiting real-GUI confirmation)

- CVBS-mode System dropdown: PAL/NTSC/SECAM (system_options_rect[3]; the
  button label tracks the decoder's ACTUAL format; click maps through
  gui_cvbs_set_format's 0/1/2 space). The dormant tape-mode plumbing
  (video_tape_apply_runtime_args) moved to the same 3-way selected_system
  space so SECAM cannot mislabel as PAL if that mode is ever re-enabled.
- OSD Stats: a SECAM-only 4th line (locked fB/fR, block offset, D'R parity
  + confidence + ident source, cal/reuse/nominal field counters) via
  gui_cvbs_get_secam_diag.
- VHS FM overlay: the horizontal [Frame][Stats][System][Decoder] row +
  second-row Budget became a right-aligned vertical stack (uniform column
  width, the dropdown owner [Stats] last so its options drop below the
  column over the video — the CVBS System pattern).

## Validation (commands + results)

1. `python -m mesonbuild.mesonmain compile -C build-local gui_cvbs_test` +
   `& build-local\gui_cvbs_test.exe`:
   - SECAM numeric bell match: carriers 0.651% / deviation 9.012% /
     skirt 22.461% (unchanged, within tiers).
   - SECAM numeric de-emphasis match: deviation 1.507% / high 2.911%
     (tiered contract: <2% deviation band, <8% above).
   - All three bars fixtures: every ASSERT passes (VBI lock, calibration
     within tens of Hz, 7/7 fields calibrated, exact 10*fH split, ident
     conf 1.00, hue order, black bar). Bars max RGB error 0.31-0.32 on the
     pre-corrected fixtures vs PAL's 0.06-0.15 class: the residual is the
     encoder limiter's transition clamps on 100% saturated bars (real SECAM
     behaviour; cyan's D'R plateau itself clips at +1.25 = the +350 kHz
     Table-4 limit) — >0.14 channels print as diagnostics, they are not
     contract failures.
   - PAL/NTSC contracts unchanged (max error 0.059-0.150, the pre-existing
     NTSC @20 MHz 0.150 print remains).
2. `python misrc_tools/test/ci_guard_tests.py --static-only`: 43/43 PASS
   (the SECAM FM-chroma mode contract now also pins the System-selector
   options, the dropdown click wiring, the OSD diag readout, and
   system_options_rect[3]).
3. `pwsh -File scripts/build-local.ps1`: guards + build 70/70 + link OK.
4. `& build-local\misrc_gui.exe --smoke-test`: exit 0.
5. `python -m mesonbuild.mesonmain test -C build-local`: 34/34 OK, 0 fail.

## TEMP diagnostics used and REMOVED before commit

SECAMDIAG/SECAMCOMMIT stderr blocks, the SECAMFDUMP per-bar dump, the
SECAMWAVE per-sample dump (all harness-only via CVBS_LIVE_DIAG or in-run
scratch), and the per-bar U/V inversion block in run_secam_bars. Also fixed:
the MATEERIAL typo, gen_secam's unused `centre`, check_bars gained the
bar7_chroma_tol parameter (0.10 strict for PAL/NTSC/pre-corrected; 0.30 for
the flat robustness fixture).

## Real-GUI feedback round 2 (user testing 15:25-16:04) - fixes

1. System box too wide -> 110 px floor (in line with Frame/OSD/Levels;
   the 140 floor was sized for the old long "PAL/SECAM" label).
2. "Sync lost when switching full-frame/active" -> the frame-view toggle
   was calling a FULL decoder reset on every switch; the decode geometry
   has been view-independent since it became a render-time crop, so the
   reset is removed (the toggle just re-crops; the lock carries over).
3. "System config should be persistent, not just PAL always" -> new
   `video_system` setting (0/1/2): the System dropdown writes the pick
   into app->settings AND the live session default; settings save at
   exit; every panel decoder is created in the persisted system
   (gui_cvbs_set_default_system pushed from misrc_gui after
   gui_settings_load).
4. ROOT CAUSE of "switching back to PAL" + "sync not fixed": switching
   PANEL VIEWS (Waveform<->Video) destroyed and re-created the view
   state - a fresh decoder (full re-acquisition = the sync loss)
   initialized to PAL. FIX: a ONE-SLOT view-state cache in the panel
   framework (channel_panel_config_t gains left/right_cached_view+state;
   panel_config_set_left_view/right_view/set_split park the live state
   instead of destroying it and restore it when switching back; a
   different view evicts the parked state; cleanup destroys both). A
   parked panel is neither rendered nor processed; its decoder resumes
   with one stream-jump relock instead of a full re-acquisition (levels,
   rate calibration and SECAM calibration all persist). Benefits every
   panel type (waveform zoom etc.), not just Video.

Validation after round 2: 43/43 guards, GUI+harness build OK, smoke 0,
harness asserts green, meson 34/34.

## Commit round (user: "commit and push current works, we will resume
improvements later")

- Dev note `misrc_tools/misrc_gui/dev/secam_fm_chroma_decoder.md` +
  dev_notes_README entry; backlog item 4 marked SHIPPED with the OPEN
  rolling follow-up recorded (NOT fully DONE - the real-capture sync
  remains under investigation, honestly scoped).
- Restore point: `C:\Users\Harry\MISRC-GUI-restore-points\
  secam-fm-chroma-2026-10-20\` (every changed file + the note).
- Real-GUI status at commit: colour decoding user-verified on a real SECAM
  capture; the System dropdown/persistence/view-cache/width confirmed
  through two feedback rounds; the LIVE profile line and the rolling
  diagnosis remain OPEN (need the excerpt).
- Committed + pushed; CI (push trigger) must be green before close.

## Continuation round (2026-10-21): the rolling DIAGNOSED

User: "continue work". Instead of waiting on the excerpt, the loaded
playback source was recovered from the saved settings (video_system=2
persisted - the persistence fix is live - with playback_file_a still the
Diamler-Chrysler NTSC SP Tape_03 RF capture).

- FFprobe: 20 kHz header (the GUI FLACs store MSPS as kHz - same convention
  as the PAL EBU captures; -t is in 1/1000 s units), 16-bit mono.
- 10 real-second excerpt (`-ss 60000 -t 10000`), headless panel-path diag:
  - CONTROL `--ntsc`: locked=0 accepted=0 rejected=0 meas_rate=0.00.
  - `--secam`: identical - no lock in either system mode.
  - `cvbs_vbi_probe.py`: "no broad pulse found".
  - Per-line probe (scratch, $env:TEMP\secam-diag\probe.py): ~0.4
    zero-crossings/sample (a ~4 MHz FM carrier), near_lo=0.00 (no sync-tip
    plateaus), 10% midline smear - RF-domain (VHS video RF).
- DIAGNOSIS: the rolling was the RF-domain VHS capture fed to the baseband
  CVBS decoder - wrong domain, not a SECAM (or sync) defect. RF tapes
  belong in the VHS FM decoder mode. SECAM needs a BASEBAND capture.
- Fixed en route: the harness LIVE header labelled `--secam` runs "PAL"
  (now "SECAM"); meson 1.10->1.11 update needed
  `meson setup --reconfigure misrc_tools build-local` (source dir is
  misrc_tools, NOT the repo root).
- LIVE profile note: the SECAM-stage budget line on this capture is
  INVALID (the stage is gated on pll.locked, which never engaged) - the
  budget measurement still needs a real baseband SECAM capture.
- Validated after the label fix: harness asserts green, guards 43/43.
- Uncommitted: the label fix + these doc updates (awaiting the user's
  commit word).

## Continuation round 2 (2026-10-21, later): the baseband rolling FIXED

User pointed at the HackDAC V2.5 capture set
(Desktop\\EBU_HackDac2_MISRC_V2.5): PAL controls + a fresh 10 s baseband
SECAM EBU bars capture (40 msps 12-bit FLAC + a capture log).

The SECAM capture initially reproduced the rolling headlessly (locked H,
0 accepted). Root causes (ALL measured - the classifier diag + per-chunk
resampler checksums, see the dev note):

1. The harness --panel mode never applied --secam to the plugin decoder
   (persisted default = PAL) - all earlier panel runs decoded PAL format.
2. The VBI classifier's fixed width bands break on the AC-coupling
   baseline sag through the vertical blanking (+19-26 IRE; the DC-clamped
   threshold rides 9-16 IRE above the pulse midpoint): the sag-inflated
   train EQs (3.4-3.7 us) crossed the 3.5 EQ/H edge (eq_run reset -
   zero candidates) and the 3rd+ broads read shallow/never crossed (the
   3-broad handshake failed). Fixed with two cadence-assisted
   reclassification rules (spacing is threshold-invariant) + firing the
   candidate on the 2nd broad (the anchor math uses the pre-EQ train,
   unchanged).
3. The system soxr's INT16 output dithers with a run-varying seed -
   identical input produced different resampled streams run-to-run
   (measured per-chunk checksums; the outcome flipped 0 vs 282 accepted
   between identical runs). Fixed: FLOAT32 output (normalized +-1.0,
   rescaled x32768) + deterministic lrintf conversion; the decode is now
   bit-identical run to run.

The fix path was measured step by step: the A/B classifier diag (PAL
control vs SECAM), the nondeterminism hunt (two-run diffs, per-chunk
checksums), the raw-signal probes (baseline sag), and the train
handshake traces (eq_run/v_run per pulse).

En route also: the meson 1.10->1.11 version bump required a reconfigure
- a MANUAL `meson setup --reconfigure` loses build-local.ps1's
PKG_CONFIG_PATH (the system raylib won over the vendored one; every
raylib-linking target failed with __imp_glfw* undefined). Fixed by
re-running `pwsh -File scripts\\build-local.ps1` (wipe + correct env).
The from-scratch rebuild then hit the machine's memory pressure
(page-file exhaustion at default parallelism; transient CreateProcess
failures) - completed at `ninja -j1/-j2`.

FINAL VERIFIED (the real captures):
- SECAM: locked=1 accepted=364 rejected=5 (acquisition-period cadence;
  steady ~97% after ~2 s); calibration fob=4249794 for=4406044 (within
  206 Hz, 490/491 fields, porch source); ident conf 0.96; bars: red
  103.4 deg (PAL EBU ref 103.3), yellow 163.4, magenta 62.2, cyan 284.2,
  green 238.2, blue 350.0, coherence 0.83-1.00; budget 19.0 us/line,
  0.51x real time (SECAM stage +~6 us/line over PAL).
- PAL control: 499/499 accepted, yellow 167.1/red 103.3 deg coherence
  1.00 - no regression.
- Static guards 43/43; full rebuild + smoke 0; meson test 34/34;
  synthetic suite unchanged.

Uncommitted (awaiting the user's word): the classifier rules, the FLOAT32
resampler, the harness --secam panel fix, and these doc updates. STILL
OPEN: the user's real-GUI confirmation on this capture (bars in correct
colours, no rolling after the acquisition, the OSD SECAM line stable).

## Confirmation round (2026-10-21, evening) - user-verified stable

User rebuilt, tested the capture in the GUI and confirmed: "This is now
stable commit and push." Per the standing rules: restore-point zip +
commit + push (CI on the push). Backlog item 4's remaining open item
(the real-GUI confirmation) is CLOSED.

## Pending (resumable, per the standing rules)

- OPEN: vertical rolling on the user's real SECAM capture while the H-PLL
  holds - the Stats OSD `VBI rej (cad/par)` + reason while rolling, and a
  20 s raw excerpt through `gui_cvbs_test.exe --secam --s16 --rate=<MHz>
  [--panel]`. Same excerpt serves backlog item 3.
- The LIVE profile line on a real capture (SECAM stage within the field
  budget) — same capture session.
