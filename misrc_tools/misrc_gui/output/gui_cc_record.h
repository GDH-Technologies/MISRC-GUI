/*
 * MISRC GUI - EIA-608 closed-caption sidecar recording
 *
 * Captions are read from the capture dongle's RAW VBI node (/dev/vbiN), not
 * from the MISRC RF path and not from the preview picture. em28xx-class
 * dongles clamp video capture to 480 active lines, so line 21 never appears
 * in a frame this program could see -- but the same chip exposes the vertical
 * interval on a separate node, and that node can be open at the same time as
 * the video one. That is the whole design.
 *
 * Shape, deliberately unlike gui_video_record:
 *
 *   ffmpeg --(opens /dev/vbiN itself)--> captions.scc
 *      ^ spawn, poll, signal, reap.  No thread. No ring. No tap.
 *
 * There is nothing to feed it, so there is no path by which captions can
 * stall the capture thread or the RF writers. The node is EXCLUSIVE -- one
 * process, one open -- so a second consumer is not merely wasteful, it is an
 * EBUSY failure.
 *
 * Requires an ffmpeg providing the "v4l2vbi" input device. That is probed for
 * at runtime and never assumed: a rebuild without it makes the device vanish
 * with no other symptom, and the toggle must refuse rather than record
 * nothing.
 *
 * Two properties worth knowing before reading the implementation:
 *   - SCC is append-only text with no trailer, so a killed child leaves a
 *     VALID file truncated at the last flush. -flush_packets bounds that loss
 *     to well under a second.
 *   - the SCC muxer writes no null padding, so a tape carrying no captions
 *     produces a header-only file. That is the common case and is NOT a
 *     fault; it is also indistinguishable from a broken muxer by inspecting
 *     the file alone, which is what --cc-probe --live exists to resolve.
 */

#ifndef GUI_CC_RECORD_H
#define GUI_CC_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "Scenarist_SCC V1.0\n\n" -- a file this size carries no captions at all. */
#define CC_SCC_HEADER_BYTES 20

typedef enum {
    CC_PROBE_OK = 0,
    CC_PROBE_UNSUPPORTED,      /* not a Linux build */
    CC_PROBE_NO_FFMPEG,        /* no ffmpeg path was supplied or it is not executable */
    CC_PROBE_NO_V4L2VBI,       /* ffmpeg is present but was not built with the input device */
    CC_PROBE_NO_MUXER,         /* ffmpeg cannot write scc */
    CC_PROBE_NO_DEVICE,        /* no /dev/vbiN offering raw VBI capture */
    CC_PROBE_DEVICE_BUSY,      /* EBUSY -- the node is exclusive and someone else holds it */
    CC_PROBE_DEVICE_DENIED     /* EACCES -- not in the 'video' group */
} cc_probe_state_t;

/* Cached, and cheap enough to call from the render pass every frame: the
 * ffmpeg interrogation runs once per resolved binary and the device open at
 * most once per internal TTL. Never opens the node while our own child holds
 * it, so a live recording does not report itself as BUSY. */
cc_probe_state_t gui_cc_record_probe(void);
bool             gui_cc_record_available(void);   /* probe() == CC_PROBE_OK */

/* The sentence the UI hint row shows. File-scope storage, so it stays valid
 * for Clay's deferred text draw. */
const char *gui_cc_record_probe_hint(void);

/* The /dev/vbiN actually resolved, or "" when none was. */
const char *gui_cc_record_device(void);

/* The ffmpeg binary is resolved once, by gui_video_record. It is INJECTED
 * here rather than looked up again, for the reason given in gui_rtsp_stream.c:
 * a duplicated PATH search lets two consumers disagree about which ffmpeg they
 * are using, which is exactly the difference nobody checks when only one of
 * them misbehaves. Injecting also keeps this file free of project includes,
 * which is what lets the argv guard compile it standalone.
 * An unchanged path is a strcmp and does not disturb the cache. */
void gui_cc_record_set_ffmpeg(const char *path);

/* The video node the preview is using, so the VBI node on the SAME dongle can
 * be picked out by sysfs correlation. "" disables correlation and falls back
 * to the first usable node. */
void gui_cc_record_set_preview_device(const char *path);

/* Explicit /dev/vbiN from settings; "" restores automatic selection. */
void gui_cc_record_set_device(const char *path);

void gui_cc_record_invalidate_probe(void);

typedef struct {
    bool     running;
    bool     starting;          /* spawned; the startup verdict is still pending */
    bool     error;
    char     err_text[192];
    int      child_pid;
    char     device[64];        /* the node actually handed to ffmpeg */
    char     path[512];         /* the .scc being written */
    uint64_t output_bytes;      /* stat(), 1 Hz */
    uint32_t caption_lines;     /* data lines in the finished file */
    bool     captions_seen;     /* grew past the SCC header */
    double   start_offset_s;    /* spawn completed minus recording start */
} gui_cc_record_status_t;

/* Spawns ffmpeg. Copies its arguments, so the caller's may be stack
 * temporaries. Returns non-zero with a displayable err on refusal. Takes no
 * preview hold: the VBI node is independent of /dev/videoN by design. */
int  gui_cc_record_start(const char *device, const char *out_path,
                         char *err, size_t err_cap);

/* Drive from the frame loop, beside gui_rtsp_stream_poll(). One
 * waitpid(WNOHANG) plus a 1 Hz stat(); resolves the deferred startup verdict.
 * A headless harness must call it too. */
void gui_cc_record_poll(void);

/* SIGINT -- ffmpeg's graceful quit, which flushes and closes the file.
 * Returns immediately, so it is safe on the render thread. Call it the moment
 * the user stops, so the sidecar ends when the RF does rather than when
 * finalize gets round to it. */
void gui_cc_record_request_stop(void);

/* Reaps with a deadline and tallies the output. Blocks; finalize thread. */
void gui_cc_record_finish(void);
void gui_cc_record_shutdown(void);

gui_cc_record_status_t gui_cc_record_get_status(void);
uint64_t gui_cc_record_output_bytes(void);
bool     gui_cc_record_is_running(void);

typedef enum {
    CC_INJECT_NONE = 0,
    CC_INJECT_KILL,          /* SIGKILL mid-capture */
    CC_INJECT_HANG,          /* SIGSTOP: it stops writing, forever */
    CC_INJECT_BAD_ARGS,      /* a muxer this ffmpeg does not have */
    CC_INJECT_BUSY_DEVICE    /* hold the node ourselves so the spawn hits EBUSY */
} cc_inject_t;
void gui_cc_record_set_inject(cc_inject_t what);

/* Test seam: builds the argv this module would exec, into caller storage, and
 * spawns nothing. Exposed so the argv guard can compile this file standalone
 * -- a guard that had to spawn ffmpeg to inspect a command line could not run
 * in CI. Returns the token count, or -1. */
int gui_cc_record_build_argv_test(const char *device, const char *out_path,
                                  char *buf, size_t buf_cap,
                                  const char *argv_out[], size_t argv_cap);

/* Headless modes. probe_main(live) runs the 3 s self-test when live is true. */
int gui_cc_record_probe_main(bool live);
int gui_cc_record_test_main(const char *device, const char *out_dir,
                            int seconds, const char *inject_spec);
int gui_cc_record_argv_dump_main(const char *device, const char *out_dir);

#endif /* GUI_CC_RECORD_H */
