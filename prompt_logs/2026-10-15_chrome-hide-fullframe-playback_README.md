# Prompt Log — 2026-10-15 — chrome hide (all panels + video modes), stats arrow, full-frame entire-signal render + hybrid CRT levels, playback reader, waveform-playback accuracy

Companion to `prompt_readme_chrome-hide_fullframe_playback.md` (full commands, outputs,
measurements, before/after PPM evidence, and the four child-agent handoffs + the playback-reader
lead takeover after its run errored).

Headless-verified state (not GUI-confirmed): canonical build OK + smoke test PASS; full meson
suite 28/28 twice; extended playback resolve harness 301 checks / 0 failures; real-tape NTSC
verification (height 480->526 full frame, bottom lines present, registration GOOD, black lifted
11.4->25.4 darkest-0.1%, whites keep headroom, VBI contrast spread 38.1->49.7).

## Pending GUI verification checklist (user)

1. Double-click chrome hide — hide AND restore (double-click again):
   - all 3 video decoder modes: CVBS, Tape, VHS FM (stats + reason line + all buttons incl.
     CVBS/Tape OSD; the hidden state survives decoder-mode cycling).
   - all 5 analysis panels: Waveform, FFT, Waterfall, Spectro, Histogram, Demod (their own
     dropdowns/labels/controls; the data visualization stays visible; waveform trigger-level
     drag still works while hidden).
   - While hidden, clicking where a hidden control was must NOT trigger it; single-click then
     double-click must restore (two fresh clicks after a toggle).
2. VHS FM stats button: no arrow inside the label; dropdown All/Basic/Off still switches.
3. Frame: Active/Full button — Full shows the ENTIRE signal frame:
   - NTSC: 640x526 (VBI top + full picture incl. the bottom lines + tail VBI), PAL: 768x626,
     drawn at the frame's own aspect (slightly taller than 4:3; no squash).
   - Active area: CRT/production-monitor levels — black properly lifted (not crushed at floor),
     sync visible below black in Full, whites with headroom.
   - Full-mode VBI: hybrid background contrast (blanking band bright enough to inspect
     EQ/broad/sync structure; compare against tbc-tools analyse hybrid for feel).
   - Mode persists across worker rebinds (System PAL/NTSC toggle) and DSP resets.
4. Player formats (one per line, playback + timeline/seek + length):
   - native FLAC (unchanged behavior, incl. MISRC recorder 16-bit files — waveform must match
     live capture 1:1 as before).
   - the real tape file `L:\Tape_03\Diamler_Chrysler_NTSC_SP_Tape_03_video_rf_20msps.flac` —
     the WAVEFORM PANEL must now show the actual data amplitude (previously ~1/16 too small).
   - raw `.u8`/`.u16`/`.s16`/`.s8` (+ `.r8`/`.r16`, reversed `.8u`/`.8s`/`.16u`/`.16s`,
     `.raw`/`.bin`/`.pcm`) with an `<n>msps` filename hint; without the hint a clear error.
   - PCM WAV (audio rate as-is; RF-header rates via /1000).
   - Ogg FLAC `.ldf`/`.oga`/`.ogg` (remux temp; exact totals).
   - packed 10-bit `.lds` (40 MSPS default, msps hint overrides).
   - renamed files (magic sniffing works); non-FLAC Ogg / ID3-tagged FLAC give specific errors.
5. Waveform playback accuracy: on the real tape FLAC the waveform shape/amplitude should match
   the actual RF data (convention auto-detection: recorder <<4 files restore bit-exact; direct
   ±2k-domain files pass through; full-range 16-bit scale into the display container).

## Deliberately not done
- No commits/push (not requested); everything is in the working tree (18 files, +4,008/-339)
  on top of the uncommitted Windows MF gui_capture.c fix.
- Zip restore point (`git archive` snapshot) waits for the user's "fully working" confirmation
  per the user's rule; update this log + create it after confirmation.
- Known flake: `gui_rf_playback_start` can fail under heavy parallel load (pre-existing; passes
  standalone and in both final full suites — if it fails in CI/local runs, re-run it).
