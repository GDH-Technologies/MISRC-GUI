# Decoupled Capture Finalization — Design

Date: 2026-08-18
Repo: GDH-Technologies/MISRC-GUI (misrc_tools vendored at `misrc_tools/`)
Status: approved by Reece 2026-08-18 (design presented and accepted in chat)

## Goal

Stopping a recording hands all finalization work to the background so the operator can
immediately configure and start the next capture. Closing the app can never again abandon
a finalize mid-write. Finalize itself becomes near-instant on new captures by eliminating
the whole-file metadata rewrite.

## Background: why finalize is slow and fragile today

1. **No PADDING block in RF FLACs** — adding ~200 bytes of vorbis duration tags forces
   libFLAC to rewrite the whole file (~10–20 min on a full-tape capture) through a
   `.metadata_edit` temp file.
2. **Finalize reads/writes module globals** in `misrc_gui/output/gui_record.c`
   (`s_flac_writer_a/b`, `s_file_a/b`, `s_record_path_a/b`, writer-thread handles, the
   session-log `FILE*`, spill-channel state, buffer-stat baselines), so
   `gui_record_start()` must refuse to run until finalize completes.
3. **`gui_record_cleanup()` is never called** — the one function that joins the finalize
   thread has no caller, so app exit kills the finalize thread wherever it happens to be.
   This truncated a real capture's finalize on 2026-08-18 (Good Morning Vietnam: the
   143 GB hifiRF tag rewrite died at 71 GB when the app was closed).

## Change 1 — Reserve padding at file creation (`common/flac_writer.c`)

Add a `FLAC__METADATA_TYPE_PADDING` block (4096 bytes; new config field
`padding_bytes` defaulting to 4096) to the metadata array passed to
`FLAC__stream_encoder_set_metadata()`, after the seektable.

New file layout: `STREAMINFO, VORBIS_COMMENT(vendor), SEEKTABLE, PADDING(4096)`.

Applies to both the GUI and the `misrc_capture` CLI. Readers (hifi-decode, ffmpeg,
metaflac, libsndfile) are indifferent to padding. Mirror a stub in the
libFLAC-disabled `#else` section.

## Change 2 — In-place tag embed (`common/flac_writer.c` + `gui_record.c`)

New common-layer function:

```c
// Appends key=value comments to the file's VORBIS_COMMENT block.
// In-place when trailing padding can absorb the growth; falls back to
// a full rewrite (with *rewrote=true) on legacy files without padding.
bool flac_writer_embed_tags(const char *path, const flac_writer_tag_t *tags,
                            size_t n_tags, bool *rewrote);
```

Implemented with the FLAC metadata **chain** API (same pattern as
`flac_writer_finalize_streaminfo`): read chain → append comments to the existing
VORBIS_COMMENT block → `FLAC__metadata_chain_write(use_padding=true,
preserve_file_stats=true)`. With trailing padding, libFLAC shrinks the padding to absorb
the growth and rewrites only the metadata region — milliseconds instead of minutes.

`gui_record_embed_flac_duration_metadata()` becomes a thin wrapper that formats the five
duration tags (`DURATION_SECONDS`, `LENGTH`, `RF_TOTAL_SAMPLES`, `RF_SAMPLE_RATE`,
`RF_SAMPLE_RATE_KHZ`) and calls this; when `rewrote` comes back true (legacy file), it
logs a WARN so slow finalizes are visible and explained. Stub in the libFLAC-disabled
section.

## Change 3 — Per-session state (`gui_record.c`)

Introduce `gui_record_session_t` owning everything one recording needs through finalize:

- writer thread handles + running flag, FLAC writers, raw `FILE*`s, output paths,
  sample rates
- latched `video_started` flag
- session log `FILE*` (today the global `s_capture_log_file`)
- spill-channel state (today the global `s_record_spill` array) and write-error flags
- timing (`recording_start_time`, `stop_request_time`) and **stat snapshots**:
  `gui_record_stop()` reads the buffer wait/drop counters at stop time and stores the
  deltas in the session — more correct than today, where a still-running capture keeps
  incrementing them under finalize's feet

The module keeps two pointers: `s_active` (recording in progress) and `s_finalizing`
(owned exclusively by the finalize thread; **at most one in flight** — chosen scope).
Stop moves `s_active` → finalize thread and clears it; a new recording builds a fresh
session immediately.

Gates change as follows:

- `gui_record_start()`: finalize gate removed. One new guard: if the requested output
  paths collide with the finalizing session's paths, refuse with a clear status
  ("Previous recording is still finalizing these files"). Normal overwrite-confirmation
  flow unchanged.
- `gui_record_stop()`: if a previous finalize is still in flight (rare once finalize is
  fast), join it first with a "waiting for previous finalize" status, then proceed.
- Record button (`gui_ui.c`): no longer hijacked by finalize state; it always reflects
  the new session. Finalize progress stays visible via the existing orange
  indicator/status text, including the red-blink write-error signal.

## Change 4 — Shutdown safety (`core/misrc_gui.c`)

- Wire `gui_record_cleanup()` into `main()`'s teardown (before `gui_app_cleanup`),
  fixing the never-called bug.
- While a finalize is in flight, `WindowShouldClose` does not exit the main loop; the app
  stays open showing "Finalizing recording — please wait…" and exits automatically when
  the finalize completes.
- Device loss needs no special handling: finalize touches only output files, never the
  device, so a capture device disappearing cannot hurt it — only process exit could, and
  that path is now closed.

## Testing

- Extend the CI guard C test (`misrc_tools/test/`): new files contain the PADDING block;
  `flac_writer_embed_tags()` on a padded file requires no tempfile (asserted directly via
  libFLAC's `FLAC__metadata_chain_check_if_tempfile_needed`) and tags round-trip; a
  legacy-layout file reports `rewrote=true`.
- `ci_guard_tests.py --static-only` and `--post-build` still pass.
- Hardware pass (operator): short capture → stop → immediately configure and start a
  second capture while the first finalizes; then close the app during a finalize and
  confirm it holds open briefly and the file comes out fully tagged.

## Out of scope

- Fully-concurrent finalizes (one-in-flight chosen).
- vhs-decode-side changes.
- Repairing existing legacy captures (handled operationally case-by-case; the 2026-08-18
  Good Morning Vietnam capture was recovered manually).

## Delivery

Branch `fix/decoupled-finalize` off `main`; PR to GDH-Technologies/MISRC-GUI `main`
(push target identified by URL via `git remote -v`, never by remote name; never push to
upstream). Conventional commit style matching recent fork PRs.
