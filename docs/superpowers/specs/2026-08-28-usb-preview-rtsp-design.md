# RTSP Streaming of the USB Preview — Design

Date: 2026-08-28
Repo: GDH-Technologies/MISRC-GUI (misrc_tools vendored at `misrc_tools/`)
Status: **proposed** — not yet approved. Written against capture-node as the reference
implementation, at Reece's request. Open decisions are collected at the end.

## Goal

Publish the USB dongle's preview picture as an RTSP stream so the tape being captured can
be watched from another machine, the way capture-node already publishes `VCR0` / `CAM0` /
`CAM1`. The stream is for monitoring only: the RF FLACs remain the archival master and the
MKV reference recording remains the QC artifact.

Hard requirement from the outset: **the mediamtx instance must not interfere with
capture-node's.** capture-node runs three mediamtx processes on this very host
(`workflow-master`), one per environment tier, and its port scheme is pinned by a test in
that repo (`python/tests/config/test_committed_env_seeds.py`). MISRC GUI must stay
entirely out of that namespace.

## Background: how capture-node does it, and why we cannot copy it verbatim

capture-node's pipeline is:

```
/dev/vcr0-ref-video --(-f v4l2)--> ffmpeg --(-f tee)--> [f=null]
                                                     \-> rtsp://127.0.0.1:8554/VCR0 --> mediamtx
```

ffmpeg opens the V4L2 device **itself**. mediamtx is spawned as a child of the FastAPI app
(`python/capture_node/core/rtsp_server.py`, `ManagedRtspServer`) with a per-env config
file, and every listener except RTSP is explicitly disabled.

We cannot reuse that shape, for one structural reason: **MISRC GUI already owns the V4L2
device exclusively.** `gui_preview_v4l2.c` is a singleton reader — one device, one stream —
and a V4L2 capture node will not admit a second streaming user. Pointing an ffmpeg
`-f v4l2` at `/dev/videoN` while the preview panel is live returns `EBUSY`.

What we have instead is better: `gui_preview_v4l2.h` already exposes a **frame tap** that
hands every good frame's raw YUYV to a callback on the capture thread, before the RGBA
conversion, and a **hold refcount** (`gui_preview_hold_acquire/release`) that keeps the
device open independently of whether any panel is showing. `gui_video_record.c` is already
built on exactly these two hooks:

```
preview capture thread --tap--> bounded ring --> writer thread --> ffmpeg --> mkv
     (never blocks)                                  (send)
```

The RTSP publisher is the same shape with a different ffmpeg argv and an RTSP URL where
the file path was. That is the whole feature.

## Architecture

```
                                      ┌─> ring ─> writer ─> ffmpeg ─> file.mkv
preview capture thread --tap--> mux --┤        (gui_video_record)
     (never blocks)                   └─> ring ─> writer ─> ffmpeg ─> rtsp://127.0.0.1:8654/misrc-preview
                                               (gui_rtsp_stream)                    │
                                                                                    v
                                                                          mediamtx (child of misrc_gui)
                                                                                    │
                                                                     rtsp://<host>:8654/misrc-preview
```

Four pieces, three of them new.

### Piece 1 — `gui_preview_tap_mux` (new, small)

`gui_preview_v4l2.c` publishes the tap through a **single** atomic slot:

```c
static _Atomic(const preview_tap_t *) g_tap;
static atomic_int g_tap_inflight;
```

Installing a second tap silently displaces the first, so today starting an RTSP stream
would stop the reference recording from receiving frames — a silent data-loss bug, not an
error. The mux fixes that without touching the preview module's lock-free contract:

- The mux owns **one** `preview_tap_t` with static storage and installs it via the
  existing `gui_preview_tap_install()`.
- Consumers register with `gui_preview_mux_add(const preview_tap_t *)` /
  `gui_preview_mux_remove(...)`, kept in a small fixed array (4 slots is generous) of
  atomic pointers.
- The mux's callback iterates the array and forwards. It installs the underlying tap on
  the first registration and removes it on the last, so the preview module sees exactly
  the same install/remove lifecycle it does now.
- `gui_preview_mux_remove()` reuses the quiescence trick already proven in
  `gui_preview_tap_remove()`: publish NULL into the slot, then spin until that slot's
  in-flight count is zero, so a consumer can free its state safely.

`gui_video_record.c` changes by two lines — `gui_preview_tap_install(&vr_tap)` becomes
`gui_preview_mux_add(&vr_tap)`, and likewise for removal. Nothing else about it moves.

> Alternative considered and rejected: one ffmpeg with `-f tee` writing both the MKV and
> the RTSP branch, which is exactly what capture-node does
> (`core/reference_capture.py:362-397`). It avoids the mux entirely, but it welds the two
> lifecycles together — you could not start or stop streaming without restarting the
> recording, and stopping a recording would drop the stream. capture-node gets away with
> it because a "session" there *is* the unit of work. Here, streaming and recording are
> independently useful, and the mux is ~80 lines.

### Piece 2 — `gui_rtsp_stream.c` / `.h` (new, modelled on `gui_video_record.c`)

Deliberately a sibling of the recorder, not a generalization of it. The two differ in
enough places (encoder choice, drop policy under stall, restart-on-failure, no trailer to
flush) that merging them would leave a module with two personalities.

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
    int      child_pid;
    char     url[256];           /* the reader-facing URL, not the loopback publish one */
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
requires, and `pitch` comes from `gui_preview_negotiated_pitch()` — the preview header
warns that it may exceed `width*2`, and a consumer that assumes otherwise shears the
picture. The RTSP module must handle it the same way the recorder does.

The module takes `gui_preview_hold_acquire()` for the life of the stream, so closing the
preview panel does not silently end the broadcast.

**Ring and drop policy.** Same bounded ring as the recorder, same structural guarantee:
if ffmpeg stalls, the socket fills, then the ring fills, then frames are dropped, and
`VIDIOC_QBUF` is never delayed. Note this is why we do **not** need capture-node's
`-f tee -use_fifo 1 [f=null]|...` trick — that exists purely to stop a wedged RTSP server
back-pressuring a `-f v4l2` input. Our ring already provides that property upstream of
ffmpeg, so the RTSP output can be a plain `-f rtsp`. One less moving part.

Where the two modules genuinely differ: the recorder must not drop frames if it can help
it (it dupes to keep the timeline honest), whereas the streamer should always prefer the
freshest frame. Under sustained ring pressure the streamer discards oldest, the recorder
does not.

### Piece 3 — the ffmpeg publisher

Encoder selection mirrors `gui_video_record_probe()`: ask the resolved ffmpeg binary what
it has (`-encoders`), cache the answer, prefer hardware. On this host both are present
(`h264_nvenc` on the RTX 3080, plus `libx264`).

Preferred, NVENC — capture-node's low-latency preview settings
(`python/capture_node/core/usb_stream.py:565-640`):

```
ffmpeg -hide_banner -nostdin -nostats -loglevel warning
  -f rawvideo -pixel_format yuyv422 -video_size <WxH> -framerate <num/den> -i -
  -an -sn
  -vf format=nv12
  -c:v h264_nvenc -preset p2 -tune ull -rc vbr -cq 0 -b:v 0
  -g <fps> -bf 0 -forced-idr 1 -zerolatency 1
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
  (`rf_capture_models/usb_stream.py:105`) while its fleet default is `udp`; TCP is the
  right default here because we are publishing over loopback where TCP costs nothing and
  never fragments. Readers can still pull over UDP.
- `-g <fps>` (a 1-second GOP) and `-bf 0` keep join latency low for a new viewer.
- `-threads 2` on the software path only: "spare cores belong to the RF recorders" is as
  true here as it is in capture-node, and more so — this app *is* the RF recorder.
- `-an -sn`: the preview is video-only. MISRC GUI's audio lives on a separate path
  (`gui_audio.c`) and is not frame-synchronous with the dongle. See open decisions.
- No deinterlacer by default. The source is 480i/576i off a VCR, so `bwdif` would help a
  remote viewer, but it costs latency and CPU and the local preview panel does not
  deinterlace either. Offer it as a setting, default off.

The publisher process is spawned with the same `nice(+5)` posture capture-node uses for
its ffmpeg children, for the same reason: RTSP fan-out must yield to RF ingest.

### Piece 4 — `gui_mediamtx.c` / `.h` (new, modelled on `ManagedRtspServer`)

Spawn mediamtx as a child of `misrc_gui`, kill it on exit. ~120 lines, the same size as
capture-node's supervisor.

- **Binary resolution** mirrors `gui_video_record_set_ffmpeg_path()` /
  `gui_video_record_probe()`: an explicit path from settings, else `mediamtx` on `PATH`.
  If it is absent, the feature is simply unavailable and the UI says so — no hard failure,
  unlike capture-node which hard-fails startup preflight. MISRC GUI is a desktop app and
  must still run without it. (Not bundled into the AppImage: the binary is 54 MB, which is
  a lot of AppImage for an optional monitoring feature.)
- **Config is generated at runtime**, not shipped, into
  `$XDG_RUNTIME_DIR/misrc-gui/mediamtx.yml` (falling back to `$TMPDIR`). Generating it
  guarantees the ports in the file are the ports the app actually chose, so the two can
  never drift, and `$XDG_RUNTIME_DIR` is cleaned on reboot.
- **Lifecycle**: start before the first publisher, stop on app exit
  (`SIGTERM` → wait 3 s → `SIGKILL` → wait 2 s, capture-node's exact ladder). Idempotent
  if already alive.
- Improvement over the reference: capture-node has **no health check and no restart** for
  mediamtx — if it dies mid-run it is simply gone until the daemon restarts. Since we are
  writing this fresh, poll the child once a second and surface `running`/`last_error` in
  the status panel, so a dead server is visible rather than mysterious.

## The mediamtx config

### Ports — verified free on `workflow-master`

capture-node's live listeners on this host, read from `ss -lnp` against the three running
mediamtx PIDs:

| tier | RTSP (TCP) | RTP (UDP) | RTCP (UDP) |
|---|---|---|---|
| prod | 8554 | 8000 | 8001 |
| preview | 18554 | 18000 | 18001 |
| dev | 28554 | 28000 | 28001 |

Its scheme is `tier_digit × 10000 + canonical`, canonical being `8554 / 8000 / 8001`. So
every future tier capture-node might add is `N8554 / N8000 / N8001`. Choosing a **different
canonical base** therefore avoids not just today's ports but every port that scheme can
ever produce.

Proposed for MISRC GUI: **RTSP `8654`, RTP `8100`, RTCP `8101`.** All three are free on
this host today, none can collide with capture-node's scheme at any tier, and the `+100`
offset from its canonical base keeps the relationship readable.

Every other listener is disabled. This is load-bearing, not tidiness: mediamtx defaults
`rtmp`/`hls`/`webrtc`/`srt`/`moq` to **yes**, so omitting them would have MISRC GUI quietly
claim 1935, 8888, 8889, 8189, 8890 and 8892 — ports capture-node deliberately leaves free
and might want later.

### Generated file

```yaml
# Generated by MISRC GUI - do not edit; rewritten on every launch.
#
# Ports deliberately avoid capture-node's scheme (tier x 10000 + 8554/8000/8001).
# Every non-RTSP listener is off: mediamtx defaults rtmp/hls/webrtc/srt/moq to
# yes, and claiming those ports would collide with a service we do not own.

rtsp: yes
rtspAddress: 127.0.0.1:8654
rtpAddress: 127.0.0.1:8100
rtcpAddress: 127.0.0.1:8101
rtspTransports: [udp, tcp]
rtspEncryption: "no"

readTimeout: 10s
writeTimeout: 10s
writeQueueSize: 512

rtmp: no
hls: no
webrtc: no
srt: no
moq: no

# api/metrics/pprof/playback default to no; left unset so we bind nothing on
# 9996-9999.

paths:
  misrc-preview:
    source: publisher
    sourceOnDemand: no
    overridePublisher: yes
    maxReaders: 0
```

`127.0.0.1` is the **default**, not the only option — see open decisions. Bound to
loopback the stream is reachable only on this machine; a "share on LAN" setting swaps the
three addresses to `:8654` / `:8100` / `:8101`.

Path name `misrc-preview` is deliberately outside capture-node's naming convention
(`VCR0`, `VCR1`, `CAM0`, `CAM1`), so the two are never confusable in a viewer's bookmarks
even if someone does eventually merge the configs.

## UI and settings

A **Stream** section in the existing Monitor & Control surface, next to the reference
recording controls, since it is the same source and the same mental model:

- Enable/disable toggle, with the reader-facing URL shown and selectable for copying.
- Live status: state, encode fps, bitrate, frames dropped — the same vocabulary the
  recording panel already uses.
- Settings: mediamtx binary path (blank = auto), bind loopback vs. LAN, RTSP port,
  encoder (auto / NVENC / software), bitrate, deinterlace on/off.

Persisted through `gui_settings.c` alongside the existing video-record settings.

## Failure modes

Worth being explicit, because capture-node's operational history says these are the ones
that actually happen:

| Failure | Detection | Behaviour |
|---|---|---|
| `mediamtx` not installed | resolution at probe time | feature greyed out with a reason; app unaffected |
| mediamtx dies mid-stream | 1 Hz child poll | status shows error; publisher will fail next |
| RTSP port already bound | mediamtx exits immediately on start | surfaced as start error naming the port |
| ffmpeg exits at startup | 1 s post-spawn `waitpid` probe, capture-node's trick | classify stderr (`Connection refused`, `CUDA_ERROR_NO_DEVICE`, `Invalid argument`) into a readable message rather than "ffmpeg failed" |
| NVENC unavailable at runtime | same classification | fall back to `libx264` once, then report |
| Stream stalls / viewer wedges | ring fills | frames dropped, counter climbs, **capture unaffected** |
| Preview device unplugged | existing preview state machine | stream stops with the preview; no separate handling |

The last row is the important one, and it is why this design leans so hard on the existing
tap: there is exactly one place that owns the device and one place that decides it is gone.

Explicitly **not** included, matching capture-node: no publisher auto-restart. A dead
publisher stays dead until the operator restarts it. Auto-restart against a genuinely
broken device is a way to burn CPU and fill logs, and capture-node's comment on this
("the operator decides via the GUI") has held up.

## Implementation phases

1. **`gui_preview_tap_mux`** + migrate `gui_video_record` onto it. No behaviour change;
   verifiable with the existing `--video-record-test` harness and a second dummy consumer.
2. **`gui_mediamtx`** supervisor + generated config. Verifiable headless: start it, assert
   `8654` is listening and `8554`/`18554`/`28554` are untouched, stop it, assert the port
   is released and no mediamtx child survives.
3. **`gui_rtsp_stream`** publisher against a manually started mediamtx, with a
   `--rtsp-stream-test <dev> <seconds>` headless harness mirroring
   `gui_video_record_test_main()`.
4. **Wire the two together** + fault injection (`kill` / `hang` / `bad-args`), mirroring
   `vr_inject_t`, which is the pattern this codebase already uses for exactly these paths.
5. **UI + settings.**
6. **Concurrency proof**: recording and streaming simultaneously for a sustained run, with
   RF capture live, asserting zero RF underruns and the recorder's frame counters
   unchanged versus a streaming-off baseline. This is the acceptance test for the whole
   feature — everything else is plumbing.

## Open decisions

These need Reece's call before implementation starts; each changes real work.

1. **Loopback or LAN by default?** Loopback is the safe default, but the entire point of
   RTSP is watching from elsewhere, and capture-node binds `*` and advertises
   `rtsp://workflow-master.gdhvc.lan:8554/CAM1`. Recommendation: default loopback, one
   setting to open it to the LAN, no auth (matching capture-node, which has none).
2. **Audio in the stream?** capture-node muxes ALSA audio into its RTSP stream. MISRC GUI's
   audio path is separate from the V4L2 preview and not frame-synchronous with it, so
   adding audio means solving A/V sync that the reference recording does not currently
   solve either. Recommendation: video-only for v1.
3. **Should the reference recording move to a tee instead of the mux?** If the answer to
   the sync question above ever becomes "yes, and it must match the MKV exactly", then
   capture-node's single-ffmpeg tee becomes the right shape after all, and the mux is
   wasted work. Recommendation: mux now, revisit only if audio lands.
4. **Ports** — `8654 / 8100 / 8101` proposed above. If capture-node might ever want a
   fourth tier at a different canonical base, worth confirming now while it is one line.
5. **Bundle mediamtx in the AppImage?** 54 MB says no; a self-contained AppImage that
   "just works" for a non-technical operator says maybe. Recommendation: no, resolve from
   `PATH`, document the install.
