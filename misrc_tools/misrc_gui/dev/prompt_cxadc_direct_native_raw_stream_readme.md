# Prompt log — CXADC direct native RAW stream (byte-exact `.u8`/`.u16`) + RAW naming by content

- Date: 2026-10-02
- Type: feature (per user report of "nonsense conversion" in the CXADC RAW path)
- Status: implemented, built, all automated tests pass. **Not yet confirmed on real
  CXADC hardware** (needs user validation before this is considered working).

## User input (task)

1. CXADC capture did not save the direct raw stream when no resampling and no
   compression were enabled (`.u8`/`.u16` output). Referenced:
   - `misrc_gui/input/gui_cxadc.c:1767` (per-sample decode loop instead of a
     byte-buffer copy)
   - `misrc_gui/input/gui_cxadc.c:620` (unsigned 8-bit becomes signed 16-bit
     instead of staying native)
2. Follow-up decision: "non-resampled stuff stays native unsigned samples for
   CXADC" — clarified via question to mean **RAW only** (FLAC off). FLAC files
   keep today's sample format (8 bps `b-128`, 16 bps 12-bit-scaled), since the
   user confirmed "RAW only (current plan)".
3. Earlier agreed decisions: name files by content; CXADC-only scope (tap
   designed so DdD/FX3 can plug in later); direct mode is automatic (no
   settings toggle), logged in the capture log and shown in the status bar.

## Root cause (what the old path did)

- `cxadc_capture_thread` decoded every sample (`(b-128)<<4` / `((u16-32768)>>4)`)
  and packed both channels into u32 words → `BUF_CAPTURE_RF`.
- The extraction thread unpacked that into int16 and wrote 128 KiB int16 blocks
  to `BUF_RECORD_A/B`.
- `raw_writer_thread` converted again: 8-bit via `gui_record_sample_12bit_to_i8`
  (signed int8 = `b-128`), 16-bit as the 12-bit value in a signed int16.
- Net effect: `.u8` files held `b-128` (signed), `.u16` files held the decoded
  12-bit value (not the card's native words) — mislabeled per the ld-decode
  convention (`.u8`/`.u16` = unsigned native, `.s8`/`.s16` = signed converted).
- Latent defects on this path: fixed 2 MiB writer blocks lose the final
  partial block at every stop; ring-first-then-spill can reorder under
  backpressure; the A/B pairing truncates both reads to the shorter one and
  drops card A's data when card B returns 0; odd byte counts are truncated in
  tenbit mode.

## Changes

- New `misrc_gui/output/gui_record_direct.c/.h` (no raylib/GUI dependency):
  - `gui_record_direct_push()` — producer tap: enabled/in-flight handshake,
    backpressure policy (2 × 1 ms like extraction), spill fallback, ordering
    rule (while the spill backlog is non-zero, pushes go to the spill), drop
    reported as `PUSH_DROPPED` for the caller to log.
  - `gui_record_direct_writer_thread()` — byte-exact fwrite, variable-length
    reads (min(fill, 2 MiB)), ring-first-then-spill drain, full tail drain at
    stop, held-block write-error retry every ~2 s (bounded final attempt at
    stop).
- `misrc_gui/input/gui_cxadc.c`: taps after each successful card `read()`
  (`gui_record_direct_tap_push(0, card_buf_a, ...)` /
  `(1, card_buf_b, ...)`) placed **before** the A/B pairing truncation, so
  nothing recorded is truncated or dropped. New
  `gui_cxadc_direct_record_available()` (running capture, card index, live
  tenbit mode must match the requested RAW width).
- `misrc_gui/output/gui_record.c`:
  - `gui_record_direct_channel_eligible()` — shared predicate (CXADC device,
    running capture, FLAC off, per-channel resample off, per-channel capture
    on, live width match). Used by auto-naming, session log, and start.
  - RAW start: per-channel direct decision, direct contexts wired to the real
    spill/log/write-error/accounting, direct writer threads started instead of
    `raw_writer_thread` for direct channels, taps enabled after writers and
    before extraction recording.
  - Stop: taps disabled + waited idle **before** `is_recording` clears, so the
    writers' final drain cannot race a producer.
  - Naming: `raw_ext_for_bits(bits, direct)` → direct `.u8`/`.u16`, converted
    `.s8`/`.s16`.
  - Logging: `capture_format: RAW direct native passthrough`, per-channel
    `RAW output:` line, status `Recording (RAW direct)...`, end-of-recording
    `Direct RAW channel A/B: bytes_read=... bytes_written=...` (mismatch made
    visible).
  - Spill API made byte-generic + public (`gui_record_spill_enqueue` takes
    `const void *`, `gui_record_spill_read_block`, `gui_record_spill_backlog_bytes`).
- `misrc_gui/processing/gui_extract.c/.h`: `gui_extract_set_recording()` gains
  `direct_a/direct_b`; the extraction thread skips record writes for direct
  channels (single producer per ring). Disk guard and display unchanged.
- `misrc_gui/core/gui_settings.c`: load-time preview names converted output
  `.s8/.s16` (no device knowledge at load; record start recomputes with the
  predicate).
- `misrc_tools/meson.build`: module in the GUI build; new test
    `gui_record_direct` (harness + module + real buffer manager/ringbuffer).
- `misrc_tools/test/gui_record_direct_harness.c`: real push + writer through a
  real ringbuffer (64 KiB) with an in-memory spill stub: tap handshake, spill
  fallback under ring-full, byte-exact pattern comparison over
  3×2 MiB + 12345 bytes in odd chunk sizes (ordering + tail drain), and
  write-error recovery via unbuffered `/dev/full` then a swapped-in output
  (held block first, then ordered drain).
- `misrc_tools/test/ci_guard_tests.py`: new `raw direct passthrough contract`
  (tap placement before pairing truncation, handshake order, producer/writer
  ordering, naming-by-content mapping in both files, predicate conditions,
  single-producer skip, stop/start order) and `direct RAW writer runtime`
  (compiles + runs the harness with `cc`).
- `TECHNICAL.md`: new "Direct native RAW recording (CXADC passthrough) and RAW
  naming" section.

## Commands run (and results)

    cc -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
       misrc_tools/test/gui_record_direct_harness.c \
       misrc_tools/misrc_gui/output/gui_record_direct.c \
       misrc_tools/common/buffer_manager.c \
       misrc_tools/common/ringbuffer.c \
       misrc_tools/common/rb_event.c -lpthread -o /tmp/gui_record_direct_guard \
       && /tmp/gui_record_direct_guard
    → PASS: direct RAW record harness (56 checks, 0 failures)
      (first run caught a real bug: the writer stopped with a held write-error
      block instead of a bounded final retry — fixed)

    meson compile -C build-local          → built (pre-existing warnings only)
    ./build-local/misrc_gui --smoke-test  → pass
    meson test -C build-local             → 11/11 OK (incl. gui_record_direct)
    python3 misrc_tools/test/ci_guard_tests.py → 37/37 PASS
      (incl. new "direct RAW writer runtime" and "raw direct passthrough contract")

## Follow-up (same day): local automated load + capture testing (CLI -> GUI)

User direction: automated testing must work locally (not only the net
server/trigger route), using the existing CLI->GUI pattern; add any CLI
features needed and document them with human copy-paste examples.

Added CLI features (`misrc_gui.c`):

- `--select-device <index-or-name>` — pick the capture device at launch
  (list index or case-insensitive name substring, first match; exit 1 when
  no match). Required because startup prefers hsdaoh > MISRC Clockgen >
  CXADC, so automated runs could not select CXADC.
- `--auto-capture` — unattended local capture start ~1 s after launch (same
  reconnect-pending mechanism as `--auto-connect` server mode; Local mode,
  no `--config` required).
- `--auto-record <seconds>` — full unattended record cycle (start capture,
  settle 2 s, record, wait for file finalize, stop capture, exit 0; exit 1
  and `[AUTO] ERROR:` markers on failure). `[AUTO]` progress markers on
  stderr for scripted verification.
- `gui_record.c`: the `overwrite_files` setting now actually skips the
  overwrite confirmation popup (the toggle existed but was ignored) —
  without this an unattended run stalls on the popup.

Docs: `TECHNICAL.md` "Automated local load + capture testing (CLI -> GUI)"
section with a full copy-paste example (config + run + CPU load +
verification). CI guard: new `GUI auto-test flags contract` in
`ci_guard_tests.py` (flags parsed, [AUTO] markers, nonzero-failure exit,
usage text, overwrite bypass).

Commands and results (all on the real rig: 2x CXADC + clockgen, 36-core,
/dev/cxadc0 and /dev/cxadc1):

    meson compile -C build-local && ./build-local/misrc_gui --smoke-test
    meson test -C build-local          -> 11/11 OK
    python3 misrc_tools/test/ci_guard_tests.py -> 38/38 PASS
      (incl. new "GUI auto-test flags contract")

    # The automated local load + capture test (exactly the documented example):
    cd .ci-artifacts/direct_raw_autotest
    DISPLAY=:0 ../../build-local/misrc_gui --config settings.config \
      --select-device "CXADC Clockgen" --auto-record 15 > gui.log 2>&1 &
    GUIPID=$!; for i in $(seq 1 24); do timeout 26 sha256sum /dev/zero >/dev/null & done
    wait $GUIPID   -> GUI_EXIT=0

    [AUTO] markers in gui.log: --select-device -> auto-capture armed ->
    capture running; settling 2 s -> recording started (15 s) ->
    recording stopped after 15 s -> capture stopped -> record cycle complete

    [EXTRACT] Recording enabled (... direct A:yes B:yes)  <- both channels direct
    [REC] Recording stopped: A=572.00 MB (599785472 bytes), B=572.00 MB
          (599785472 bytes), waits=0, drops=0   <- under 24 CPU load workers
    [REC] Direct RAW channel A: read=599785472 written=599785472 (exact)
    [REC] Direct RAW channel B: read=599785472 written=599785472 (exact)

    ls -l *.u8  -> rfA/rfB_directtest_2026.10.02_16.13.12_8-bit.u8
                   both exactly 599785472 bytes (== bytes_read == log)
    capture log -> capture_format: RAW direct native passthrough
                   RAW output: A=direct native u8 B=direct native u8
                   no spill WARN, no data loss, no spill temp files left
    throughput  -> 599785472 B / 15 s = 39.99 MB/s per channel (40 MSPS)
                   on BOTH channels concurrently under load
    unsigned    -> live dd if=/dev/cxadc0 mean 126.0; rfA file mean 126.0,
                   rfB file mean 126.1 (first 4 MiB each) — the files are the
                   card's native unsigned bytes (the old mislabeled-converted
                   .u8 would read ~254 at this DC, i.e. b-128 wrapped)

Observation (pre-existing, not from this change): at capture stop the
CXADC clockgen ALSA audio thread logs `readi error ... errno -77 (EBADFD)`
and exits — the pcm is torn down under the blocked read. Non-fatal and
untouched by the direct RAW work; worth a look if audio capture at stop
ever misbehaves.

## Follow-up (same day): CXADC measured feed-rate check (stale sysfs crystal)

User report: a single 40 MHz-modded card showed resample cycle options only
up to 28.6 — correct behavior IF the card's sysfs is right, but a modded
card can carry the stock 28.636 MHz `crystal` parameter while actually
feeding 40 MSPS. Direction: with raw mode able to read the card's true
feed, add a startup check that measures what the card is actually capable
of.

Added (2026-10-02):

- `misrc_tools/common/cxadc_rate_tiers.h` (dependency-free): known feed-rate
  tiers {14.3, 17.9, 20, 27, 28.636, 40, 54} MHz + 2%-tolerance snap
  (`cxadc_snap_rate_tier_hz`). 35.8 (upsampled 8-bit tenxfsc=1) deliberately
  NOT a tier (that mode's sysfs presentation stays the crystal rate).
- `gui_cxadc.c`: `cxadc_measure_card_rate_hz()` times a blocking read window
  (startup 2 MiB fresh-open; capture start 4 MiB on the already-open fd),
  `cxadc_probe_and_cache_card_rate()` snaps + caches + compares with sysfs
  and logs agree/mismatch (mismatch also sets a status line),
  `gui_cxadc_get_effective_rate_hz()` (measured if cached, else sysfs),
  `gui_cxadc_probe_card_rates()` (startup hook; skips while capturing and
  with `MISRC_GUI_NO_CXADC_RATE_PROBE=1`).
- `gui_cxadc_start`: probes each card right after the fds open and before
  the RF thread starts; the measured rate overrides
  `card_sample_rate_hz[i]` and refreshes `rf_sample_rate_hz`.
- `gui_capture.c`: startup probe in `gui_app_enumerate_devices` (after
  `gui_cxadc_detect_cards`); CXADC profile sync now uses the effective rate.
- `gui_ui.c`: `gui_ui_cxadc_base_rate_khz` and
  `gui_ui_cxadc_hw_rate_for_tenbit` now use the effective rate — the
  reported bug's 28.6 resample cap becomes the 40-base preset set
  (5/10/20/40) once the measurement runs.
- Tests: `test/cxadc_rate_tiers_test.c` (meson test `cxadc_rate_tiers`, 17
  checks incl. the excluded 35.8 tier), new `CXADC rate probe contract`
  guard in ci_guard_tests.py.
- `TECHNICAL.md`: "CXADC measured feed-rate check (sysfs crystal can lie)"
  section.

Validation on the real rig (first attempt found a real bug in the probe):

    meson compile + smoke            -> ok
    meson test -C build-local        -> 12/12 OK (incl. cxadc_rate_tiers)
    python3 misrc_tools/test/ci_guard_tests.py -> 39/39 PASS

    Automated local load+capture test rerun (same unattended command):
    GUI_EXIT=0
    [CXADC] rate check: card 0 measured 40.000 MHz, sysfs 40.000 MHz (agree)
    [CXADC] rate check: card 1 measured 40.000 MHz, sysfs 40.000 MHz (agree)
      (at startup enumeration AND again at capture start)
    direct RAW run unchanged: both channels direct, 599785472 bytes each,
    bytes_read == bytes_written, waits=0 drops=0 under 24 CPU load workers.

    FIRST attempt measured 72.5/61.5/50.3 MHz (no tier -> sysfs kept, capture
    unaffected) — the card free-runs since boot, so /dev/cxadcN returns a
    large backlog faster than the live feed; a naive timed window measures
    the backlog, not the feed. Fixed with the two-phase drain-then-measure
    probe + conservative failure handling (no live stream within caps or no
    tier match -> negative cache, sysfs-derived rate stays). After the fix,
    both cards measured exactly 40.000 MHz (agree).

Remaining: the mismatch path (sysfs 28.6 stale, measured 40 -> override +
WARN) is validated by the tier-snap unit test and the agree-path hardware
run; a genuinely stale-sysfs card (the reporting user's machine) is the
final end-to-end confirmation.

Docs follow-up (user request, same day): `TECHNICAL.md` rewritten in full
detail — the direct RAW section now documents the complete data flow (tap
placement before the A/B pairing), all eligibility conditions, the ordering
invariant and why the legacy path violates it, the start/stop handshake,
backpressure numbers, exact log lines, and unit/guard/hardware verification
(with the rig numbers). The automated-testing section documents every flag
semantic, the 5-phase auto-record machine, the full [AUTO] marker list, the
SIGHUP note, and the validated numbers. The rate-check section documents
the two-phase algorithm with all constants, the tier table, the 35.8
exclusion, consumers, all log-line variants, and the first-attempt
backlog-bug story. Also fixed the stale `s_record_write_policy` line in
the readout section (it is 2x1 ms + spill, not immediate-drop) and added
the direct tap to the data-flow list.

## Not done / open

- Real-hardware validation — DONE 2026-10-02 via the automated local load +
  capture test above (8-bit, 2 cards, direct A+B, under CPU load):
  file size == bytes_read == bytes_written, `.u8` extension, unsigned native
  means matching the live card, `capture_format: RAW direct native
  passthrough`, zero waits/drops. Tenbit (`.u16`) and the resampled
  (`.s8/.s16`) variants are still worth one manual confirmation run.
- No commit/restore point yet (only after user confirmation, per workflow).
- Legacy (converted) RAW path still has the fixed-block tail loss and the
  ring/spill reordering hazard under backpressure; the direct path avoids
  both by design. Fixing the shared legacy path is a separate follow-up.
- The legacy capture loop still truncates/drops A/B reads for the *display*
  path (the taps bypass it for recording only).
