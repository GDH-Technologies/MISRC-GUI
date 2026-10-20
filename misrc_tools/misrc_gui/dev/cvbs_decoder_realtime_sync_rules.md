# DEV NOTE (HARD): CVBS decoder — real-time budget, stream discontinuities, sync slicing

Status: incident-driven (2026-10-19, the "decoder chokes after 8-10 s" / shifted
top rows saga: five rounds of PLL/VBI/field-buffer changes before the root causes
were measured). Read this before touching `misrc_tools/misrc_gui/signal/gui_cvbs.c`
(the per-sample loop, `decode_line_to_pixels`, `pll_process_hsync`, the VBI model,
`deinterlace_fields`) or the display-thread feed (`gui_extract.c` -> `BUF_DISPLAY`
-> `gui_display_thread.c` -> `panel_process_all`).

Companion notes: `gui_output_verification_rules.md` (render-path verification),
session log `prompt_logs/2026-10-19_cvbs-sync-shift-root-cause_README.md`.

## The incidents (why this note exists)

1. THROUGHPUT. The panel decode (soxr 40->17.9 MHz + per-sample sync pass + per-line
   pixel decode) measured 1.08x REAL TIME on one core (`decode_line_to_pixels` alone
   56 us/line = 88% of a PAL field budget: per-pixel divisions, `cos/sin`, two window
   loops, a 64-bit division, `cvbs_rate()`). The display thread fell behind the feed;
   `BUF_DISPLAY` (up to 256 MB, no-wait "silently drop when full" policy, 160 MB/s
   fill) overran after ~8-10 s and then dropped 64K-sample (~26-line) chunks every
   few fields. Every drop is a stream jump; the PLL snap absorbed it but the frequency
   integrators ingested the wild phase error as a PERIOD measurement: the frozen
   period walked ~3.5 samples (3000 ppm), 29% of VBI trains cadence-rejected, fields
   completing at 264/337 lines, the post-VBI rows shifted horizontally. Toggling the
   decoder off let the ring drain ("works again for 8-10 s"). Playback paces through
   the same ring -> identical symptom on recorded files. The OSD "ms" value (16.8 /
   31.9 ms vs a 20 ms field) showed it on screen all along.
2. BASELINE WANDER. Measured on the raw 40 MSPS EBU HackDAC capture: after the
   vertical broad pulses the whole signal rises ~+22..26 IRE and settles over ~30
   lines (AC coupling, tau ~16 lines). With a FIXED sync slicing threshold (the
   steady-state tip/blank midpoint) the sync tips of field lines ~5-31 never crossed
   it (no H sync at all), line 32 sliced +11 samples late and each following line a
   little less as the offset decayed. Each decoded line's origin followed that error
   (the top-of-picture horizontal shift decaying over ~20 rows); the same offset made
   those rows brighter. NOT a PLL problem; the ring showed it in one look.
3. INT16 TRAP. The first clamp implementation silently never engaged: its gate used
   `levels.range` (int16). In the 16x s16 sample domain `white - tip` = 38619 wraps
   negative and the commit clamps it to 100, so the gate became `|offset| < 50`.
   Found only through a per-line trace of the measured values.

## HARD RULES

1. REAL-TIME BUDGET IS A CONTRACT. The video panel's whole cost per field (resampler
   + per-sample pass + per-line decode) must stay well under the field budget (PAL
   20 ms / NTSC 16.7 ms) on ONE core with headroom for the other panels: target
   <= 0.5x real time measured headlessly. Any change to the per-sample loop or the
   per-pixel loop lands WITH a before/after `LIVE profile:` line from the harness
   (`gui_cvbs_test.exe --panel` on a real capture, see Verification). Reference after
   the 2026-10-19 rewrite: 13-15 us/line, total 0.40-0.43x real time.
2. NOTHING PER-LINE-CONSTANT INSIDE THE PIXEL LOOP. `decode_line_to_pixels` pass 2
   runs ~18M times/s: no divisions, trig, rate lookups, `cvbs_us_to_samples`, or
   window loops per pixel. Window quantities are PREFIX-SUM differences
   (`g_pf_s/c/n/i/q`), reciprocals are per-line / per-window-length tables, the window
   centre is an exact Bresenham walk. Keep the luma FIR in the outer-taps /
   inner-samples form (it vectorises at -O3; the per-sample inner reduction did not).
3. A STREAM JUMP IS NOT A PERIOD MEASUREMENT. Width-classified H syncs always SNAP
   the phase (jump recovery), but every frequency integrator (AUTO `phase_integral`,
   FIXED acquisition integral, FIXED `fixed_period_ema`, transitional EMA) only accepts
   a GOOD sync (|phase_error| < lock_threshold). Never feed a wild error into a period
   estimate. `stream_jumps` (locked H > 0.3 line off prediction) is surfaced in the
   Stats OSD as `jumps:` - a clean feed reads 0; a climbing value means the FEED is
   dropping (`[BUFMGR] ... DISP:fill%/waits/drops` on stderr every ~2 s), not that the
   PLL is broken. Diagnose the feed first.
4. SLICE SYNC LIKE A CLAMPED SEPARATOR. The sync threshold is the committed tip/blank
   midpoint PLUS the current line's measured back-porch offset (`dc_offset`, measured
   mid-line the moment the porch is complete, gated on PLL lock and a plausible
   tip-to-porch swing 0.35..1.6x nominal, |offset| < one swing). The same offset
   shifts that line's luma black (DC restoration). Do not go back to a per-field or
   per-chunk constant threshold: real sources wander by tens of IRE across the VBI.
   The field blanking commit prefers the on-baseline lines (clean subset) so a sag
   never biases the committed black.
5. THE VBI BOUNDARY IS A CADENCE, NOT A PULSE. A field boundary candidate is only
   evaluated after a pre-EQ train (>= 3 EQ at half-line cadence) is followed by three
   broad pulses at half-line cadence (`v_run == 3`), as in the RF decoder
   (`rf_vertical_start`). Never trigger on "first EQ-width pulse after an H": real
   signals produce stray EQ-width pulses. Candidates must equal accepted + rejected.
6. FIELD BUFFERS: NO STALE ROWS. A new anchored field clears its destination buffer
   and resets its valid-row count; the deinterlacer weaves/bobs only the valid rows of
   each field and never writes past `frame_height` (625/525 are odd: the bob paths
   bounds-check the last odd row - the first version crashed `gui_cvbs_baseband` with
   0xc0000005). The OSD deinterlace label comes from `last_weave`, not the ready
   flags (the current field's flag is cleared at its start).
7. INT16 LEVEL FIELDS ARE NOT SAFE FOR RANGES. `cvbs_levels_t.range` and friends are
   int16; the s16 (12-bit<<4) domain exceeds them. Any new gate/ratio on signal levels
   computes in int32/double from `white_level - sig_min` (or the committed swing
   `porch_level - sig_min`), never from `levels.range`. The commit's range clamp now
   saturates instead of wrapping.
8. NO FILE I/O ON THE DISPLAY THREAD. `CVBS_LIVE_DIAG` (periodic 2 MB PPM + state
   dumps) is 0 in the GUI build; the headless harness defines it to 1 before including
   the decoder. Do not add logging/dumps to the decode path that run in the GUI.
9. MEASURE BEFORE THEORISING. The 2026-10-19 saga spent four rounds on plausible
   PLL/VBI/buffer theories. The facts came from: the diag ring (per-line phase error
   -> which lines have no sync / arrive late), the row-edge scan of the woven frame
   (which rows shift and by how much), the raw-signal probe (per-line levels and
   pulse widths around a VBI), and the throughput profile. Run those BEFORE changing
   decoder logic, and put the numbers in the prompt log.

## Verification procedure (mandatory for any decoder/sync/feed change)

1. Extract a real capture to raw s16 (FLAC headers say 40 kHz; `-t` is in header
   seconds: `ffmpeg.exe -t 20000 -i <flac> -f s16le -acodec pcm_s16le out.raw` = 20 s
   at 40 MSPS = 1.6 GB). Reference capture: the 1-minute EBU PAL HackDAC file
   (`cvbs-pal-ebu_2026.10.19_18.02.31_Video_RF_12-bit.flac`).
2. Headless EXACT GUI path (soxr to 4x fsc + FIXED sync + full frame), from a scratch
   dir so the diag dumps land there:
   `build-local\gui_cvbs_test.exe --pal --rate=40 --s16 --panel --full --ppm=out.ppm out.raw`
   Required: `accepted=N rejected=0`, every bar within +-1 deg of the EBU vectors with
   coherence ~1.00, `fixed=` period flat across the dumps, `LIVE profile` total
   <= 0.5x real time, `dc=` small mid-field.
3. Row-edge scan of the woven frame (`python misrc_tools/test/cvbs_row_edges.py out.ppm
   [rows...]`: per-row left/right edge of the white bar + row mean luma): the first
   picture rows must sit at the same x as mid-frame and bottom rows; row luma flat.
   Raw-signal probe around a VBI (per-line levels + pulse widths/kinds at the
   steady-state threshold): `python misrc_tools/test/cvbs_vbi_probe.py out.raw 0.30`.
4. Diag ring in `cvbs_live_diag_N.txt`: H syncs present from the first post-VBI
   lines (PAL from ~line 6-9), per-line phase error ~0; the `dc` clamp trace shows
   `acc=1` on normal lines.
5. `meson test -C build-local` (34/34) and `scripts\build-local.ps1` smoke.
6. GUI: run >= 60 s on playback AND live; OSD Minimal "ms" well under the field
   budget; OSD Stats `jumps:` stays 0 and `VBI rej` does not climb; user confirms.

## Contracts in force

- `gui_cvbs_profile_get()` + the harness `LIVE profile:` line (throughput split).
- `stream_jumps` counter (Stats OSD `jumps:`), `dc_offset` (`dc=` in LIVE line and dumps).
- `misrc_tools/test/gui_cvbs_test.c`: synthetic PAL/NTSC colour + no-burst contracts,
  damaged-VBI cadence reject, full-view hybrid contracts, render-blit contract, and the
  `--panel` live diagnostic with per-bar U/V, profile and PPM dump.

## Backlog (user-prioritised, 2026-10-19)

1. DONE (2026-10-20): player-mode drag-and-drop of capture files - per-channel
   panel zones + audio-rate monitor source in one drop (user-confirmed); see
   `playback_drag_drop_audio.md`.
2. DONE (2026-10-20): the Windows move/resize freeze - a subclassed window proc
   drives render-only WM_TIMER frames inside DefWindowProc's modal loop
   (user-confirmed); see `windows_modal_resize_render.md`.
3. Sync robustness on real media: live decode still loses sync on some 4:3 material
   (e.g. anime with fast action) and trips a little on the Philips test pattern.
   Capture those as raw excerpts and run the Verification procedure on them; expect
   level/threshold interaction with high-contrast content and large luma swings next
   to the sync (front-porch clamp window, tip tracking during whites), plus
   head-switching/tape sources needing the RF decoder's split-broad recovery ported.
4. SHIPPED (2026-10-20, colour user-verified on a real capture): the SECAM FM-chroma
   decoder - the tbc-tools secamdecoder pipeline ported to the streaming per-line
   decode (bell-free picture demod, causal pre-warped de-emphasis IIR, porch
   calibration + ident ladder, rails/concealment, one-line hold), a SECAM entry in
   the CVBS System dropdown (+ the persisted `video_system` setting + a one-slot
   panel view-state cache so view switches keep the decoders alive), the OSD SECAM
   diag line, and physically-correct harness fixtures. OPEN follow-up: vertical
   rolling on the same real capture while the H-PLL holds - needs the Stats OSD
   VBI counters + a 20 s raw excerpt through the Verification procedure (same ask
   as item 3); see `secam_fm_chroma_decoder.md` +
   `prompt_logs/2026-10-20_secam-fm-chroma-port_README.md`.
