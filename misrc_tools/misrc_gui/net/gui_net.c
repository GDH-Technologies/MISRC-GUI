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
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

/* -------------------------------------------------------------------------
 * Cross-platform sockets + sync primitives
 * ------------------------------------------------------------------------- */

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET net_sock_t;
  #define NET_INVALID_SOCKET INVALID_SOCKET
  #define net_close(s) closesocket(s)
  #define net_errno ((int)WSAGetLastError())
  typedef CRITICAL_SECTION net_mutex_t;
  typedef CONDITION_VARIABLE net_cond_t;
  /* Forward declare to avoid pulling windows.h into other headers. */
  extern void __stdcall InitializeCriticalSection(CRITICAL_SECTION*);
  extern void __stdcall DeleteCriticalSection(CRITICAL_SECTION*);
  extern void __stdcall EnterCriticalSection(CRITICAL_SECTION*);
  extern void __stdcall LeaveCriticalSection(CRITICAL_SECTION*);
  extern void __stdcall InitializeConditionVariable(CONDITION_VARIABLE*);
  extern int __stdcall SleepConditionVariableCS(CONDITION_VARIABLE*, CRITICAL_SECTION*, unsigned long);
  extern void __stdcall WakeAllConditionVariable(CONDITION_VARIABLE*);
  #define net_mutex_init(m)   InitializeCriticalSection(m)
  #define net_mutex_destroy(m) DeleteCriticalSection(m)
  #define net_mutex_lock(m)   EnterCriticalSection(m)
  #define net_mutex_unlock(m) LeaveCriticalSection(m)
  #define net_cond_init(c)    InitializeConditionVariable(c)
  #define net_cond_destroy(c) ((void)0)
  #define net_cond_broadcast(c) WakeAllConditionVariable(c)
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <signal.h>
  typedef int net_sock_t;
  #define NET_INVALID_SOCKET (-1)
  #define net_close(s) close(s)
  #define net_errno errno
  typedef pthread_mutex_t net_mutex_t;
  typedef pthread_cond_t net_cond_t;
  #define net_mutex_init(m)   ((void)pthread_mutex_init(m, NULL))
  #define net_mutex_destroy(m) ((void)pthread_mutex_destroy(m))
  #define net_mutex_lock(m)   ((void)pthread_mutex_lock(m))
  #define net_mutex_unlock(m) ((void)pthread_mutex_unlock(m))
  #define net_cond_init(c)    ((void)pthread_cond_init(c, NULL))
  #define net_cond_destroy(c) ((void)pthread_cond_destroy(c))
  #define net_cond_broadcast(c) ((void)pthread_cond_broadcast(c))
#endif

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
    extern __declspec(dllimport) int __stdcall CloseHandle(void*);
    if (t && *t) { CloseHandle(*t); *t = (thrd_t)0; }
#else
    if (t) pthread_detach(*t);
#endif
}

/* Set a socket non-blocking / blocking. */
static int net_set_nonblocking(net_sock_t fd) {
#ifdef _WIN32
    extern __declspec(dllimport) int __stdcall ioctlsocket(net_sock_t, long, unsigned long*);
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
    extern __declspec(dllimport) int __stdcall ioctlsocket(net_sock_t, long, unsigned long*);
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
 * Broadcast fanout: one producer (capture tap), N subscribers (/rf clients).
 * Chunks are refcounted; a chunk is freed once no subscriber references it.
 * ------------------------------------------------------------------------- */

typedef struct net_chunk {
    size_t len;
    int refcount;
    struct net_chunk *next;
    uint8_t data[];
} net_chunk_t;

typedef struct {
    net_mutex_t mtx;
    net_cond_t cond;
    net_chunk_t *head;
    net_chunk_t *tail;
    int subscribers;
    bool shutdown;
    bool initialized;
} net_fanout_t;

typedef struct {
    net_fanout_t *f;
    net_chunk_t *cur;
    size_t off;
} net_fanout_sub_t;

static void net_fanout_init(net_fanout_t *f) {
    memset(f, 0, sizeof(*f));
    net_mutex_init(&f->mtx);
    net_cond_init(&f->cond);
    f->initialized = true;
}

static void net_fanout_free_chunk(net_chunk_t *c) {
    /* Decrement refcount; free if zero. Caller may pass NULL. */
    if (!c) return;
    if (--c->refcount <= 0) {
        free(c);
    }
}

static void net_fanout_destroy(net_fanout_t *f) {
    if (!f || !f->initialized) return;
    net_mutex_lock(&f->mtx);
    f->shutdown = true;
    net_cond_broadcast(&f->cond);
    net_chunk_t *c = f->head;
    while (c) {
        net_chunk_t *n = c->next;
        /* Drop producer reference. Subscribers should already be gone. */
        c->refcount = 0;
        free(c);
        c = n;
    }
    f->head = f->tail = NULL;
    net_mutex_unlock(&f->mtx);
    net_cond_destroy(&f->cond);
    net_mutex_destroy(&f->mtx);
    f->initialized = false;
}

static void net_fanout_subscribe(net_fanout_t *f, net_fanout_sub_t *s) {
    s->f = f;
    s->cur = NULL;
    s->off = 0;
    net_mutex_lock(&f->mtx);
    f->subscribers++;
    /* Start at head (oldest buffered) if any. */
    if (f->head) {
        s->cur = f->head;
        s->cur->refcount++;
    }
    net_mutex_unlock(&f->mtx);
}

static void net_fanout_unsubscribe(net_fanout_sub_t *s) {
    if (!s || !s->f) return;
    net_fanout_t *f = s->f;
    net_mutex_lock(&f->mtx);
    if (f->subscribers > 0) f->subscribers--;
    net_chunk_t *cur = s->cur;
    s->cur = NULL;
    s->off = 0;
    net_mutex_unlock(&f->mtx);
    net_fanout_free_chunk(cur);
    s->f = NULL;
}

/* Producer: append data. If there are no subscribers, drop it (no copy/alloc). */
static void net_fanout_push(net_fanout_t *f, const void *data, size_t len) {
    if (!f || !f->initialized || len == 0) return;
    net_mutex_lock(&f->mtx);
    if (f->shutdown || f->subscribers <= 0) {
        net_mutex_unlock(&f->mtx);
        return;
    }
    net_chunk_t *c = (net_chunk_t *)malloc(sizeof(net_chunk_t) + len);
    if (!c) {
        net_mutex_unlock(&f->mtx);
        return;
    }
    c->len = len;
    c->refcount = f->subscribers; /* each current subscriber will read it */
    c->next = NULL;
    memcpy(c->data, data, len);
    if (f->tail) f->tail->next = c; else f->head = c;
    f->tail = c;
    net_cond_broadcast(&f->cond);
    net_mutex_unlock(&f->mtx);
}

/* Subscriber: read up to cap bytes into dst. Blocks up to timeout_ms for data.
 * Returns bytes read (0 if timeout with no data), -1 on shutdown/EOF. */
static ssize_t net_fanout_read(net_fanout_sub_t *s, void *dst, size_t cap, int timeout_ms) {
    if (!s || !s->f) return -1;
    net_fanout_t *f = s->f;
    size_t copied = 0;
    net_mutex_lock(&f->mtx);
    while (cap > 0) {
        if (s->cur && s->off >= s->cur->len) {
            /* Advance to next chunk, release current. */
            net_chunk_t *next = s->cur->next;
            if (next) next->refcount++;
            net_chunk_t *done = s->cur;
            s->cur = next;
            s->off = 0;
            net_mutex_unlock(&f->mtx);
            net_fanout_free_chunk(done);
            net_mutex_lock(&f->mtx);
            continue;
        }
        if (s->cur) {
            size_t avail = s->cur->len - s->off;
            size_t n = (avail < cap) ? avail : cap;
            memcpy((char *)dst + copied, s->cur->data + s->off, n);
            s->off += n;
            copied += n;
            cap -= n;
            if (cap == 0) break;
            continue;
        }
        /* No current chunk: wait for producer or shutdown. */
        if (f->shutdown) {
            break;
        }
        if (timeout_ms <= 0) {
            break;
        }
        /* Wait with timeout. */
#ifdef _WIN32
        if (!SleepConditionVariableCS(&f->cond, &f->mtx, (unsigned long)timeout_ms)) {
            break;
        }
#else
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int rc = pthread_cond_timedwait(&f->cond, &f->mtx, &ts);
        if (rc == ETIMEDOUT) {
            timeout_ms = 0;
            break;
        }
#endif
        /* Loop back: maybe a new chunk arrived or shutdown. */
        if (!s->cur && f->head) {
            s->cur = f->head;
            s->cur->refcount++;
        }
        if (f->shutdown) break;
    }
    net_mutex_unlock(&f->mtx);
    return (ssize_t)copied;
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
} net_server_t;

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
    /* Prefer the dedicated net_status buffer (written via gui_net_set_status).
     * Fall back to a computed string only if net_status is empty (e.g. the
     * app just started in Local mode and nothing has written it yet). */
    if (app->net_status[0]) {
        snprintf(buf, len, "%s", app->net_status);
        return;
    }
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
        } else if (atomic_load(&s_client->error)) {
            snprintf(buf, len, "Connection error: %s:%u", s_client->host, (unsigned)s_client->port);
        } else if (s_client->host[0]) {
            snprintf(buf, len, "Connecting to %s:%u ...", s_client->host, (unsigned)s_client->port);
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
    snprintf(buf, len, "Local (no network)");
}

/* -------------------------------------------------------------------------
 * Server: HTTP request handling
 * ------------------------------------------------------------------------- */

/* Build /stats JSON into buf. Reads gui_app_t atomics + selected device. */
static void server_build_stats(gui_app_t *app, char *buf, size_t len) {
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
    snprintf(buf, len,
        "{\"state\":%d,\"sample_rate\":%u,\"total_samples\":%llu,"
        "\"frames\":%u,\"errors\":%u,\"selected_device\":%d,"
        "\"device_count\":%d,\"device_name\":\"%s\",\"device_type\":%d,"
        "\"audio_frame_bytes\":%d}",
        state, (unsigned)sr, (unsigned long long)total,
        (unsigned)frames, (unsigned)errors, sel,
        dcount, dname, dtype, audio_frame);
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
    snprintf(buf, len,
        "{\"misrc_mode\":%s,\"rf_bits_a\":%u,\"rf_bits_b\":%u,"
        "\"cxadc_tenbit_a\":%s,\"cxadc_tenbit_b\":%s,"
        "\"resample_a\":%s,\"resample_b\":%s,"
        "\"resample_rate_a\":%.1f,\"resample_rate_b\":%.1f,"
        "\"use_flac\":%s,\"flac_level\":%d}",
        app->settings.misrc_mode ? "true" : "false",
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
static void server_handle_request(gui_app_t *app, net_sock_t fd, const char *method, char *uri) {
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
        char json[512];
        server_build_stats(app, json, sizeof(json));
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
        net_fanout_t *f = (uri[1] == 'r') ? &s_server->rf : &s_server->audio;
        atomic_int *ctr = (uri[1] == 'r') ? &s_server->rf_clients : &s_server->audio_clients;
        atomic_fetch_add(ctr, 1);
        net_fanout_sub_t sub;
        net_fanout_subscribe(f, &sub);
        uint8_t buf[64 * 1024];
        for (;;) {
            ssize_t n = net_fanout_read(&sub, buf, sizeof(buf), 1000);
            if (n < 0) break;
            if (n == 0) {
                /* No data for 1s; check socket still alive with a zero-byte probe is
                 * awkward cross-platform, so just keep going. */
                continue;
            }
            if (net_send_all(fd, buf, (size_t)n) != 0) break;
        }
        net_fanout_unsubscribe(&sub);
        atomic_fetch_sub(ctr, 1);
        return;
    }
    net_send_str(fd, "HTTP/1.0 404 Not Found\r\n\r\n");
}

/* Per-client thread: read one request, serve it, close. For /rf + /baseband
 * the serve loops until the client disconnects. */
static int server_client_thread(void *arg) {
    typedef struct { gui_app_t *app; net_sock_t fd; } ctx_t;
    ctx_t *c = (ctx_t *)arg;
    gui_app_t *app = c->app;
    net_sock_t fd = c->fd;
    free(c);

    /* Bound recv/send so a stuck/missing client cannot hold this (detached)
     * thread forever on a blocking call. */
    net_set_timeouts(fd, 5000, 5000);

    char buf[0x1000];
    int len = net_read_headers(fd, buf, sizeof(buf));
    if (len <= 0) {
        net_close(fd);
        return 0;
    }
    char method[8] = {0};
    char uri[256] = {0};
    int v1 = 0, v2 = 0;
    if (4 != sscanf(buf, "%7s %255s HTTP/%d.%d", method, uri, &v1, &v2)) {
        net_send_str(fd, "HTTP/1.0 400 Bad Request\r\n\r\n");
        net_close(fd);
        return 0;
    }
    server_handle_request(app, fd, method, uri);
    net_close(fd);
    return 0;
}

static int server_listen_thread(void *arg) {
    net_server_t *srv = (net_server_t *)arg;
    gui_app_t *app = srv->app;
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
        typedef struct { gui_app_t *app; net_sock_t fd; } ctx_t;
        ctx_t *c = (ctx_t *)malloc(sizeof(ctx_t));
        if (!c) { net_close(cfd); continue; }
        c->app = app;
        c->fd = cfd;
        thrd_t t;
        if (thrd_create(&t, server_client_thread, c) != thrd_success) {
            net_close(cfd);
            free(c);
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
    net_fanout_init(&srv->rf);
    net_fanout_init(&srv->audio);

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
    fprintf(stderr, "[NET] server listening on :%u\n", (unsigned)port);
    return 0;
}

static void server_stop(net_server_t *srv) {
    if (!srv) return;
    atomic_store(&srv->stop_flag, true);
    atomic_store(&srv->running, false);
    atomic_store(&srv->beacon_stop, true);
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
    thrd_join(srv->listen_thread, NULL);
    thrd_join(srv->beacon_thread, NULL);
    /* Wake any /rf + /baseband streaming subscribers so they exit. */
    net_fanout_destroy(&srv->rf);
    net_fanout_destroy(&srv->audio);
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

/* Send "GET <path> HTTP/1.0\r\n\r\n", read headers, return the body starting
 * offset in *body_off and leave the rest of the header buffer in buf. Copies
 * any body bytes already read into body_out (up to body_cap). Returns 0 on
 * success, -1 on error. */
static int client_get(const char *host, uint16_t port, const char *path,
                      char *body_out, size_t body_cap, size_t *body_len) {
    net_sock_t fd = client_connect(host, port);
    if (!net_sock_valid(fd)) return -1;
    char req[512];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n\r\n", path, host);
    if (net_send_str(fd, req) != 0) { net_close(fd); return -1; }
    /* Read headers + any body that arrives in the same recv loop. */
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
        if (!got_headers) {
            char *eoh = strstr(hbuf, "\r\n\r\n");
            if (eoh) {
                got_headers = 1;
                body_off = (size_t)(eoh + 4 - hbuf);
                break;
            }
        }
    }
    int rc = -1;
    if (got_headers) {
        /* Verify 200 status. */
        if (strncmp(hbuf, "HTTP/1.0 200", 12) == 0 || strncmp(hbuf, "HTTP/1.1 200", 12) == 0) {
            size_t have = total - body_off;
            if (body_out && body_cap) {
                size_t copy = (have < body_cap) ? have : body_cap - 1;
                memcpy(body_out, hbuf + body_off, copy);
                body_out[copy] = '\0';
                if (body_len) *body_len = copy;
            }
            rc = 0;
        }
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
     * bufmgr_reset also rewinds head/tail to 0. */
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

/* Worker thread: connect, poll /stats + /devices + /controls, mirror, and
 * forward queued command flags to the server. Does NOT touch is_capturing or
 * buffers directly (the main thread starts/stops ingest on state changes). */
static int client_worker_thread(void *arg) {
    net_client_t *cli = (net_client_t *)arg;
    gui_app_t *app = cli->app;
    int backoff_ms = 500;
    int last_peer_state = -1;
    int miss_streak = 0;  /* consecutive /stats failures; tear down ingest only after several */
    bool first_contact = false;  /* fetch /devices immediately on first good /stats */
    while (!atomic_load(&cli->stop_flag)) {
        char stats[1024];
        size_t blen = 0;
        if (client_get(cli->host, cli->port, "/stats", stats, sizeof(stats), &blen) != 0) {
            /* Transient failure: don't immediately tear down ingest. A single
             * missed /stats (e.g. server briefly busy, network blip) used to
             * flap the whole ingest start/stop cycle. Only mark disconnected
             * and stop ingest after several consecutive misses. */
            miss_streak++;
            if (miss_streak >= 5) {
                atomic_store(&cli->connected, false);
                atomic_store(&cli->error, true);
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
         * immediately so the toolbar device dropdown populates right away
         * instead of waiting up to ~3s for the periodic tick. */
        if (!first_contact) {
            first_contact = true;
            char *dj0 = (char *)malloc(16384);
            size_t blen0 = 0;
            if (dj0 && client_get(cli->host, cli->port, "/devices", dj0, 16384, &blen0) == 0) {
                int count = json_int(dj0, "count", 0);
                int selected = json_int(dj0, "selected", -1);
                if (count < 0) count = 0;
                if (count > MAX_DEVICES) count = MAX_DEVICES;
                net_mutex_lock(&cli->dev_mtx);
                cli->staged_device_count = 0;
                const char *p = dj0;
                for (int i = 0; i < count; i++) {
                    const char *nm = strstr(p, "\"name\":\"");
                    if (!nm) break;
                    const char *idx = strstr(p, "\"index\":");
                    int ti = idx ? atoi(idx + 8) : i;
                    char name[80];
                    json_str(nm, "name", name, sizeof(name));
                    device_info_t *d = &cli->staged_devices[cli->staged_device_count];
                    snprintf(d->name, sizeof(d->name), "%s", name);
                    d->serial[0] = '\0';
                    d->type = DEVICE_TYPE_SIMPLE_CAPTURE;  /* generic remote tag */
                    d->index = ti;
                    cli->staged_device_count++;
                    p = nm + 8;
                }
                cli->staged_selected = selected;
                cli->staged_dirty = true;
                net_mutex_unlock(&cli->dev_mtx);
            }
            free(dj0);
        }

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
        /* Drive ingest from the connection state, NOT peer capture state. This
         * keeps the /rf + /baseband pump streams open continuously while the
         * client is connected, so the feed doesn't flap (tear down + rebuild
         * the whole ingest) every time the server starts/stops capturing. The
         * pump threads just see no data (recv timeout) while the server is
         * idle, and data flows immediately when the server captures again. */
        atomic_store(&cli->ingest_want, atomic_load(&cli->connected));

        if (st != last_peer_state) {
            fprintf(stderr, "[NET] client: peer state -> %d (sr=%d)\n", st, sr);
            last_peer_state = st;
        }

        /* Forward queued commands (drain flags the UI/main set for the client). */
        if (atomic_exchange(&app->net_cmd_start, false)) {
            (void)client_get(cli->host, cli->port, "/start", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_stop, false)) {
            (void)client_get(cli->host, cli->port, "/stop", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_record_on, false)) {
            (void)client_get(cli->host, cli->port, "/record?on=1", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_record_off, false)) {
            (void)client_get(cli->host, cli->port, "/record?on=0", NULL, 0, NULL);
        }
        if (atomic_exchange(&app->net_cmd_select_device, false)) {
            int n = atomic_exchange(&app->net_cmd_device_index, 0);
            char path[64];
            snprintf(path, sizeof(path), "/device?%d", n);
            (void)client_get(cli->host, cli->port, path, NULL, 0, NULL);
        }

        /* Periodically refresh the device list + controls (every ~3s). */
        static int dev_tick = 0;
        if ((dev_tick++ % 3) == 0) {
            char *dj = (char *)malloc(16384);
            if (dj && client_get(cli->host, cli->port, "/devices", dj, 16384, &blen) == 0) {
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
            free(dj);

            char cj[512];
            if (client_get(cli->host, cli->port, "/controls", cj, sizeof(cj), &blen) == 0) {
                /* Mirror the server's capture controls into local settings so
                 * the client UI reflects the master's configuration. We write
                 * here (worker thread); the UI/main reads next frame. These are
                 * the master-owned settings the slave must follow. */
                bool mm = json_bool(cj, "misrc_mode", false);
                app->settings.misrc_mode = mm;
                app->user_capture_mode_misrc = mm;
                int rba = json_int(cj, "rf_bits_a", 16);
                int rbb = json_int(cj, "rf_bits_b", 16);
                if (rba == 8 || rba == 12 || rba == 16) app->settings.rf_bits_a = (uint8_t)rba;
                if (rbb == 8 || rbb == 12 || rbb == 16) app->settings.rf_bits_b = (uint8_t)rbb;
                app->settings.cxadc_tenbit_mode_card[0] = json_bool(cj, "cxadc_tenbit_a", false);
                app->settings.cxadc_tenbit_mode_card[1] = json_bool(cj, "cxadc_tenbit_b", false);
                app->settings.enable_resample_a = json_bool(cj, "resample_a", false);
                app->settings.enable_resample_b = json_bool(cj, "resample_b", false);
                int rra = json_int(cj, "resample_rate_a", 0);
                int rrb = json_int(cj, "resample_rate_b", 0);
                if (rra > 0) app->settings.resample_rate_a = (float)rra;
                if (rrb > 0) app->settings.resample_rate_b = (float)rrb;
                app->settings.use_flac = json_bool(cj, "use_flac", true);
                int fl = json_int(cj, "flac_level", -1);
                if (fl >= 0 && fl <= 8) app->settings.flac_level = fl;
            }
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

    /* Start the UDP discovery listener so the UI can show found servers, even
     * before a specific host is selected. */
    if (thrd_create(&cli->discovery_thread, client_discovery_thread, cli) != thrd_success) {
        fprintf(stderr, "[NET] client: discovery thread failed\n");
        net_mutex_destroy(&cli->dev_mtx);
        net_mutex_destroy(&cli->disc_mtx);
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
    free(cli);
    s_client = NULL;
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
    }
    return 0;
}

void gui_net_tap_rf(gui_app_t *app, const void *data, size_t bytes) {
    (void)app;
    if (s_server && s_server->rf.initialized) {
        net_fanout_push(&s_server->rf, data, bytes);
    }
}

void gui_net_tap_audio(gui_app_t *app, const void *data, size_t bytes) {
    (void)app;
    if (s_server && s_server->audio.initialized) {
        net_fanout_push(&s_server->audio, data, bytes);
    }
}

void gui_net_poll_commands(gui_app_t *app) {
    if (!app) return;
    /* Server mode: execute queued commands locally on the main thread. */
    if (s_server) {
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
        bool want = atomic_load(&s_client->ingest_want);
        bool active = atomic_load(&s_client->ingest_active);
        if (want && !active && atomic_load(&s_client->connected)) {
            client_start_ingest(app, s_client);
        } else if (!want && active) {
            client_stop_ingest(app, s_client);
        }
        /* Mirror peer sample rate into app for display. */
        int psr = atomic_load(&s_client->peer_sample_rate);
        if (psr > 0) atomic_store(&app->sample_rate, (uint32_t)psr);
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
