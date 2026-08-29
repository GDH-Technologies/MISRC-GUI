# RTSP Streaming of the USB Preview — Design

Date: 2026-08-28 (decisions resolved 2026-08-29)
Repo: GDH-Technologies/MISRC-GUI (misrc_tools vendored at `misrc_tools/`)
Status: **approved** — all open decisions resolved; see [Resolved decisions](#resolved-decisions)
for the record of what was chosen and why. Written against capture-node as the reference
implementation, at Reece's request.

## Goal

Publish the USB dongle's preview picture **and its audio** as a live stream, so a tape being
captured can be watched from another machine — a phone in another room, a laptop
downstairs — the way capture-node already publishes `VCR0` / `CAM0` / `CAM1`. The stream is
for monitoring only: the RF FLACs remain the archival master and the MKV reference recording
remains the QC artifact.

Viewing targets, which drive the protocol set:

- **VLC on macOS and iOS**, over the LAN → RTSP
- **Safari**, macOS and iOS → WebRTC
- **Desktop browsers** → HLS (via hls.js)

Hard requirement from the outset: **the mediamtx instance must not interfere with
capture-node's.** capture-node runs three mediamtx processes on this very host
(`workflow-master`), one per environment tier, and its port scheme is pinned by a test in
that repo (`python/tests/config/test_committed_env_seeds.py`). MISRC GUI must stay entirely
out of that namespace.

## Background: how capture-node does it, and why we cannot copy it verbatim

capture-node's pipeline is:

```
/dev/vcr0-ref-video --(-f v4l2)--> ffmpeg --(-f tee)--> [f=null]
                                                     \-> rtsp://127.0.0.1:8554/VCR0 --> mediamtx
```

ffmpeg opens the V4L2 device **itself**. mediamtx is spawned as a child of the FastAPI app
(`python/capture_node/core/rtsp_server.py`, `ManagedRtspServer`) with a per-env config file,
and every listener except RTSP is explicitly disabled.

We cannot reuse that shape, for one structural reason: **MISRC GUI already owns the V4L2
device exclusively.** `gui_preview_v4l2.c` is a singleton reader — one device, one stream —
and a V4L2 capture node will not admit a second streaming user. Pointing an ffmpeg `-f v4l2`
at `/dev/videoN` while the preview panel is live returns `EBUSY`.

What we have instead is better: `gui_preview_v4l2.h` already exposes a **frame tap** that
hands every good frame's raw YUYV to a callback on the capture thread, before the RGBA
conversion, and a **hold refcount** (`gui_preview_hold_acquire/release`) that keeps the
device open independently of whether any panel is showing. `gui_video_record.c` is already
built on exactly these two hooks:

```
preview capture thread --tap--> bounded ring --> writer thread --> ffmpeg --> mkv
     (never blocks)                                  (send)
```

The RTSP publisher is the same shape with a different ffmpeg argv and an RTSP URL where the
file path was. That is the whole feature.

**Audio does not change that shape.** The dongle exposes its own ALSA capture node, so
ffmpeg opens the audio itself as a second input, exactly as capture-node does. Audio never
touches our ring, never crosses our threads, and needs no sync work — see
[Audio](#piece-3--the-ffmpeg-publisher).

## Architecture

```
                                    ┌─> ring ─> writer ─> ffmpeg -an ─> file.mkv
preview capture thread --tap--> mux ─┤        (gui_video_record, unchanged)
     (never blocks)                 └─> ring ─> writer ─> ffmpeg ─┬─ video: rawvideo on stdin
                                              (gui_rtsp_stream)   └─ audio: -f alsa (dongle)
                                                                        │
                                                                        v
                                                    rtsp://127.0.0.1:8654/misrc-preview
                                                                        │
                                                          mediamtx (child of misrc_gui)
                                                                        │
                                            ┌───────────────────────────┼───────────────┐
                                            v                           v               v
                                     RTSP  :8654                 WebRTC :8989      HLS  :8988
                                     (VLC, mac/iOS)              (Safari)          (hls.js)
```

Four pieces, three of them new.

### Piece 1 — `gui_preview_tap_mux` (new, small)

`gui_preview_v4l2.c` publishes the tap through a **single** atomic slot:

```c
static _Atomic(const preview_tap_t *) g_tap;
static atomic_int g_tap_inflight;
```

Installing a second tap silently displaces the first, so today starting an RTSP stream would
stop the reference recording from receiving frames — a silent data-loss bug, not an error.
The mux fixes that without touching the preview module's lock-free contract:

- The mux owns **one** `preview_tap_t` with static storage and installs it via the existing
  `gui_preview_tap_install()`.
- Consumers register with `gui_preview_mux_add(const preview_tap_t *)` /
  `gui_preview_mux_remove(...)`, kept in a small fixed array (4 slots is generous) of atomic
  pointers.
- The mux's callback iterates the array and forwards. It installs the underlying tap on the
  first registration and removes it on the last, so the preview module sees exactly the same
  install/remove lifecycle it does now.
- `gui_preview_mux_remove()` reuses the quiescence trick already proven in
  `gui_preview_tap_remove()`: publish NULL into the slot, then spin until that slot's
  in-flight count is zero, so a consumer can free its state safely.

`gui_video_record.c` changes by two lines — `gui_preview_tap_install(&vr_tap)` becomes
`gui_preview_mux_add(&vr_tap)`, and likewise for removal. Nothing else about it moves.

> Alternative considered and rejected: one ffmpeg with `-f tee` writing both the MKV and the
> RTSP branch, which is exactly what capture-node does
> (`core/reference_capture.py:362-397`). It avoids the mux entirely, but it welds the two
> lifecycles together — you could not start or stop streaming without restarting the
> recording, and stopping a recording would drop the stream. capture-node gets away with it
> because a "session" there *is* the unit of work. Here, streaming and recording are
> independently useful, and the mux is ~80 lines.
>
> Adding audio was the one thing that could have justified the tee, since a shared ffmpeg is
> the natural place to mux A/V. It does not, because the audio comes from a **separate ALSA
> device that ffmpeg opens for itself**. The MKV stays video-only, so the two never contend.

### Piece 2 — `gui_rtsp_stream.c` / `.h` (new, modelled on `gui_video_record.c`)

Deliberately a sibling of the recorder, not a generalization of it. The two differ in enough
places (encoder choice, drop policy under stall, restart-on-failure, no trailer to flush,
audio) that merging them would leave a module with two personalities.

Public surface, mirroring `gui_video_record.h` so it reads as house style:

```c
typedef struct {
    bool     running;
    bool     error;
    char     err_text[192];
    uint64_t frames_submitted;
    uint64_t frames_dropped;     /* ring was full */
    uint64_t frames_written;
    double   encode_fps;         /* parsed from ffmpeg progress */
    double   encode_bitrate_kbps;
    bool     audio_active;       /* false => video-only, see audio_note */
    char     audio_note[96];     /* why audio is off, when it is */
    int      child_pid;
    char     url_rtsp[256];      /* reader-facing URLs, not the loopback publish one */
    char     url_webrtc[256];
    char     url_hls[256];
} gui_rtsp_stream_status_t;

int  gui_rtsp_stream_start(uint32_t width, uint32_t height, uint32_t pitch,
                           uint32_t fps_num, uint32_t fps_den,
                           char *err, size_t err_cap);
void gui_rtsp_stream_request_stop(void);
void gui_rtsp_stream_finish(void);
void gui_rtsp_stream_shutdown(void);
gui_rtsp_stream_status_t gui_rtsp_stream_get_status(void);
bool gui_rtsp_stream_is_running(void);
```

Geometry and rate are the **negotiated** values, exactly as `gui_video_record_start()`
requires, and `pitch` comes from `gui_preview_negotiated_pitch()` — the preview header warns
that it may exceed `width*2`, and a consumer that assumes otherwise shears the picture. The
RTSP module must handle it the same way the recorder does.

The module takes `gui_preview_hold_acquire()` for the life of the stream, so closing the
preview panel does not silently end the broadcast.

**Ring and drop policy.** Same bounded ring as the recorder, same structural guarantee: if
ffmpeg stalls, the socket fills, then the ring fills, then frames are dropped, and
`VIDIOC_QBUF` is never delayed. Note this is why we do **not** need capture-node's
`-f tee -use_fifo 1 [f=null]|...` trick — that exists purely to stop a wedged RTSP server
back-pressuring a `-f v4l2` input. Our ring already provides that property upstream of
ffmpeg, so the RTSP output can be a plain `-f rtsp`. One less moving part.

Where the two modules genuinely differ: the recorder must not drop frames if it can help it
(it dupes to keep the timeline honest), whereas the streamer would rather be current than
complete. It carries **four** slots against the recorder's sixteen, and under pressure it
**drops the incoming frame** while the consumer keeps the one it is sending.

> **Amended 2026-08-29.** This originally read "the streamer discards oldest". It does not,
> and the reason is worth recording, because the obvious reading is a bug.
>
> A slot holds one raw YUYV frame — 829,440 bytes at 720x576 — and the writer thread holds
> `slots[tail]` for the whole of its `send()`, because a partial send must resume from the
> same buffer. The ring can only fill *after* the 4 MiB socket buffer has filled, which
> means the writer is by then parked inside `send()` on that slot. Discarding oldest would
> hand the producer that exact buffer to overwrite while the kernel is copying out of it:
> a torn frame, half old and half new, delivered to every viewer with `frames_written`
> still incrementing. The policy would be unsafe precisely and only in the situation it
> exists for.
>
> Doing it safely is possible — a per-slot seqlock plus a scratch copy on the consumer, so
> no slot is ever held across a `send()` — at one extra 810 KiB memcpy per frame on the
> writer thread. It was not worth it. The ring is 160 ms deep, so that bounds the entire
> difference: after a stall clears, a viewer sees up to 160 ms of slightly stale video
> before catching up, instead of jumping straight to live. Measured runs drop no frames at
> all, so this buys a sixth of a second in a failure mode not yet observed, and pays for it
> with a second concurrency mechanism in a path whose current correctness argument is
> "the consumer owns `slots[tail]`, full stop."
>
> Revisit if `frames_dropped` is ever seen climbing in the field.

**Audio is not this module's data path.** The module resolves the device name and puts it in
the argv; ffmpeg does the rest. If resolution fails the stream still starts, video-only,
with `audio_active = false` and a human-readable `audio_note`. Losing the picture because
the sound card moved would be a poor trade.

### Piece 3 — the ffmpeg publisher

Encoder selection mirrors `gui_video_record_probe()`: ask the resolved ffmpeg binary what it
has (`-encoders`), cache the answer, prefer hardware. On this host both are present
(`h264_nvenc` on the RTX 3080, plus `libx264`).

Preferred, NVENC — capture-node's low-latency preview settings
(`python/capture_node/core/usb_stream.py:565-640`):

```
ffmpeg -hide_banner -nostdin -nostats -loglevel warning
  -f rawvideo -pixel_format yuyv422 -video_size <WxH> -framerate <num/den> -i -
  -f alsa -ac 2 -ar 48000 -i plughw:CARD=MS210x,DEV=0
  -sn
  -vf format=nv12
  -c:v h264_nvenc -preset p2 -tune ull -rc vbr -cq 0 -b:v 0
  -g <fps> -bf 0 -forced-idr 1 -zerolatency 1
  -c:a libopus -b:a 64k -ar 48000 -ac 2
  -f rtsp -rtsp_transport tcp rtsp://127.0.0.1:8654/misrc-preview
```

Fallback, software:

```
  -vf format=yuv420p
  -c:v libx264 -preset veryfast -tune zerolatency -threads 2
  -b:v 2M -maxrate 2M -bufsize 4M -g <fps> -keyint_min <fps> -bf 0
```

Notes on the choices, all inherited from capture-node's hard-won defaults:

- `-rtsp_transport tcp`. capture-node's per-device model default is `tcp`
  (`rf_capture_models/usb_stream.py:105`) while its fleet default is `udp`; TCP is the right
  default here because we are publishing over loopback where TCP costs nothing and never
  fragments. Readers can still pull over UDP.
- `-g <fps>` (a 1-second GOP) and `-bf 0` keep join latency low for a new viewer.
- `-threads 2` on the software path only: "spare cores belong to the RF recorders" is as
  true here as it is in capture-node, and more so — this app *is* the RF recorder.
- `-sn` only. `-an` is gone: the stream carries audio.
- No deinterlacer by default. The source is 480i/576i off a VCR, so `bwdif` would help a
  remote viewer, but it costs latency and CPU and the local preview panel does not
  deinterlace either. Offer it as a setting, default off.

The publisher process is spawned with the same `nice(+5)` posture capture-node uses for its
ffmpeg children, for the same reason: RTSP fan-out must yield to RF ingest.

#### Audio

The dongle is a single USB device that presents both a video and an audio node. Verified on
`workflow-master`, 2026-08-29:

```
/proc/asound/cards:  3 [MS210x] USB-Audio - MS210x
                       MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed
v4l2-ctl:            USB Video (usb-0000:00:14.0-13.3) -> /dev/video0
                                 ^^^^^^^^^^^^^^^^^^^^ same USB path
```

Same device, same clock, so **A/V sync needs no work** — the reason this is the audio source
rather than the RF-demodulated path in `gui_audio.c`, which is archival-grade but sits on the
CXADC clock (`card 0: CXADC+ADC-ClockGen`) and would need resampling and drift correction
against the dongle's video. For a monitoring-only stream, monitoring-grade audio that is
free to sync is the right trade.

**Device resolution order.** Card *indices* are not stable across reboots or replugs, so
`hw:3,0` must never be written down:

1. Explicit device string from settings, if set.
2. **Auto (default): match USB topology.** Read the preview V4L2 device's USB path, then
   find the ALSA card in `/proc/asound/cards` whose bus address matches. This binds the audio
   to *the dongle currently supplying the picture*, which is correct even with two capture
   dongles attached.
3. Fall back to the first ALSA card whose name matches the video device's, as
   `plughw:CARD=<name>,DEV=0`.
4. If none resolves, stream video-only and set `audio_note`.

`plughw:` rather than `hw:` deliberately: `hw:` demands an exact format match and fails
outright when the dongle's native rate is not 48 kHz, whereas `plughw:` inserts the
conversion. The cost is a resampler ffmpeg would otherwise apply anyway.

**Codec: Opus.** MediaMTX does not transcode. Its supported audio codecs are Opus / G722 /
G711 for WebRTC and Opus / FLAC / AAC for HLS, so **Opus is the only codec that serves both**
from a single publish, and RTSP carries it fine for VLC. This is a decision with one known
gap; see [Risks](#risks).

### Piece 4 — `gui_mediamtx.c` / `.h` (new, modelled on `ManagedRtspServer`)

Spawn mediamtx as a child of `misrc_gui`, kill it on exit. ~120 lines, the same size as
capture-node's supervisor.

- **Binary resolution**: bundled copy inside the AppImage first (see
  [Bundling](#bundling-mediamtx)), then an explicit path from settings, then `mediamtx` on
  `PATH`. If none resolves the feature is simply unavailable and the UI says so — no hard
  failure, unlike capture-node which hard-fails startup preflight. MISRC GUI is a desktop app
  and must still run without it.
- **Config is generated at runtime**, not shipped, into
  `$XDG_RUNTIME_DIR/misrc-gui/mediamtx.yml` (falling back to `$TMPDIR`). Generating it
  guarantees the ports in the file are the ports the app actually chose, so the two can never
  drift, and `$XDG_RUNTIME_DIR` is cleaned on reboot.
- **Lifecycle**: start before the first publisher, stop on app exit (`SIGTERM` → wait 3 s →
  `SIGKILL` → wait 2 s, capture-node's exact ladder). Idempotent if already alive.
- Improvement over the reference: capture-node has **no health check and no restart** for
  mediamtx — if it dies mid-run it is simply gone until the daemon restarts. Since we are
  writing this fresh, poll the child once a second and surface `running`/`last_error` in the
  status panel, so a dead server is visible rather than mysterious.

## The mediamtx config

### Ports — verified free on `workflow-master`, 2026-08-29

capture-node's live listeners on this host, read from `ss -lnp` against the three running
mediamtx PIDs:

| tier | RTSP (TCP) | RTP (UDP) | RTCP (UDP) |
|---|---|---|---|
| prod | 8554 | 8000 | 8001 |
| preview | 18554 | 18000 | 18001 |
| dev | 28554 | 28000 | 28001 |

Its scheme is `tier_digit × 10000 + canonical`, canonical being `8554 / 8000 / 8001`. So
every future tier capture-node might add is `N8554 / N8000 / N8001`. Choosing a **different
canonical base** therefore avoids not just today's ports but every port that scheme can ever
produce.

MISRC GUI uses **mediamtx canonical + 100**, so the whole set follows one rule:

| listener | port | canonical |
|---|---|---|
| RTSP | 8654 tcp | 8554 |
| RTP | 8100 udp | 8000 |
| RTCP | 8101 udp | 8001 |
| HLS | 8988 tcp | 8888 |
| WebRTC HTTP | 8989 tcp | 8889 |
| WebRTC ICE | 8289 udp | 8189 |

All six verified free on this host, and none is reachable by capture-node's scheme at any
tier, which only ever yields `N8554 / N8000 / N8001`.

Every remaining listener is disabled. This is load-bearing, not tidiness: mediamtx defaults
`rtmp`/`srt`/`moq` to **yes**, so omitting them would have MISRC GUI quietly claim 1935, 8890
and 8892 — ports capture-node deliberately leaves free and might want later.

### Generated file

```yaml
# Generated by MISRC GUI - do not edit; rewritten on every launch.
#
# Ports are mediamtx canonical + 100. capture-node's scheme is
# tier x 10000 + 8554/8000/8001, which can never produce these.
# Every listener we do not serve is off: mediamtx defaults rtmp/srt/moq
# to yes, and claiming those ports would collide with a service we do not own.

rtsp: yes
rtspAddress: 127.0.0.1:8654
rtpAddress: 127.0.0.1:8100
rtcpAddress: 127.0.0.1:8101
# Transports deliberately exclude udpMulticast, which mediamtx enables by
# default: it would bind multicastRTPPort 8002 / multicastRTCPPort 8003.
rtspTransports: [udp, tcp]
# Keeps RTSPS off, so nothing binds 8322.
rtspEncryption: "no"

hls: yes
hlsAddress: 127.0.0.1:8988
hlsVariant: lowLatency
hlsAlwaysRemux: no

webrtc: yes
webrtcAddress: 127.0.0.1:8989
webrtcLocalUDPAddress: 127.0.0.1:8289
# Loopback mode: advertise only the loopback ICE candidate. In LAN mode this
# becomes `webrtcIPsFromInterfaces: yes` (mediamtx's default) so that remote
# browsers get a reachable candidate; leaving it on in loopback mode would
# advertise every LAN address for a server nothing off-box can reach.
webrtcIPsFromInterfaces: no
webrtcAdditionalHosts: [127.0.0.1]

readTimeout: 10s
writeTimeout: 10s
writeQueueSize: 512

rtmp: no
srt: no
moq: no

# api/metrics/pprof/playback default to no; left unset so we bind nothing on
# 9996-9999.

paths:
  misrc-preview:
    source: publisher
    sourceOnDemand: no
    overridePublisher: yes
    # mediamtx reads 0 as "no limit", not "no readers".
    maxReaders: 0
```

`127.0.0.1` is the **default**, not the only option. Bound to loopback the stream is
reachable only on this machine; the "share on LAN" setting swaps all six addresses to
`:8654` / `:8100` / `:8101` / `:8988` / `:8989` / `:8289` and flips
`webrtcIPsFromInterfaces` to `yes`. There is no auth, matching
capture-node, which has none either — so LAN mode means anyone on `gdhvc.lan` can watch the
tape being captured. The setting says so.

Path name `misrc-preview` is deliberately outside capture-node's naming convention (`VCR0`,
`VCR1`, `CAM0`, `CAM1`), so the two are never confusable in a viewer's bookmarks even if
someone does eventually merge the configs.

### Reader URLs

Shown in the UI, selectable for copying. `<host>` is `127.0.0.1` in loopback mode and the
machine's hostname in LAN mode:

```
RTSP    rtsp://<host>:8654/misrc-preview      VLC, macOS and iOS
WebRTC  http://<host>:8989/misrc-preview      Safari, macOS and iOS
HLS     http://<host>:8988/misrc-preview      desktop browsers (hls.js)
```

## Bundling mediamtx

mediamtx ships **inside the Linux AppImage**, for both arches the workflow builds
(`x86_64` and `arm64`). The macOS `.dmg` and Windows `.exe` keep resolving from settings or
`PATH`: the stream host is `workflow-master`, and the macOS and iOS devices are viewers, not
servers.

- CI fetches the pinned mediamtx release per arch and **verifies its checksum** before it
  goes into `AppDir/usr/bin/`. An unverified download baked into a release artifact is not
  acceptable, and pinning stops a silent upstream change altering behaviour between builds.
- The version is pinned in one place and surfaced in the UI next to the binary path, so a
  bug report says which mediamtx it was.
- mediamtx is MIT-licensed. Its licence text ships in the AppImage alongside the existing
  `LICENSE.*` files, and the bundled version is recorded there too.
- Cost: roughly 54 MB per AppImage arch. Accepted deliberately — a self-contained artifact
  that streams with no install step is worth more than the download saving.

## UI and settings

A **Stream** section in the existing Monitor & Control surface, next to the reference
recording controls, since it is the same source and the same mental model:

- Enable/disable toggle, with the three reader-facing URLs shown and selectable for copying.
- Live status: state, encode fps, bitrate, frames dropped, audio active — the same vocabulary
  the recording panel already uses. When audio is off, the reason is shown, not just the
  absence.
- Settings: mediamtx binary path (blank = bundled or `PATH`), bind loopback vs LAN, RTSP
  port, encoder (auto / NVENC / software), bitrate, deinterlace on/off, audio device
  (blank = auto-detect from the video device's USB path).

Persisted through `gui_settings.c` alongside the existing video-record settings.

## Failure modes

Worth being explicit, because capture-node's operational history says these are the ones that
actually happen:

| Failure | Detection | Behaviour |
|---|---|---|
| `mediamtx` not installed | resolution at probe time | feature greyed out with a reason; app unaffected |
| mediamtx dies mid-stream | 1 Hz child poll | status shows error; publisher will fail next |
| RTSP/HLS/WebRTC port already bound | mediamtx exits immediately on start | surfaced as start error naming the port |
| ffmpeg exits at startup | 1 s post-spawn `waitpid` probe, capture-node's trick | classify stderr (`Connection refused`, `CUDA_ERROR_NO_DEVICE`, `Invalid argument`) into a readable message rather than "ffmpeg failed" |
| NVENC unavailable at runtime | same classification | fall back to `libx264` once, then report |
| **ALSA device absent** | resolution returns nothing | stream starts **video-only**; `audio_note` says why |
| **ALSA device busy** | ffmpeg stderr classified | stream restarts once video-only rather than failing outright |
| **ALSA card index moved** | resolution is by USB path, not index | no user-visible effect; this is why indices are never stored |
| Stream stalls / viewer wedges | ring fills | frames dropped, counter climbs, **capture unaffected** |
| Preview device unplugged | existing preview state machine | stream stops with the preview; no separate handling |

The last row is the important one, and it is why this design leans so hard on the existing
tap: there is exactly one place that owns the device and one place that decides it is gone.

Explicitly **not** included, matching capture-node: no publisher auto-restart. A dead
publisher stays dead until the operator restarts it. Auto-restart against a genuinely broken
device is a way to burn CPU and fill logs, and capture-node's comment on this ("the operator
decides via the GUI") has held up. The one exception is the single video-only retry on an
audio failure above, which is a degradation rather than a restart loop.

## Implementation phases

1. **`gui_preview_tap_mux`** + migrate `gui_video_record` onto it. No behaviour change;
   verifiable with the existing `--video-record-test` harness and a second dummy consumer.
2. **`gui_mediamtx`** supervisor + generated config. Verifiable headless: start it, assert
   all six ports are bound (three TCP listening, three UDP unconnected) and
   `8554`/`18554`/`28554` are untouched, stop it, assert the ports are released and no
   mediamtx child survives.
3. **`gui_rtsp_stream`** publisher (video + Opus audio) against a manually started mediamtx,
   with a `--rtsp-stream-test <dev> <seconds>` headless harness mirroring
   `gui_video_record_test_main()`. Includes the USB-path audio device resolver, which is
   unit-testable against fixture `/proc/asound/cards` content.
4. **Wire the two together** + fault injection (`kill` / `hang` / `bad-args` / `no-audio` /
   `busy-audio`), mirroring `vr_inject_t`, which is the pattern this codebase already uses
   for exactly these paths.
5. **CI bundling**: fetch + checksum mediamtx per AppImage arch, ship its licence, surface
   the pinned version. Guard test asserts the checksum step exists and the version is pinned.
6. **UI + settings.**
7. **Concurrency proof**: recording and streaming simultaneously for a sustained run, with RF
   capture live, asserting zero RF underruns and the recorder's frame counters unchanged
   versus a streaming-off baseline. This is the acceptance test for the whole feature —
   everything else is plumbing.

## Risks

- **Opus and iOS native Safari HLS.** Opus is the only codec MediaMTX serves to both WebRTC
  and HLS without transcoding, but Apple's native HLS player expects AAC, so an iPhone
  opening the HLS URL directly in Safari may get no audio. This is accepted: on iOS, VLC
  (RTSP) and Safari (WebRTC) both work, and neither goes through native HLS. If it ever does
  bite, the fix is MediaMTX's documented `runOnDemand` pattern — a second path that
  republishes with `-c:v copy -c:a aac`, roughly six lines of config and no code.
- **ALSA card index drift.** Card numbering changes across reboots and replugs, so a stored
  `hw:N,0` would break silently and intermittently — the worst failure shape. Resolution is
  by USB topology, and no index is ever persisted. Worth a guard test, since machine-specific
  values that dirty behaviour are a class this repo already guards
  (`check_no_tracked_generated_dirty_sources`).
- **AppImage size.** +54 MB per arch is a real cost paid by every download, including users
  who never stream. Accepted; revisit if release size becomes a complaint.

## Resolved decisions

Recorded because each one changed real work, and the reasoning matters more than the answer.

1. **Loopback or LAN by default?** → **Loopback, with one setting to open it to the LAN.**
   Streaming off-machine is an explicit act. capture-node binds `*` with no auth, so LAN mode
   matches house norms, but the default should not.
2. **Audio in the stream?** → **Yes, in v1, from the dongle's own ALSA node.** This was the
   decision that could have reshaped the architecture; it did not, because the dongle's audio
   is a separate device ffmpeg opens for itself.
3. **Reference recording on a tee instead of the mux?** → **Mux.** Closed by decision 2: with
   audio arriving out-of-band, the tee buys nothing and still welds the two lifecycles
   together. The MKV stays video-only (`gui_video_record.c:423`), which also avoids ALSA
   contention, since `hw:` devices are exclusive.
4. **Ports** → **mediamtx canonical + 100** across all six listeners.
5. **Bundle mediamtx in the AppImage?** → **Yes, Linux AppImage only**, both arches, fetched
   and checksummed in CI. macOS and Windows resolve from `PATH`.
6. **Which browser protocol?** → **Both WebRTC and HLS**, alongside RTSP for VLC.
7. **Audio codec, given WebRTC needs Opus and HLS prefers AAC?** → **Opus, single publish.**
   The only codec serving both without transcoding; see [Risks](#risks) for the accepted gap.
