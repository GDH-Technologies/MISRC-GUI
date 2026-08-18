/*
 * MISRC GUI - reference video recording. See gui_video_record.h for the shape.
 */

#include "gui_video_record.h"
#include "../input/gui_preview_v4l2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <time.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

/* 16 frames of 720x576 YUYV is 13.3 MB and 640 ms of runway at 25 fps. The
 * socket already absorbs ~400 ms, and x264 runs well over realtime, so this
 * only has to cover a scheduler or filesystem hiccup. */
#define VR_RING_FRAMES     16
#define VR_SNDBUF_BYTES    (4 << 20)
#define VR_DRAIN_LIMIT_S   5.0
#define VR_REAP_LIMIT_S    10.0
#define VR_STAT_INTERVAL_S 1.0

static struct {
    /* --- ring: producer is the preview capture thread --- */
    uint8_t  *slots[VR_RING_FRAMES];
    uint8_t  *last_frame;          /* what a duplicate re-sends */
    size_t    frame_bytes;
    atomic_uint_fast32_t head;     /* producer */
    atomic_uint_fast32_t tail;     /* consumer */
    atomic_uint_fast32_t pending_dupes;

    /* --- transport --- */
    int       sock;                /* our end; -1 once closed */
    int       wake_fd;
    int       child_pid;

    /* --- writer thread --- */
    pthread_t thread;
    bool      thread_running;
    atomic_bool run;

    /* --- geometry --- */
    uint32_t  width, height, pitch, fps_num, fps_den;

    /* --- status --- */
    atomic_flag lock;
    gui_video_record_status_t status;

    char ffmpeg_log[600];
    preview_tap_t tap;
} vr;

/* ffmpeg probe cache */
static struct {
    bool probed;
    bool found;
    char path[512];
    char version[80];
    bool has_x264, has_ffv1;
} ff;

static double vr_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void vr_lock(void)   { while (atomic_flag_test_and_set(&vr.lock)) {} }
static void vr_unlock(void) { atomic_flag_clear(&vr.lock); }

static void vr_set_error(const char *what, int err)
{
    vr_lock();
    vr.status.error = true;
    if (err) snprintf(vr.status.err_text, sizeof(vr.status.err_text), "%s: %s", what, strerror(err));
    else     snprintf(vr.status.err_text, sizeof(vr.status.err_text), "%s", what);
    vr_unlock();
}

gui_video_record_status_t gui_video_record_get_status(void)
{
    vr_lock();
    gui_video_record_status_t copy = vr.status;
    vr_unlock();
    return copy;
}

uint64_t gui_video_record_output_bytes(void)
{
    vr_lock();
    uint64_t b = vr.status.output_bytes;
    vr_unlock();
    return b;
}

bool gui_video_record_is_running(void)
{
    vr_lock();
    bool r = vr.status.running;
    vr_unlock();
    return r;
}

/* ------------------------------------------------------------ ffmpeg probe */

static bool vr_is_executable(const char *p)
{
    return p && p[0] && access(p, X_OK) == 0;
}

/* PATH is searched by hand rather than using posix_spawnp, because the
 * absolute path must be known before a recording starts: that is what lets a
 * refusal be clean and lets the log name which binary was chosen. */
static bool vr_find_in_path(char *out, size_t cap)
{
    const char *path = getenv("PATH");
    if (!path || !path[0]) return false;
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0 && len < cap - 16) {
            char cand[512];
            snprintf(cand, sizeof(cand), "%.*s/ffmpeg", (int)len, p);
            if (vr_is_executable(cand)) { snprintf(out, cap, "%s", cand); return true; }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return false;
}

static bool vr_resolve_ffmpeg(char *out, size_t cap)
{
    const char *env = getenv("MISRC_FFMPEG");
    if (vr_is_executable(env)) { snprintf(out, cap, "%s", env); return true; }
    if (vr_find_in_path(out, cap)) return true;

    char selfdir[512];
    ssize_t n = readlink("/proc/self/exe", selfdir, sizeof(selfdir) - 1);
    if (n > 0) {
        selfdir[n] = '\0';
        char *slash = strrchr(selfdir, '/');
        if (slash) {
            *slash = '\0';
            char cand[600];
            snprintf(cand, sizeof(cand), "%s/ffmpeg", selfdir);
            if (vr_is_executable(cand)) { snprintf(out, cap, "%s", cand); return true; }
        }
    }
    const char *fallbacks[] = { "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" };
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (vr_is_executable(fallbacks[i])) { snprintf(out, cap, "%s", fallbacks[i]); return true; }
    }
    return false;
}

bool gui_video_record_probe(void)
{
    if (ff.probed) return ff.found;
    ff.probed = true;
    memset(ff.path, 0, sizeof(ff.path));

    if (!vr_resolve_ffmpeg(ff.path, sizeof(ff.path))) {
        ff.found = false;
        return false;
    }

    /* A binary being present does not mean it can encode what we need -- a
     * distro build may well lack libx264. Ask it. */
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "\"%s\" -hide_banner -encoders 2>/dev/null", ff.path);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, " libx264 ")) ff.has_x264 = true;
            if (strstr(line, " ffv1 "))    ff.has_ffv1 = true;
        }
        pclose(fp);
    }

    snprintf(cmd, sizeof(cmd), "\"%s\" -hide_banner -version 2>/dev/null", ff.path);
    fp = popen(cmd, "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            snprintf(ff.version, sizeof(ff.version), "%s", line);
        }
        pclose(fp);
    }

    ff.found = ff.has_x264 || ff.has_ffv1;
    return ff.found;
}

const char *gui_video_record_ffmpeg_path(void)    { gui_video_record_probe(); return ff.path; }
const char *gui_video_record_ffmpeg_version(void) { gui_video_record_probe(); return ff.version; }

bool gui_video_record_codec_available(video_codec_t codec)
{
    gui_video_record_probe();
    return (codec == VIDEO_CODEC_FFV1) ? ff.has_ffv1 : ff.has_x264;
}

/* --------------------------------------------------------------- the tap */

/* Runs on the preview capture thread between DQBUF and QBUF. Everything here
 * is bounded: a memcpy, two atomics and one eventfd write. The ring being full
 * is a compare-and-return, which is what makes a hung encoder unable to stall
 * the capture thread. */
static void vr_tap_cb(const uint8_t *yuyv, size_t pitch, uint32_t w, uint32_t h, void *user)
{
    (void)user;
    size_t want = pitch * (size_t)h;
    if (w != vr.width || h != vr.height || want != vr.frame_bytes) {
        return;   /* geometry changed under us; drop rather than write short */
    }

    uint_fast32_t head = atomic_load_explicit(&vr.head, memory_order_relaxed);
    uint_fast32_t tail = atomic_load_explicit(&vr.tail, memory_order_acquire);

    if (head - tail >= VR_RING_FRAMES) {
        /* Count a duplicate rather than just a loss: ffmpeg assigns PTS by
         * frames read, so a silently missing frame shortens the file and
         * slides it out of step with the RF timeline. */
        atomic_fetch_add_explicit(&vr.pending_dupes, 1, memory_order_relaxed);
        vr_lock();
        vr.status.frames_dropped++;
        vr_unlock();
        return;
    }

    memcpy(vr.slots[head % VR_RING_FRAMES], yuyv, vr.frame_bytes);
    atomic_store_explicit(&vr.head, head + 1, memory_order_release);

    vr_lock();
    vr.status.frames_submitted++;
    vr_unlock();

    uint64_t one = 1;
    ssize_t wrote = write(vr.wake_fd, &one, sizeof(one));
    (void)wrote;
}

/* ------------------------------------------------------------ writer thread */

/* MSG_NOSIGNAL is why this is a socketpair and not a pipe: a dead ffmpeg
 * returns EPIPE here instead of raising a process-wide SIGPIPE, in a process
 * that deliberately installs no signal handlers. */
static bool vr_send_all(const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(vr.sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        vr_set_error("reference video encoder stopped reading", errno);
        return false;
    }
    return true;
}

static void *vr_writer_thread(void *arg)
{
    (void)arg;
    double last_stat = 0.0;
    double drain_started = 0.0;
    bool   have_last = false;

    for (;;) {
        bool running = atomic_load_explicit(&vr.run, memory_order_acquire);
        uint_fast32_t tail = atomic_load_explicit(&vr.tail, memory_order_relaxed);
        uint_fast32_t head = atomic_load_explicit(&vr.head, memory_order_acquire);

        if (tail == head) {
            if (!running) break;              /* stop requested and drained */
            struct pollfd pfd = { vr.wake_fd, POLLIN, 0 };
            poll(&pfd, 1, 20);
            uint64_t sink;
            while (read(vr.wake_fd, &sink, sizeof(sink)) > 0) { }
            continue;
        }

        if (!running) {
            /* Bound the drain so a stalled encoder cannot hold finalize open. */
            if (drain_started == 0.0) drain_started = vr_now();
            else if (vr_now() - drain_started > VR_DRAIN_LIMIT_S) break;
        }

        double now = vr_now();
        if (now - last_stat >= VR_STAT_INTERVAL_S) {
            /* stat() the output rather than counting piped bytes: piped bytes
             * are raw and bear no relation to the encoded file. Done here, on
             * the thread that already blocks on I/O -- never in the disk
             * guard, which runs on the RF hot path. */
            struct stat st;
            if (vr.status.path[0] && stat(vr.status.path, &st) == 0) {
                vr_lock();
                vr.status.output_bytes = (uint64_t)st.st_size;
                vr_unlock();
            }
            last_stat = now;
        }

        /* Duplicates first, so they occupy the timeline slots the dropped
         * frames would have. */
        uint_fast32_t dupes = atomic_exchange_explicit(&vr.pending_dupes, 0, memory_order_relaxed);
        if (dupes && have_last) {
            for (uint_fast32_t i = 0; i < dupes; i++) {
                if (!vr_send_all(vr.last_frame, vr.frame_bytes)) goto out;
                vr_lock();
                vr.status.frames_duped++;
                vr.status.frames_written++;
                vr_unlock();
            }
        }

        const uint8_t *slot = vr.slots[tail % VR_RING_FRAMES];
        if (!vr_send_all(slot, vr.frame_bytes)) goto out;

        memcpy(vr.last_frame, slot, vr.frame_bytes);
        have_last = true;
        atomic_store_explicit(&vr.tail, tail + 1, memory_order_release);

        vr_lock();
        vr.status.frames_written++;
        vr_unlock();
    }

out:
    /* Closing our end delivers EOF, so ffmpeg flushes its encoder and writes a
     * real trailer instead of being killed mid-file. */
    if (vr.sock >= 0) { close(vr.sock); vr.sock = -1; }
    return NULL;
}

/* ----------------------------------------------------------------- spawn */

static void vr_build_argv(char *argv[], int *argc_out, video_codec_t codec,
                          char *size_buf, size_t size_cap,
                          char *rate_buf, size_t rate_cap,
                          const char *out_path)
{
    snprintf(size_buf, size_cap, "%ux%u", vr.width, vr.height);
    if (vr.fps_den <= 1) snprintf(rate_buf, rate_cap, "%u", vr.fps_num);
    else                 snprintf(rate_buf, rate_cap, "%u/%u", vr.fps_num, vr.fps_den);

    int n = 0;
    argv[n++] = ff.path;
    argv[n++] = (char *)"-hide_banner";
    argv[n++] = (char *)"-nostdin";
    argv[n++] = (char *)"-nostats";
    argv[n++] = (char *)"-loglevel"; argv[n++] = (char *)"warning";
    argv[n++] = (char *)"-y";
    argv[n++] = (char *)"-f";             argv[n++] = (char *)"rawvideo";
    argv[n++] = (char *)"-pixel_format";  argv[n++] = (char *)"yuyv422";
    argv[n++] = (char *)"-video_size";    argv[n++] = size_buf;
    argv[n++] = (char *)"-framerate";     argv[n++] = rate_buf;
    argv[n++] = (char *)"-i";             argv[n++] = (char *)"-";
    argv[n++] = (char *)"-an";
    argv[n++] = (char *)"-sn";
    /* 720x576 is non-square PAL; without this every player shows a 5:4
     * stretch. Yields SAR 16:15 DAR 4:3. */
    argv[n++] = (char *)"-aspect";        argv[n++] = (char *)"4:3";

    if (codec == VIDEO_CODEC_FFV1) {
        argv[n++] = (char *)"-c:v";     argv[n++] = (char *)"ffv1";
        argv[n++] = (char *)"-level";   argv[n++] = (char *)"3";
        argv[n++] = (char *)"-coder";   argv[n++] = (char *)"1";
        argv[n++] = (char *)"-context"; argv[n++] = (char *)"1";
        argv[n++] = (char *)"-g";       argv[n++] = (char *)"1";
        argv[n++] = (char *)"-slices";  argv[n++] = (char *)"4";
        argv[n++] = (char *)"-slicecrc";argv[n++] = (char *)"1";
    } else {
        argv[n++] = (char *)"-c:v";     argv[n++] = (char *)"libx264";
        argv[n++] = (char *)"-preset";  argv[n++] = (char *)"veryfast";
        argv[n++] = (char *)"-crf";     argv[n++] = (char *)"18";
        argv[n++] = (char *)"-g";       argv[n++] = (char *)"25";
    }
    argv[n++] = (char *)"-pix_fmt"; argv[n++] = (char *)"yuv422p";
    /* Capped so the encoder cannot compete with FLAC level-8 RF encode. */
    argv[n++] = (char *)"-threads"; argv[n++] = (char *)"2";
    argv[n++] = (char *)"-f";       argv[n++] = (char *)"matroska";
    /* Bounds what a SIGKILLed session loses to the current cluster. */
    argv[n++] = (char *)"-cluster_time_limit"; argv[n++] = (char *)"1000";
    argv[n++] = (char *)"-flush_packets";      argv[n++] = (char *)"1";
    argv[n++] = (char *)out_path;
    argv[n]   = NULL;
    *argc_out = n;
}

static void vr_free_ring(void)
{
    for (int i = 0; i < VR_RING_FRAMES; i++) { free(vr.slots[i]); vr.slots[i] = NULL; }
    free(vr.last_frame); vr.last_frame = NULL;
}

int gui_video_record_start(const char *out_path, video_codec_t codec,
                           uint32_t width, uint32_t height, uint32_t pitch,
                           uint32_t fps_num, uint32_t fps_den,
                           char *err, size_t err_cap)
{
    if (vr.thread_running) return 0;
    if (!out_path || !out_path[0]) {
        snprintf(err, err_cap, "no output path for the reference video");
        return -1;
    }
    if (!gui_video_record_probe()) {
        snprintf(err, err_cap, "ffmpeg was not found; set its path in Settings or turn "
                               "Reference video off");
        return -1;
    }
    if (!gui_video_record_codec_available(codec)) {
        snprintf(err, err_cap, "%s cannot encode %s", ff.path,
                 codec == VIDEO_CODEC_FFV1 ? "FFV1" : "H.264");
        return -1;
    }

    memset(&vr.status, 0, sizeof(vr.status));
    atomic_flag_clear(&vr.lock);
    vr.width = width; vr.height = height; vr.pitch = pitch;
    vr.fps_num = fps_num ? fps_num : 25;
    vr.fps_den = fps_den ? fps_den : 1;
    vr.frame_bytes = (size_t)pitch * height;
    vr.sock = -1;
    vr.wake_fd = -1;
    vr.child_pid = 0;
    atomic_store(&vr.head, 0);
    atomic_store(&vr.tail, 0);
    atomic_store(&vr.pending_dupes, 0);
    snprintf(vr.status.path, sizeof(vr.status.path), "%s", out_path);
    snprintf(vr.ffmpeg_log, sizeof(vr.ffmpeg_log), "%s.ffmpeg.log", out_path);

    for (int i = 0; i < VR_RING_FRAMES; i++) {
        vr.slots[i] = malloc(vr.frame_bytes);
        if (!vr.slots[i]) { vr_free_ring(); snprintf(err, err_cap, "out of memory for the video ring"); return -1; }
    }
    vr.last_frame = calloc(1, vr.frame_bytes);
    if (!vr.last_frame) { vr_free_ring(); snprintf(err, err_cap, "out of memory for the video ring"); return -1; }

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        vr_free_ring();
        snprintf(err, err_cap, "could not create the encoder socket: %s", strerror(errno));
        return -1;
    }
    /* The default 208 KB is a quarter of one frame; without this every send
     * round-trips to ffmpeg's read loop. */
    int want = VR_SNDBUF_BYTES;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));

    vr.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (vr.wake_fd < 0) {
        close(sv[0]); close(sv[1]); vr_free_ring();
        snprintf(err, err_cap, "could not create the video wake eventfd: %s", strerror(errno));
        return -1;
    }

    char size_buf[32], rate_buf[32];
    char *argv[64];
    int argc = 0;
    vr_build_argv(argv, &argc, codec, size_buf, sizeof(size_buf),
                  rate_buf, sizeof(rate_buf), out_path);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    /* Keeps the terminal clean while leaving a failure diagnosable -- the
     * difference between "video failed" and "video failed: No space left". */
    posix_spawn_file_actions_addopen(&fa, 2, vr.ffmpeg_log,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);

    pid_t pid = 0;
    /* posix_spawn for the same reason as the preview popout: this process has
     * display, audio, capture and libusb threads plus a live GL context, and
     * only async-signal-safe calls are legal between fork and exec. Note it
     * returns the error directly and does NOT set errno. */
    int rc = posix_spawn(&pid, ff.path, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);   /* mandatory: otherwise closing our end never delivers EOF */

    if (rc != 0) {
        close(sv[0]); close(vr.wake_fd); vr.wake_fd = -1;
        vr_free_ring();
        snprintf(err, err_cap, "could not start ffmpeg: %s", strerror(rc));
        return -1;
    }

    vr.sock = sv[0];
    vr.child_pid = (int)pid;
    vr.status.child_pid = (int)pid;
    vr.status.running = true;
    atomic_store(&vr.run, true);

    /* Normal priority, deliberately: the RF writers run SCHED_FIFO, and a
     * blocking send() to a possibly-hung child must never sit in that class. */
    if (pthread_create(&vr.thread, NULL, vr_writer_thread, NULL) != 0) {
        close(vr.sock); vr.sock = -1;
        close(vr.wake_fd); vr.wake_fd = -1;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        vr.child_pid = 0;
        vr.status.running = false;
        vr_free_ring();
        snprintf(err, err_cap, "could not start the video writer thread");
        return -1;
    }
    vr.thread_running = true;

    vr.tap.fn = vr_tap_cb;
    vr.tap.user = NULL;
    gui_preview_tap_install(&vr.tap);
    return 0;
}

void gui_video_record_request_stop(void)
{
    if (!vr.thread_running) return;
    gui_preview_tap_remove();          /* returns once no tap call is in flight */
    atomic_store(&vr.run, false);
    uint64_t one = 1;
    ssize_t w = write(vr.wake_fd, &one, sizeof(one));
    (void)w;
}

static void vr_reap_child(void)
{
    if (vr.child_pid <= 0) return;
    double deadline = vr_now() + VR_REAP_LIMIT_S;
    int wstatus = 0;

    while (vr_now() < deadline) {
        pid_t r = waitpid(vr.child_pid, &wstatus, WNOHANG);
        if (r == vr.child_pid) goto reaped;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(vr.child_pid, SIGTERM);
    deadline = vr_now() + 2.0;
    while (vr_now() < deadline) {
        if (waitpid(vr.child_pid, &wstatus, WNOHANG) == vr.child_pid) goto reaped;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(vr.child_pid, SIGKILL);
    waitpid(vr.child_pid, &wstatus, 0);

reaped:
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "ffmpeg exited with code %d", WEXITSTATUS(wstatus));
        vr_set_error(msg, 0);
    } else if (WIFSIGNALED(wstatus)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "ffmpeg terminated by signal %d (%s)",
                 WTERMSIG(wstatus), strsignal(WTERMSIG(wstatus)));
        vr_set_error(msg, 0);
    }
    vr.child_pid = 0;
    vr_lock();
    vr.status.child_pid = 0;
    vr_unlock();
}

void gui_video_record_finish(void)
{
    if (!vr.thread_running) return;

    gui_preview_tap_remove();
    atomic_store(&vr.run, false);
    uint64_t one = 1;
    ssize_t w = write(vr.wake_fd, &one, sizeof(one));
    (void)w;

    pthread_join(vr.thread, NULL);
    vr.thread_running = false;

    if (vr.sock >= 0) { close(vr.sock); vr.sock = -1; }
    vr_reap_child();

    if (vr.wake_fd >= 0) { close(vr.wake_fd); vr.wake_fd = -1; }

    struct stat st;
    if (vr.status.path[0] && stat(vr.status.path, &st) == 0) {
        vr_lock();
        vr.status.output_bytes = (uint64_t)st.st_size;
        vr_unlock();
    }

    vr_free_ring();
    vr_lock();
    vr.status.running = false;
    vr_unlock();
}

void gui_video_record_shutdown(void)
{
    if (vr.thread_running) gui_video_record_finish();
    else if (vr.child_pid > 0) vr_reap_child();
}

/* ------------------------------------------------------------- diagnostics */

int gui_video_record_probe_main(void)
{
    bool ok = gui_video_record_probe();
    printf("ffmpeg: %s\n", ok ? gui_video_record_ffmpeg_path() : "not found");
    if (ok) {
        printf("version: %s\n", gui_video_record_ffmpeg_version());
        printf("libx264: %s\n", gui_video_record_codec_available(VIDEO_CODEC_H264) ? "yes" : "no");
        printf("ffv1:    %s\n", gui_video_record_codec_available(VIDEO_CODEC_FFV1) ? "yes" : "no");
    }
    return ok ? 0 : 1;
}

int gui_video_record_test_main(const char *device, const char *out_path,
                              int seconds, const char *codec_name)
{
    if (!out_path) { fprintf(stderr, "usage: --video-record-test <dev> <out.mkv> <secs> [x264|ffv1]\n"); return 2; }
    video_codec_t codec = (codec_name && strcmp(codec_name, "ffv1") == 0)
                        ? VIDEO_CODEC_FFV1 : VIDEO_CODEC_H264;

    gui_preview_init(NULL);
    gui_preview_refresh_devices();
    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    if (n == 0) { fprintf(stderr, "no preview devices\n"); return 1; }

    int idx = 0;
    if (device) {
        idx = -1;
        for (size_t i = 0; i < n; i++) if (strcmp(devs[i].path, device) == 0) { idx = (int)i; break; }
        if (idx < 0) { fprintf(stderr, "no such device: %s\n", device); return 1; }
    }
    gui_preview_select(idx, 0);
    if (gui_preview_connect() != 0) {
        preview_status_t ps = gui_preview_get_status();
        fprintf(stderr, "connect failed: %s\n", ps.err_text);
        return 2;
    }

    preview_status_t ps = gui_preview_get_status();
    char err[192] = {0};
    /* Geometry and rate come from what the device negotiated, never from what
     * was requested. */
    uint32_t pitch = gui_preview_negotiated_pitch();
    if (pitch == 0) pitch = ps.width * 2;
    if (gui_video_record_start(out_path, codec, ps.width, ps.height, pitch,
                               ps.fps_num, ps.fps_den, err, sizeof(err)) != 0) {
        fprintf(stderr, "video record start failed: %s\n", err);
        gui_preview_disconnect();
        gui_preview_shutdown();
        return 2;
    }

    printf("recording %ux%u @ %u/%u -> %s (%s), ffmpeg pid %d\n",
           ps.width, ps.height, ps.fps_num, ps.fps_den, out_path,
           codec == VIDEO_CODEC_FFV1 ? "ffv1" : "x264",
           gui_video_record_get_status().child_pid);

    if (seconds <= 0) seconds = 10;
    /* Deliberately not a whole number of seconds: the frame period is 40ms and
     * sampling at an exact multiple aliases against it. */
    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 7 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        gui_video_record_status_t vs = gui_video_record_get_status();
        printf("  t=%2ds submitted=%llu written=%llu dropped=%llu duped=%llu bytes=%llu%s%s\n",
               i + 1,
               (unsigned long long)vs.frames_submitted, (unsigned long long)vs.frames_written,
               (unsigned long long)vs.frames_dropped, (unsigned long long)vs.frames_duped,
               (unsigned long long)vs.output_bytes,
               vs.error ? "  err=" : "", vs.error ? vs.err_text : "");
        fflush(stdout);
        if (vs.error) break;
    }

    double t0 = vr_now();
    gui_video_record_request_stop();
    gui_video_record_finish();
    double stop_s = vr_now() - t0;

    gui_video_record_status_t vs = gui_video_record_get_status();
    gui_preview_disconnect();
    gui_preview_shutdown();

    printf("stopped in %.2fs: submitted=%llu written=%llu dropped=%llu duped=%llu bytes=%llu\n",
           stop_s,
           (unsigned long long)vs.frames_submitted, (unsigned long long)vs.frames_written,
           (unsigned long long)vs.frames_dropped, (unsigned long long)vs.frames_duped,
           (unsigned long long)vs.output_bytes);
    if (vs.error) printf("error: %s\n", vs.err_text);

    /* Assertions, so the harness can actually fail. Without these it reports
     * success for a run that wrote nothing at all. */
    int rc = 0;
    if (vs.frames_written == 0) {
        printf("FAIL: no frames reached the encoder\n");
        rc = 1;
    }
    /* Every frame the capture thread handed over, plus every duplicate
     * inserted to cover a drop, must reach ffmpeg -- otherwise the file is
     * short and slides out of alignment with the RF timeline. */
    if (vs.frames_written != vs.frames_submitted + vs.frames_duped) {
        printf("FAIL: written(%llu) != submitted(%llu) + duped(%llu)\n",
               (unsigned long long)vs.frames_written,
               (unsigned long long)vs.frames_submitted,
               (unsigned long long)vs.frames_duped);
        rc = 1;
    }
    if (vs.output_bytes == 0) {
        printf("FAIL: output file is empty\n");
        rc = 1;
    }
    if (vs.error) rc = 1;
    printf("%s\n", rc ? "VIDEO RECORD TEST FAILED" : "video record test passed");
    return rc;
}

#else  /* !__linux__ */

bool gui_video_record_probe(void) { return false; }
const char *gui_video_record_ffmpeg_path(void) { return ""; }
const char *gui_video_record_ffmpeg_version(void) { return ""; }
bool gui_video_record_codec_available(video_codec_t c) { (void)c; return false; }
int  gui_video_record_start(const char *o, video_codec_t c, uint32_t w, uint32_t h,
                            uint32_t p, uint32_t fn, uint32_t fd, char *err, size_t cap)
{ (void)o;(void)c;(void)w;(void)h;(void)p;(void)fn;(void)fd;
  snprintf(err, cap, "reference video recording requires Linux"); return -1; }
void gui_video_record_request_stop(void) { }
void gui_video_record_finish(void) { }
void gui_video_record_shutdown(void) { }
gui_video_record_status_t gui_video_record_get_status(void)
{ gui_video_record_status_t s; memset(&s, 0, sizeof(s)); return s; }
uint64_t gui_video_record_output_bytes(void) { return 0; }
bool gui_video_record_is_running(void) { return false; }
int gui_video_record_test_main(const char *d, const char *o, int s, const char *c)
{ (void)d;(void)o;(void)s;(void)c; fprintf(stderr, "requires Linux\n"); return 2; }
int gui_video_record_probe_main(void) { fprintf(stderr, "requires Linux\n"); return 2; }

#endif /* __linux__ */
