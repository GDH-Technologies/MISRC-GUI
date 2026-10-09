# VHS RF preview

The VHS FM panel provides continuous PAL/NTSC VHS SP video-RF preview. It
weaves two original fields into alternating rows, without deinterlacing,
field duplication or motion interpolation. Output is RGBA at 768x576 PAL
or 640x480 NTSC, displayed at 4:3. Nominal output rates are 25 and 29.97 fps.

## Operation

1. Open a Video panel for the desired RF channel.
2. Cycle Decoder to **VHS FM** and select PAL or NTSC.
3. Connect a capture device or select FLAC playback files and connect.
4. The shared Budget button selects 1.00, 0.75 or 0.50 logical cores and is
   saved with the existing application settings.

Only VHS FM uses this RF pipeline. CVBS and Tape Decode retain their existing
processing paths. The system and tape speed are not detected automatically.
The input must contain video RF; Hi-Fi audio RF does not produce a picture.

The OSD reports physical input rate, H/V/color lock, decoded/displayed fps,
DSP time, worker age, vertical candidates, accepted/rejected fields, queue
loss, pressure skips and worker CPU. Display fps counts unique frames drawn.
Worker age starts at preview submission; it excludes device buffering, the
upstream capture FIFO and final UI presentation. These counters are separate
from recording drops.

## Sample rates and continuity

The decoder processes RF internally at 20 MSPS:

| Physical input | Conversion |
| --- | --- |
| 20 MHz | No rate conversion |
| 40 MHz | Stateful halfband FIR, 2:1 |
| 80 MHz | Two stateful halfband FIR stages, 4:1 |
| Other 16..160 MHz | libsoxr HQ, when available |

Without libsoxr, only 20/40/80 MHz are supported. Unsupported rates fail
explicitly. Rate support does not establish usable signal bandwidth or
picture quality for every device.

Every input block carries its physical rate, absolute sample position,
epoch and discontinuity flag. Rate changes, seeks, looping, channel mapping
changes, missing input and dropped preview blocks reset resampler/decoder
history. Hardware writes publish sparse metadata at absolute FIFO positions;
preview rejects ambiguous boundaries while recording keeps the acquired RF.
Unavailable preview metadata disables preview for that block and preserves
recording channel presence and mapping using available capture information.

FLAC uses actual decoded sample counts; final-block padding is not decoded as
RF. Its physical rate is retained independently of whether length is known.
The paired reader currently requires equal physical clocks for both channels.
Mixed-rate pairs show an explicit unsupported message; independent channel
cursors/resampling are outside this change.

## Processing and ownership

`capture/playback -> raw FIFO + metadata -> extraction -> bounded preview
queue -> rate normalization -> RF decoder -> newest RGBA -> UI`

- Extraction submits every block independently of the waveform/display stride.
  It never waits for decoder, renderer, frame mutex, CPU permit or queue space.
- Visible VHS FM panels subscribe to a worker keyed by channel and PAL/NTSC
  system. Matching panels share decoding and one input submission. Workers are
  created lazily and released after the last subscriber and in-flight copy.
- Each worker has 256 slots of 65536 int16 samples, about 32 MiB. Queued input
  duration and age are capped at 200 ms. Overrun drops stale preview work and
  reacquires from the next accepted block.
- Workers own resampler, filter, sync and color state. The UI owns textures and
  its frame copy; capture does not access Raylib textures.

The streaming decoder performs quadrature FM demodulation, luma deemphasis,
independent color-under IQ recovery, horizontal/equalization/vertical sync,
measured line resampling, burst phase/gain correction and PAL/NTSC conversion.
Y and chroma share the line map. Field parity and cadence must agree before
publishing a woven frame. Missing chroma falls back to grayscale.

After 40 lines without H sync, acquisition discards learned thresholds, pulse
history and unfinished fields. Continuous input can then recover automatically
when tape playback returns, without reconnecting or changing PAL/NTSC. Sample
positions, DSP filters, diagnostic totals and the capture session stay intact.

Luma uses fixed profile levels with bounded, slow timing correction rather
than noisy per-line black/gain normalization. NTSC display deemphasis is
independent of the sync plane. Chroma averaging uses neighbors from the same
field and retains the original luma rows.

The input-rate separation, field assembly and color-under concepts were
informed by vhs-decode-dotnet. The halfband coefficients originate from its
`VHSDecode.Core/Rf/HalfRateSampleLoader.cs`; source attribution is retained in
`gui_rf_resampler.c`. This preview does not implement the complete offline
.NET TBC/color pipeline or promise output equivalence. SECAM, VHS LP/EP,
S-VHS, Betamax, dropout repair and advanced comb filtering are not implemented.

## Capture protection

All workers share a soft CPU-time budget, default one logical core, with FIFO
arbitration and at most one active processing block. Budget waits occur only
inside workers. Windows uses thread CPU time and requests below-normal
priority; unavailable thread accounting falls back to processing wall time.

This allowance covers preview processing, not the entire GUI or an OS-enforced
quota. A block cannot be preempted; block size and coarse OS CPU accounting can
produce short bursts above the selected rate.

Extraction monitors capture and selected recording buffers. Preview pauses
at 85% fill or a new write drop, or after 150 ms continuously at 60% fill.
Recovery requires fill at or below 35% without new drops for two seconds.
A new session clears old pressure state. Paused workers reject input before
copying and freeze the last complete frame; resume reacquires from fresh RF.

## FLAC startup and timeline

Connect prepares an independent playback context on a joinable background
thread. The UI publishes it and starts playback after preparation succeeds.
Cancel/Space request cancellation; shutdown joins the loader before freeing
application state. Disconnect joins producers/indexing before retiring their
context, so a later connection cannot inherit old decoder or index results.

Usable metadata or a validated persistent cache supplies the length directly.
Otherwise, startup attempts a bounded libFLAC tail probe: metadata reads at
most 1 MiB, tail access/read totals at most 8 MiB, callback reads at most
64 KiB, and an approximately 250 ms cooperative deadline. A filesystem call
already in progress cannot be interrupted by that deadline.

The tail endpoint is provisional and appears with `~` on the timeline. It
allows best-effort seeking while an independent background reader verifies
all frame grammar/integrity from the beginning. Tail failure leaves ordinary
unknown-length playback available. The producer's actual EOF controls looping;
a provisional endpoint does not truncate playback.

Successful verification publishes the total and caches it without changing
position, speed, pause/loop state or epoch. Failure withdraws the provisional
length and seeking while playback can continue. The cache validates file
identity, size, mtime and bounded head/tail fingerprints; it is not a complete
content hash. Full verification is not a PCM/STREAMINFO MD5 check. A successful
seek to valid embedded frames does not establish their stream provenance.

## Build and tests

Install the dependencies described in the repository README. From its root:

```sh
meson setup build misrc_tools --buildtype=release
meson compile -C build
meson test -C build --print-errorlogs --num-processes 1
```

The registered tests cover synthetic PAL/NTSC RF, field weave, sync loss and
recovery, color, resampling, queue/lifecycle/CPU policy, input stamps,
recording isolation under missing metadata, FLAC startup/cancel/seek/cache,
physical-rate tags and timeline state. The timeline helper test is registered
for native, single-command compilers when Python is available. Tests do not
require a capture device or media file.
On Windows, verify static RTL-SDR linkage and startup separately from
hardware capture validation.

For optional media checks, raw input is signed little-endian int16 carrying
the normalized signed 12-bit capture values. A padded 16-bit capture must be
converted to that scale first. Commands below use POSIX executable names;
Windows executables have `.exe`:

```sh
build/gui_rf_preview_file input.s16le 20000000 ntsc output.ppm --realtime
build/gui_rf_preview_file input.s16le 40000000 pal output.ppm --max
build/gui_rf_preview_record_load input.s16le 20000000 ntsc output-prefix 60 on 1 4 1.0
```

The file probe links the production worker/resampler/core, prints metrics and
saves the last complete frame. Maximum-speed mode drains all submitted input;
realtime mode follows its physical clock. The record-load probe compares
recorded samples and reports drops; `off` supplies its baseline without
preview. These probes bypass the physical capture device and native window.

The longer reconnect integration is built explicitly and needs an existing
output directory plus at least one second of original padded 16-bit, 20 MHz
NTSC raw RF. It writes these samples as FLAC16 and production playback then
reduces them to the capture scale; do not pre-scale this probe's input:

```sh
meson compile -C build gui_rf_playback_reconnect_test
build/gui_rf_playback_reconnect_test padded16-ntsc20.s16le output-dir
```

It re-encodes supplied RF as FLAC and exercises the actual playback FIFO,
extraction, routes, worker/core, frame swap and display-thread lifecycle over
four sessions, including 30 seconds disconnected. It checks both known and
unknown header lengths. Graphics and inactive recording services are stubbed;
this is not physical-device or visible-window validation.

Real media quality, weak-machine performance and sustained hardware recording
still need separate validation. Short headless checks cannot establish those
properties for every capture or computer.
