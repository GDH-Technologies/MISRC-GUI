/*
 * MISRC GUI - reference video recording
 *
 * Encodes the USB preview dongle's picture to an MKV via an ffmpeg subprocess,
 * alongside the RF and audio outputs. The RF remains the archival master; this
 * is a reference for QC -- framing, tracking, tape condition.
 *
 * Shape:
 *
 *   preview capture thread --tap--> bounded ring --> writer thread --> ffmpeg
 *        (never blocks)                                 (send)          (mkv)
 *
 * The capture thread's only cost is a memcpy and two atomics, and its only
 * failure mode is "ring full", which is a compare-and-return. If ffmpeg hangs
 * the socket fills, then the ring fills, and frames are dropped -- VIDIOC_QBUF
 * is never delayed. That property is structural, not a matter of timing.
 */

#ifndef GUI_VIDEO_RECORD_H
#define GUI_VIDEO_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/gui_settings.h"

typedef enum {
    VIDEO_CODEC_H264 = 0,   /* libx264, CRF 18 by default; ~0.3-3 MB/s */
    VIDEO_CODEC_FFV1 = 1    /* lossless; ~5-16 MB/s, a third of an RF channel */
} video_codec_t;

/* How the reference video is encoded. Every field is read when ffmpeg is
 * spawned, so a change applies to the next recording. Out-of-range values are
 * clamped by gui_video_encode_opts_clamp(), never rejected. */
typedef enum {
    VIDEO_INTERLACE_AUTO = 0,        /* SD rasters and V4L2-interlaced sources */
    VIDEO_INTERLACE_ON = 1,
    VIDEO_INTERLACE_OFF = 2
} video_interlace_t;

typedef struct {
    video_codec_t codec;
    int  chroma;          /* 420 or 422 */
    int  crf;             /* H.264 only: 0-51 */
    char preset[16];      /* H.264 only: one of gui_video_h264_presets */
    int  keyint;          /* H.264 only: frames between keyframes, 1-300 */
    int  interlace;       /* video_interlace_t: halves 4:2:0 chroma per field */
} video_encode_opts_t;

/* The defaults: H.264 4:2:0 (a GPU decodes it; High 4:2:2 has no common
 * hardware decoder and VLC 3.0 then shows no picture), CRF 18, veryfast,
 * keyframe 25; FFV1 4:2:2 (lossless, the dongle's full chroma). */
#define VIDEO_H264_CHROMA_DEFAULT 420
#define VIDEO_FFV1_CHROMA_DEFAULT 422
#define VIDEO_H264_CRF_DEFAULT    18
#define VIDEO_H264_PRESET_DEFAULT "veryfast"
#define VIDEO_H264_KEYINT_DEFAULT 25

/* The x264 presets offered, fastest first. Slower ones than medium cost more
 * than the encoder's two threads are worth beside an RF capture. The settings
 * table validates against the same list. */
#define VIDEO_H264_PRESET_LIST "ultrafast", "superfast", "veryfast", "faster", "fast", "medium"
extern const char *const gui_video_h264_presets[];
extern const int gui_video_h264_preset_count;

void gui_video_encode_opts_default(video_encode_opts_t *o, video_codec_t codec);
void gui_video_encode_opts_clamp(video_encode_opts_t *o);
/* The options the settings select: usbref_codec, that codec's chroma, and
 * the H.264 and interlace fields. Clamped. */
void gui_video_encode_opts_from_settings(const gui_settings_t *s, video_encode_opts_t *o);
/* "H.264 4:2:0 CRF 18 veryfast, keyframe 25, interlace auto" */
void gui_video_encode_summary(const video_encode_opts_t *o, char *buf, size_t cap);

typedef struct {
    bool     running;
    bool     error;
    char     err_text[192];
    uint64_t frames_submitted;   /* handed over by the capture thread */
    uint64_t frames_dropped;     /* ring was full */
    uint64_t frames_duped;       /* re-sent to keep the timeline honest */
    uint64_t frames_skipped;     /* arrived a slot early: the timeline had no room */
    uint64_t frames_written;     /* actually sent to ffmpeg */
    /* The source's real frame rate against the nominal, from the frames'
     * kernel timestamps: a camcorder's servo crystal, plus any fields the
     * dongle lost. Informational; the timeline already follows it. */
    double   source_rate_ppm;
    /* The dongle's audio, recorded as a FLAC track in the same MKV. */
    bool     audio_active;
    char     audio_device[96];   /* what ffmpeg opened, when audio_active */
    char     audio_note[256];    /* why there is no audio, when not */
    uint64_t output_bytes;       /* stat() of the mkv, 1 Hz */
    int      child_pid;
    char     path[512];
} gui_video_record_status_t;

/* Resolve the ffmpeg binary and ask it what it can encode. Cached; safe to
 * call from UI code. Returns false if no usable ffmpeg was found. */
/* Explicit ffmpeg path from settings; "" restores automatic resolution.
 * Invalidates the probe cache. */
void gui_video_record_set_ffmpeg_path(const char *path);
bool gui_video_record_probe(void);
const char *gui_video_record_ffmpeg_path(void);     /* "" when not found */
const char *gui_video_record_ffmpeg_version(void);

/* The ffmpeg the caption recorder (gui_cc_record) is handed. It needs the
 * v4l2vbi input device, which stock ffmpeg lacks, so the build carrying it is
 * installed BESIDE the distro's ffmpeg under GUI_CC_FFMPEG_NAME rather than in
 * place of it: a private build on PATH as plain `ffmpeg` breaks every caller
 * the moment an OS upgrade bumps a soname under it (Fedora 45 did).
 *
 * Order: the explicit ffmpeg_path setting, then MISRC_FFMPEG -- either governs
 * captions and video alike -- then GUI_CC_FFMPEG_NAME on PATH, then the video
 * recorder's own binary. So with no override, captions use the v4l2vbi build
 * and video and RTSP use the distro's ffmpeg. Cached; re-resolved after
 * gui_video_record_set_ffmpeg_path(). "" when nothing was found. */
#define GUI_CC_FFMPEG_NAME "ffmpeg-vbi"
const char *gui_video_record_cc_ffmpeg_path(void);
bool gui_video_record_codec_available(video_codec_t codec);

/* Spawns ffmpeg, starts the writer thread, installs the preview tap.
 * Geometry and rate must be the NEGOTIATED values, not the requested ones.
 *
 * audio_device: an ALSA capture device for ffmpeg to open itself (the dongle's
 * own, from gui_alsa_resolve_for_video_device), recorded as a FLAC track; ""
 * or NULL records video-only. If ffmpeg cannot open it, the recording goes on
 * video-only and the status says why -- reference audio is never a reason to
 * lose the reference video, let alone the RF.
 *
 * Timing: both tracks are placed by when they ARRIVED, on one origin taken at
 * spawn. Frames come from the tap's kernel timestamps onto a fixed-rate
 * timeline (repeat or skip at most when a whole frame off); audio comes from
 * ffmpeg's ALSA arrival stamps, kept on them by aresample. Counting frames
 * would not do: the camcorder's crystal sets the frame rate and the dongle's
 * the sample rate (measured on wm, 2026-11-17: a Video8 camcorder on the
 * StarTech's S-Video at +61 ppm, the StarTech's audio at +22 ppm), and a
 * dongle that loses sync drops whole fields (the same StarTech fed a blanked
 * composite input delivered 0.6% fewer frames than nominal).
 *
 * enc: NULL records with the H.264 defaults.
 *
 * On failure returns non-zero and fills err with a displayable message. */
int  gui_video_record_start(const char *out_path, const video_encode_opts_t *enc,
                            uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t fps_num, uint32_t fps_den,
                            const char *audio_device,
                            char *err, size_t err_cap);

/* Removes the tap and asks the writer to drain. Does not block on the child;
 * safe to call from the render thread. */
void gui_video_record_request_stop(void);

/* Joins the writer (which closes the socket, so ffmpeg flushes and writes a
 * proper trailer) and reaps the child with a deadline. Blocks; call from the
 * finalize thread. */
void gui_video_record_finish(void);

/* Last-resort teardown for app exit. */
void gui_video_record_shutdown(void);

gui_video_record_status_t gui_video_record_get_status(void);
uint64_t gui_video_record_output_bytes(void);
bool     gui_video_record_is_running(void);

/* Fault injection, test builds only: exercises the paths that matter most and
 * are hardest to reach by hand. */
typedef enum {
    VR_INJECT_NONE = 0,
    VR_INJECT_KILL,      /* SIGKILL ffmpeg mid-stream */
    VR_INJECT_HANG,      /* SIGSTOP ffmpeg so it stops reading, forever */
    VR_INJECT_BAD_ARGS   /* spawn ffmpeg with a codec it cannot honour */
} vr_inject_t;
void gui_video_record_set_inject(vr_inject_t what);

/* Headless harness. codec_name: "h264" (or "x264") or "ffv1", optionally
 * followed by ",key=value" options -- chroma=420|422, crf=N, preset=P,
 * keyint=N, interlace=auto|on|off (crf/preset/keyint are H.264-only) -- and
 * then ":kill@N" / ":hang@N" / ":bad-args" fault injection. An unknown codec
 * or key lists the accepted ones and returns 2. The finished MKV's pixel
 * format (and H.264 profile) must match the options. audio_mode: NULL or "noaudio" = video only, "audio" =
 * the device's own ALSA card; anything else lists the accepted values and
 * returns 2. With audio, the MKV is ffprobed: one FLAC stream, and both
 * streams must start and end within one frame of each other. */
int gui_video_record_test_main(const char *device, const char *out_path,
                               int seconds, const char *codec_name,
                               const char *audio_mode);
/* The fixed-rate timeline's placement rule against synthetic timestamps:
 * crystal drift, USB jitter, lost fields and a burst. No device needed. */
int gui_video_record_timeline_selftest_main(void);
/* The ffmpeg argv for a matrix of encode options: pixel format, profile, the
 * per-field chroma filter, CRF/preset/keyframe, zerolatency only with audio,
 * clamping. No device needed; Linux only (elsewhere it reports a skip). */
int gui_video_encode_selftest_main(void);
int gui_video_record_probe_main(void);
/* Prints what both auto-name functions produce for the same settings, so the
 * pair cannot silently drift. */
int gui_video_record_name_test_main(void);

#endif /* GUI_VIDEO_RECORD_H */
