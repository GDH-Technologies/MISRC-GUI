# USB reference dongle audio in the reference MKV: design

Status: approved by Reece, 2026-11-17. Fork-only.

## Intent

For Video8/Hi8 the archival audio is decoded from the AFM carriers in the RF. The
camcorder's analog line out is also wired to the USB A/V dongle that supplies the reference
picture. Record that line-out audio as an audio stream inside the reference video MKV. It
serves as QC, and as a fallback or comparison source against the RF-decoded AFM audio. It can
be extracted later with `ffmpeg -i ref.mkv -map 0:a -c copy out.flac`. There is no separate
audio file.

## Decisions

- The reference-video ffmpeg opens the dongle's ALSA node itself. No audio code enters the
  GUI process; the RF capture path is untouched.
- Both consumers of the dongle's audio (the recording and the RTSP stream) open it through
  ALSA `dsnoop`, so the stream keeps its audio during a recording.
- One FLAC track, 48 kHz / 16-bit stereo, for both H.264 and FFV1 reference video.
- If the audio cannot be opened, record video-only and say why: the session log, the status
  line and the dialog hint. Never stop or refuse the RF capture over it.
- UI: an **Audio** row in the USB Reference Video dialog, between Picture and Stream:
  `[ ON ] Record dongle audio in the MKV` plus a hint naming the resolved device or why there
  is none. Default ON. Greyed when Reference video is OFF or no card matches the dongle.
  The automation trigger is the `usbref_audio_enabled` settings key.
- `usbref_rtsp_audio_device`, when set, overrides the device for both consumers, verbatim.

## Synchronisation

The dongle has two clocks. Its video decoder locks to the camcorder's sync, so frames arrive
at the camcorder's playback rate (its drum servo crystal). Its audio ADC runs on the dongle's
own crystal; the USB endpoint is ASYNC. Counting frames and samples therefore drifts by the
crystals' ppm difference: 50 ppm is 0.36 s over 2 h.

The fix is OBS's (`plugins/linux-v4l2/v4l2-input.c`): both tracks are timed by **arrival** on
the computer's clock, from one shared start instant, and the video output stays at a fixed
frame rate by repeating or skipping a frame.

- **Video.** Each frame carries its V4L2 kernel timestamp (CLOCK_MONOTONIC), corrected to
  the frame's start. em28xx stamps at the frame's end (no `TSTAMP_SRC` flag = EOF,
  `finish_buffer()`), so one frame period is subtracted. uvcvideo stamps at the start
  (`TSTAMP_SRC_SOE`), so its stamp is used as-is. If a driver does not stamp MONOTONIC,
  `CLOCK_MONOTONIC` is read at DQBUF. Slot `k` of the timeline is `mono0 + k·P`.
  - A frame landing ≥ 0.75 slot late is preceded by repeats of the last frame.
  - A frame landing ≥ 0.75 slot early is skipped.
  - After a correction the frame sits about 0.25 slot on the other side of its slot, half a
    slot from either limit, so USB timestamp jitter (a few ms) cannot flip-flop it.
  - Frames the dongle lost become repeats, and the slots before the first frame are filled
    with that frame.
  - ffmpeg gets `-fps_mode passthrough`, so the writer is the only place a frame is repeated
    or skipped.
- **Audio.** ffmpeg's ALSA demuxer stamps each packet with the capture time of its first
  sample, smoothed, on CLOCK_REALTIME (`av_gettime`). `-copyts` keeps those stamps (without
  it ffmpeg shifts every input to its own zero and the offset is lost), and
  `-itsoffset -<real0>` makes them relative to the spawn instant. `mono0` and `real0` are
  sampled back to back, and MONOTONIC and REALTIME are slewed by NTP alike.
  `aresample=async=96:first_pts=0` then keeps the sample count on those stamps (at most
  2000 ppm of stretch, inaudible) and pads the start, so a player that clocks audio by its
  sample count stays in sync.
- **Known limit.** A step of the realtime clock mid-recording shifts the audio by the step.
  chrony slews by default on Fedora.

## Verified against the sources

| Source | What it shows |
| --- | --- |
| `em28xx-video.c` | `TIMESTAMP_MONOTONIC`, stamped in `finish_buffer()` |
| `uvc_queue.c` | `TIMESTAMP_MONOTONIC \| TSTAMP_SRC_SOE` |
| `alsa_dec.c` | pts = timefilter(`av_gettime()` − delay) |
| `libavutil/time.c` | `av_gettime()` = `gettimeofday` |
| `ffmpeg_demux.c` | `ts_offset = itsoffset − start_time` unless `-copyts` |
| `swresample.c` | `async` sets `min_comp = 0.001` and `max_soft_comp = async/rate` |
| alsa-lib `pcm_direct.c` | IPC key = base + card/device; a second client copies the first opener's params unchecked, so the name pins `FORMAT=S16_LE,RATE=48000` |

The full argv ran on wm's Fedora ffmpeg 9.0.2 (exit 0, no warnings), and two concurrent
`dsnoop` opens of the StarTech ran.

## ffmpeg argv shape

- **Input 0, ALSA.** Listed first so a busy device fails at once, not after the first frame:
  `-f alsa -thread_queue_size 1024 -sample_rate 48000 -ch_layout stereo -itsoffset -<real0>
  -i dsnoop:CARD=<id>,DEV=0,FORMAT=S16_LE,RATE=48000`
- **Input 1, video:** the existing rawvideo stdin pipe.
- **Output options:** `-copyts -map 1:v -map 0:a -fps_mode passthrough`, the existing video
  encoder, `-af aresample=async=96:first_pts=0 -c:a flac`,
  `-metadata:s:a:0 title=Camcorder line out (USB reference)`, `-shortest` (the ALSA input
  never ends on its own), and the existing Matroska flags.
- **Audio failure:** if ffmpeg exits early and its log names the audio, the writer respawns
  it once video-only and the status carries the note, with the device's holder when one is
  found.

## Not in scope (owed)

- A WirePlumber exclusion for the reference dongles. This is host config: today PipeWire
  suspends idle nodes, so the opens work.
- The RTSP stream's own long-run A/V drift. It only gets the `dsnoop` name here.
- Interlaced field-order tagging in the MKV.
