# The record path (GDH fork)

How RF gets from the capture to the recorded files, and why. This replaces upstream's
per-block spill, which `TECHNICAL.md` still describes for the direct path; on the fork the
ordered record FIFO below applies to every record mode (FLAC, converted RAW, direct CXADC
RAW).

## Why it changed

On 2026-09-15 the 1998 Christmas Gift capture (VHS, wm, output on local NVMe) had both FLAC
writers stall to about 17 % of real time for ~50 s. Record ring A (1280 MiB) filled, and
upstream's overflow path engaged: on a full ring the extraction thread spilled that one
128 KiB block to a temp file, then retried the ring on the very next block. The writer read the
ring before the spill, so every spilled block was written after newer ones: 815 ring/spill
switches in 32 s scrambled the video RF, and the decode showed 771 dropped/duplicate fields.

On 2026-09-18 the Martin Luther 1953 capture recorded to `/mnt/cx_storage` (NFS from cs0). The
spill file lived in the output folder, on NFS, and the extraction thread wrote it itself: when
the server was slow the extraction thread fell behind real time and the capture ring dropped
2,909 blocks -- a loss the capture log never mentioned ("drops=0").

## The design

```
capture thread -> BUF_CAPTURE_RF -> extraction thread --push--> record FIFO A/B --next--> writer -> file
                                    (CXADC direct tap --push-->)
```

**One ordered FIFO per channel** (`misrc_gui/output/gui_record_fifo.{c,h}`):

- the channel's record ring (`BUF_RECORD_A/B`) while it has room;
- a spill file on a **local** disk for whatever does not fit;
- a **segment log**: the producer publishes `{store, first stream byte, file offset}` before
  a segment's first byte, and the reader walks the segments in log order, each capped at the
  next segment's start. The ring keeps taking data whenever it has room (leaving the spill
  needs `switch_back_free_bytes`, 8 MiB or ring/4, free -- hysteresis that bounds the segment
  count), so RAM stays in use and only true overflow touches the disk.

The contract: every byte `push()` accepts reaches the writer exactly once, in capture order,
including the partial tail after stop. A byte is lost only through an explicit drop (ring full
**and** the spill failed), which is returned, logged with its RF sample position and counted.

**The capture side never waits and never touches the output filesystem.** `push()` takes no
lock and does no wait; the spill is positional I/O (`pwrite`/`pread`; `OVERLAPPED` on
Windows) on an unlinked file the producer alone writes and truncates. Consumed space goes back
by hole punching where the filesystem has it (`fallocate`/`F_PUNCHHOLE`), otherwise by
reusing the file once fully drained. The destination's free space, the output file sizes for
the UI and the capture log itself go through a per-session **record monitor thread**; capture
threads only queue log lines.

**Spill location.** `record_spill_dir` (client-local). Empty = automatic: the output folder
if it is a local filesystem, otherwise this machine's cache folder
(`~/.cache/misrc_gui/spill`, `~/Library/Caches/misrc_gui/spill`,
`%LOCALAPPDATA%\misrc_gui\spill`). NFS, SMB, FUSE and tmpfs are refused
(`gui_record_fifo_fs_kind`). The choice is logged at record start.

**Producers.** The extraction thread (converted channels) and the CXADC capture thread's
direct tap (native channels) push between `gui_record_producer_enter()`/`_leave()` (or the
tap's iteration enter/leave) on one shared gate. Stop disables the gate, waits for it to go
idle, then closes the FIFOs; the writers drain to the end and exit.

**Writers.** FLAC, converted RAW and direct RAW read only their FIFO, in variable lengths up
to 2 MiB. RAW output goes through `gui_record_sink`: a failed `fwrite` holds only the unwritten
remainder and retries every 2 s while the FIFO backs up behind it (no reorder, no duplicate).
A dead FLAC encoder cannot recover (libFLAC stays in `CLIENT_ERROR`), so it stops the capture
with `GUI_DROPOUT_RECORD_OUTPUT`; the writer keeps draining so the capture never backs up, and
STREAMINFO gets the samples that really reached the file. Resampling never falls back to the
unresampled block, and the resampler's tail is flushed at stop.

**Ring buffer.** `common/ringbuffer.c` counters only grow (index = counter mod size). The old
reader rebased both counters at a wrap with two separate atomic updates, which a producer
could observe half-done and lose its commit or overwrite unread data.

## What the capture log says

- `Record overflow spill: <dir> (<fs>, <free>); output <path> is local|network (<fs>)`
- `Record overflow on channel A: RAM buffer full at rf_sample=N (t=…s) …` -- one line per
  episode, not one per block;
- `Record overflow on channel A: spill backlog 512.00 MiB` every 256 MiB;
- `Record overflow on channel A ended: rf_sample=N .. rf_sample=M went through the disk spill
  in order; spilled … in S segment(s), peak backlog …`;
- `Record output is slower than capture: in … MB/s, out … MB/s …; the overflow spill fills in
  ~N min` when the backlog has grown for a minute;
- `Capture buffer overflow: K RF block(s) lost before the record path near rf_sample=N` --
  real gaps, also honouring `stop_on_dropout`;
- at finalize, `Record channel A summary: accepted=… in RAM=… spilled=… dropped=…
  discarded=…`, plus a `MISMATCH` WARN when anything was lost.

The sidecar's `result.waits` counts pushes that overflowed to the spill (kept, in order);
`result.drops` counts losses.

## Proof

- `misrc_gui --record-order-selftest [dir]`: the Simulated device emits a self-checking
  counter stream (`gui_record_order_check.h`), the writers stall for 3 s at a 1 GB memory
  budget, and the FLAC and RAW recordings must hold every sample once, in order, on both
  channels. On upstream's path it fails (gaps 14, reorders 3, A/B lengths differ); with an NFS
  `dir` it also proves the spill stays local.
- `test/gui_record_fifo_harness.c` (meson `gui_record_fifo`, guard `record FIFO runtime`, also
  run under ThreadSanitizer): random pressure, 200 random-moment stops, recycle/truncate
  stress, a 4-slot segment log, failing spill writes, the sink, and a replay of the old
  algorithm that must be caught reordering.
- Guards: `record FIFO contract` (static), `raw direct passthrough contract`,
  `record order selftest (post-build)`.
