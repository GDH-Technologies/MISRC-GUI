/*
 * MISRC GUI - RTSP publisher for the USB preview
 *
 * Publishes the dongle's picture and audio to the private mediamtx
 * (gui_mediamtx.h) so a tape being captured can be watched from another
 * machine. Monitoring only: the RF FLACs remain the archival master and the
 * MKV reference recording remains the QC artifact.
 *
 * Shape -- deliberately a sibling of gui_video_record.c rather than a
 * generalisation of it. They differ on encoder choice, drop policy, restart
 * behaviour and audio, and merging them would leave one module with two
 * personalities.
 *
 *   preview capture thread --tap--> bounded ring --> writer thread --+
 *        (never blocks)                                (send)        |
 *                                                                    v
 *                                        ffmpeg --+-- video: rawvideo on stdin
 *                                                 +-- audio: -f alsa (the dongle)
 *                                                     |
 *                                                     v  rtsp://127.0.0.1:PORT/misrc-preview
 *
 * Audio is NOT this module's data path. The dongle exposes its own ALSA node,
 * so ffmpeg opens it directly as a second input: the audio never enters our
 * ring, never crosses our threads, and needs no sync work, because it is the
 * same physical device on the same clock as the picture. See gui_alsa_device.h.
 *
 * If the audio device cannot be resolved the stream still starts, video-only,
 * and says why. Losing the picture because a sound card moved would be a poor
 * trade for a monitoring feed.
 */

#ifndef GUI_RTSP_STREAM_H
#define GUI_RTSP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gui_mediamtx.h"

typedef enum {
    RTSP_ENCODER_AUTO = 0,   /* hardware if the resolved ffmpeg has it */
    RTSP_ENCODER_NVENC,
    RTSP_ENCODER_SOFTWARE
} rtsp_encoder_t;

typedef struct {
    /* Geometry and rate must be the NEGOTIATED values, not the requested ones.
     * pitch comes from gui_preview_negotiated_pitch() and may exceed width*2;
     * a consumer that assumes otherwise shears the picture. */
    uint32_t width, height, pitch;
    uint32_t fps_num, fps_den;

    /* The V4L2 node supplying the picture. Used to find the audio node on the
     * same USB device; never opened for streaming here. */
    const char *video_device;
    /* Explicit ALSA device, or "" to resolve it from video_device. */
    const char *audio_device;

    rtsp_encoder_t encoder;
    uint32_t bitrate_kbps;    /* software encoder only; 0 = 2000 */
    bool     deinterlace;     /* bwdif; off by default, it costs latency */

    /* Where to publish, and what the reader-facing URLs should say. */
    gui_mediamtx_config_t ports;
    const char *reader_host;  /* "" = derive from the bind mode */
} gui_rtsp_stream_opts_t;

typedef struct {
    bool     running;
    /* Spawned, but not yet known to have survived its first frames. The
     * check is deferred to gui_rtsp_stream_poll() rather than slept for,
     * because starting a stream must not freeze the window. */
    bool     starting;
    bool     error;
    char     err_text[192];
    uint64_t frames_submitted;   /* handed over by the capture thread */
    uint64_t frames_dropped;     /* ring was full */
    uint64_t frames_written;     /* actually sent to ffmpeg */
    bool     audio_active;
    char     audio_note[96];     /* why audio is off, when it is */
    char     audio_device[96];   /* what was resolved */
    int      child_pid;
    char     url_rtsp[256];
    char     url_webrtc[256];
    char     url_hls[256];
} gui_rtsp_stream_status_t;

/* Which encoders the resolved ffmpeg actually has. Cached. */
bool gui_rtsp_stream_probe(void);
bool gui_rtsp_stream_has_nvenc(void);

/* Spawns ffmpeg, starts the writer thread, registers with the preview mux and
 * takes a preview hold so closing the panel does not end the broadcast.
 * On failure returns non-zero and fills err with a displayable message. */
int  gui_rtsp_stream_start(const gui_rtsp_stream_opts_t *opts, char *err, size_t err_cap);

/* Deregisters the tap and asks the writer to drain. Does not block on the
 * child; safe to call from the render thread. */
void gui_rtsp_stream_request_stop(void);

/* Joins the writer and reaps the child with a deadline. Blocks. */
void gui_rtsp_stream_finish(void);

/* Last-resort teardown for app exit. */
void gui_rtsp_stream_shutdown(void);

/* Completes a deferred startup check and applies the one video-only retry.
 * Cheap -- a clock read, and a waitpid(WNOHANG) only once the window elapses.
 * Call about once a frame, beside gui_mediamtx_poll(). */
void gui_rtsp_stream_poll(void);

gui_rtsp_stream_status_t gui_rtsp_stream_get_status(void);
bool gui_rtsp_stream_is_running(void);

/* Fault injection, test builds only. */
typedef enum {
    RS_INJECT_NONE = 0,
    RS_INJECT_KILL,        /* SIGKILL ffmpeg mid-stream */
    RS_INJECT_HANG,        /* SIGSTOP ffmpeg so it stops reading, forever */
    RS_INJECT_BAD_ARGS,    /* spawn ffmpeg with an encoder it cannot honour */
    RS_INJECT_NO_AUDIO,    /* pretend the ALSA device could not be resolved */
    RS_INJECT_BUSY_AUDIO   /* point at an ALSA device that cannot be opened */
} rs_inject_t;
void gui_rtsp_stream_set_inject(rs_inject_t what);

/* Headless harness: starts mediamtx, publishes for `seconds`, asserts frames
 * flowed and the stream was readable. */
int gui_rtsp_stream_test_main(const char *device, int seconds);

/* Runs the recorder and the stream against the same tap, injects one fault, and
 * asserts the documented behaviour AND that the recorder never noticed. */
int gui_rtsp_stream_fault_test_main(const char *device, const char *fault, int seconds);

/* The acceptance test: recorder + RF ingest with the stream off, then on, and a
 * comparison of the two halves of the same run. */
int gui_stream_soak_main(const char *device, const char *rf_device, int seconds);

#endif /* GUI_RTSP_STREAM_H */
