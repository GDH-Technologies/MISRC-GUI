# DEV NOTE (HARD): 8 MB capture-output stdio buffer + RF rate provisions — affects ALL capture devices and ALL output/network types

Status: merged (PR #9 + provisions commit). Read this before touching ANY capture,
record, output, resampler or worker rate/buffer path.

## The change (PR #9, r3dfx080)

`gui_record_start_confirmed` now sets an 8 MB fully-buffered stdio buffer
(`setvbuf(fp, NULL, _IOFBF, 8 MB)`) on EVERY FLAC and RAW capture output file,
right after the (UTF-8) open, for both channels. Reason (measured on the PR):
libFLAC emits one small fwrite() per frame (often <1 KB); with the CRT default
4 KB buffer each flush becomes a separate synchronous write, which is
round-trip bound on SMB/NFS. Measured on Windows -> Samba over 1 GbE:
4 KB sync writes ~5.8 MB/s vs 256 KB ~26.4 MB/s. An 8-bit 20 MSPS FLAC capture
is ~13 MB/s, so default buffering backpressures the record path onto a share,
builds a spill backlog, and spends a long time writing it out after Stop.

## WHY THIS IS A HARD NOTE — it affects every capture device and every target

1. ALL capture devices: the buffer is set in `gui_record_start_confirmed` — the
   single path every recording goes through. That means hsdaoh/MISRC (A/B),
   CXADC cards (incl. direct native RAW taps), DdD, simple_capture/MF, the
   simulated device, and FLAC/RAW re-record from playback. No device bypasses
   it; any change to record-open must keep (or consciously revisit) the buffer.
2. ALL output/network types: the win is on high-latency targets (SMB, NFS,
   USB, network shares); on local NVMe it is neutral-to-positive. The buffer
   is 8 MB PER FILE (16 MB across A+B) of extra write-backlog latency:
   - Stop/write-out latency grows by up to one buffer per channel.
   - libFLAC's final STREAMINFO rewrite goes through fseek(), which flushes
     first, so finalization still happens — but a process KILLED during the
     write-out still leaves the file unfinalized (total_samples=0, zero MD5).
     The bigger the buffer, the wider that window. That risk exists on every
     device and every target; it is a latency trade, not a correctness change.
3. Sustained throughput is still the target's job: buffering only batches.
   The output target must still exceed the per-channel byte rate or the
   record path backpressures (spill file + backlog) exactly as before. Use the
   numbers below when qualifying an output target.

## RF rate provisions (this session's tweaks)

- **2x 40 MSPS dual-channel**: two per-channel preview workers, each on the
  40 MSPS exact halfband path (80 MSPS exact path exists for single streams).
  Two 40 MSPS workers need ~2.2 logical cores each at realtime (~4.5 total) —
  inside the 8.0-core shared preview budget ceiling.
- **Up to 200 MSPS single-stream**: the rate ceiling was raised 160 -> 200 MSPS
  in the resampler (`RF_MAX_INPUT_RATE_HZ`), the preview worker
  (`RF_MAX_RATE`), and the offline harnesses (`gui_rf_preview_file`,
  `gui_rf_preview_record_load`). Arbitrary rates between the exact paths go
  through soxr; the resampler contract test now rejects >200 MSPS and covers
  a 200 MSPS soxr case.

## Byte-rate table (qualify your output target against these)

RAW bytes/s = rate x bytes-per-sample; FLAC is compressed (grows with depth
and signal entropy; the 13 MB/s figure is a measured 8-bit 20 MSPS RF).

- 40 MSPS 16-bit RAW: 80 MB/s per channel, 160 MB/s for 2x40 dual.
- 40 MSPS FLAC: ~13-45 MB/s per channel (depth-dependent).
- 80 MSPS 16-bit RAW: 160 MB/s single stream.
- 200 MSPS 16-bit RAW: 400 MB/s single stream — 1 GbE SMB (~110 MB/s) CANNOT
  sustain it; local NVMe or 10 GbE+ only.
- 200 MSPS 8-bit FLAC: ~130 MB/s estimated (10x the measured 8-bit 20 MSPS).

1 GbE SMB sustains ~26 MB/s even with large buffered writes: fine for
compressed FLAC at 20-40 MSPS, NOT fine for 2x40 16-bit RAW or anything at
200 MSPS. The 8 MB buffer delays backpressure; it does not remove it.

## Files touched by the provisions

- `misrc_tools/misrc_gui/signal/gui_rf_resampler.c` — input-rate window
  16..200 MSPS (exact 20/40/80 + soxr arbitrary).
- `misrc_tools/misrc_gui/processing/gui_rf_preview_worker.c` — submit/config
  rate window 16..200 MSPS.
- `misrc_tools/test/gui_rf_preview_file.c`, `gui_rf_preview_record_load.c` —
  harness rate windows.
- `misrc_tools/test/gui_rf_resampler_test.c` — contract now rejects >200 MSPS
  and adds a 200 MSPS soxr continuity/gain case.
