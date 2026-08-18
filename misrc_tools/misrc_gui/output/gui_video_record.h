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

typedef enum {
    VIDEO_CODEC_H264 = 0,   /* libx264 CRF 18; ~0.3-3 MB/s */
    VIDEO_CODEC_FFV1 = 1    /* lossless; ~5-16 MB/s, a third of an RF channel */
} video_codec_t;

typedef struct {
    bool     running;
    bool     error;
    char     err_text[192];
    uint64_t frames_submitted;   /* handed over by the capture thread */
    uint64_t frames_dropped;     /* ring was full */
    uint64_t frames_duped;       /* re-sent to keep the timeline honest */
    uint64_t frames_written;     /* actually sent to ffmpeg */
    uint64_t output_bytes;       /* stat() of the mkv, 1 Hz */
    int      child_pid;
    char     path[512];
} gui_video_record_status_t;

/* Resolve the ffmpeg binary and ask it what it can encode. Cached; safe to
 * call from UI code. Returns false if no usable ffmpeg was found. */
bool gui_video_record_probe(void);
const char *gui_video_record_ffmpeg_path(void);     /* "" when not found */
const char *gui_video_record_ffmpeg_version(void);
bool gui_video_record_codec_available(video_codec_t codec);

/* Spawns ffmpeg, starts the writer thread, installs the preview tap.
 * Geometry and rate must be the NEGOTIATED values, not the requested ones.
 * On failure returns non-zero and fills err with a displayable message. */
int  gui_video_record_start(const char *out_path, video_codec_t codec,
                            uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t fps_num, uint32_t fps_den,
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

/* Headless harness. */
int gui_video_record_test_main(const char *device, const char *out_path,
                               int seconds, const char *codec_name);
int gui_video_record_probe_main(void);

#endif /* GUI_VIDEO_RECORD_H */
