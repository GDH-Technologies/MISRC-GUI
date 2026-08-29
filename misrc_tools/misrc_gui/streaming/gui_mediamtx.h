/*
 * MISRC GUI - mediamtx supervisor
 *
 * Runs a private mediamtx as a child of misrc_gui, so the RTSP publisher has
 * somewhere to publish and viewers have somewhere to read. Modelled on
 * capture-node's ManagedRtspServer, with one deliberate improvement: that one
 * has no health check, so a mediamtx that dies mid-run is simply gone until the
 * daemon restarts. This polls the child and surfaces the state.
 *
 * The hard requirement this module carries: capture-node runs three mediamtx
 * instances on this same host, and its port scheme is `tier x 10000 +
 * 8554/8000/8001`. We must stay out of that namespace entirely, which is why
 * every port is mediamtx canonical + 100 and every listener we do not serve is
 * explicitly disabled -- mediamtx defaults rtmp/srt/moq to ON.
 *
 * The config is GENERATED at runtime rather than shipped, so the ports in the
 * file are always the ports the app actually chose; the two cannot drift.
 *
 * If the binary is absent the feature is simply unavailable and the UI says so.
 * This is a desktop app: it must still run without mediamtx installed, unlike
 * capture-node which hard-fails its startup preflight.
 */

#ifndef GUI_MEDIAMTX_H
#define GUI_MEDIAMTX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* false: bind 127.0.0.1, reachable only on this machine.
     * true:  bind every interface, so anyone on the LAN can WATCH. Reading is
     *        deliberately open (a password is opt-in, see the settings);
     *        PUBLISHING is restricted to loopback in both modes, because our
     *        own ffmpeg is the only thing that should ever own this path. */
    bool     lan;
    uint16_t rtsp;          /* TCP */
    uint16_t rtp;           /* UDP */
    uint16_t rtcp;          /* UDP */
    uint16_t hls;           /* TCP */
    uint16_t webrtc_http;   /* TCP */
    uint16_t webrtc_ice;    /* UDP */
    /* Prometheus metrics. ALWAYS bound to loopback, in both bind modes: it is
     * how the panel learns the stream's real bitrate, and it is nobody else's
     * business. mediamtx canonical 9998 + 100. */
    uint16_t metrics;       /* TCP, loopback only */
    /* Credential viewers must present to READ the stream. Empty means the
     * stream is open, which is the default: a password is opt-in. Publishing
     * is never affected -- that is restricted by address, not by secret. */
    char     read_password[40];
} gui_mediamtx_config_t;

typedef struct {
    bool running;
    bool available;             /* a usable binary was resolved */
    char err_text[192];
    char binary_path[512];
    char config_path[512];
    int  child_pid;
} gui_mediamtx_status_t;

/* mediamtx canonical ports + 100, loopback. See the header comment. */
gui_mediamtx_config_t gui_mediamtx_default_config(void);

/* Render the mediamtx.yml body into `out`.
 * Returns bytes written excluding the NUL, or -1 if `cap` is too small --
 * never a truncated config, which would start mediamtx on its own defaults
 * with every listener we meant to disable. */
int gui_mediamtx_render_config(const gui_mediamtx_config_t *cfg, char *out, size_t cap);

/* Explicit binary path; "" restores automatic resolution (bundled, then PATH).
 * Invalidates the probe cache. */
void gui_mediamtx_set_binary_path(const char *path);
bool gui_mediamtx_probe(void);
const char *gui_mediamtx_binary_path(void);   /* "" when not found */
const char *gui_mediamtx_version(void);

/* Writes the config and spawns the child. Idempotent: a no-op if already
 * running. On failure returns non-zero and fills err with a displayable
 * message. */
int  gui_mediamtx_start(const gui_mediamtx_config_t *cfg, char *err, size_t err_cap);

/* SIGTERM -> 3s -> SIGKILL -> 2s, capture-node's ladder. Blocks. */
void gui_mediamtx_stop(void);

/* Last-resort teardown for app exit. */
void gui_mediamtx_shutdown(void);

/* Cheap child liveness check; call about once a second from the render thread
 * so a server that died is visible rather than mysterious. */
void gui_mediamtx_poll(void);

/* The stream's real published bitrate, in kbit/s, sampled from mediamtx's
 * loopback metrics endpoint every couple of seconds. 0 means not known yet:
 * no stream, or no sample taken since it started.
 *
 * This is the only place the number exists. ffmpeg's RTSP muxer reports
 * total_size and bitrate as N/A, and /proc/<pid>/io counts only write(2), not
 * the send(2) family a socket uses -- both measured, not assumed. */
uint32_t gui_mediamtx_stream_kbps(void);

/* The credential the RUNNING server is enforcing, or "" when the stream is
 * open. Read from here rather than from the settings so the panel can never
 * show a password that is not the one actually in force. */
const char *gui_mediamtx_read_password(void);

gui_mediamtx_status_t gui_mediamtx_get_status(void);
bool gui_mediamtx_is_running(void);

/* Headless harness: starts the server, asserts our ports are bound and
 * capture-node's are untouched, stops it, asserts they are released. */
int gui_mediamtx_test_main(int seconds);

#endif /* GUI_MEDIAMTX_H */
