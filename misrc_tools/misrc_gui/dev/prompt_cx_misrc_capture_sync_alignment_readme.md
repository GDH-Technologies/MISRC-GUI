# Prompt log — CX/MISRC capture start alignment (byte-exact A/B + audio record transitions)

- Date: 2026-10-06
- Type: fix (per a third-party comment on capture start skew, verified on this rig)
- Status: implemented, built, all automated tests pass, hardware-verified on the
  2x CXADC + clockgen rig (t10/t11: byte-exact equal A/B counts, two runs in a
  row, under 24 CPU load workers). Definitive content-level A/B confirmation
  (decode + aaa.exe stream-align on a playing tape) still pending a player
  session. Full investigation log: host
  `prompt-logs/cx-misrc-sync-investigation-2026-10-06/README.md`.

## User input (task)

A third-party comment claimed: capture tools don't produce equal sample
counts; an mbuffer + killall capture does (exactly, consistently);
auto-audio-align assumes the input audio starts at the first decoded field;
capture start times are off by ~0.1-0.3 s; the PCM1802 is "not really in
sync"; and therefore shared-clockgen sync is unjustified vs +-30 ppm
crystals. The user asked for verification on this rig and fixes so "the
feeds are aligned exactly" (RF A/B + clockgen audio).

## Verification (hard data, 2026-10-06)

- Driver (cxadc.c): cards free-run DMA into a 64 MiB ring since module load;
  open() latches a 512-page-aligned boundary index (initial_page); reads
  release one 2 MiB boundary per ~52.4 ms at 40 MB/s (page-quantized). A card
  file's content starts at its fd's latch — equal counts do NOT mean aligned
  content.
- GUI baseline (strace-instrumented auto-record load test): rfA/rfB
  byte-exact equal (lockstep) BUT the A/B CONTENT offset was 114.076 ms (the
  sequential capture-start probes; preserved forever by lockstep) and the
  audio WAV span exceeded the RF span by +275 ms (full-196,608 B block
  transitions). mbuffer+killall: 5/5 runs byte-exact (whole-2 MiB-release
  quantization + shared-clock phase lock). auto-audio-align (vendored C#):
  output sample 0 maps to the RF sample at the FIRST decoded field — no
  offset search, no lead/lag parameter (any start skew propagates 1:1).
  Crystal claim nuanced: +-30 ppm between independent crystals = up to
  108 ms/hour and auto-audio-align does NOT compensate audio-card clock
  error — the clockgen's actual value is rate lock (drift = 0 by
  construction); start sync is a separate problem (fixed here).
- Server (local-capture.sh, 3 runs): video = hifi byte-exact every run
  (191 x 2 MiB whole-release quantization); per-stream starts within
  ~8-55 ms; the baseband hw: feed runs at 48 kHz vs the GUI's native
  46,875 Hz usbstream: feed (a rate-basis difference between tools).
- Scripts: never byte-equal (termination granularity: dd whole blocks, sox
  ~4 MiB); start offsets ~1 ms when started together, ~0.3 s when sequential.

## Root cause (GUI side)

1. Sequential per-card capture-start rate probes (~105-155 ms each) leave
   card 0's stream position ~one probe duration behind card 1's in
   wall-clock content time; the RF thread's lockstep preserves the offset
   for the whole capture.
2. The audio thread only observed the record state after a full 196,608 B
   (~350 ms of clockgen audio) block read succeeded, writing up to one
   full block of pre-record audio at record START (measured +275 ms span
   excess).
3. (Found during verification) the record start/stop handshake could flip
   the live tap_enabled flag BETWEEN the A and B pushes of one lockstep
   iteration, splitting the channels by one 64 KiB chunk (observed once in
   three runs before the gate).

## Changes

- `misrc_gui/input/gui_cxadc.c`
  - `cxadc_sync_card_starts()`: exact boundary-grid alignment — drain ->
    grid-align to the next 2,097,152 B boundary multiple of the fd's total
    consumed count -> whole 2 MiB grid reads until one blocks a full
    release period (the stream is exactly at lgpcnt: a boundary with
    nothing readable) -> a 1-byte read that blocks to the NEXT boundary
    crossing and completes within microseconds of it (that completion IS
    the content time of the position; measured, not derived) -> skip to
    T_ref = max(card crossings) + 1 ms with EXACT byte counts (64 KiB
    chunks + an exact-remainder final read). Runs after the capture-start
    probes, before the RF thread, gated on card_count > 1.
  - `s_cxadc_probe_consumed_bytes[]`: per-fd consumed byte tracking from
    the capture-start probe (the grid reference; reset per card at the
    capture-start probe so the startup-enumeration session is cleared).
  - `CXADC_AUDIO_READ_FRAMES` 1024 -> 48 (~1 ms ALSA readi period; the
    ALSA buffer is 64 periods ~ 65 ms) so BUF_CAPTURE_AUDIO's "now"
    advances at the USB delivery quantum.
  - The lockstep tap gate in `cxadc_capture_thread`: latch the record
    decision once per iteration (`gui_record_tap_iteration_enter` on both
    active channels -> `tap_this_iter = a_on && b_on` ->
    `gui_record_direct_tap_push_latched` on both channels or neither ->
    leave), placed after the A/B pairing and before the display-ring write.
- `misrc_gui/output/gui_audio.c`: record-state transitions handled at the
  TOP of the audio loop (the read timeout is 2 ms, so transitions are
  observed within ~2-4 ms of the record button); START discards the ring
  to "now" (`AUDIO_SYNC_CHUNK_BYTES` 576 B = the new producer granularity)
  so the WAV files begin at the record instant; STOP flushes the ring to
  "now" (safety net — the record-stop entry's exit-drain normally ends the
  files); `was_recording` initialized from pre-opened files.
- `misrc_gui/output/gui_record_direct.c/.h`:
  `gui_record_direct_tap_iteration_enter/leave()` (mark in-flight, then
  latch enabled — the stopper's disable + wait-idle waits for the whole
  gated iteration) and `gui_record_direct_push_latched()` (honors the
  latched decision; no live re-check); the push body factored into
  `gui_record_direct_push_inner()`.
- `misrc_gui/output/gui_record.c/.h`: channel-indexed wrappers
  (`gui_record_direct_tap_channel_active`, `gui_record_tap_iteration_enter/leave`,
  `gui_record_direct_tap_push_latched`) + the shared
  `gui_record_direct_report_drop` helper.
- `misrc_tools/test/ci_guard_tests.py`: new `audio record alignment
  contract` and `CXADC synchronized start contract`; the `raw direct
  passthrough contract` updated for the gated tap placement (after the
  A/B pairing: enter -> latch -> both latched pushes -> leave, before the
  display-ring write).
- `TECHNICAL.md`: "Multi-stream capture start alignment (CXADC cards +
  clockgen audio)" section (Fixes 1-3 + residuals + log lines).

## Commands run (and results)

    meson compile -C build-local   -> BUILD_RC=0 (pre-existing warnings only)
    meson test -C build-local      -> 12/12 Ok
    python3 misrc_tools/test/ci_guard_tests.py -> 44/44 PASS

    # Hardware (2x CXADC + clockgen rig, 24 sha256sum load workers, strace):
    t10: rfA = rfB = 600,178,688 B exactly equal (read==written exact,
         waits=0 drops=0); sync: card0 crossing@+148.792 ms, card1
         crossing@+314.266 ms, card0 skip = 6,502.9 KB = exactly
         166.474 ms x 40 MB/s; audio discard 151,488 B (~269.3 ms) of
         pre-record audio; WAV 15.027541 s vs RF span 15.004467 s (+23.1 ms)
    t11: rfA = rfB = 600,113,152 B exactly equal again; sync math exact
         again (skip = 6,503.1 KB); audio discard 150,912 B (~268.3 ms);
         WAV 15.024469 s vs RF span 15.002829 s (+21.6 ms)
    Baseline for comparison: A/B content offset 114.076 ms; audio span
    excess +275.25 ms; one 64 KiB A/B count split in three runs pre-gate.

## Known residuals / floor

- A/B content alignment residual: the 1-byte crossing probe's IRQ/wakeup
  slop (~tens of microseconds, near-identical per card on the same system)
  + the measured-rate error over the skip (microseconds). Byte-exact by
  construction; the crossing timestamps are logged per capture.
- Audio-vs-RF: the WAV begins ~3-7 ms after the record instant (~1 ms USB
  delivery quantum + ~2-4 ms observation) and ends ~2-10 ms after the stop
  (the record-stop entry clears is_recording and stops the audio thread
  within ~us of each other; the exit-drain ends the files at "now").
  Measured span difference vs the RF files: +13 to +23 ms across runs
  (baseline +275 ms). Without hardware timestamping on the USB audio path
  this is the rig's physical floor.

## Not done / open

- Phase 2 end-to-end (player + decode + aaa.exe stream-align) for the
  definitive content-level A/B + audio confirmation.
- Restore point (per workflow):
  `../MISRC-GUI-restore-points/cx-misrc-sync-alignment-2026-10-06`.
