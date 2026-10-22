# CVBS sync instability / horizontal row shift — root cause session (2026-10-19, round 5/6)

Status: FIXED and USER-CONFIRMED in the GUI ("dead stable now"; top-area shift gone
after round 6). Committed to main with the HARD dev note
`misrc_tools/misrc_gui/dev/cvbs_decoder_realtime_sync_rules.md` (rules, verification
procedure, backlog); restore point zip + NOTE in `C:\Users\Harry\MISRC-GUI_restorepoints`.

## Symptom (user reports, both LIVE capture and FLAC PLAYBACK)
- Picture decodes correctly for ~8-10 s, then "the decoder chokes": a few rows at the
  top of the frame shift horizontally, a tearing band appears mid-frame, VBI/field
  acceptance degrades, and it gets worse over time. FIXED and AUTO sync both affected.
- Toggling the decoder off/on restores it for another ~8-10 s.
- User sample: `C:\Users\Harry\Desktop\EBU_PAL_HackDac2_MISRC_V2.5\1min\cvbs-pal-ebu_2026.10.19_18.02.31_Video_RF_12-bit.flac`
  (ffprobe: FLAC mono s16 (12-bit), header rate 40000 Hz, tags RF_SAMPLE_RATE=40000000,
  RF_TOTAL_SAMPLES=2400190464, 60.0 s, 1.29 GB. mediainfo not on PATH.)

## Two earlier attempts this session (kept, but NOT the root cause)
1. Field-buffer hygiene: clear the destination field buffer at each anchored field
   start, track valid rows per field (`field_line_count[2]`), weave/bob only valid rows,
   odd-height (625/525) bottom row handled without the +1 overrun (a first version of
   the bob path crashed `gui_cvbs_baseband` with 0xc0000005 — fixed before shipping).
2. VBI candidate gating like the RF decoder: a field boundary is only evaluated after a
   pre-EQ train (>=3 EQ at half-line cadence) is followed by 3 broad pulses at half-line
   cadence (`v_run == 3`), instead of on every first EQ-width pulse. Effect measured in
   the user's live dumps: candidates == accepted + rejected afterwards (false first-EQ
   candidates gone). The row shift remained -> not the cause.

## Evidence that located the real cause
Live dumps from the GUI run (build-local/cvbs_live_diag_44..49.txt, 18:05 local):
- frames 5625 -> 6250 in 18 s = 34.7 fields/s against real-time 50 fields/s.
- frozen FIXED period walked 1145.597 -> 1149.098 (3.5 samples = ~3000 ppm) with
  `ema=+0.60`; VBI rejects 2004/6985 candidates (29%), all cadence; fields completing at
  line 264 and 337 (overflow safety net) -> the elapsed sample count between real VBI
  trains was off by whole lines: the SAMPLE STREAM itself was discontinuous.

Headless reproduction on the SAME capture (first 20 s, exact GUI path = soxr 40->17.9 MHz
+ FIXED sync + full frame):
```
ffmpeg.exe -hide_banner -loglevel error -y -t 20000 -i <flac> -f s16le -acodec pcm_s16le .ci-artifacts\ebu_1min_first20s_40msps.raw
  (FLAC header says 40 kHz, so -t is in "header seconds": 20000 -> 800,000,000 samples = 1.6 GB)
build-local\gui_cvbs_test.exe --pal --rate=40 --s16 --panel --full --ppm=panel_20s_full.ppm .ci-artifacts\ebu_1min_first20s_40msps.raw
LIVE ... locked=1 accepted=999 rejected=0 (cad 0 par 0) frames=999
LIVE sync: fixed=1145.597 ema=+0.0000 ... phase_err=+0.01
bars: yellow 167.1 / cyan 283.5 / green 240.7 / magenta 60.5 / red 103.4 / blue 347.1 deg, coherence 1.00
WALL_SECONDS=22.8   <- 20 s of signal took >20 s of CPU on one core
```
=> The decoder is deterministic and correct on this signal. It was simply SLOWER THAN
REAL TIME, and the GUI feed is lossy:
- `gui_extract.c` writes 65536-sample display frames into `BUF_DISPLAY` with the no-wait
  policy (`max_wait_attempts = 0`): "If display buffer full, frame is silently dropped".
- `BUF_DISPLAY` is up to 256 MB; fill rate 40 MSPS x 2 ch x 2 B = 160 MB/s. A 10-20%
  drain deficit fills it in 8-16 s -> then continuous 64K-sample (~26-line) drops.
- Each drop is a stream jump: the H-sync snap absorbs it, but the frequency
  integrators (AUTO `phase_integral`, FIXED `fixed_period_ema`) ingested the wild
  phase error as a period measurement -> the period walked, the 7.5 un-snapped VBI
  lines accumulated the error -> the first lines after each VBI (top of frame) shifted;
  a jump inside a field tears a band; a jump inside a VBI train loses that boundary.
- Decoder off -> the thread catches up and the ring drains -> fine again for ~8-10 s.
- Playback paces at real time through the same ring -> identical behaviour.
- Also visible on screen all along: the OSD "ms" value (16.8 / 31.9 ms in the user's
  screenshots) is the per-field panel cost against a 20 ms PAL field budget.

## Profile (added `gui_cvbs_profile_get()` + the harness "LIVE profile" line)
Before:
```
signal 20.00 s | resample 2.76 s (14%) | process 18.80 s (94%) of which line decode 17.54 s (88%)
| 56.1 us/line | total CPU 21.56 s = 1.08x real time
```
`decode_line_to_pixels` pass 2 ran, PER PIXEL (~18M px/s): two 4-sample window loops,
~5 divisions, `cos(alpha)/sin(alpha)`, `cvbs_rate()`, an int64 division for the window
centre.

## The fix (misrc_tools/misrc_gui/signal/gui_cvbs.c)
1. Pixel loop rewritten as O(1) per pixel: per-line prefix sums over the carrier frame
   (`g_pf_s/c/n/i/q`: sum src, sum cos, sum sin, sum src*cos, sum src*sin) built in the
   same pass as the incremental carrier rotation; the burst and every pixel's
   mean-subtracted I/Q + box-mean luma are prefix differences. Per-line constants hoisted
   (`cos/sin(alpha)`, reciprocals), whole-cycle window snap + reciprocals by window
   length in a small table, exact Bresenham window centre (bit-identical to the old
   64-bit division). The float carrier tables are gone.
2. Luma FIR restructured outer-taps/inner-samples so -O3 vectorises it.
3. PLL discontinuity hardening: all frequency integrators (AUTO integral, FIXED
   acquisition integral, FIXED drift EMA, transitional EMA) only accept a GOOD sync
   (|phase_error| < lock_threshold). A width-valid H far off prediction still snaps the
   phase (stream-jump recovery) but never enters the period estimate.
4. `stream_jumps` counter (locked H > 0.3 line off prediction) shown in the Stats OSD as
   `jumps:N` - a clean feed reads 0; any feed drop now shows as a number.
5. OSD per-field "ms" now includes the resampler (true panel cost vs the field budget).
6. `CVBS_LIVE_DIAG` defaults to 0 in the GUI build (the 2 MB PPM dumps were written on
   the display thread every ~2.5 s); the harness defines it to 1 before including the
   decoder, so headless runs still leave `cvbs_live_diag_N.txt/.ppm` in the CWD.
7. Kept from earlier in the session: per-field buffer clearing / valid-row weave, and
   the EQ+broad cadence gate for VBI candidates (RF-decoder behaviour).

After (same command, same capture):
```
signal 20.00 s | resample 2.83 s (14%) | process 5.26 s (26%) of which line decode 4.06 s (20%)
| 13.0 us/line | total CPU 8.09 s = 0.40x real time
accepted=999 rejected=0 ; period 1145.597 flat over 20 s ; bars identical to 3 decimals
```

## Validation
- `scripts\build-local.ps1` -> OK (misrc_gui.exe 2026-10-19 18:42:07 local)
- `meson test -C build-local` -> Ok: 34  Fail: 0 (incl. gui_cvbs_baseband: PAL/NTSC
  colour + no-burst, damaged-VBI cadence reject, full-view hybrid, render blit contract)
- Headless 20 s EBU capture via `--panel --full`: see above.
- Artifacts: `.ci-artifacts\panel_run\panel_20s_full*.ppm`, `cvbs_live_diag_0..6.txt/.ppm`
  (7 dumps over 20 s, all `rej=0`, period 1145.5966..1145.5969).

## GUI checklist for the user (rebuilt build-local\misrc_gui.exe)
1. Play the 1-minute FLAC (and then the live HackDAC feed) with Decoder: CVBS,
   Frame: Full, Sync: Fixed, Colour: Auto for AT LEAST 60 s (the old failure began
   at 8-10 s and worsened).
2. OSD: Minimal -> the "ms" figure should sit well under 20 ms (expect roughly
   7-9 ms incl. resampler on this machine).
3. OSD: Stats -> `jumps:` should stay 0 and `VBI rej` should not climb. If `jumps`
   climbs, the feed is still dropping: report the ms figure and whether another heavy
   panel (RF preview / FFT) is active.
4. Top rows and the mid-frame band must stay aligned for the whole run; toggling the
   decoder must no longer be needed.
5. Report back; then the restore-point zip + log note (per rules) and a commit.

## Round 6 — user GUI result: "dead stable now", remaining top-area horizontal shift
User screenshot (18:14): stable, bob/weave fine, but the first picture rows still shift.

### Measurement (headless frame dump of the same capture, exact GUI path)
`.ci-artifacts\panel_run\row_edges.py panel_20s_full_final.ppm`: white-bar left edge per
row: rows 52-65 = 181-183 (correct, = rows 300+), rows 66-69 = 170-173 (13 px LEFT),
creeping back to 181 by row ~88. Row mean luma 129 at row 52 decaying to 79 by row 300.
Diag ring (per field, identical in all 7 fields): NO H sync processed for field lines
0-31; line 32 arrives +11.4 samples late; lines 33+ early by -2.3, -1.7, -1.3 ... (x0.85).

Raw 40 MSPS probe (`vbi_probe.py`, t=0.31 s): pre-EQ widths 2.35->2.15 us, broad
27.05->20.2 us (baseline rising through the trains); NO pulse crosses the steady-state
threshold from line ~5 to 29 (sync tips at ~-6000 vs threshold -6867 = whole signal ~+22
IRE high); first visible pulse 3.40 us (EQ-classified), then H widths 3.80->4.62 us over
25 lines; picture lines from 26 start +7000 counts (+26 IRE) high and settle by ~59.
=> SOURCE baseline wander (AC coupling, tau ~16 lines) after the vertical broad pulses.
A fixed sync slicer loses the syncs for ~24 lines, then slices the recovering edges
late; each decoded line's origin follows that error = the shift; the luma ramp is the
same offset in the picture.

### Fix: per-line DC restoration (back-porch clamp), gui_cvbs.c
- Mid-line (back porch complete, `dc_p1` samples in) measure this line's raw porch and
  sync-tip means; accept when the tip-to-porch swing is 0.35..1.6x the committed swing
  and |offset| < one swing; `dc_offset` = porch - committed blanking.
- The slicing threshold = committed midpoint + dc_offset from that moment on (next sync
  edge already corrected); `decode_line_to_pixels` shifts the line's luma black by the
  same offset.
- First attempt did not engage: the gate used `levels.range` (int16, wraps negative in
  the 16x s16 domain and clamps to 100 -> gate became |offset| < 50). Gate now uses the
  committed swing; the range clamp saturates.
- Field blanking commit prefers the on-baseline lines (clean subset, >= 50% of porch
  samples) so the ~30 sagging lines no longer bias the committed black (+1.5 IRE).
- Diag: per-line clamp trace (first 64 lines) in the dump; `dc=` in the LIVE line.
- OSD deinterlace label fixed (`last_weave`; the ready flag is cleared at field start).

### Result (same 20 s capture, panel path)
H syncs from field line 9 (was 32), phase errors ~0; rows 52-96 white-left 183-184 ==
rows 300/616; row luma flat 80-81 from the first picture row; VBI blank rows at 52-53
(was 77-88); hues unchanged (red 103.3 deg); 999/0 fields; 0.43x real time; 34/34 tests.
GUI exe rebuilt 19:40:22.

### Vertical placement (user: "a bit of black at the bottom and top, not just at the top")
Measured in the dump: picture rows run to frame row 624 (field line 312) with bars; the
top 8 rows (field lines 0-3 = the EQ/broad trains, decoded before the boundary is known)
are black, rows 8-42 the VBI band, data lines at rows 43-48, picture from row 52. A
standard-compliant source leaves lines 311-312.5 / 624-625 blank (would show ~5 blank
rows at the bottom); this EBU file carries bars to the last line. Options to confirm with
the user: (A) decode field lines 0-3 in arrears from a raw-sample delay line (top shows
the pulse trains like tbc-tools, no black band), (B) a Full-view vertical offset that
centres the margins (costs the last ~4 picture lines of THIS source; a compliant source
loses nothing). Not changed yet.

## Known notes
- Two CVBS panels at once (CH1 + CH2 split) cost ~0.8x of one core on the display
  thread; still under real time but with less headroom.
- The display ring (256 MB) also means up to ~1.6 s of preview latency before any
  drop would occur; not changed here.
- The EQ+broad cadence gate assumes intact broad pulses (true for TBC'd / clean
  composite); raw tape sources with split broad pulses may need the RF decoder's
  split-broad recovery ported if VBI acceptance drops there.
