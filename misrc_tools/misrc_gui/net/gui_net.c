/*
 * MISRC GUI - Server/Client networking implementation.
 * See gui_net.h for the protocol and thread-safety model.
 */

#include "gui_net.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../../common/buffer_manager.h"
#include "../../common/rb_event.h"
#include "../../common/threading.h"
#include "../core/gui_settings.h"
#include "../output/gui_record.h"
#include "../ui/gui_ui.h"
#include "gui_net_query.h"
#include "version.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

/* -------------------------------------------------------------------------
 * Cross-platform sockets + sync primitives
 * ------------------------------------------------------------------------- */

#ifdef _WIN32
  /* Avoid Win32 API name collisions with raylib symbols (Rectangle, CloseWindow, ShowCursor). */
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOGDI
  #define NOGDI
  #endif
  #ifndef NOUSER
  #define NOUSER
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET net_sock_t;
  #define NET_INVALID_SOCKET INVALID_SOCKET
  #define net_close(s) closesocket(s)
  #define net_errno ((int)WSAGetLastError())
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  typedef int net_sock_t;
  #define NET_INVALID_SOCKET (-1)
  #define net_close(s) close(s)
  #define net_errno errno
#endif

/* Mutex/condvar shims shared with the fanout, and the fanout itself. */
#include "gui_net_sync.h"
#include "gui_net_fanout.h"

static inline int net_sock_valid(net_sock_t s) {
#ifdef _WIN32
    return s != NET_INVALID_SOCKET;
#else
    return s >= 0;
#endif
}

/* Portable thread handle + detach. The codebase's threading.h gives thrd_t /
 * thrd_create / thrd_join, but no detach. Detached worker threads (per-client
 * HTTP handlers) need a detach so their handles don't leak. */
static void net_thread_detach(thrd_t *t) {
#ifdef _WIN32
    if (t && *t) { CloseHandle(*t); *t = (thrd_t)0; }
#else
    if (t) pthread_detach(*t);
#endif
}

/* Set a socket non-blocking / blocking. */
static int net_set_nonblocking(net_sock_t fd) {
#ifdef _WIN32
    unsigned long mode = 1;
    return (ioctlsocket(fd, /* FIONBIO */ 0x8004667CUL, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) ? 0 : -1;
#endif
}

static int net_set_blocking(net_sock_t fd) {
#ifdef _WIN32
    unsigned long mode = 0;
    return (ioctlsocket(fd, 0x8004667CUL, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0) ? 0 : -1;
#endif
}

/* Set SO_RCVTIMEO / SO_SNDTIMEO so blocking recv/send do not hang forever.
 * This is what lets the client worker/pump threads wake up to check their
 * stop flags, so thrd_join() from the UI thread returns promptly. */
static void net_set_timeouts(net_sock_t fd, int rcv_ms, int snd_ms) {
    if (!net_sock_valid(fd)) return;
#ifdef _WIN32
    DWORD rcv = (DWORD)rcv_ms, snd = (DWORD)snd_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv, sizeof(rcv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&snd, sizeof(snd));
#else
    struct timeval tv;
    tv.tv_sec = rcv_ms / 1000;
    tv.tv_usec = (rcv_ms % 1000) * 1000L;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    tv.tv_sec = snd_ms / 1000;
    tv.tv_usec = (snd_ms % 1000) * 1000L;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#endif
}

/* Send all bytes; returns 0 on success, -1 on error. */
static int net_send_all(net_sock_t fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
#ifdef _WIN32
        int n = send(fd, p, (int)len, 0);
        if (n == SOCKET_ERROR) return -1;
#else
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
#endif
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int net_send_str(net_sock_t fd, const char *s) {
    return net_send_all(fd, s, strlen(s));
}

/* Read until we have seen "\r\n\r\n" (end of HTTP headers) or buffer full.
 * Returns total bytes read into buf (including the terminator), or -1 on
 * error/EOF before headers complete. Mirrors the reference http_thread loop. */
static int net_read_headers(net_sock_t fd, char *buf, size_t cap) {
    size_t len = 0;
    if (cap < 4) return -1;
    while (len < cap - 1) {
#ifdef _WIN32
        int n = recv(fd, buf + len, (int)(cap - 1 - len), 0);
        if (n == SOCKET_ERROR || n == 0) return -1;
        len += (size_t)n;
#else
        ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return -1; }
        len += (size_t)n;
#endif
        buf[len] = '\0';
        if (strstr(buf, "\r\n\r\n")) return (int)len;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * Net state
 * ------------------------------------------------------------------------- */

/* UDP discovery: servers broadcast a beacon on this port so clients can list
 * and select them without typing host:port. */
#define NET_DISCOVERY_PORT     8091
#define NET_MAX_DISCOVERED     8
#define NET_DISCOVERY_TTL_MS   8000

typedef struct {
    char     host[64];     /* IP address string of the server */
    uint16_t port;          /* TCP control port */
    char     name[64];      /* server hostname label */
    uint64_t last_seen_ms;
} net_discovered_t;

/* One accepted connection. The listener registers it before the serving
 * thread starts and that thread releases it on exit, so server_stop() can
 * shut every live socket down and wait for the threads to finish before it
 * frees anything they touch. */
typedef struct net_client_conn {
    net_sock_t fd;
    struct net_client_conn *next;
} net_client_conn_t;

/* A queued GET /set request and the main thread's answer to it. The HTTP
 * thread validates the value against the published settings, queues the
 * request and waits (bounded) for the answer; only the main thread applies
 * it, with the same checks and side effects the settings panel has. */
#define NET_SET_QUEUE 8
#define NET_SET_WAIT_MS 1000
typedef struct {
    uint32_t seq;
    char key[64];
    char value[512];
} net_set_req_t;

typedef struct {
    uint32_t seq;
    int http;               /* 200 applied; 400/409/422 refused */
    uint32_t generation;    /* gui_settings_generation() after the attempt */
    char msg[160];          /* canonical value on 200, the reason otherwise */
} net_set_res_t;

typedef struct {
    gui_app_t *app;
    uint16_t port;
    thrd_t listen_thread;
    atomic_bool running;
    atomic_bool stop_flag;
    net_sock_t listen_fd;
    net_fanout_t rf;
    net_fanout_t audio;
    atomic_int rf_clients;
    atomic_int audio_clients;
    /* UDP discovery beacon broadcaster. */
    thrd_t beacon_thread;
    atomic_bool beacon_stop;
    /* Live client connections and the threads serving them. */
    net_mutex_t clients_mtx;
    net_client_conn_t *clients;
    atomic_int client_threads;
    /* /set requests in flight (HTTP threads enqueue, main thread answers). */
    net_mutex_t set_mtx;
    net_cond_t set_cv;
    net_set_req_t set_q[NET_SET_QUEUE];
    int set_q_count;
    net_set_res_t set_res[NET_SET_QUEUE];
    int set_res_count;
    uint32_t set_seq;
    /* What the HTTP threads publish: main-thread copies refreshed by
     * server_publish() from gui_net_poll_commands(), so no handler reads
     * app->settings or the status line while the main thread writes them. */
    net_mutex_t pub_mtx;
    gui_settings_t published;
    uint32_t published_generation;
    bool published_effective_misrc;
    char published_status[256];
    uint32_t published_status_seq;
    uint64_t published_disk_free;
    bool published_once;            /* main thread only */
    double published_disk_time;     /* main thread only */
} net_server_t;

typedef struct {
    net_server_t *srv;
    net_client_conn_t *conn;
} server_client_ctx_t;

typedef struct {
    gui_app_t *app;
    char host[128];
    uint16_t port;
    thrd_t worker_thread;
    atomic_bool running;
    atomic_bool stop_flag;
    atomic_bool connected;
    atomic_bool error;
    /* Mirror snapshot (written by worker, read by main poll). */
    atomic_int peer_state;          /* 0 idle, 1 capturing, 2 recording */
    atomic_int peer_sample_rate;    /* Hz */
    atomic_int peer_device_count;
    atomic_int peer_selected;
    atomic_int peer_audio_frame_bytes; /* for /baseband re-framing */
    /* Staged mirrored device list (applied to app->devices by main thread). */
    net_mutex_t dev_mtx;
    device_info_t staged_devices[MAX_DEVICES];
    int staged_device_count;
    int staged_selected;
    bool staged_dirty;
    /* Ingest control (main thread starts/stops; pump threads run). */
    atomic_bool ingest_want;        /* worker sets when peer is capturing */
    atomic_bool ingest_active;      /* main thread sets when ingest running */
    thrd_t rf_pump_thread;
    thrd_t audio_pump_thread;
    atomic_bool pump_stop;
    atomic_bool worker_started;   /* true once the stats/ingest worker thread is running */
    /* UDP discovery: listener thread + discovered-server list (mutex-guarded). */
    thrd_t discovery_thread;
    atomic_bool discovery_stop;
    net_mutex_t disc_mtx;
    net_discovered_t discovered[NET_MAX_DISCOVERED];
    int discovered_count;
    atomic_bool disc_dirty;
    /* Forwarded-command flags are the shared app->net_cmd_* atomics; the
     * worker drains them and sends HTTP GETs. */

    /* The server's settings, staged by the worker from /settings and applied
     * by the main thread in gui_net_poll_mirror(). Under set_mtx. */
    net_mutex_t set_mtx;
    gui_settings_t staged_settings;
    uint32_t staged_generation;
    bool staged_effective_misrc;
    bool staged_settings_valid;
    bool staged_settings_dirty;
    bool staged_controls_dirty;      /* older server: mode + swap via /controls only */
    int server_settings_support;     /* -1 unknown, 0 no /settings, 1 yes */
    uint64_t staged_settings_time_ms;
    atomic_bool settings_refresh_req;
    /* Outbound /set requests (main thread queues, worker sends) and their
     * answers (worker stores, main thread consumes). Under set_mtx. */
    net_set_req_t out_q[NET_SET_QUEUE * 2];
    int out_count;
    uint32_t out_seq;
    net_set_res_t results[NET_SET_QUEUE * 2];
    int res_count;
    /* Recording relay from /stats. */
    atomic_uint_fast64_t peer_rec_elapsed_ms;
    atomic_uint_fast64_t peer_rec_bytes;
    atomic_uint_fast64_t peer_rec_raw_a;
    atomic_uint_fast64_t peer_rec_raw_b;
    atomic_uint_fast64_t peer_rec_comp_a;
    atomic_uint_fast64_t peer_rec_comp_b;
    atomic_uint_fast64_t peer_disk_free;
    atomic_uint_fast32_t peer_rec_drops;
    atomic_bool peer_rec_pending;
    atomic_bool peer_rec_finalizing;
    char peer_status[256];           /* under set_mtx */
    uint32_t peer_status_seq;
    bool peer_status_dirty;
} net_client_t;

typedef struct {
    net_client_t *cli;
    bool is_audio;          /* false => /rf into BUF_CAPTURE_RF, true => /baseband into BUF_CAPTURE_AUDIO */
    buffer_id_t buf_id;
    int frame_bytes;        /* alignment for re-framing (4 for RF, peer_audio_frame_bytes for audio) */
} net_pump_ctx_t;

/* The single active net state (Local => NULL). Stored on app->net_state. */
static net_server_t *s_server = NULL;
static net_client_t *s_client = NULL;

/* Client view state (defined with the view functions below). */
static void client_view_reset(void);
static void client_apply_snapshot(gui_app_t *app, net_client_t *cli);
static void client_apply_set_results(gui_app_t *app, net_client_t *cli);
static void client_expire_pending(void);
static bool s_peer_valid;
static bool s_peer_controls_only;
static bool s_peer_effective_misrc;
static gui_settings_t s_peer_applied;
/* --net-client-probe mirrors without ingesting (no buffers, no extraction). */
static bool s_probe_no_ingest = false;

/* raylib's clock is only initialised by InitWindow; the headless modes have
 * no window, and a NaN elapsed time must read as 0, not as a huge number. */
static double net_now_s(void) {
    double t = GetTime();
    return isfinite(t) ? t : 0.0;
}

/* The server the buffer-manager write tap pushes into, and how many pushes
 * are in flight on the capture thread. server_stop() clears the pointer and
 * then waits for the count to reach zero before it destroys the fanouts. */
static _Atomic(net_server_t *) s_tap_server = NULL;
static atomic_int s_tap_inflight = 0;

/* Runs on the producer's thread from bufmgr_write_end() for every commit to
 * BUF_CAPTURE_RF / BUF_CAPTURE_AUDIO, whichever backend wrote it. Cheap when
 * no server is running or no client is streaming. */
static void server_write_tap(void *ctx, buffer_id_t id, const void *data, size_t bytes) {
    (void)ctx;
    atomic_fetch_add(&s_tap_inflight, 1);
    net_server_t *srv = atomic_load(&s_tap_server);
    if (srv) {
        if (id == BUF_CAPTURE_RF) net_fanout_push(&srv->rf, data, bytes);
        else if (id == BUF_CAPTURE_AUDIO) net_fanout_push(&srv->audio, data, bytes);
    }
    atomic_fetch_sub(&s_tap_inflight, 1);
}

/* Idempotent. Also re-run from gui_net_poll_commands() so a buffer-manager
 * re-init (memory budget change) cannot silently detach the stream. */
static void server_install_tap(gui_app_t *app) {
    if (bufmgr_get_write_tap(&app->buffers, BUF_CAPTURE_RF) != server_write_tap) {
        bufmgr_set_write_tap(&app->buffers, BUF_CAPTURE_RF, server_write_tap, NULL);
        bufmgr_set_write_tap(&app->buffers, BUF_CAPTURE_AUDIO, server_write_tap, NULL);
    }
}

/* Global init state. */
static bool s_globals_init = false;

void gui_net_init_globals(void) {
    if (s_globals_init) return;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        s_globals_init = true;
    }
#else
    signal(SIGPIPE, SIG_IGN);
    s_globals_init = true;
#endif
}

void gui_net_cleanup_globals(void) {
    if (!s_globals_init) return;
#ifdef _WIN32
    WSACleanup();
#endif
    s_globals_init = false;
}

bool gui_net_is_client(const gui_app_t *app) {
    (void)app;
    return s_client != NULL;
}

bool gui_net_is_server(const gui_app_t *app) {
    (void)app;
    return s_server != NULL;
}

bool gui_net_active(const gui_app_t *app) {
    (void)app;
    if (s_server && atomic_load(&s_server->running)) return true;
    if (s_client && atomic_load(&s_client->connected)) return true;
    return false;
}

const char *gui_net_mode_name(int mode) {
    switch (mode) {
        case GUI_NET_MODE_SERVER: return "Server";
        case GUI_NET_MODE_CLIENT: return "Client";
        default: return "Local";
    }
}

/* Write the network status line shown ONLY in the info window's Network
 * section. Never touches app->status_message (the bottom status bar). */
void gui_net_set_status(gui_app_t *app, const char *msg) {
    if (!app || !msg) return;
    snprintf(app->net_status, sizeof(app->net_status), "%s", msg);
}

void gui_net_status_string(const gui_app_t *app, char *buf, size_t len) {
    if (!buf || len == 0) return;
    if (s_server) {
        if (atomic_load(&s_server->running)) {
            snprintf(buf, len, "Listening on :%u", (unsigned)s_server->port);
        } else {
            snprintf(buf, len, "Server failed to listen on :%u", (unsigned)s_server->port);
        }
        return;
    }
    if (s_client) {
        if (atomic_load(&s_client->connected)) {
            snprintf(buf, len, "Connected to %s:%u", s_client->host, (unsigned)s_client->port);
        } else if (atomic_load(&s_client->worker_started) && s_client->host[0]) {
            snprintf(buf, len, "Connecting to %s:%u ...", s_client->host, (unsigned)s_client->port);
        } else if (atomic_load(&s_client->error)) {
            snprintf(buf, len, "Disconnected from %s:%u", s_client->host, (unsigned)s_client->port);
        } else if (s_client->host[0]) {
            snprintf(buf, len, "Client idle: %s:%u (press Connect)", s_client->host, (unsigned)s_client->port);
        } else {
            int n = 0;
            net_mutex_lock(&s_client->disc_mtx);
            n = s_client->discovered_count;
            net_mutex_unlock(&s_client->disc_mtx);
            if (n > 0) {
                snprintf(buf, len, "Client mode: %d server(s) found - select one", n);
            } else {
                snprintf(buf, len, "Client mode: scanning for servers on the LAN...");
            }
        }
        return;
    }
    if (app && app->net_status[0]) {
        snprintf(buf, len, "%s", app->net_status);
        return;
    }
    if (app) {
        if (app->settings.net_mode == GUI_NET_MODE_SERVER) {
            snprintf(buf, len, "Server mode: not active");
            return;
        }
        if (app->settings.net_mode == GUI_NET_MODE_CLIENT) {
            snprintf(buf, len, "Client mode: starting discovery...");
            return;
        }
    }
    snprintf(buf, len, "Local (no network)");
}

/* -------------------------------------------------------------------------
 * Server: HTTP request handling
 * ------------------------------------------------------------------------- */

/* Main thread only (from gui_net_poll_commands): copy what the HTTP threads
 * publish. Copies only when something changed (the settings generation, the
 * effective mode, the status line) and refreshes the output folder's free
 * space about once a second, so no handler reads app->settings or calls
 * statvfs off the main thread. */
static void server_publish(net_server_t *srv, gui_app_t *app) {
    uint32_t gen = gui_settings_generation();
    bool eff = app->is_recording ? app->capture_mode_runtime_misrc : app->user_capture_mode_misrc;
    bool status_changed = strncmp(srv->published_status, app->status_message,
                                  sizeof(srv->published_status) - 1) != 0;
    double now = net_now_s();
    bool disk_due = (now - srv->published_disk_time) >= 1.0;
    if (srv->published_once && gen == srv->published_generation &&
        eff == srv->published_effective_misrc && !status_changed && !disk_due) {
        return;
    }
    uint64_t disk_free = srv->published_disk_free;
    if (disk_due || !srv->published_once) {
        uint64_t f = 0;
        disk_free = gui_record_get_output_free_space_bytes(app, &f) ? f : 0;
        srv->published_disk_time = now;
    }
    net_mutex_lock(&srv->pub_mtx);
    if (!srv->published_once || gen != srv->published_generation) {
        srv->published = app->settings;
        srv->published_generation = gen;
    }
    srv->published_effective_misrc = eff;
    if (status_changed || !srv->published_once) {
        snprintf(srv->published_status, sizeof(srv->published_status), "%s", app->status_message);
        srv->published_status_seq++;
    }
    srv->published_disk_free = disk_free;
    srv->published_once = true;
    net_mutex_unlock(&srv->pub_mtx);
}

/* Build /stats JSON into buf. Reads gui_app_t atomics + selected device, and
 * the recording relay a client shows in place of its own record readouts. */
static void server_build_stats(net_server_t *srv, gui_app_t *app, char *buf, size_t len) {
    int state = 0;
    if (app->is_recording) state = 2;
    else if (app->is_capturing) state = 1;
    uint32_t sr = atomic_load(&app->sample_rate);
    uint64_t total = atomic_load(&app->total_samples);
    uint32_t frames = atomic_load(&app->frame_count);
    uint32_t errors = atomic_load(&app->error_count);
    int sel = app->selected_device;
    int dcount = app->device_count;
    char dname[80] = "none";
    int dtype = -1;
    if (sel >= 0 && sel < dcount) {
        snprintf(dname, sizeof(dname), "%s", app->devices[sel].name);
        dtype = (int)app->devices[sel].type;
    }
    /* Audio frame size: the capture callback pads 24-bit/4ch to 12 bytes.
     * Report 12 so clients can re-frame /baseband correctly. */
    int audio_frame = 12;

    /* Recording relay. */
    uint64_t rec_elapsed_ms = 0;
    if (app->is_recording) {
        double e = net_now_s() - app->recording_start_time;
        if (isfinite(e) && e > 0.0) rec_elapsed_ms = (uint64_t)(e * 1000.0);
    }
    uint64_t rec_bytes = atomic_load(&app->recording_bytes);
    uint64_t raw_a = atomic_load(&app->recording_raw_a);
    uint64_t raw_b = atomic_load(&app->recording_raw_b);
    uint64_t comp_a = atomic_load(&app->recording_compressed_a);
    uint64_t comp_b = atomic_load(&app->recording_compressed_b);
    uint32_t rec_drops = (uint32_t)(atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops) +
                                    atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops));
    bool rec_pending = gui_record_is_pending();
    bool rec_finalizing = gui_record_is_finalizing();
    char status_esc[600];
    uint32_t status_seq, gen;
    uint64_t disk_free;
    net_mutex_lock(&srv->pub_mtx);
    gui_settings_json_escape(srv->published_status, status_esc, sizeof(status_esc));
    status_seq = srv->published_status_seq;
    gen = srv->published_generation;
    disk_free = srv->published_disk_free;
    net_mutex_unlock(&srv->pub_mtx);

    snprintf(buf, len,
        "{\"state\":%d,\"sample_rate\":%u,\"total_samples\":%llu,"
        "\"frames\":%u,\"errors\":%u,\"selected_device\":%d,"
        "\"device_count\":%d,\"device_name\":\"%s\",\"device_type\":%d,"
        "\"audio_frame_bytes\":%d,"
        "\"rec_elapsed_ms\":%llu,\"rec_bytes\":%llu,"
        "\"rec_raw_a\":%llu,\"rec_raw_b\":%llu,\"rec_comp_a\":%llu,\"rec_comp_b\":%llu,"
        "\"rec_drops\":%u,\"disk_free\":%llu,\"rec_pending\":%s,\"rec_finalizing\":%s,"
        "\"status\":\"%s\",\"status_seq\":%u,\"generation\":%u}",
        state, (unsigned)sr, (unsigned long long)total,
        (unsigned)frames, (unsigned)errors, sel,
        dcount, dname, dtype, audio_frame,
        (unsigned long long)rec_elapsed_ms, (unsigned long long)rec_bytes,
        (unsigned long long)raw_a, (unsigned long long)raw_b,
        (unsigned long long)comp_a, (unsigned long long)comp_b,
        (unsigned)rec_drops, (unsigned long long)disk_free,
        rec_pending ? "true" : "false", rec_finalizing ? "true" : "false",
        status_esc, (unsigned)status_seq, (unsigned)gen);
}

static const char *http_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 422: return "Unprocessable Entity";
        case 503: return "Service Unavailable";
        default:  return "Error";
    }
}

static void server_send_json(net_sock_t fd, int status, const char *body) {
    char hdr[192];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
        status, http_reason(status), strlen(body));
    net_send_str(fd, hdr);
    net_send_str(fd, body);
}

static void server_send_error(net_sock_t fd, int status, const char *error, uint32_t generation) {
    char esc[400];
    gui_settings_json_escape(error, esc, sizeof(esc));
    char body[560];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\",\"generation\":%u}",
             esc, (unsigned)generation);
    server_send_json(fd, status, body);
}

/* GET /set?key=<k>&value=<percent-encoded>. Validates on this thread against
 * the published copy (so a value the table refuses never reaches the main
 * thread), then queues the request and waits up to NET_SET_WAIT_MS for the
 * main thread's answer. A timeout answers 503; the request stays queued and
 * shows up in the next /settings. */
static void server_handle_set(net_server_t *srv, net_sock_t fd, const char *query) {
    uint32_t gen_now;
    net_mutex_lock(&srv->pub_mtx);
    gen_now = srv->published_generation;
    net_mutex_unlock(&srv->pub_mtx);

    char key[64];
    char value[1600];
    if (!query || !net_query_get(query, "key", key, sizeof(key)) || !key[0]) {
        server_send_error(fd, 400, "missing key", gen_now);
        return;
    }
    if (!net_query_get(query, "value", value, sizeof(value))) {
        server_send_error(fd, 400, "missing value", gen_now);
        return;
    }
    if (!net_percent_decode(value)) {
        server_send_error(fd, 400, "malformed percent-encoding", gen_now);
        return;
    }
    const gui_setting_desc_t *d = gui_settings_find(key);
    if (!d) {
        server_send_error(fd, 400, "unknown key", gen_now);
        return;
    }
    if (d->flags & (GS_CLIENT_LOCAL | GS_WRITE_ONLY | GS_LOAD_ONLY | GS_NO_REMOTE_SET)) {
        server_send_error(fd, 400, "key is not remotely settable", gen_now);
        return;
    }
    gui_settings_t *scratch = (gui_settings_t *)malloc(sizeof(*scratch));
    if (!scratch) { net_send_str(fd, "HTTP/1.0 500 Internal Error\r\n\r\n"); return; }
    net_mutex_lock(&srv->pub_mtx);
    *scratch = srv->published;
    net_mutex_unlock(&srv->pub_mtx);
    char err[160];
    int arc = gui_settings_apply_key(scratch, key, value, true, err, sizeof(err));
    free(scratch);
    if (arc != 0) {
        char msg[224];
        snprintf(msg, sizeof(msg), "invalid value: %s", err);
        server_send_error(fd, 422, msg, gen_now);
        return;
    }

    uint32_t seq;
    net_mutex_lock(&srv->set_mtx);
    if (srv->set_q_count >= NET_SET_QUEUE) {
        net_mutex_unlock(&srv->set_mtx);
        server_send_error(fd, 503, "server busy; retry", gen_now);
        return;
    }
    seq = ++srv->set_seq;
    net_set_req_t *r = &srv->set_q[srv->set_q_count++];
    r->seq = seq;
    snprintf(r->key, sizeof(r->key), "%s", key);
    snprintf(r->value, sizeof(r->value), "%s", value);
    net_mutex_unlock(&srv->set_mtx);

    net_set_res_t res;
    memset(&res, 0, sizeof(res));
    bool got = false;
    int waited = 0;
    net_mutex_lock(&srv->set_mtx);
    while (!got && waited <= NET_SET_WAIT_MS && !atomic_load(&srv->stop_flag)) {
        for (int i = 0; i < srv->set_res_count; i++) {
            if (srv->set_res[i].seq == seq) {
                res = srv->set_res[i];
                memmove(&srv->set_res[i], &srv->set_res[i + 1],
                        (size_t)(srv->set_res_count - i - 1) * sizeof(res));
                srv->set_res_count--;
                got = true;
                break;
            }
        }
        if (got) break;
        net_cond_timedwait_ms(&srv->set_cv, &srv->set_mtx, 50);
        waited += 50;
    }
    net_mutex_unlock(&srv->set_mtx);

    if (!got) {
        server_send_error(fd, 503, "server busy; retry (the change is still queued)", gen_now);
        return;
    }
    if (res.http == 200) {
        char kesc[160], vesc[520];
        gui_settings_json_escape(key, kesc, sizeof(kesc));
        gui_settings_json_escape(res.msg, vesc, sizeof(vesc));
        char body[800];
        snprintf(body, sizeof(body), "{\"ok\":true,\"key\":\"%s\",\"value\":\"%s\",\"generation\":%u}",
                 kesc, vesc, (unsigned)res.generation);
        server_send_json(fd, 200, body);
    } else {
        server_send_error(fd, res.http, res.msg, res.generation);
    }
}

static void server_build_devices(gui_app_t *app, char *buf, size_t len) {
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off, "{\"count\":%d,\"selected\":%d,\"devices\":[",
                            app->device_count, app->selected_device);
    for (int i = 0; i < app->device_count && off + 64 < len; i++) {
        const device_info_t *d = &app->devices[i];
        off += (size_t)snprintf(buf + off, len - off,
            "%s{\"index\":%d,\"type\":%d,\"name\":\"%s\"}",
            (i ? "," : ""), i, (int)d->type, d->name);
    }
    if (off < len - 2) {
        buf[off++] = ']';
        buf[off++] = '}';
        buf[off] = '\0';
    } else if (len > 0) {
        buf[len - 1] = '\0';
    }
}

static void server_build_controls(gui_app_t *app, char *buf, size_t len) {
    /* Publish the mode the extraction thread is actually running, not the
     * saved setting. The two diverge on purpose: for a CXADC device the UI
     * sync and the capture-settings clamp force the effective mode off
     * (card 0 -> A, card 1 -> B) while leaving settings.misrc_mode alone, so
     * a client mirroring the setting swapped A/B against a server that did
     * not. While recording the runtime latch is what the record path uses.
     * The V1.5/V2.5 wiring flag inverts the swap on whichever side applies
     * it, so it must travel with the mode. */
    bool effective_misrc = app->is_recording ? app->capture_mode_runtime_misrc
                                             : app->user_capture_mode_misrc;
    snprintf(buf, len,
        "{\"misrc_mode\":%s,\"misrc_v15_v25_ab_swap\":%s,"
        "\"rf_bits_a\":%u,\"rf_bits_b\":%u,"
        "\"cxadc_tenbit_a\":%s,\"cxadc_tenbit_b\":%s,"
        "\"resample_a\":%s,\"resample_b\":%s,"
        "\"resample_rate_a\":%.1f,\"resample_rate_b\":%.1f,"
        "\"use_flac\":%s,\"flac_level\":%d}",
        effective_misrc ? "true" : "false",
        app->settings.misrc_v15_v25_ab_swap ? "true" : "false",
        (unsigned)app->settings.rf_bits_a, (unsigned)app->settings.rf_bits_b,
        app->settings.cxadc_tenbit_mode_card[0] ? "true" : "false",
        app->settings.cxadc_tenbit_mode_card[1] ? "true" : "false",
        app->settings.enable_resample_a ? "true" : "false",
        app->settings.enable_resample_b ? "true" : "false",
        app->settings.resample_rate_a, app->settings.resample_rate_b,
        app->settings.use_flac ? "true" : "false",
        app->settings.flac_level);
}

/* Parse an integer query arg like "?N" or "?on=1" from the URI. Returns the
 * integer found, or def. The reference splits on '&'; we only need one arg. */
static int server_parse_arg(const char *uri_after_path, const char *key, int def) {
    if (!uri_after_path) return def;
    /* uri_after_path points at the char after '?'. Look for key= or a bare number. */
    const char *p = uri_after_path;
    if (key && key[0]) {
        size_t klen = strlen(key);
        while (p && *p) {
            if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
                return atoi(p + klen + 1);
            }
            p = strchr(p, '&');
            if (!p) break;
            p++;
        }
        return def;
    }
    /* Bare numeric arg (e.g. /device?2). */
    return atoi(p);
}

/* Split URI into path + query (in-place in a local copy). */
static void server_handle_request(net_server_t *srv, net_sock_t fd, const char *method, char *uri) {
    gui_app_t *app = srv->app;
    if (0 != strcmp(method, "GET")) {
        net_send_str(fd, "HTTP/1.0 405 Method Not Allowed\r\n\r\n");
        return;
    }
    char *query = strchr(uri, '?');
    if (query) *query++ = '\0';

    if (strcmp(uri, "/") == 0 || strcmp(uri, "/version") == 0) {
        char body[128];
        snprintf(body, sizeof(body), "MISRC %s\r\n", MIRSC_TOOLS_VERSION);
        char hdr[128];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n",
            strlen(body));
        net_send_str(fd, hdr);
        net_send_str(fd, body);
        return;
    }
    if (strcmp(uri, "/stats") == 0) {
        char json[2048];
        server_build_stats(srv, app, json, sizeof(json));
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
            strlen(json));
        net_send_str(fd, hdr);
        net_send_str(fd, json);
        return;
    }
    if (strcmp(uri, "/devices") == 0) {
        char *json = (char *)malloc(8192);
        if (!json) { net_send_str(fd, "HTTP/1.0 500 Internal Error\r\n\r\n"); return; }
        server_build_devices(app, json, 8192);
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
            strlen(json));
        net_send_str(fd, hdr);
        net_send_str(fd, json);
        free(json);
        return;
    }
    if (strcmp(uri, "/controls") == 0) {
        char json[512];
        server_build_controls(app, json, sizeof(json));
        char hdr[160];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.0 200 OK\r\nContent-Type: text/json\r\nContent-Length: %zu\r\n\r\n",
            strlen(json));
        net_send_str(fd, hdr);
        net_send_str(fd, json);
        return;
    }
    if (strcmp(uri, "/settings") == 0) {
        /* Every server-owned setting (client-local and write-only keys are
         * excluded), from the published copy, plus the generation a client
         * uses to tell whether its snapshot is current and the effective
         * mode its extraction must run (see server_build_controls). */
        const size_t cap = 32768;
        char *body = (char *)malloc(cap);
        if (!body) { net_send_str(fd, "HTTP/1.0 500 Internal Error\r\n\r\n"); return; }
        int state = app->is_recording ? 2 : (app->is_capturing ? 1 : 0);
        net_mutex_lock(&srv->pub_mtx);
        int n = snprintf(body, cap,
            "{\"generation\":%u,\"state\":%d,\"misrc_mode_effective\":%s,\"settings\":",
            (unsigned)srv->published_generation, state,
            srv->published_effective_misrc ? "true" : "false");
        size_t need = 0;
        if (n > 0 && (size_t)n < cap) {
            need = gui_settings_to_json(&srv->published, GS_CLIENT_LOCAL | GS_WRITE_ONLY,
                                        body + n, cap - (size_t)n);
        }
        net_mutex_unlock(&srv->pub_mtx);
        if (n <= 0 || (size_t)n + need + 2 >= cap) {
            free(body);
            net_send_str(fd, "HTTP/1.0 500 Internal Error\r\n\r\n");
            return;
        }
        body[(size_t)n + need] = '}';
        body[(size_t)n + need + 1] = '\0';
        server_send_json(fd, 200, body);
        free(body);
        return;
    }
    if (strcmp(uri, "/set") == 0) {
        server_handle_set(srv, fd, query);
        return;
    }
    if (strcmp(uri, "/start") == 0) {
        atomic_store(&app->net_cmd_start, true);
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nstart requested\r\n");
        return;
    }
    if (strcmp(uri, "/stop") == 0) {
        atomic_store(&app->net_cmd_stop, true);
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nstop requested\r\n");
        return;
    }
    if (strcmp(uri, "/record") == 0) {
        int on = server_parse_arg(query, "on", 1);
        if (on) atomic_store(&app->net_cmd_record_on, true);
        else atomic_store(&app->net_cmd_record_off, true);
        const char *msg = on ? "record on requested\r\n" : "record off requested\r\n";
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n\r\n", strlen(msg));
        net_send_str(fd, hdr);
        net_send_str(fd, msg);
        return;
    }
    if (strcmp(uri, "/device") == 0) {
        int n = server_parse_arg(query, NULL, 0);
        atomic_store(&app->net_cmd_select_device, true);
        atomic_store(&app->net_cmd_device_index, n);
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\ndevice select requested\r\n");
        return;
    }
    if (strcmp(uri, "/rf") == 0 || strcmp(uri, "/baseband") == 0) {
        net_send_str(fd, "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
        bool is_rf = (uri[1] == 'r');
        net_fanout_t *f = is_rf ? &srv->rf : &srv->audio;
        atomic_int *ctr = is_rf ? &srv->rf_clients : &srv->audio_clients;
        atomic_fetch_add(ctr, 1);
        net_fanout_sub_t sub;
        net_fanout_subscribe(f, &sub);
        uint8_t buf[64 * 1024];
        for (;;) {
            int n = net_fanout_read(&sub, buf, sizeof(buf), 1000);
            if (n < 0) break;                  /* fanout shut down: server stopping */
            if (n == 0) {
                /* Nothing captured for 1 s (server idle). Keep the connection. */
                if (atomic_load(&srv->stop_flag)) break;
                continue;
            }
            if (net_send_all(fd, buf, (size_t)n) != 0) break;   /* client went away */
        }
        uint64_t delivered = 0, dropped = 0;
        net_fanout_sub_stats(&sub, &delivered, &dropped, NULL);
        net_fanout_unsubscribe(&sub);
        atomic_fetch_sub(ctr, 1);
        fprintf(stderr, "[NET] %s client done: sent %llu bytes, dropped %llu\n",
                uri, (unsigned long long)delivered, (unsigned long long)dropped);
        return;
    }
    net_send_str(fd, "HTTP/1.0 404 Not Found\r\n\r\n");
}

/* Unregister a connection, close its socket and drop the thread count. The
 * count is the last thing a client thread touches in srv. */
static void server_conn_release(net_server_t *srv, net_client_conn_t *conn) {
    net_mutex_lock(&srv->clients_mtx);
    for (net_client_conn_t **pp = &srv->clients; *pp; pp = &(*pp)->next) {
        if (*pp == conn) { *pp = conn->next; break; }
    }
    net_mutex_unlock(&srv->clients_mtx);
    net_close(conn->fd);
    free(conn);
    atomic_fetch_sub(&srv->client_threads, 1);
}

/* Per-client thread: read one request, serve it, close. For /rf + /baseband
 * the serve loops until the client disconnects or the server stops. */
static int server_client_thread(void *arg) {
    server_client_ctx_t *c = (server_client_ctx_t *)arg;
    net_server_t *srv = c->srv;
    net_client_conn_t *conn = c->conn;
    net_sock_t fd = conn->fd;
    free(c);

    /* Bound recv/send so a stuck/missing client cannot hold this (detached)
     * thread forever on a blocking call. */
    net_set_timeouts(fd, 5000, 5000);

    char buf[0x1000];
    int len = net_read_headers(fd, buf, sizeof(buf));
    if (len > 0) {
        char method[8] = {0};
        /* /set carries a percent-encoded setting value (up to ~1.5 KB). */
        char uri[2048] = {0};
        int v1 = 0, v2 = 0;
        if (4 == sscanf(buf, "%7s %2047s HTTP/%d.%d", method, uri, &v1, &v2)) {
            server_handle_request(srv, fd, method, uri);
        } else {
            net_send_str(fd, "HTTP/1.0 400 Bad Request\r\n\r\n");
        }
    }
    server_conn_release(srv, conn);
    return 0;
}

static int server_listen_thread(void *arg) {
    net_server_t *srv = (net_server_t *)arg;
    /* Non-blocking accept loop: select() with a short timeout so this thread
     * checks stop_flag frequently and thrd_join() returns within ~200ms on
     * server_stop(). A blocking accept() would never wake from close() alone
     * on POSIX (the thread holds its own fd reference), freezing the UI. */
    net_set_nonblocking(srv->listen_fd);
    while (!atomic_load(&srv->stop_flag)) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(srv->listen_fd, &rset);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;  /* 200ms */
        int sr = select((int)srv->listen_fd + 1, &rset, NULL, NULL, &tv);
        if (sr <= 0) {
            /* timeout (sr==0) or error: re-check stop_flag and loop */
            continue;
        }
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        net_sock_t cfd = accept(srv->listen_fd, (struct sockaddr *)&caddr, &clen);
        if (!net_sock_valid(cfd)) {
            if (atomic_load(&srv->stop_flag)) break;
            continue;
        }
        net_client_conn_t *conn = (net_client_conn_t *)calloc(1, sizeof(*conn));
        server_client_ctx_t *c = (server_client_ctx_t *)malloc(sizeof(*c));
        if (!conn || !c) { net_close(cfd); free(conn); free(c); continue; }
        conn->fd = cfd;
        c->srv = srv;
        c->conn = conn;
        /* Register before the thread exists, so once the listener has been
         * joined server_stop() sees every connection there is. */
        net_mutex_lock(&srv->clients_mtx);
        conn->next = srv->clients;
        srv->clients = conn;
        net_mutex_unlock(&srv->clients_mtx);
        atomic_fetch_add(&srv->client_threads, 1);
        thrd_t t;
        if (thrd_create(&t, server_client_thread, c) != thrd_success) {
            free(c);
            server_conn_release(srv, conn);
            continue;
        }
        net_thread_detach(&t);
    }
    return 0;
}

/* UDP discovery beacon: broadcast "MISRC\n<tcpport>\n<hostname>" every 2s to
 * NET_DISCOVERY_PORT so clients on the LAN can list and select this server
 * without typing host:port. */
static int server_beacon_thread(void *arg) {
    net_server_t *srv = (net_server_t *)arg;
    net_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!net_sock_valid(fd)) return 0;
    int bcast = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char *)&bcast, sizeof(bcast));
    char name[128] = {0};
    gethostname(name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    char pkt[256];
    int plen = snprintf(pkt, sizeof(pkt), "MISRC\n%u\n%s", (unsigned)srv->port, name);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_BROADCAST;
    dst.sin_port = htons(NET_DISCOVERY_PORT);
    while (!atomic_load(&srv->beacon_stop)) {
        sendto(fd, pkt, plen, 0, (struct sockaddr *)&dst, sizeof(dst));
        /* Sleep 2s in 100ms slices so beacon_stop is seen promptly on stop. */
        for (int i = 0; i < 20 && !atomic_load(&srv->beacon_stop); i++) {
            thrd_sleep_ms(100);
        }
    }
    net_close(fd);
    return 0;
}

static int server_start(gui_app_t *app, uint16_t port) {
    if (s_server) return 0;
    net_server_t *srv = (net_server_t *)calloc(1, sizeof(*srv));
    if (!srv) return -1;
    srv->app = app;
    srv->port = port;
    srv->listen_fd = NET_INVALID_SOCKET;
    atomic_store(&srv->running, false);
    atomic_store(&srv->stop_flag, false);
    atomic_store(&srv->rf_clients, 0);
    atomic_store(&srv->audio_clients, 0);
    atomic_store(&srv->beacon_stop, false);
    net_fanout_init(&srv->rf, 0);
    net_fanout_init(&srv->audio, 0);
    net_mutex_init(&srv->clients_mtx);
    srv->clients = NULL;
    atomic_store(&srv->client_threads, 0);
    net_mutex_init(&srv->set_mtx);
    net_cond_init(&srv->set_cv);
    net_mutex_init(&srv->pub_mtx);
    srv->set_q_count = 0;
    srv->set_res_count = 0;
    srv->set_seq = 0;
    srv->published_once = false;
    srv->published_disk_time = -10.0;

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!net_sock_valid(srv->listen_fd)) {
        fprintf(stderr, "[NET] server: socket() failed\n");
        free(srv);
        return -1;
    }
    int reuse = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[NET] server: bind(:%u) failed\n", (unsigned)port);
        net_close(srv->listen_fd);
        free(srv);
        return -1;
    }
    if (listen(srv->listen_fd, 16) < 0) {
        fprintf(stderr, "[NET] server: listen() failed\n");
        net_close(srv->listen_fd);
        free(srv);
        return -1;
    }
    if (thrd_create(&srv->listen_thread, server_listen_thread, srv) != thrd_success) {
        net_close(srv->listen_fd);
        free(srv);
        return -1;
    }
    atomic_store(&srv->running, true);
    /* Start the UDP discovery beacon so clients can find us. */
    if (thrd_create(&srv->beacon_thread, server_beacon_thread, srv) != thrd_success) {
        fprintf(stderr, "[NET] server: beacon thread failed (non-fatal)\n");
    }
    s_server = srv;
    app->net_state = srv;
    atomic_store(&app->net_connected, true);
    /* Feed the fanouts from every BUF_CAPTURE_RF / BUF_CAPTURE_AUDIO commit. */
    server_install_tap(app);
    atomic_store(&s_tap_server, srv);
    fprintf(stderr, "[NET] server listening on :%u\n", (unsigned)port);
    return 0;
}

static void server_stop(net_server_t *srv) {
    if (!srv) return;
    /* Stop the capture-thread tap first; a push already inside it finishes
     * before the fanouts go away (the in-flight wait below). */
    atomic_store(&s_tap_server, NULL);
    atomic_store(&srv->stop_flag, true);
    atomic_store(&srv->running, false);
    atomic_store(&srv->beacon_stop, true);
    /* Wake any /set handler waiting for an answer; it sees stop_flag. */
    net_mutex_lock(&srv->set_mtx);
    net_cond_broadcast(&srv->set_cv);
    net_mutex_unlock(&srv->set_mtx);
    if (net_sock_valid(srv->listen_fd)) {
        /* shutdown() wakes any blocking select()/accept() on this socket in
         * addition to the non-blocking loop, then close(). */
#ifdef _WIN32
        shutdown(srv->listen_fd, 2 /* SD_BOTH */);
#else
        shutdown(srv->listen_fd, SHUT_RDWR);
#endif
        net_close(srv->listen_fd);
        srv->listen_fd = NET_INVALID_SOCKET;
    }
    thrd_join(srv->listen_thread, NULL);   /* no new client threads after this */
    thrd_join(srv->beacon_thread, NULL);
    /* Wake every /rf + /baseband reader (their reads now return -1) and abort
     * any client blocked in recv()/send() by shutting its socket down. The
     * threads own their sockets and close them on the way out. */
    net_fanout_shutdown(&srv->rf);
    net_fanout_shutdown(&srv->audio);
    net_mutex_lock(&srv->clients_mtx);
    for (net_client_conn_t *conn = srv->clients; conn; conn = conn->next) {
#ifdef _WIN32
        shutdown(conn->fd, 2 /* SD_BOTH */);
#else
        shutdown(conn->fd, SHUT_RDWR);
#endif
    }
    net_mutex_unlock(&srv->clients_mtx);
    /* Wait for the client threads to be done with srv. Bounded by their 5 s
     * socket timeouts even if the shutdown() above were not enough. */
    int waited_ms = 0;
    while (atomic_load(&srv->client_threads) > 0) {
        thrd_sleep_ms(5);
        waited_ms += 5;
        if (waited_ms == 2000) {
            fprintf(stderr, "[NET] server: still waiting for %d client thread(s)\n",
                    atomic_load(&srv->client_threads));
        }
    }
    while (atomic_load(&s_tap_inflight) > 0) {
        thrd_sleep_ms(1);
    }
    net_fanout_destroy(&srv->rf);
    net_fanout_destroy(&srv->audio);
    net_mutex_destroy(&srv->clients_mtx);
    net_cond_destroy(&srv->set_cv);
    net_mutex_destroy(&srv->set_mtx);
    net_mutex_destroy(&srv->pub_mtx);
    if (srv->app) {
        atomic_store(&srv->app->net_connected, false);
        srv->app->net_state = NULL;
    }
    free(srv);
    s_server = NULL;
}

/* -------------------------------------------------------------------------
 * Client: HTTP helper (one short request), then the worker + pump threads.
 * ------------------------------------------------------------------------- */

/* Connect TCP to host:port with a bounded timeout. Non-blocking connect +
 * select() so an unreachable host does not block the caller for the full TCP
 * timeout (tens of seconds). Returns a blocking socket with SO_RCVTIMEO /
 * SO_SNDTIMEO set so later recv/send calls also cannot hang forever. */
#define NET_CONNECT_TIMEOUT_MS 2000
#define NET_IO_TIMEOUT_MS      1000

static net_sock_t client_connect(const char *host, uint16_t port) {
    net_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!net_sock_valid(fd)) return NET_INVALID_SOCKET;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        /* Not a dotted-quad; resolve hostname. */
        struct hostent *he = gethostbyname(host);
        if (!he || he->h_addrtype != AF_INET) {
            net_close(fd);
            return NET_INVALID_SOCKET;
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    }
    net_set_nonblocking(fd);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
#ifdef _WIN32
        int err = (int)WSAGetLastError();
        /* WSAEWOULDBLOCK == 10035; any other error is fatal here. */
        if (err != 10035) { net_close(fd); return NET_INVALID_SOCKET; }
#else
        if (errno != EINPROGRESS) { net_close(fd); return NET_INVALID_SOCKET; }
#endif
        /* Wait for the socket to become writable (connect completes) up to
         * NET_CONNECT_TIMEOUT_MS, so we don't block the UI thread on stop. */
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval tv;
        tv.tv_sec = NET_CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (NET_CONNECT_TIMEOUT_MS % 1000) * 1000L;
        int sr = select((int)fd + 1, NULL, &wset, NULL, &tv);
        if (sr <= 0) { net_close(fd); return NET_INVALID_SOCKET; }
        /* Verify the connect actually succeeded (no SO_ERROR). */
        int soerr = 0;
        socklen_t solen = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &solen) != 0 || soerr != 0) {
            net_close(fd);
            return NET_INVALID_SOCKET;
        }
    }
    net_set_blocking(fd);
    net_set_timeouts(fd, NET_IO_TIMEOUT_MS, NET_IO_TIMEOUT_MS);
    return fd;
}

/* Send "GET <path> HTTP/1.0\r\n\r\n", read headers + full body into body_out
 * (up to body_cap). HTTP/1.0 closes the connection at EOF, so keep reading
 * until recv returns 0 (EOF) or the body buffer is full. Returns 0 on a 200,
 * -1 otherwise. body_len receives the number of body bytes stored (excluding
 * the NUL terminator); status_out (optional) the HTTP status, 0 when no
 * response was parsed. The body is read for every status, so a refused /set
 * can carry its reason. */
static int client_get(const char *host, uint16_t port, const char *path,
                      char *body_out, size_t body_cap, size_t *body_len, int *status_out) {
    if (status_out) *status_out = 0;
    if (body_len) *body_len = 0;
    if (body_out && body_cap) body_out[0] = '\0';
    net_sock_t fd = client_connect(host, port);
    if (!net_sock_valid(fd)) return -1;
    char req[2304];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    if (net_send_str(fd, req) != 0) { net_close(fd); return -1; }
    /* Read into a header buffer until we find the end-of-headers marker. */
    char hbuf[2048];
    size_t total = 0;
    int got_headers = 0;
    size_t body_off = 0;
    while (total < sizeof(hbuf) - 1) {
#ifdef _WIN32
        int n = recv(fd, hbuf + total, (int)(sizeof(hbuf) - 1 - total), 0);
        if (n == SOCKET_ERROR || n == 0) break;
        total += (size_t)n;
#else
        ssize_t n = recv(fd, hbuf + total, sizeof(hbuf) - 1 - total, 0);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        total += (size_t)n;
#endif
        hbuf[total] = '\0';
        char *eoh = strstr(hbuf, "\r\n\r\n");
        if (eoh) {
            got_headers = 1;
            body_off = (size_t)(eoh + 4 - hbuf);
            break;
        }
    }
    int status = 0;
    if (got_headers && strncmp(hbuf, "HTTP/1.", 7) == 0) {
        const char *sp = strchr(hbuf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (status_out) *status_out = status;
    int rc = -1;
    if (got_headers && status > 0) {
        /* Copy any body bytes that arrived with the headers, then keep
         * reading until EOF (HTTP/1.0 closes the socket) so the full body
         * is captured - a small JSON body often arrives in a later TCP
         * packet than the headers, and the old code returned an empty
         * body in that case. */
        size_t have = total - body_off;
        if (body_out && body_cap) {
            size_t copy = (have < body_cap) ? have : body_cap - 1;
            memcpy(body_out, hbuf + body_off, copy);
            size_t body_total = copy;
            while (body_total < body_cap - 1) {
#ifdef _WIN32
                int n = recv(fd, body_out + body_total, (int)(body_cap - 1 - body_total), 0);
                if (n == SOCKET_ERROR) break;
                if (n == 0) break;  /* EOF: full body received */
                body_total += (size_t)n;
#else
                ssize_t n = recv(fd, body_out + body_total, body_cap - 1 - body_total, 0);
                if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
                body_total += (size_t)n;
#endif
            }
            body_out[body_total] = '\0';
            if (body_len) *body_len = body_total;
        }
        rc = (status == 200) ? 0 : -1;
    }
    net_close(fd);
    return rc;
}

/* Tiny JSON field extractors for the mirror. */
static int json_int(const char *j, const char *key, int def) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    return atoi(p + strlen(pat));
}

static void json_str(const char *j, const char *key, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return;
    p += strlen(pat);
    const char *e = strchr(p, '"');
    if (!e) return;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static bool json_bool(const char *j, const char *key, bool def) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    return (strncmp(p, "true", 4) == 0);
}

static uint64_t json_u64(const char *j, const char *key, uint64_t def) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(j, pat);
    if (!p) return def;
    return (uint64_t)strtoull(p + strlen(pat), NULL, 10);
}

/* Ingest pump thread: connect /rf (or /baseband), read body bytes, re-frame to
 * frame_bytes alignment, write into the buffer manager. Reconnects on any
 * transient stream drop (recv=0/error) with backoff so the feed stays alive
 * across server capture start/stop and brief network blips; only exits when
 * pump_stop is set (ingest torn down on real disconnect / mode change). */
static int client_pump_thread(void *arg) {
    net_pump_ctx_t *pc = (net_pump_ctx_t *)arg;
    net_client_t *cli = pc->cli;
    gui_app_t *app = cli->app;
    const char *path = pc->is_audio ? "/baseband" : "/rf";
    const int frame = pc->frame_bytes > 0 ? pc->frame_bytes : 4;
    uint8_t inbuf[128 * 1024];
    int reconnect_ms = 500;

    while (!atomic_load(&cli->pump_stop)) {
        net_sock_t fd = client_connect(cli->host, cli->port);
        if (!net_sock_valid(fd)) {
            /* Connect failed: backoff and retry (keep trying while the
             * server is unreachable but we're still in client mode). */
            thrd_sleep_ms(reconnect_ms);
            if (reconnect_ms < 3000) reconnect_ms += 500;
            continue;
        }
        reconnect_ms = 500;
        char req[256];
        snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, cli->host);
        if (net_send_str(fd, req) != 0) {
            net_close(fd);
            thrd_sleep_ms(reconnect_ms);
            continue;
        }
        /* Read headers. */
        char hbuf[1024];
        size_t total = 0;
        int got_headers = 0;
        size_t body_off = 0;
        while (total < sizeof(hbuf) - 1 && !atomic_load(&cli->pump_stop)) {
#ifdef _WIN32
            int n = recv(fd, hbuf + total, (int)(sizeof(hbuf) - 1 - total), 0);
            if (n == 0) break;
            if (n == SOCKET_ERROR) {
                int err = (int)WSAGetLastError();
                if (err == 10060 /* WSAETIMEDOUT */) { continue; }  /* recv timeout: re-check stop */
                break;
            }
            total += (size_t)n;
#else
            ssize_t n = recv(fd, hbuf + total, sizeof(hbuf) - 1 - total, 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;
            }
            total += (size_t)n;
#endif
            hbuf[total] = '\0';
            char *eoh = strstr(hbuf, "\r\n\r\n");
            if (eoh) { got_headers = 1; body_off = (size_t)(eoh + 4 - hbuf); break; }
        }
        if (!got_headers) {
            net_close(fd);
            thrd_sleep_ms(reconnect_ms);
            continue;  /* no headers / stream closed: reconnect */
        }
        /* Carry over any body bytes already in hbuf. */
        size_t in_have = total - body_off;
        if (in_have > sizeof(inbuf)) in_have = sizeof(inbuf);
        memcpy(inbuf, hbuf + body_off, in_have);

        fprintf(stderr, "[NET] client pump %s streaming (frame=%d)\n", path, frame);
        /* Body loop: read until the stream closes, then reconnect. */
        while (!atomic_load(&cli->pump_stop)) {
            if (in_have >= sizeof(inbuf)) {
                size_t aligned = (in_have / frame) * frame;
                if (aligned == 0) { in_have = 0; continue; }
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, pc->buf_id, aligned, NULL);
                if (out) { memcpy(out, inbuf, aligned); bufmgr_write_end(&app->buffers, pc->buf_id, aligned); bufmgr_signal_data(&app->buffers, pc->buf_id); }
                size_t leftover = in_have - aligned;
                if (leftover) memmove(inbuf, inbuf + aligned, leftover);
                in_have = leftover;
                continue;
            }
#ifdef _WIN32
            int n = recv(fd, (char *)(inbuf + in_have), (int)(sizeof(inbuf) - in_have), 0);
            if (n == 0) break;  /* stream closed: reconnect */
            if (n == SOCKET_ERROR) {
                int err = (int)WSAGetLastError();
                if (err == 10060 /* WSAETIMEDOUT */) { continue; }  /* timeout: keep waiting */
                break;  /* real error: reconnect */
            }
            in_have += (size_t)n;
#else
            ssize_t n = recv(fd, inbuf + in_have, sizeof(inbuf) - in_have, 0);
            if (n == 0) break;  /* stream closed: reconnect */
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                break;  /* real error: reconnect */
            }
            in_have += (size_t)n;
#endif
            size_t aligned = (in_have / frame) * frame;
            if (aligned > 0) {
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, pc->buf_id, aligned, NULL);
                if (out) { memcpy(out, inbuf, aligned); bufmgr_write_end(&app->buffers, pc->buf_id, aligned); bufmgr_signal_data(&app->buffers, pc->buf_id); }
                size_t leftover = in_have - aligned;
                if (leftover) memmove(inbuf, inbuf + aligned, leftover);
                in_have = leftover;
                atomic_store(&app->last_callback_time_ms, get_time_ms());
                atomic_store(&app->stream_synced, true);
            }
        }
        /* Stream ended (recv 0 / error) or pump_stop: close and either reconnect
         * or exit. */
        net_close(fd);
        if (atomic_load(&cli->pump_stop)) break;
        fprintf(stderr, "[NET] client pump %s stream dropped, reconnecting...\n", path);
        thrd_sleep_ms(reconnect_ms);
    }
    free(pc);
    fprintf(stderr, "[NET] client pump %s exiting\n", path);
    return 0;
}

/* Main-thread: start ingest backend (extraction + display + pump threads). */
static void client_start_ingest(gui_app_t *app, net_client_t *cli) {
    if (atomic_load(&cli->ingest_active)) return;
    if (app->is_capturing) return; /* already capturing locally somehow */
    fprintf(stderr, "[NET] client: starting ingest backend\n");

    /* Reset the capture/audio buffers' head/tail (not just stats) so a stale
     * fill level from a previous capture session doesn't pin RF Buffer at
     * ~99% on the client readout. bufmgr_reset_stats only clears stats;
     * bufmgr_reset also rewinds head/tail to 0. BUT bufmgr_reset is a no-op
     * if the buffer isn't initialized yet (lazy init), so ensure_init first. */
    (void)bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF);
    (void)bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO);
    bufmgr_reset(&app->buffers, BUF_CAPTURE_RF);
    bufmgr_reset(&app->buffers, BUF_CAPTURE_AUDIO);
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);
    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    uint32_t peer_sr = (uint32_t)atomic_load(&cli->peer_sample_rate);
    if (peer_sr > 0) atomic_store(&app->sample_rate, peer_sr);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    /* Treat the network feed as a MISRC-style dual-channel raw stream. */
    app->capture_backend_upstream = false;
    app->capture_has_channel_b = true;
    app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
    app->is_capturing = true;
    app->capture_start_time = GetTime();
    app->reconnect_pending = false;
    app->reconnect_attempts = 0;

    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[NET] client: failed to start extraction\n");
        app->is_capturing = false;
        return;
    }
    if (app->display_thread) {
        (void)gui_display_thread_start(app->display_thread, app, &app->buffers);
    }
    (void)gui_audio_start(app, &app->buffers);

    atomic_store(&cli->pump_stop, false);
    net_pump_ctx_t *rf = (net_pump_ctx_t *)calloc(1, sizeof(*rf));
    if (rf) {
        rf->cli = cli; rf->is_audio = false; rf->buf_id = BUF_CAPTURE_RF; rf->frame_bytes = 4;
        if (thrd_create(&cli->rf_pump_thread, client_pump_thread, rf) != thrd_success) {
            free(rf);
        }
    }
    int af = (int)atomic_load(&cli->peer_audio_frame_bytes);
    if (af <= 0) af = 12;
    net_pump_ctx_t *au = (net_pump_ctx_t *)calloc(1, sizeof(*au));
    if (au) {
        au->cli = cli; au->is_audio = true; au->buf_id = BUF_CAPTURE_AUDIO; au->frame_bytes = af;
        if (thrd_create(&cli->audio_pump_thread, client_pump_thread, au) != thrd_success) {
            free(au);
        }
    }
    atomic_store(&cli->ingest_active, true);
    gui_net_set_status(app, "Client ingest running (mirroring server capture)");
}

/* Main-thread: stop ingest backend. */
static void client_stop_ingest(gui_app_t *app, net_client_t *cli) {
    if (!atomic_load(&cli->ingest_active)) return;
    fprintf(stderr, "[NET] client: stopping ingest backend\n");
    atomic_store(&cli->pump_stop, true);
    /* The pump threads will exit when recv returns 0/error or pump_stop is seen.
     * They own their sockets; closing happens inside the thread. Join them. */
    thrd_join(cli->rf_pump_thread, NULL);
    thrd_join(cli->audio_pump_thread, NULL);

    app->is_capturing = false;
    if (app->display_thread) gui_display_thread_stop(app->display_thread);
    gui_audio_stop(app);
    gui_extract_stop();
    atomic_store(&app->stream_synced, false);
    gui_app_clear_display(app);
    atomic_store(&cli->ingest_active, false);
    gui_net_set_status(app, "Client ingest stopped");
}

/* Parse a /stats body: the peer state the mirror always had, plus the
 * recording relay and the status line. Worker thread. */
static void client_apply_stats(net_client_t *cli, const char *stats) {
    int st = json_int(stats, "state", 0);
    int sr = json_int(stats, "sample_rate", 0);
    int sel = json_int(stats, "selected_device", -1);
    int dc = json_int(stats, "device_count", 0);
    int af = json_int(stats, "audio_frame_bytes", 12);
    atomic_store(&cli->peer_state, st);
    atomic_store(&cli->peer_sample_rate, sr);
    atomic_store(&cli->peer_selected, sel);
    atomic_store(&cli->peer_device_count, dc);
    atomic_store(&cli->peer_audio_frame_bytes, af);

    atomic_store(&cli->peer_rec_elapsed_ms, (uint_fast64_t)json_u64(stats, "rec_elapsed_ms", 0));
    atomic_store(&cli->peer_rec_bytes, (uint_fast64_t)json_u64(stats, "rec_bytes", 0));
    atomic_store(&cli->peer_rec_raw_a, (uint_fast64_t)json_u64(stats, "rec_raw_a", 0));
    atomic_store(&cli->peer_rec_raw_b, (uint_fast64_t)json_u64(stats, "rec_raw_b", 0));
    atomic_store(&cli->peer_rec_comp_a, (uint_fast64_t)json_u64(stats, "rec_comp_a", 0));
    atomic_store(&cli->peer_rec_comp_b, (uint_fast64_t)json_u64(stats, "rec_comp_b", 0));
    atomic_store(&cli->peer_disk_free, (uint_fast64_t)json_u64(stats, "disk_free", 0));
    atomic_store(&cli->peer_rec_drops, (uint_fast32_t)json_int(stats, "rec_drops", 0));
    atomic_store(&cli->peer_rec_pending, json_bool(stats, "rec_pending", false));
    atomic_store(&cli->peer_rec_finalizing, json_bool(stats, "rec_finalizing", false));

    char status[256];
    uint32_t sseq = (uint32_t)json_int(stats, "status_seq", 0);
    uint32_t gen = (uint32_t)json_int(stats, "generation", 0);
    bool stale = false;
    net_mutex_lock(&cli->set_mtx);
    if (gui_settings_find_value(stats, "status", status, sizeof(status), true) &&
        sseq != cli->peer_status_seq) {
        snprintf(cli->peer_status, sizeof(cli->peer_status), "%s", status);
        cli->peer_status_seq = sseq;
        cli->peer_status_dirty = true;
    }
    /* A server-side edit bumps the generation; fetch the snapshot now rather
     * than on the 3 s tick. */
    if (cli->staged_settings_valid && gen > cli->staged_generation) stale = true;
    net_mutex_unlock(&cli->set_mtx);
    if (stale) atomic_store(&cli->settings_refresh_req, true);
}

/* Parse a /devices body into the staged device list. Worker thread. */
static void client_apply_devices(net_client_t *cli, const char *dj) {
    int count = json_int(dj, "count", 0);
    int selected = json_int(dj, "selected", -1);
    if (count < 0) count = 0;
    if (count > MAX_DEVICES) count = MAX_DEVICES;
    /* Parse device entries by finding "name":"..." occurrences. */
    net_mutex_lock(&cli->dev_mtx);
    cli->staged_device_count = 0;
    const char *p = dj;
    for (int i = 0; i < count; i++) {
        const char *idx = strstr(p, "\"index\":");
        const char *nm = strstr(p, "\"name\":\"");
        if (!nm) break;
        int ti = idx ? atoi(idx + 8) : i;
        char name[80];
        json_str(nm, "name", name, sizeof(name));
        device_info_t *d = &cli->staged_devices[cli->staged_device_count];
        snprintf(d->name, sizeof(d->name), "%s", name);
        d->serial[0] = '\0';
        /* We don't know the real device type; mark as a generic
         * "remote" entry using SIMPLE_CAPTURE as a neutral tag so
         * the dropdown renders. The client never opens it. */
        d->type = DEVICE_TYPE_SIMPLE_CAPTURE;
        d->index = ti;
        cli->staged_device_count++;
        p = nm + 8;
    }
    cli->staged_selected = selected;
    cli->staged_dirty = true;
    net_mutex_unlock(&cli->dev_mtx);
}

/* GET /settings and stage the server's settings. An older server without
 * the endpoint (404) gets the two fields the client's own pipeline needs
 * from /controls instead. Worker thread. */
static void client_fetch_settings(net_client_t *cli) {
    const size_t cap = 32768;
    char *sj = (char *)malloc(cap);
    if (!sj) return;
    int status = 0;
    size_t blen = 0;
    int rc = client_get(cli->host, cli->port, "/settings", sj, cap, &blen, &status);
    if (rc == 0) {
        gui_settings_t *snap = (gui_settings_t *)malloc(sizeof(*snap));
        if (snap) {
            gui_settings_init_defaults(snap);
            size_t n = 0;
            const gui_setting_desc_t *t = gui_settings_table(&n);
            char val[600];
            for (size_t i = 0; i < n; i++) {
                if (t[i].flags & (GS_CLIENT_LOCAL | GS_WRITE_ONLY | GS_LOAD_ONLY)) continue;
                if (!gui_settings_find_value(sj, t[i].key, val, sizeof(val), true)) continue;
                (void)gui_settings_apply_key(snap, t[i].key, val, false, NULL, 0);
            }
            uint32_t gen = (uint32_t)json_int(sj, "generation", 0);
            bool eff = json_bool(sj, "misrc_mode_effective", snap->misrc_mode);
            net_mutex_lock(&cli->set_mtx);
            cli->staged_settings = *snap;
            cli->staged_generation = gen;
            cli->staged_effective_misrc = eff;
            cli->staged_settings_valid = true;
            cli->staged_settings_dirty = true;
            cli->staged_settings_time_ms = get_time_ms();
            cli->server_settings_support = 1;
            net_mutex_unlock(&cli->set_mtx);
            free(snap);
        }
    } else if (status == 404) {
        char cj[512];
        bool have = client_get(cli->host, cli->port, "/controls", cj, sizeof(cj), &blen, NULL) == 0;
        net_mutex_lock(&cli->set_mtx);
        cli->server_settings_support = 0;
        if (have) {
            cli->staged_effective_misrc = json_bool(cj, "misrc_mode", false);
            cli->staged_settings.misrc_v15_v25_ab_swap = json_bool(cj, "misrc_v15_v25_ab_swap", false);
            cli->staged_controls_dirty = true;
        }
        net_mutex_unlock(&cli->set_mtx);
    }
    free(sj);
}

/* Send every queued /set and store the answers; re-fetch /settings after
 * so the canonical values arrive with the generation that carries them.
 * Worker thread. */
static void client_send_pending_sets(net_client_t *cli) {
    bool sent_any = false;
    for (;;) {
        net_set_req_t req;
        bool have = false;
        net_mutex_lock(&cli->set_mtx);
        if (cli->out_count > 0) {
            req = cli->out_q[0];
            memmove(&cli->out_q[0], &cli->out_q[1], (size_t)(cli->out_count - 1) * sizeof(req));
            cli->out_count--;
            have = true;
        }
        net_mutex_unlock(&cli->set_mtx);
        if (!have) break;

        char enc[1600];
        net_percent_encode(req.value, enc, sizeof(enc));
        char path[1800];
        snprintf(path, sizeof(path), "/set?key=%s&value=%s", req.key, enc);
        char body[1024];
        int status = 0;
        size_t blen = 0;
        int rc = client_get(cli->host, cli->port, path, body, sizeof(body), &blen, &status);
        net_set_res_t res;
        memset(&res, 0, sizeof(res));
        res.seq = req.seq;
        res.http = (rc == 0) ? 200 : status;
        res.generation = (uint32_t)json_int(body, "generation", 0);
        if (res.http == 200) {
            (void)gui_settings_find_value(body, "value", res.msg, sizeof(res.msg), true);
        } else if (status > 0) {
            if (!gui_settings_find_value(body, "error", res.msg, sizeof(res.msg), true)) {
                snprintf(res.msg, sizeof(res.msg), "HTTP %d", status);
            }
        } else {
            snprintf(res.msg, sizeof(res.msg), "no response from server");
        }
        fprintf(stderr, "[NET] client: /set %s -> %d%s%s\n", req.key, res.http,
                res.http == 200 ? "" : ": ", res.http == 200 ? "" : res.msg);
        net_mutex_lock(&cli->set_mtx);
        if (cli->res_count >= (int)(NET_SET_QUEUE * 2)) {
            memmove(&cli->results[0], &cli->results[1], (size_t)(NET_SET_QUEUE * 2 - 1) * sizeof(res));
            cli->res_count = NET_SET_QUEUE * 2 - 1;
        }
        cli->results[cli->res_count++] = res;
        net_mutex_unlock(&cli->set_mtx);
        sent_any = true;
    }
    if (sent_any) client_fetch_settings(cli);
}

/* Worker thread: connect, poll /stats + /devices + /settings, stage the
 * mirror, forward queued command flags and /set requests to the server.
 * Does NOT touch is_capturing, buffers or app->settings (the main thread
 * starts/stops ingest and applies the snapshot). */
static int client_worker_thread(void *arg) {
    net_client_t *cli = (net_client_t *)arg;
    gui_app_t *app = cli->app;
    int backoff_ms = 500;
    int last_peer_state = -1;
    int miss_streak = 0;  /* consecutive /stats failures; tear down ingest only after several */
    bool first_contact = false;  /* fetch /devices + /settings immediately on first good /stats */
    while (!atomic_load(&cli->stop_flag)) {
        char stats[4096];
        size_t blen = 0;
        if (client_get(cli->host, cli->port, "/stats", stats, sizeof(stats), &blen, NULL) != 0) {
            /* Transient failure: don't immediately tear down ingest. A single
             * missed /stats (e.g. server briefly busy, network blip) used to
             * flap the whole ingest start/stop cycle. Only mark disconnected
             * and stop ingest after several consecutive misses. */
            miss_streak++;
            if (miss_streak >= 5) {
                atomic_store(&cli->connected, false);
                atomic_store(&cli->error, true);
                atomic_store(&cli->peer_state, 0);
                last_peer_state = -1;
                atomic_store(&cli->ingest_want, false);
                first_contact = false;  /* re-fetch /devices on reconnect */
            }
            thrd_sleep_ms(backoff_ms);
            if (backoff_ms < 2000) backoff_ms += 250;
            continue;
        }
        miss_streak = 0;
        atomic_store(&cli->connected, true);
        atomic_store(&cli->error, false);
        backoff_ms = 500;

        /* On the first successful /stats after (re)connect, fetch /devices
         * and /settings immediately so the dropdown and the settings panel
         * populate right away instead of waiting up to ~3s for the tick. */
        if (!first_contact) {
            first_contact = true;
            char *dj0 = (char *)malloc(16384);
            if (dj0 && client_get(cli->host, cli->port, "/devices", dj0, 16384, NULL, NULL) == 0) {
                client_apply_devices(cli, dj0);
            }
            free(dj0);
            client_fetch_settings(cli);
        }

        client_apply_stats(cli, stats);
        /* Drive ingest from the connection state, NOT peer capture state. This
         * keeps the /rf + /baseband pump streams open continuously while the
         * client is connected, so the feed doesn't flap (tear down + rebuild
         * the whole ingest) every time the server starts/stops capturing. The
         * pump threads just see no data (recv timeout) while the server is
         * idle, and data flows immediately when the server captures again. */
        atomic_store(&cli->ingest_want, atomic_load(&cli->connected));

        int st = atomic_load(&cli->peer_state);
        if (st != last_peer_state) {
            fprintf(stderr, "[NET] client: peer state -> %d (sr=%d)\n", st,
                    atomic_load(&cli->peer_sample_rate));
            last_peer_state = st;
        }

        /* Forward queued commands (drain flags the UI/main set for the client). */
        if (atomic_exchange(&app->net_cmd_start, false)) {
            (void)client_get(cli->host, cli->port, "/start", NULL, 0, NULL, NULL);
        }
        if (atomic_exchange(&app->net_cmd_stop, false)) {
            (void)client_get(cli->host, cli->port, "/stop", NULL, 0, NULL, NULL);
        }
        if (atomic_exchange(&app->net_cmd_record_on, false)) {
            (void)client_get(cli->host, cli->port, "/record?on=1", NULL, 0, NULL, NULL);
        }
        if (atomic_exchange(&app->net_cmd_record_off, false)) {
            (void)client_get(cli->host, cli->port, "/record?on=0", NULL, 0, NULL, NULL);
        }
        if (atomic_exchange(&app->net_cmd_select_device, false)) {
            int n = atomic_exchange(&app->net_cmd_device_index, 0);
            char path[64];
            snprintf(path, sizeof(path), "/device?%d", n);
            (void)client_get(cli->host, cli->port, path, NULL, 0, NULL, NULL);
        }

        /* Settings edits made on this client. */
        client_send_pending_sets(cli);

        /* Periodically refresh the device list + settings (every ~3s), or
         * sooner when asked (a /set answered, a generation bump seen). */
        static int dev_tick = 0;
        bool tick = ((dev_tick++ % 3) == 0);
        if (tick) {
            char *dj = (char *)malloc(16384);
            if (dj && client_get(cli->host, cli->port, "/devices", dj, 16384, &blen, NULL) == 0) {
                client_apply_devices(cli, dj);
            }
            free(dj);
        }
        if (tick || atomic_exchange(&cli->settings_refresh_req, false)) {
            client_fetch_settings(cli);
        }

    /* 1s poll interval: halves TCP connection churn vs 500ms while staying
     * responsive to peer state changes. */
    thrd_sleep_ms(1000);
    }
    atomic_store(&cli->ingest_want, false);
    return 0;
}

/* UDP discovery listener: bind NET_DISCOVERY_PORT (SO_REUSEADDR so multiple
 * clients on one host coexist), collect server beacons into the discovered
 * list, and prune entries older than NET_DISCOVERY_TTL_MS. Sets disc_dirty so
 * the UI knows to re-read the list. */
static int client_discovery_thread(void *arg) {
    net_client_t *cli = (net_client_t *)arg;
    net_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!net_sock_valid(fd)) return 0;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    /* SO_REUSEPORT lets multiple clients on the same host all receive broadcast
     * datagrams on this port; without it the kernel delivers each datagram to
     * only one bound socket, hiding discovered servers from the others. */
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *)&reuse, sizeof(reuse));
#endif
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(NET_DISCOVERY_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[NET] discovery: bind(:%u) failed\n", NET_DISCOVERY_PORT);
        net_close(fd);
        return 0;
    }
    net_set_timeouts(fd, 1000, 1000);  /* 1s recv timeout */
    while (!atomic_load(&cli->discovery_stop)) {
        char buf[256];
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &slen);
        if (n > 0) {
            buf[n] = '\0';
            /* Beacon format: "MISRC\n<port>\n<name>" */
            if (strncmp(buf, "MISRC\n", 6) == 0) {
                char *p = buf + 6;
                long pport = strtol(p, &p, 10);
                if (p && *p == '\n') p++;
                char name[64] = {0};
                snprintf(name, sizeof(name), "%s", p ? p : "");
                /* Strip a trailing newline from the name. */
                char *nl = strchr(name, '\n');
                if (nl) *nl = '\0';
                if (pport >= 1 && pport <= 65535) {
                    char host[64];
#ifdef _WIN32
                    snprintf(host, sizeof(host), "%u.%u.%u.%u",
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b1),
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b2),
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b3),
                             (unsigned)(src.sin_addr.S_un.S_un_b.s_b4));
#else
                    inet_ntop(AF_INET, &src.sin_addr, host, sizeof(host));
#endif
                    uint64_t now = get_time_ms();
                    net_mutex_lock(&cli->disc_mtx);
                    /* Update existing or append. */
                    int slot = -1;
                    for (int i = 0; i < cli->discovered_count; i++) {
                        if (strcmp(cli->discovered[i].host, host) == 0 &&
                            cli->discovered[i].port == (uint16_t)pport) {
                            slot = i;
                            break;
                        }
                    }
                    if (slot < 0 && cli->discovered_count < NET_MAX_DISCOVERED) {
                        slot = cli->discovered_count++;
                    }
                    if (slot >= 0) {
                        net_discovered_t *d = &cli->discovered[slot];
                        bool was_new = (d->last_seen_ms == 0);
                        snprintf(d->host, sizeof(d->host), "%s", host);
                        d->port = (uint16_t)pport;
                        snprintf(d->name, sizeof(d->name), "%s", name);
                        d->last_seen_ms = now;
                        if (was_new) {
                            fprintf(stderr, "[NET] discovery: found server %s:%u (%s)\n",
                                    d->host, (unsigned)d->port, d->name[0] ? d->name : "?");
                        }
                    }
                    atomic_store(&cli->disc_dirty, true);
                    net_mutex_unlock(&cli->disc_mtx);
                }
            }
        }
        /* Prune stale entries (not seen for > TTL). */
        uint64_t now = get_time_ms();
        net_mutex_lock(&cli->disc_mtx);
        bool changed = false;
        for (int i = 0; i < cli->discovered_count; ) {
            if (now - cli->discovered[i].last_seen_ms > NET_DISCOVERY_TTL_MS) {
                /* Remove by swapping with last. */
                cli->discovered[i] = cli->discovered[--cli->discovered_count];
                changed = true;
            } else {
                i++;
            }
        }
        if (changed) atomic_store(&cli->disc_dirty, true);
        net_mutex_unlock(&cli->disc_mtx);
    }
    net_close(fd);
    return 0;
}

/* Start just the stats/ingest worker thread (requires a known host). */
static int client_start_worker(net_client_t *cli) {
    if (atomic_load(&cli->worker_started)) return 0;
    if (!cli->host[0]) return -1;
    atomic_store(&cli->stop_flag, false);
    if (thrd_create(&cli->worker_thread, client_worker_thread, cli) != thrd_success) {
        return -1;
    }
    atomic_store(&cli->worker_started, true);
    fprintf(stderr, "[NET] client worker started -> %s:%u\n", cli->host, (unsigned)cli->port);
    return 0;
}

/* Stop just the stats/ingest worker (+ pump/ingest) thread. */
static void client_stop_worker(net_client_t *cli) {
    if (!atomic_load(&cli->worker_started)) return;
    atomic_store(&cli->stop_flag, true);
    atomic_store(&cli->ingest_want, false);
    thrd_join(cli->worker_thread, NULL);
    atomic_store(&cli->worker_started, false);
    if (atomic_load(&cli->ingest_active)) {
        client_stop_ingest(cli->app, cli);
    }
    atomic_store(&cli->connected, false);
    atomic_store(&cli->error, false);
    atomic_store(&cli->peer_state, 0);
    atomic_store(&cli->stop_flag, false);
}

static int client_start(gui_app_t *app, const char *host, uint16_t port) {
    if (s_client) return 0;
    net_client_t *cli = (net_client_t *)calloc(1, sizeof(*cli));
    if (!cli) return -1;
    cli->app = app;
    snprintf(cli->host, sizeof(cli->host), "%s", host);
    cli->port = port;
    atomic_store(&cli->running, false);
    atomic_store(&cli->stop_flag, false);
    atomic_store(&cli->connected, false);
    atomic_store(&cli->error, false);
    atomic_store(&cli->peer_state, 0);
    atomic_store(&cli->peer_sample_rate, 0);
    atomic_store(&cli->peer_device_count, 0);
    atomic_store(&cli->peer_selected, -1);
    atomic_store(&cli->peer_audio_frame_bytes, 12);
    atomic_store(&cli->ingest_want, false);
    atomic_store(&cli->ingest_active, false);
    atomic_store(&cli->pump_stop, false);
    atomic_store(&cli->worker_started, false);
    cli->staged_device_count = 0;
    cli->staged_dirty = false;
    cli->discovered_count = 0;
    atomic_store(&cli->disc_dirty, false);
    atomic_store(&cli->discovery_stop, false);
    net_mutex_init(&cli->dev_mtx);
    net_mutex_init(&cli->disc_mtx);
    net_mutex_init(&cli->set_mtx);
    cli->staged_settings_valid = false;
    cli->staged_settings_dirty = false;
    cli->staged_controls_dirty = false;
    cli->server_settings_support = -1;
    cli->staged_generation = 0;
    cli->out_count = 0;
    cli->out_seq = 0;
    cli->res_count = 0;
    cli->peer_status[0] = '\0';
    cli->peer_status_seq = 0;
    cli->peer_status_dirty = false;
    atomic_store(&cli->settings_refresh_req, false);
    atomic_store(&cli->peer_rec_pending, false);
    atomic_store(&cli->peer_rec_finalizing, false);

    /* Start the UDP discovery listener so the UI can show found servers, even
     * before a specific host is selected. */
    if (thrd_create(&cli->discovery_thread, client_discovery_thread, cli) != thrd_success) {
        fprintf(stderr, "[NET] client: discovery thread failed\n");
        net_mutex_destroy(&cli->dev_mtx);
        net_mutex_destroy(&cli->disc_mtx);
        net_mutex_destroy(&cli->set_mtx);
        free(cli);
        return -1;
    }
    /* Start the stats/ingest worker only if a host is already known. */
    if (host && host[0]) {
        (void)client_start_worker(cli);
    }
    atomic_store(&cli->running, true);
    s_client = cli;
    app->net_state = cli;
    fprintf(stderr, "[NET] client mode: discovery active%s\n",
            (host && host[0]) ? "" : " (no host selected yet)");
    return 0;
}

static void client_stop(net_client_t *cli) {
    if (!cli) return;
    atomic_store(&cli->discovery_stop, true);
    atomic_store(&cli->running, false);
    /* Stop the worker first (if running), then the discovery listener. */
    client_stop_worker(cli);
    thrd_join(cli->discovery_thread, NULL);
    if (cli->app) {
        atomic_store(&cli->app->net_connected, false);
        cli->app->net_state = NULL;
    }
    net_mutex_destroy(&cli->dev_mtx);
    net_mutex_destroy(&cli->disc_mtx);
    net_mutex_destroy(&cli->set_mtx);
    free(cli);
    s_client = NULL;
    client_view_reset();
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void gui_net_stop(gui_app_t *app) {
    if (s_server) server_stop(s_server);
    if (s_client) client_stop(s_client);
    if (app) {
        atomic_store(&app->net_connected, false);
        app->net_state = NULL;
    }
}

int gui_net_apply_mode(gui_app_t *app) {
    if (!app) return -1;
    gui_net_init_globals();
    int want = app->settings.net_mode;
    /* Stop whatever is running if it doesn't match the desired mode. */
    if (s_server && want != GUI_NET_MODE_SERVER) {
        server_stop(s_server);
    }
    if (s_client && want != GUI_NET_MODE_CLIENT) {
        client_stop(s_client);
    }
    if (want == GUI_NET_MODE_SERVER) {
        long p = atol(app->settings.net_server_port_str);
        if (p < 1 || p > 65535) { p = (long)app->settings.net_server_port; }
        if (p < 1 || p > 65535) p = 8080;
        app->settings.net_server_port = (uint16_t)p;
        if (s_server) {
            /* Already running: restart only if the listen port changed. */
            if (s_server->port != (uint16_t)p) {
                server_stop(s_server);
                if (server_start(app, (uint16_t)p) != 0) {
                    gui_net_set_status(app, "Failed to restart network server");
                    return -1;
                }
                char msg[96];
                snprintf(msg, sizeof(msg), "Network server listening on :%u", (unsigned)p);
                gui_net_set_status(app, msg);
            }
        } else {
            if (server_start(app, (uint16_t)p) != 0) {
                gui_net_set_status(app, "Failed to start network server");
                return -1;
            }
            char msg[96];
            snprintf(msg, sizeof(msg), "Network server listening on :%u", (unsigned)p);
            gui_net_set_status(app, msg);
        }
    } else if (want == GUI_NET_MODE_CLIENT) {
        long p = atol(app->settings.net_client_port_str);
        if (p < 1 || p > 65535) { p = (long)app->settings.net_client_port; }
        if (p < 1 || p > 65535) p = 8080;
        app->settings.net_client_port = (uint16_t)p;
        const char *h = app->settings.net_client_host;
        /* Ensure the client (discovery listener) is running in client mode even
         * with no host selected, so the discovered-server list populates. */
        if (!s_client) {
            if (client_start(app, h ? h : "", (uint16_t)p) != 0) {
                gui_net_set_status(app, "Failed to start network client");
                return -1;
            }
        }
        if (h && h[0]) {
            /* A host is selected: ensure the stats/ingest worker is running for
             * that host. Restart it if the target changed. */
            if (s_client && (strcmp(s_client->host, h) != 0 || s_client->port != (uint16_t)p)) {
                client_stop_worker(s_client);
                snprintf(s_client->host, sizeof(s_client->host), "%s", h);
                s_client->port = (uint16_t)p;
                if (client_start_worker(s_client) != 0) {
                    gui_net_set_status(app, "Failed to start client worker");
                    return -1;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "Network client connecting to %s:%u", h, (unsigned)p);
                gui_net_set_status(app, msg);
            } else if (s_client && !atomic_load(&s_client->worker_started)) {
                if (client_start_worker(s_client) != 0) {
                    gui_net_set_status(app, "Failed to start client worker");
                    return -1;
                }
                char msg[128];
                snprintf(msg, sizeof(msg), "Network client connecting to %s:%u", h, (unsigned)p);
                gui_net_set_status(app, msg);
            }
        } else {
            /* No host selected yet: stop the worker if any, keep discovery alive. */
            if (s_client) {
                client_stop_worker(s_client);
            }
            gui_net_set_status(app, "Client mode: scanning for servers on the LAN...");
        }
    } else {
        /* Local: ensure stopped (already handled above). */
        if (!s_server && !s_client) {
            atomic_store(&app->net_connected, false);
        }
        gui_net_set_status(app, "Local (no network)");
    }
    return 0;
}

void gui_net_poll_commands(gui_app_t *app) {
    if (!app) return;
    /* Server mode: execute queued commands locally on the main thread. */
    if (s_server) {
        server_install_tap(app);
        if (atomic_exchange(&app->net_cmd_start, false)) {
            if (!app->is_capturing) {
                fprintf(stderr, "[NET] server: executing /start\n");
                (void)gui_app_start_capture(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_stop, false)) {
            if (app->is_capturing) {
                fprintf(stderr, "[NET] server: executing /stop\n");
                gui_app_stop_capture(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_record_on, false)) {
            if (app->is_capturing && !app->is_recording) {
                fprintf(stderr, "[NET] server: executing /record on\n");
                (void)gui_app_start_recording(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_record_off, false)) {
            if (app->is_recording) {
                fprintf(stderr, "[NET] server: executing /record off\n");
                gui_app_stop_recording(app);
            }
        }
        if (atomic_exchange(&app->net_cmd_select_device, false)) {
            int n = atomic_exchange(&app->net_cmd_device_index, 0);
            if (!app->is_capturing && n >= 0 && n < app->device_count) {
                fprintf(stderr, "[NET] server: executing /device %d\n", n);
                app->selected_device = n;
            }
        }
        /* /set requests: apply here, on the main thread, with the settings
         * panel's own checks and side effects, then answer the waiting
         * handler. Answers nobody collects (a handler that timed out) are
         * capped and the oldest dropped. */
        for (;;) {
            net_set_req_t req;
            bool have = false;
            net_mutex_lock(&s_server->set_mtx);
            if (s_server->set_q_count > 0) {
                req = s_server->set_q[0];
                memmove(&s_server->set_q[0], &s_server->set_q[1],
                        (size_t)(s_server->set_q_count - 1) * sizeof(req));
                s_server->set_q_count--;
                have = true;
            }
            net_mutex_unlock(&s_server->set_mtx);
            if (!have) break;
            net_set_res_t res;
            memset(&res, 0, sizeof(res));
            res.seq = req.seq;
            res.http = gui_ui_apply_remote_setting(app, req.key, req.value, res.msg, sizeof(res.msg));
            res.generation = gui_settings_generation();
            fprintf(stderr, "[NET] server: /set %s -> %d%s%s\n", req.key, res.http,
                    res.http == 200 ? "" : ": ", res.http == 200 ? "" : res.msg);
            net_mutex_lock(&s_server->set_mtx);
            if (s_server->set_res_count >= NET_SET_QUEUE) {
                memmove(&s_server->set_res[0], &s_server->set_res[1],
                        (size_t)(NET_SET_QUEUE - 1) * sizeof(res));
                s_server->set_res_count = NET_SET_QUEUE - 1;
            }
            s_server->set_res[s_server->set_res_count++] = res;
            net_cond_broadcast(&s_server->set_cv);
            net_mutex_unlock(&s_server->set_mtx);
        }
        server_publish(s_server, app);
    }
    /* Client mode: the worker drains the command flags and forwards them; we
     * must NOT execute them locally. Nothing to do here for client. */
}

void gui_net_poll_mirror(gui_app_t *app) {
    if (!app) return;
    if (s_client) {
        /* Apply staged device list to the local app (main thread). */
        if (s_client->staged_dirty) {
            net_mutex_lock(&s_client->dev_mtx);
            int count = s_client->staged_device_count;
            if (count > MAX_DEVICES) count = MAX_DEVICES;
            for (int i = 0; i < count; i++) {
                app->devices[i] = s_client->staged_devices[i];
            }
            app->device_count = count;
            int sel = s_client->staged_selected;
            if (sel >= 0 && sel < count) app->selected_device = sel;
            s_client->staged_dirty = false;
            net_mutex_unlock(&s_client->dev_mtx);
        }
        /* Drive ingest start/stop from peer capture state (main thread). */
        bool want = atomic_load(&s_client->ingest_want) && !s_probe_no_ingest;
        bool active = atomic_load(&s_client->ingest_active);
        if (want && !active && atomic_load(&s_client->connected)) {
            client_start_ingest(app, s_client);
        } else if (!want && active) {
            client_stop_ingest(app, s_client);
        }
        /* Mirror peer sample rate into app for display. */
        int psr = atomic_load(&s_client->peer_sample_rate);
        if (psr > 0) atomic_store(&app->sample_rate, (uint32_t)psr);

        client_apply_snapshot(app, s_client);
        client_apply_set_results(app, s_client);
        client_expire_pending();

        /* The pipeline follows the server's effective mode and wiring flag,
         * never a settings field (the view swap may be replacing those). */
        if (s_peer_valid || s_peer_controls_only) {
            app->user_capture_mode_misrc = s_peer_effective_misrc;
            if (!app->is_recording) app->capture_mode_runtime_misrc = s_peer_effective_misrc;
            app->capture_ab_swap_invert = s_peer_applied.misrc_v15_v25_ab_swap;
        }

        /* Recording relay: the readouts that key on these counters show the
         * server's recording. is_recording itself is never set on a client. */
        int ps = atomic_load(&s_client->peer_state);
        atomic_store(&app->net_peer_state, ps);
        if (atomic_load(&s_client->connected)) {
            atomic_store(&app->recording_bytes, atomic_load(&s_client->peer_rec_bytes));
            atomic_store(&app->recording_raw_a, atomic_load(&s_client->peer_rec_raw_a));
            atomic_store(&app->recording_raw_b, atomic_load(&s_client->peer_rec_raw_b));
            atomic_store(&app->recording_compressed_a, atomic_load(&s_client->peer_rec_comp_a));
            atomic_store(&app->recording_compressed_b, atomic_load(&s_client->peer_rec_comp_b));
            if (ps == 2) {
                double elapsed_s = (double)atomic_load(&s_client->peer_rec_elapsed_ms) / 1000.0;
                app->recording_start_time = net_now_s() - elapsed_s;
            }
        }
        /* The server's bottom-bar message, for the bar's client-mode reader. */
        net_mutex_lock(&s_client->set_mtx);
        if (s_client->peer_status_dirty) {
            snprintf(app->net_peer_status, sizeof(app->net_peer_status), "%s", s_client->peer_status);
            app->net_peer_status_seq++;
            app->net_peer_status_time = net_now_s();
            s_client->peer_status_dirty = false;
        }
        net_mutex_unlock(&s_client->set_mtx);
    }
}

void gui_net_client_request_start(gui_app_t *app) {
    if (!app) return;
    atomic_store(&app->net_cmd_start, true);
}

void gui_net_client_request_stop(gui_app_t *app) {
    if (!app) return;
    atomic_store(&app->net_cmd_stop, true);
}

void gui_net_client_request_record(gui_app_t *app, bool on) {
    if (!app) return;
    if (on) atomic_store(&app->net_cmd_record_on, true);
    else atomic_store(&app->net_cmd_record_off, true);
}

void gui_net_client_request_device(gui_app_t *app, int device_index) {
    if (!app) return;
    atomic_store(&app->net_cmd_device_index, device_index);
    atomic_store(&app->net_cmd_select_device, true);
}

/* Discovery: return the current count of discovered servers (client mode). */
int gui_net_discovered_count(void) {
    if (!s_client) return 0;
    net_mutex_lock(&s_client->disc_mtx);
    int n = s_client->discovered_count;
    net_mutex_unlock(&s_client->disc_mtx);
    return n;
}

/* Discovery: copy discovered server #index into the caller buffers. Returns
 * false if index is out of range or not in client mode. */
bool gui_net_get_discovered(int index, char *host, size_t host_cap,
                            uint16_t *port, char *name, size_t name_cap) {
    if (!s_client) return false;
    bool ok = false;
    net_mutex_lock(&s_client->disc_mtx);
    if (index >= 0 && index < s_client->discovered_count) {
        const net_discovered_t *d = &s_client->discovered[index];
        if (host && host_cap) {
            snprintf(host, host_cap, "%s", d->host);
        }
        if (port) *port = d->port;
        if (name && name_cap) {
            snprintf(name, name_cap, "%s", d->name);
        }
        ok = true;
    }
    net_mutex_unlock(&s_client->disc_mtx);
    return ok;
}

/* Discovery: select discovered server #index as the connection target. Sets
 * the client host/port settings and re-applies mode so the worker reconnects. */
void gui_net_select_discovered(gui_app_t *app, int index) {
    if (!app || !s_client) return;
    char host[64] = {0};
    uint16_t port = 0;
    char name[64] = {0};
    if (!gui_net_get_discovered(index, host, sizeof(host), &port, name, sizeof(name))) {
        return;
    }
    snprintf(app->settings.net_client_host, sizeof(app->settings.net_client_host), "%s", host);
    app->settings.net_client_port = port;
    snprintf(app->settings.net_client_port_str, sizeof(app->settings.net_client_port_str), "%u", (unsigned)port);
    gui_settings_save(&app->settings);
    (void)gui_net_apply_mode(app);
    char msg[160];
    snprintf(msg, sizeof(msg), "Connecting to discovered server %s:%u (%s)", host, (unsigned)port, name);
    gui_net_set_status(app, msg);
}

/* True when client connection is active at the worker level (connected or
 * currently trying). Used by the UI Action button label/state. */
bool gui_net_client_connection_running(const gui_app_t *app) {
    (void)app;
    if (!s_client) return false;
    return atomic_load(&s_client->worker_started);
}

/* -------------------------------------------------------------------------
 * Client: the server's settings on this machine (main thread only).
 *
 * s_peer_applied is the last snapshot from /settings. s_peer_view is what the
 * UI pass sees and edits: gui_net_client_view_begin() swaps it into
 * app->settings (with the client's own client-local fields copied in) and
 * gui_net_client_view_end() swaps the client's own settings back, saves them
 * if the pass asked to, and turns every server-owned field the pass changed
 * into a /set. A field with a /set in flight is "pending": a snapshot does
 * not overwrite it until the server has answered and a snapshot at or past
 * the generation that answer carries has arrived (or 5 s pass). A refused
 * /set reverts the field and surfaces the reason.
 * ------------------------------------------------------------------------- */
#define NET_PENDING_MAX 192
#define NET_PENDING_TIMEOUT_MS 5000

typedef struct {
    bool active;
    bool acked;
    uint32_t seq;
    uint32_t clear_at_gen;
    uint64_t t0_ms;
} net_pending_t;

static bool s_view_active = false;
static bool s_peer_valid = false;
static bool s_peer_controls_only = false;     /* older server: mode + swap only */
static bool s_peer_effective_misrc = false;
static uint32_t s_peer_generation = 0;
static uint64_t s_peer_time_ms = 0;
static gui_settings_t s_own_backup;
static gui_settings_t s_peer_view;
static gui_settings_t s_peer_applied;
static net_pending_t s_pending[NET_PENDING_MAX];
static char s_last_set_error[256];
static uint64_t s_last_set_error_ms = 0;

static void client_view_reset(void) {
    s_peer_valid = false;
    s_peer_controls_only = false;
    s_peer_generation = 0;
    s_peer_time_ms = 0;
    memset(s_pending, 0, sizeof(s_pending));
    s_last_set_error[0] = '\0';
    s_last_set_error_ms = 0;
    /* s_view_active is left alone: a mode change inside the UI pass still
     * needs view_end to restore app->settings. */
}

static bool desc_is_remote(const gui_setting_desc_t *d) {
    return (d->flags & (GS_CLIENT_LOCAL | GS_WRITE_ONLY | GS_LOAD_ONLY | GS_NO_REMOTE_SET)) == 0;
}

/* Main thread: take a dirty staged snapshot into s_peer_applied and the view. */
static void client_apply_snapshot(gui_app_t *app, net_client_t *cli) {
    (void)app;
    if (!cli->staged_settings_dirty && !cli->staged_controls_dirty) return;
    net_mutex_lock(&cli->set_mtx);
    if (cli->staged_settings_dirty) {
        s_peer_applied = cli->staged_settings;
        s_peer_generation = cli->staged_generation;
        s_peer_time_ms = cli->staged_settings_time_ms;
        s_peer_effective_misrc = cli->staged_effective_misrc;
        cli->staged_settings_dirty = false;
        bool first = !s_peer_valid;
        s_peer_valid = true;
        s_peer_controls_only = false;
        size_t n = 0;
        const gui_setting_desc_t *t = gui_settings_table(&n);
        for (size_t i = 0; i < n && i < NET_PENDING_MAX; i++) {
            if (!desc_is_remote(&t[i])) {
                /* Client-local fields in the view come from this machine
                 * (view_begin refreshes them every frame); the rest are
                 * never sent. Keep the view's copy. */
                if (first) gui_settings_copy_field(&s_peer_view, &s_peer_applied, &t[i]);
                continue;
            }
            if (s_pending[i].active) {
                if (s_pending[i].acked && s_peer_generation >= s_pending[i].clear_at_gen) {
                    s_pending[i].active = false;   /* the server's canonical value wins now */
                } else {
                    continue;                      /* keep the edit until the server answers */
                }
            }
            gui_settings_copy_field(&s_peer_view, &s_peer_applied, &t[i]);
        }
    }
    if (cli->staged_controls_dirty) {
        s_peer_effective_misrc = cli->staged_effective_misrc;
        s_peer_applied.misrc_v15_v25_ab_swap = cli->staged_settings.misrc_v15_v25_ab_swap;
        s_peer_controls_only = !s_peer_valid;
        cli->staged_controls_dirty = false;
    }
    net_mutex_unlock(&cli->set_mtx);
}

/* Main thread: consume the worker's /set answers. */
static void client_apply_set_results(gui_app_t *app, net_client_t *cli) {
    for (;;) {
        net_set_res_t res;
        bool have = false;
        net_mutex_lock(&cli->set_mtx);
        if (cli->res_count > 0) {
            res = cli->results[0];
            memmove(&cli->results[0], &cli->results[1], (size_t)(cli->res_count - 1) * sizeof(res));
            cli->res_count--;
            have = true;
        }
        net_mutex_unlock(&cli->set_mtx);
        if (!have) break;

        size_t n = 0;
        const gui_setting_desc_t *t = gui_settings_table(&n);
        for (size_t i = 0; i < n && i < NET_PENDING_MAX; i++) {
            if (!s_pending[i].active || s_pending[i].seq != res.seq) continue;
            if (res.http == 200) {
                s_pending[i].acked = true;
                s_pending[i].clear_at_gen = res.generation;
            } else {
                /* Refused: back to what the server has, and say why. */
                gui_settings_copy_field(&s_peer_view, &s_peer_applied, &t[i]);
                s_pending[i].active = false;
                snprintf(s_last_set_error, sizeof(s_last_set_error), "%s: %s", t[i].key,
                         res.msg[0] ? res.msg : "refused");
                s_last_set_error_ms = get_time_ms();
                char msg[300];
                snprintf(msg, sizeof(msg), "Server refused %s", s_last_set_error);
                gui_net_set_status(app, msg);
            }
            break;
        }
    }
}

/* Main thread: a /set nobody answered in NET_PENDING_TIMEOUT_MS stops
 * protecting its field; the next snapshot wins. */
static void client_expire_pending(void) {
    uint64_t now = get_time_ms();
    for (size_t i = 0; i < NET_PENDING_MAX; i++) {
        if (s_pending[i].active && now - s_pending[i].t0_ms > NET_PENDING_TIMEOUT_MS) {
            s_pending[i].active = false;
        }
    }
}

void gui_net_client_view_begin(gui_app_t *app) {
    if (!app || !s_client || s_view_active || !s_peer_valid) return;
    s_own_backup = app->settings;
    /* The client-local fields are this machine's, whatever the view shows. */
    gui_settings_copy_fields(&s_peer_view, &app->settings, true);
    app->settings = s_peer_view;
    (void)gui_settings_suspend_save(true);
    s_view_active = true;
}

void gui_net_client_view_end(gui_app_t *app) {
    if (!app || !s_view_active) return;
    s_view_active = false;
    s_peer_view = app->settings;
    app->settings = s_own_backup;
    /* Client-local edits made during the pass belong to this machine. */
    gui_settings_copy_fields(&app->settings, &s_peer_view, true);
    bool requested = gui_settings_suspend_save(false);
    if (requested) gui_settings_save(&app->settings);
    if (!s_client) return;

    /* Every server-owned field the pass changed becomes a /set. A string
     * being typed into is sent when the edit ends, not per keystroke. */
    bool text_active = gui_ui_text_edit_active();
    uint64_t now = get_time_ms();
    size_t n = 0;
    const gui_setting_desc_t *t = gui_settings_table(&n);
    for (size_t i = 0; i < n && i < NET_PENDING_MAX; i++) {
        if (!desc_is_remote(&t[i])) continue;
        if (s_pending[i].active) continue;
        if (gui_settings_field_equal(&s_peer_view, &s_peer_applied, &t[i])) continue;
        if (t[i].type == GS_STR && text_active) continue;
        char v[512];
        if (!gui_settings_format_value(&s_peer_view, &t[i], v, sizeof(v))) continue;
        net_mutex_lock(&s_client->set_mtx);
        if (s_client->out_count < (int)(NET_SET_QUEUE * 2)) {
            uint32_t seq = ++s_client->out_seq;
            net_set_req_t *r = &s_client->out_q[s_client->out_count++];
            r->seq = seq;
            snprintf(r->key, sizeof(r->key), "%s", t[i].key);
            snprintf(r->value, sizeof(r->value), "%s", v);
            s_pending[i].active = true;
            s_pending[i].acked = false;
            s_pending[i].seq = seq;
            s_pending[i].clear_at_gen = 0;
            s_pending[i].t0_ms = now;
        }
        net_mutex_unlock(&s_client->set_mtx);
    }
}

bool gui_net_client_peer_settings_valid(const gui_app_t *app) {
    (void)app;
    return s_client != NULL && s_peer_valid;
}

uint32_t gui_net_client_peer_generation(const gui_app_t *app) {
    (void)app;
    return s_peer_valid ? s_peer_generation : 0;
}

double gui_net_client_peer_settings_age_s(const gui_app_t *app) {
    (void)app;
    if (!s_peer_valid || s_peer_time_ms == 0) return -1.0;
    return (double)(get_time_ms() - s_peer_time_ms) / 1000.0;
}

int gui_net_client_server_settings_support(const gui_app_t *app) {
    (void)app;
    if (!s_client) return -1;
    net_mutex_lock(&s_client->set_mtx);
    int r = s_client->server_settings_support;
    net_mutex_unlock(&s_client->set_mtx);
    return r;
}

void gui_net_client_request_settings_refresh(gui_app_t *app) {
    (void)app;
    if (s_client) atomic_store(&s_client->settings_refresh_req, true);
}

bool gui_net_client_last_set_error(const gui_app_t *app, char *buf, size_t cap, double *age_s) {
    (void)app;
    if (!s_last_set_error[0]) return false;
    if (buf && cap) snprintf(buf, cap, "%s", s_last_set_error);
    if (age_s) *age_s = (double)(get_time_ms() - s_last_set_error_ms) / 1000.0;
    return true;
}

bool gui_net_client_peer_recording(const gui_app_t *app) {
    (void)app;
    if (!s_client) return false;
    if (!atomic_load(&s_client->connected)) return false;
    return atomic_load(&s_client->peer_state) == 2;
}

bool gui_net_client_peer_record_pending(const gui_app_t *app) {
    (void)app;
    return s_client && atomic_load(&s_client->connected) && atomic_load(&s_client->peer_rec_pending);
}

bool gui_net_client_peer_record_finalizing(const gui_app_t *app) {
    (void)app;
    return s_client && atomic_load(&s_client->connected) && atomic_load(&s_client->peer_rec_finalizing);
}

uint32_t gui_net_client_peer_record_drops(const gui_app_t *app) {
    (void)app;
    return s_client ? (uint32_t)atomic_load(&s_client->peer_rec_drops) : 0;
}

uint64_t gui_net_client_peer_disk_free(const gui_app_t *app) {
    (void)app;
    return s_client ? (uint64_t)atomic_load(&s_client->peer_disk_free) : 0;
}

bool gui_net_client_peer_capturing(const gui_app_t *app) {
    (void)app;
    if (!s_client) return false;
    if (!atomic_load(&s_client->connected)) return false;
    return atomic_load(&s_client->peer_state) >= 1;
}

/* -------------------------------------------------------------------------
 * Headless modes (no window; return before InitWindow in misrc_gui.c).
 * ------------------------------------------------------------------------- */
static volatile sig_atomic_t s_headless_stop = 0;

static void headless_on_signal(int sig) {
    (void)sig;
    s_headless_stop = 1;
}

/* Exit codes: 0 ran and shut down cleanly; 2 no --config, or its net_mode is
 * not Server; 3 the server did not start (port in use?). */
int gui_net_serve_main(int seconds) {
    if (!gui_settings_override_active()) {
        fprintf(stderr, "--net-serve needs --config <path>; it will not run on the live settings file\n");
        return 2;
    }
    gui_app_t *app = (gui_app_t *)calloc(1, sizeof(*app));
    if (!app) return 2;
    atomic_store(&app->sample_rate, DEFAULT_SAMPLE_RATE);
    gui_settings_load(&app->settings);
    if (app->settings.net_mode != GUI_NET_MODE_SERVER) {
        fprintf(stderr, "--net-serve: the config's net_mode is %d, not Server (1)\n", app->settings.net_mode);
        free(app);
        return 2;
    }
    app->settings.capture_limit_seconds = 0;
    /* Starts the server: gui_app_init applies the config's net mode when a
     * --config override is active. */
    gui_app_init(app);
    gui_app_enumerate_devices(app);
    int rc = 0;
    if (!gui_net_is_server(app) || !gui_net_active(app)) {
        fprintf(stderr, "--net-serve: the server did not start (port %u in use?)\n",
                (unsigned)app->settings.net_server_port);
        rc = 3;
    } else {
        fprintf(stderr, "[NET] --net-serve: listening on :%u with %d device(s), for %s\n",
                (unsigned)app->settings.net_server_port, app->device_count,
                seconds > 0 ? "a bounded run" : "as long as it takes (SIGINT/SIGTERM stop it)");
        signal(SIGINT, headless_on_signal);
#ifdef SIGTERM
        signal(SIGTERM, headless_on_signal);
#endif
        uint64_t deadline_ms = seconds > 0 ? get_time_ms() + (uint64_t)seconds * 1000ULL : 0;
        while (!s_headless_stop && (deadline_ms == 0 || get_time_ms() < deadline_ms)) {
            gui_net_poll_commands(app);
            gui_net_poll_mirror(app);
            thrd_sleep_ms(20);
        }
        if (app->is_recording) gui_app_stop_recording(app);
        int waited = 0;
        while (gui_record_is_finalizing() && waited < 60000) {
            thrd_sleep_ms(50);
            waited += 50;
        }
        if (app->is_capturing) gui_app_stop_capture(app);
        gui_record_cleanup();
        gui_settings_save(&app->settings);
    }
    gui_app_cleanup(app);
    free(app);
    return rc;
}

/* Connect to host:port as a client without ingesting, mirror for `seconds`,
 * print one JSON line and exit. Exit codes: 0 a snapshot arrived and the
 * mirror never touched the local settings; 2 never connected; 3 the mirror
 * wrote the local settings; 4 connected to a server with /settings but no
 * snapshot arrived; 5 the server has no /settings (older build); 6 the
 * client's own is_recording became true. */
int gui_net_client_probe_main(const char *host, int port, int seconds) {
    if (!host || !host[0] || port <= 0 || port > 65535) {
        fprintf(stderr, "usage: --net-client-probe <host> <port> [seconds]\n");
        return 2;
    }
    if (seconds <= 0) seconds = 5;
    gui_app_t *app = (gui_app_t *)calloc(1, sizeof(*app));
    if (!app) return 2;
    atomic_store(&app->sample_rate, DEFAULT_SAMPLE_RATE);
    gui_settings_init_defaults(&app->settings);
    app->settings.net_mode = GUI_NET_MODE_CLIENT;
    snprintf(app->settings.net_client_host, sizeof(app->settings.net_client_host), "%s", host);
    app->settings.net_client_port = (uint16_t)port;
    snprintf(app->settings.net_client_port_str, sizeof(app->settings.net_client_port_str), "%d", port);
    gui_settings_t *before = (gui_settings_t *)malloc(sizeof(*before));
    if (!before) { free(app); return 2; }
    *before = app->settings;

    s_probe_no_ingest = true;
    gui_net_init_globals();
    int rc = 0;
    if (gui_net_apply_mode(app) != 0) {
        fprintf(stderr, "--net-client-probe: could not start the client\n");
        rc = 2;
    } else {
        uint64_t deadline_ms = get_time_ms() + (uint64_t)seconds * 1000ULL;
        while (get_time_ms() < deadline_ms) {
            gui_net_poll_mirror(app);
            thrd_sleep_ms(50);
        }
        bool connected = gui_net_active(app);
        int support = gui_net_client_server_settings_support(app);
        bool have = gui_net_client_peer_settings_valid(app);
        bool local_touched = memcmp(&app->settings, before, sizeof(*before)) != 0;
        if (!connected) rc = 2;
        else if (support == 0) rc = 5;
        else if (!have) rc = 4;
        else if (local_touched) rc = 3;
        else if (app->is_recording) rc = 6;

        const size_t cap = 40000;
        char *json = (char *)malloc(cap);
        if (json) {
            char status_esc[600];
            gui_settings_json_escape(app->net_peer_status, status_esc, sizeof(status_esc));
            int n = snprintf(json, cap,
                "{\"connected\":%s,\"settings_support\":%d,\"generation\":%u,\"peer_state\":%d,"
                "\"misrc_mode_effective\":%s,\"rec_bytes\":%llu,\"rec_drops\":%u,\"disk_free\":%llu,"
                "\"status\":\"%s\",\"local_settings_touched\":%s,\"settings\":",
                connected ? "true" : "false", support,
                (unsigned)gui_net_client_peer_generation(app), atomic_load(&app->net_peer_state),
                s_peer_effective_misrc ? "true" : "false",
                (unsigned long long)atomic_load(&app->recording_bytes),
                (unsigned)gui_net_client_peer_record_drops(app),
                (unsigned long long)gui_net_client_peer_disk_free(app),
                status_esc, local_touched ? "true" : "false");
            size_t need = 0;
            if (n > 0 && (size_t)n < cap) {
                need = have ? gui_settings_to_json(&s_peer_applied, GS_CLIENT_LOCAL | GS_WRITE_ONLY,
                                                   json + n, cap - (size_t)n)
                            : (size_t)snprintf(json + n, cap - (size_t)n, "null");
            }
            if (n > 0 && (size_t)n + need + 2 < cap) {
                json[(size_t)n + need] = '}';
                json[(size_t)n + need + 1] = '\0';
                printf("%s\n", json);
            }
            free(json);
        }
    }
    gui_net_stop(app);
    s_probe_no_ingest = false;
    free(before);
    free(app);
    return rc;
}

/* Toggle client connect/disconnect while remaining in Client mode:
 * - if currently connected, stop worker (disconnect; keep discovery alive)
 * - if disconnected, (re)start worker using current host:port settings. */
void gui_net_client_toggle_connection(gui_app_t *app) {
    if (!app) return;
    if (app->settings.net_mode != GUI_NET_MODE_CLIENT) {
        return;
    }
    gui_net_init_globals();

    long p = atol(app->settings.net_client_port_str);
    if (p < 1 || p > 65535) p = (long)app->settings.net_client_port;
    if (p < 1 || p > 65535) p = 8080;
    app->settings.net_client_port = (uint16_t)p;
    const char *h = app->settings.net_client_host;

    if (!s_client) {
        (void)gui_net_apply_mode(app);
        return;
    }

    bool connected_now = atomic_load(&s_client->connected);
    if (connected_now) {
        client_stop_worker(s_client);
        if (h && h[0]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Client disconnected from %s:%u", h, (unsigned)p);
            gui_net_set_status(app, msg);
        } else {
            gui_net_set_status(app, "Client disconnected (discovery still active)");
        }
        return;
    }

    if (!h || !h[0]) {
        gui_net_set_status(app, "Client mode: select a discovered server or enter host:port");
        return;
    }

    snprintf(s_client->host, sizeof(s_client->host), "%s", h);
    s_client->port = (uint16_t)p;
    if (atomic_load(&s_client->worker_started)) {
        client_stop_worker(s_client);
    }
    if (client_start_worker(s_client) != 0) {
        gui_net_set_status(app, "Failed to start client worker");
        return;
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "Network client connecting to %s:%u", h, (unsigned)p);
    gui_net_set_status(app, msg);
}
