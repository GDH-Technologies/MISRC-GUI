#include "gui_rtsp_stream.h"
#include "gui_alsa_device.h"
#include "gui_preview_tap_mux.h"
#include "../input/gui_preview_v4l2.h"
#include "../output/gui_video_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Four frames, not the recorder's sixteen. The recorder buffers because it must
 * not lose a frame; a monitoring stream would rather be current than complete,
 * and a deep ring only means the viewer watches an older picture after a stall.
 * At 25fps this bounds the backlog to ~160ms.
 *
 * Note this is drop-NEWEST-when-full, where the design said discard-oldest.
 * Overwriting the slot a consumer may be sending from is a data race no counter
 * would reveal, and with a ring this shallow the two are equivalent in effect:
 * a stall drops frames either way and drains within ~4 frames of recovery. */
#define RS_RING_FRAMES     4
#define RS_SNDBUF_BYTES    (4 << 20)
#define RS_DRAIN_LIMIT_S   3.0
#define RS_REAP_LIMIT_S    8.0
/* capture-node's posture for its ffmpeg children, for the same reason. */
#define RS_CHILD_NICE      5

static struct {
    uint8_t  *slots[RS_RING_FRAMES];
    size_t    frame_bytes;
    atomic_uint_fast32_t head;
    atomic_uint_fast32_t tail;

    int       sock;
    int       wake_fd;
    int       child_pid;

    pthread_t thread;
    bool      thread_running;
    atomic_bool run;

    uint32_t  width, height, pitch, fps_num, fps_den;

    atomic_flag lock;
    gui_rtsp_stream_status_t status;

    char ffmpeg_log[600];
    preview_tap_t tap;
    bool  holding_preview;
    double stop_at;

    /* Deferred startup verification. ffmpeg cannot fail until frames reach it,
     * so the check has to happen after the tap is live -- but sleeping for it on
     * the render thread is what made the window freeze. */
    double verify_at;              /* 0 = nothing pending */
    bool   retried_video_only;
    bool   use_nvenc;
    char   audio_device[128];      /* what the current attempt is using */
    gui_rtsp_stream_opts_t opts;   /* copy; the string fields point at the below */
    char   opt_video_device[64];
    char   opt_audio_device[128];
    char   opt_reader_host[128];
} rs;

static rs_inject_t s_inject = RS_INJECT_NONE;
void gui_rtsp_stream_set_inject(rs_inject_t what) { s_inject = what; }

static struct {
    bool probed;
    bool found;
    bool has_nvenc;
    bool has_x264;
    bool has_opus;
} enc;

static double rs_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void rs_lock(void)   { while (atomic_flag_test_and_set(&rs.lock)) {} }
static void rs_unlock(void) { atomic_flag_clear(&rs.lock); }

static void rs_set_error(const char *what)
{
    rs_lock();
    if (!rs.status.error) {
        rs.status.error = true;
        snprintf(rs.status.err_text, sizeof(rs.status.err_text), "%s", what);
    }
    rs_unlock();
}

gui_rtsp_stream_status_t gui_rtsp_stream_get_status(void)
{
    rs_lock();
    gui_rtsp_stream_status_t out = rs.status;
    rs_unlock();
    return out;
}

bool gui_rtsp_stream_is_running(void)
{
    rs_lock();
    bool r = rs.status.running;
    rs_unlock();
    return r;
}

/* ---- encoder probe -------------------------------------------------------- */

/* The ffmpeg binary is resolved once, by gui_video_record: duplicating a PATH
 * search would let the recorder and the stream disagree about which ffmpeg they
 * are using, which is exactly the kind of difference nobody would think to
 * check when only one of them misbehaves. */
bool gui_rtsp_stream_probe(void)
{
    if (enc.probed) return enc.found;
    enc.probed = true;
    enc.found = false;
    enc.has_nvenc = enc.has_x264 = enc.has_opus = false;

    if (!gui_video_record_probe()) return false;
    const char *ffmpeg = gui_video_record_ffmpeg_path();
    if (!ffmpeg || !ffmpeg[0]) return false;

    char cmd[700];
    snprintf(cmd, sizeof(cmd), "\"%s\" -hide_banner -encoders 2>/dev/null", ffmpeg);
    FILE *f = popen(cmd, "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, " h264_nvenc")) enc.has_nvenc = true;
        if (strstr(line, " libx264"))    enc.has_x264 = true;
        if (strstr(line, " libopus"))    enc.has_opus = true;
    }
    pclose(f);

    enc.found = enc.has_nvenc || enc.has_x264;
    return enc.found;
}

bool gui_rtsp_stream_has_nvenc(void)
{
    gui_rtsp_stream_probe();
    return enc.has_nvenc;
}

/* ---- tap ------------------------------------------------------------------ */

/* Runs on the preview capture thread, between VIDIOC_DQBUF and VIDIOC_QBUF.
 * Its only cost is a memcpy and two atomics, and its only failure is "ring
 * full", which is a compare-and-return. A wedged ffmpeg fills the socket, then
 * the ring, and frames are dropped -- VIDIOC_QBUF is never delayed, and RF
 * ingest never sees this thread block. */
static void rs_tap_cb(const uint8_t *yuyv, size_t pitch, uint32_t w, uint32_t h, void *user)
{
    (void)user;
    size_t want = pitch * (size_t)h;
    if (w != rs.width || h != rs.height || want != rs.frame_bytes) {
        return;   /* geometry changed under us; drop rather than send short */
    }

    uint_fast32_t head = atomic_load_explicit(&rs.head, memory_order_relaxed);
    uint_fast32_t tail = atomic_load_explicit(&rs.tail, memory_order_acquire);

    if (head - tail >= RS_RING_FRAMES) {
        /* No dupe counter here, unlike the recorder: a stream has no file whose
         * timeline could slide out of step, and a viewer would rather miss a
         * frame than watch a stale one re-sent. */
        rs_lock();
        rs.status.frames_dropped++;
        rs_unlock();
        return;
    }

    memcpy(rs.slots[head % RS_RING_FRAMES], yuyv, rs.frame_bytes);
    atomic_store_explicit(&rs.head, head + 1, memory_order_release);

    rs_lock();
    rs.status.frames_submitted++;
    rs_unlock();

    uint64_t one = 1;
    ssize_t wrote = write(rs.wake_fd, &one, sizeof(one));
    (void)wrote;
}

/* ---- writer --------------------------------------------------------------- */

/* MSG_NOSIGNAL is why this is a socketpair and not a pipe: a dead ffmpeg gives
 * EPIPE here rather than a process-wide SIGPIPE, in a process that installs no
 * signal handlers. */
static bool rs_send_all(const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(rs.sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) { sent += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!atomic_load_explicit(&rs.run, memory_order_acquire)) return false;
            continue;   /* SO_SNDTIMEO fired; re-check the stop flag */
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static void *rs_writer_thread(void *arg)
{
    (void)arg;
    while (true) {
        uint_fast32_t head = atomic_load_explicit(&rs.head, memory_order_acquire);
        uint_fast32_t tail = atomic_load_explicit(&rs.tail, memory_order_relaxed);

        if (head == tail) {
            if (!atomic_load_explicit(&rs.run, memory_order_acquire)) break;
            uint64_t val = 0;
            ssize_t r = read(rs.wake_fd, &val, sizeof(val));
            (void)r;
            continue;
        }

        if (!rs_send_all(rs.slots[tail % RS_RING_FRAMES], rs.frame_bytes)) {
            rs_set_error("the encoder stopped accepting frames");
            break;
        }
        atomic_store_explicit(&rs.tail, tail + 1, memory_order_release);

        rs_lock();
        rs.status.frames_written++;
        rs_unlock();

        if (!atomic_load_explicit(&rs.run, memory_order_acquire) &&
            rs.stop_at > 0.0 && rs_now() - rs.stop_at > RS_DRAIN_LIMIT_S) {
            break;   /* a stalled child must not hold teardown open forever */
        }
    }
    return NULL;
}

/* ---- argv ----------------------------------------------------------------- */

static void rs_build_argv(char *argv[], int *argc_out,
                          const gui_rtsp_stream_opts_t *opts,
                          bool use_nvenc, const char *audio_device,
                          char *size_buf, size_t size_cap,
                          char *rate_buf, size_t rate_cap,
                          char *gop_buf, size_t gop_cap,
                          char *br_buf, size_t br_cap,
                          char *maxr_buf, size_t maxr_cap,
                          char *bufs_buf, size_t bufs_cap,
                          char *url_buf, size_t url_cap)
{
    int n = 0;
    unsigned fps = (opts->fps_den ? opts->fps_num / opts->fps_den : 25);
    if (fps == 0) fps = 25;
    unsigned kbps = opts->bitrate_kbps ? opts->bitrate_kbps : 2000;

    snprintf(size_buf, size_cap, "%ux%u", opts->width, opts->height);
    snprintf(rate_buf, rate_cap, "%u/%u", opts->fps_num, opts->fps_den);
    snprintf(gop_buf, gop_cap, "%u", fps);
    snprintf(br_buf, br_cap, "%uk", kbps);
    snprintf(maxr_buf, maxr_cap, "%uk", kbps);
    snprintf(bufs_buf, bufs_cap, "%uk", kbps * 2);
    snprintf(url_buf, url_cap, "rtsp://127.0.0.1:%u/misrc-preview", (unsigned)opts->ports.rtsp);

    argv[n++] = (char *)gui_video_record_ffmpeg_path();
    argv[n++] = (char *)"-hide_banner";
    argv[n++] = (char *)"-nostdin";
    argv[n++] = (char *)"-nostats";
    argv[n++] = (char *)"-loglevel";
    argv[n++] = (char *)"warning";

    /* video: the tap, over stdin */
    argv[n++] = (char *)"-f";           argv[n++] = (char *)"rawvideo";
    argv[n++] = (char *)"-pixel_format"; argv[n++] = (char *)"yuyv422";
    argv[n++] = (char *)"-video_size";  argv[n++] = size_buf;
    argv[n++] = (char *)"-framerate";   argv[n++] = rate_buf;
    argv[n++] = (char *)"-i";           argv[n++] = (char *)"-";

    /* audio: the dongle's own node, opened by ffmpeg itself */
    if (audio_device && audio_device[0]) {
        argv[n++] = (char *)"-f";  argv[n++] = (char *)"alsa";
        argv[n++] = (char *)"-ac"; argv[n++] = (char *)"2";
        argv[n++] = (char *)"-ar"; argv[n++] = (char *)"48000";
        argv[n++] = (char *)"-i";  argv[n++] = (char *)audio_device;
    }
    argv[n++] = (char *)"-sn";
    if (!audio_device || !audio_device[0]) {
        argv[n++] = (char *)"-an";
    }

    if (opts->deinterlace) {
        /* Helps a remote viewer on 480i/576i, costs latency and CPU. Off by
         * default, and the local preview panel does not deinterlace either. */
        argv[n++] = (char *)"-vf";
        argv[n++] = (char *)(use_nvenc ? "bwdif,format=nv12" : "bwdif,format=yuv420p");
    } else {
        argv[n++] = (char *)"-vf";
        argv[n++] = (char *)(use_nvenc ? "format=nv12" : "format=yuv420p");
    }

    if (use_nvenc) {
        argv[n++] = (char *)"-c:v";   argv[n++] = (char *)"h264_nvenc";
        argv[n++] = (char *)"-preset"; argv[n++] = (char *)"p2";
        argv[n++] = (char *)"-tune";  argv[n++] = (char *)"ull";
        argv[n++] = (char *)"-rc";    argv[n++] = (char *)"vbr";
        argv[n++] = (char *)"-cq";    argv[n++] = (char *)"0";
        argv[n++] = (char *)"-b:v";   argv[n++] = (char *)"0";
        argv[n++] = (char *)"-forced-idr"; argv[n++] = (char *)"1";
        argv[n++] = (char *)"-zerolatency"; argv[n++] = (char *)"1";
    } else {
        argv[n++] = (char *)"-c:v";     argv[n++] = (char *)"libx264";
        argv[n++] = (char *)"-preset";  argv[n++] = (char *)"veryfast";
        argv[n++] = (char *)"-tune";    argv[n++] = (char *)"zerolatency";
        /* Spare cores belong to the RF recorders, and this app IS the RF
         * recorder -- more true here than in capture-node. */
        argv[n++] = (char *)"-threads"; argv[n++] = (char *)"2";
        argv[n++] = (char *)"-b:v";     argv[n++] = br_buf;
        argv[n++] = (char *)"-maxrate"; argv[n++] = maxr_buf;
        argv[n++] = (char *)"-bufsize"; argv[n++] = bufs_buf;
        argv[n++] = (char *)"-keyint_min"; argv[n++] = gop_buf;
    }
    /* A 1-second GOP with no B-frames keeps join latency low for a new viewer. */
    argv[n++] = (char *)"-g";  argv[n++] = gop_buf;
    argv[n++] = (char *)"-bf"; argv[n++] = (char *)"0";

    if (audio_device && audio_device[0]) {
        /* Opus, not AAC: mediamtx does not transcode, and Opus is the only
         * codec both WebRTC and HLS accept from a single publish. */
        argv[n++] = (char *)"-c:a"; argv[n++] = (char *)"libopus";
        argv[n++] = (char *)"-b:a"; argv[n++] = (char *)"64k";
    }

    if (s_inject == RS_INJECT_BAD_ARGS) {
        argv[n++] = (char *)"-c:v";
        argv[n++] = (char *)"definitely_not_an_encoder";
    }

    argv[n++] = (char *)"-f"; argv[n++] = (char *)"rtsp";
    /* TCP over loopback costs nothing and never fragments; readers may still
     * pull over UDP. */
    argv[n++] = (char *)"-rtsp_transport"; argv[n++] = (char *)"tcp";
    argv[n++] = url_buf;
    argv[n] = NULL;
    *argc_out = n;
}

/* ---- lifecycle ------------------------------------------------------------ */

static void rs_free_ring(void)
{
    for (int i = 0; i < RS_RING_FRAMES; i++) {
        free(rs.slots[i]);
        rs.slots[i] = NULL;
    }
}

static void rs_resolve_audio(const gui_rtsp_stream_opts_t *opts, char *out, size_t cap)
{
    out[0] = '\0';
    rs.status.audio_active = false;
    rs.status.audio_note[0] = '\0';
    rs.status.audio_device[0] = '\0';

    if (s_inject == RS_INJECT_NO_AUDIO) {
        snprintf(rs.status.audio_note, sizeof(rs.status.audio_note),
                 "audio disabled by fault injection");
        return;
    }
    if (s_inject == RS_INJECT_BUSY_AUDIO) {
        snprintf(out, cap, "plughw:CARD=NoSuchCard,DEV=0");
        snprintf(rs.status.audio_device, sizeof(rs.status.audio_device), "%s", out);
        rs.status.audio_active = true;
        return;
    }

    if (opts->audio_device && opts->audio_device[0]) {
        snprintf(out, cap, "%s", opts->audio_device);
    } else if (gui_alsa_resolve_for_video_device(opts->video_device, out, cap) != 0) {
        snprintf(rs.status.audio_note, sizeof(rs.status.audio_note),
                 "no ALSA capture device shares the dongle's USB address");
        return;
    }
    snprintf(rs.status.audio_device, sizeof(rs.status.audio_device), "%s", out);
    rs.status.audio_active = true;
}

static void rs_fill_urls(const gui_rtsp_stream_opts_t *opts)
{
    char host[128];
    if (opts->reader_host && opts->reader_host[0]) {
        snprintf(host, sizeof(host), "%s", opts->reader_host);
    } else if (opts->ports.lan) {
        if (gethostname(host, sizeof(host)) != 0) snprintf(host, sizeof(host), "localhost");
        host[sizeof(host) - 1] = '\0';
    } else {
        snprintf(host, sizeof(host), "127.0.0.1");
    }
    snprintf(rs.status.url_rtsp, sizeof(rs.status.url_rtsp),
             "rtsp://%s:%u/misrc-preview", host, (unsigned)opts->ports.rtsp);
    snprintf(rs.status.url_webrtc, sizeof(rs.status.url_webrtc),
             "http://%s:%u/misrc-preview", host, (unsigned)opts->ports.webrtc_http);
    snprintf(rs.status.url_hls, sizeof(rs.status.url_hls),
             "http://%s:%u/misrc-preview", host, (unsigned)opts->ports.hls);
}

typedef enum {
    RS_SPAWN_OK = 0,
    RS_SPAWN_FAIL_SETUP,   /* our own sockets/eventfd, or exec itself */
    RS_SPAWN_FAIL_AUDIO,   /* ffmpeg died on the ALSA input specifically */
    RS_SPAWN_FAIL_OTHER    /* ffmpeg died for some other reason */
} rs_spawn_result_t;

/* Spawns ffmpeg and waits a beat to see whether it survives. Owns the socket
 * pair and the wake eventfd; cleans both up on failure. The ring belongs to the
 * caller, because a retry reuses it. */
static rs_spawn_result_t rs_spawn_attempt(const gui_rtsp_stream_opts_t *opts,
                                          bool use_nvenc, const char *audio_device,
                                          int *sock_out, pid_t *pid_out,
                                          char *err, size_t err_cap)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        snprintf(err, err_cap, "could not create the encoder socket: %s", strerror(errno));
        return RS_SPAWN_FAIL_SETUP;
    }
    int want = RS_SNDBUF_BYTES;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
    /* Without a send timeout a wedged ffmpeg blocks the writer inside send()
     * forever, where it can never observe the stop flag. */
    struct timeval sndto = { 0, 200 * 1000 };
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

    rs.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (rs.wake_fd < 0) {
        close(sv[0]); close(sv[1]);
        snprintf(err, err_cap, "could not create the stream wake eventfd: %s", strerror(errno));
        return RS_SPAWN_FAIL_SETUP;
    }

    char size_buf[32], rate_buf[32], gop_buf[16], br_buf[16], maxr_buf[16], bufs_buf[16];
    char url_buf[256];
    char *argv[80];
    int argc = 0;
    rs_build_argv(argv, &argc, opts, use_nvenc, audio_device,
                  size_buf, sizeof(size_buf), rate_buf, sizeof(rate_buf),
                  gop_buf, sizeof(gop_buf), br_buf, sizeof(br_buf),
                  maxr_buf, sizeof(maxr_buf), bufs_buf, sizeof(bufs_buf),
                  url_buf, sizeof(url_buf));

    snprintf(rs.ffmpeg_log, sizeof(rs.ffmpeg_log), "%s/misrc-rtsp-ffmpeg.log",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    /* Keeps the terminal clean while leaving a failure diagnosable -- the
     * difference between "stream failed" and "stream failed: Connection
     * refused". */
    posix_spawn_file_actions_addopen(&fa, 2, rs.ffmpeg_log,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);

    pid_t pid = 0;
    int rc = posix_spawn(&pid, gui_video_record_ffmpeg_path(), &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);   /* mandatory: otherwise closing our end never delivers EOF */

    if (rc != 0) {
        close(sv[0]); close(rs.wake_fd); rs.wake_fd = -1;
        snprintf(err, err_cap, "could not start ffmpeg: %s", strerror(rc));
        return RS_SPAWN_FAIL_SETUP;
    }

    /* RTSP fan-out must yield to RF ingest. This app IS the RF recorder, so an
     * encoder competing for the same cores is not a fair fight to leave to
     * chance -- capture-node nices its ffmpeg children for the same reason.
     * Raising niceness never needs privilege, so a failure here is not worth
     * refusing the stream over. */
    if (setpriority(PRIO_PROCESS, (id_t)pid, RS_CHILD_NICE) != 0) {
        /* Nothing to do: the stream still works, it just competes harder. */
    }

    /* Frames must already be flowing before the child can be judged.
     *
     * capture-node checks its ffmpeg a second after spawning, but its ffmpeg
     * opens -f v4l2 itself and so reaches any failure unaided. Ours takes video
     * on stdin, and ffmpeg blocks reading input 0 before it opens input 1 or the
     * output. Measured: with stdin held open and empty, both a deliberately
     * unknown encoder and a nonexistent ALSA card leave ffmpeg alive
     * indefinitely -- it never gets far enough to fail. So the writer and the
     * tap go in first, and only then does the probe mean anything. */
    rs.sock = sv[0];
    rs.child_pid = (int)pid;
    rs.status.child_pid = (int)pid;
    rs.status.running = true;
    atomic_store(&rs.run, true);
    atomic_store(&rs.head, 0);
    atomic_store(&rs.tail, 0);

    if (pthread_create(&rs.thread, NULL, rs_writer_thread, NULL) != 0) {
        close(rs.sock); rs.sock = -1;
        close(rs.wake_fd); rs.wake_fd = -1;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        rs.child_pid = 0;
        rs.status.running = false;
        snprintf(err, err_cap, "could not start the stream writer thread");
        return RS_SPAWN_FAIL_SETUP;
    }
    rs.thread_running = true;

    /* Hold the preview open for the life of the stream, so closing the panel
     * does not silently end the broadcast. */
    gui_preview_hold_acquire();
    rs.holding_preview = true;
    rs.tap.fn = rs_tap_cb;
    rs.tap.user = NULL;
    gui_preview_mux_add(&rs.tap);

    /* Wired up and running. Whether ffmpeg SURVIVES is checked later, from
     * gui_rtsp_stream_poll(): it cannot fail until frames reach it, and waiting
     * here for that to happen is what froze the window. */
    if (sock_out) *sock_out = sv[0];
    if (pid_out) *pid_out = pid;
    return RS_SPAWN_OK;
}

/* Tear down what one attempt put in place, leaving the ring alone so a retry
 * can reuse it. Audio status fields are preserved: the retry sets them. */
static void rs_unwind_attempt(void)
{
    gui_preview_mux_remove(&rs.tap);
    if (rs.holding_preview) {
        gui_preview_hold_release();
        rs.holding_preview = false;
    }
    atomic_store_explicit(&rs.run, false, memory_order_release);
    uint64_t wake = 1;
    ssize_t wrote = write(rs.wake_fd, &wake, sizeof(wake));
    (void)wrote;
    pthread_join(rs.thread, NULL);
    rs.thread_running = false;
    close(rs.sock); rs.sock = -1;
    close(rs.wake_fd); rs.wake_fd = -1;
    rs.child_pid = 0;

    rs_lock();
    rs.status.running = false;
    rs.status.child_pid = 0;
    rs.status.error = false;          /* the retry deserves a clean slate */
    rs.status.err_text[0] = '\0';
    rs.status.frames_submitted = 0;
    rs.status.frames_written = 0;
    rs.status.frames_dropped = 0;
    rs_unlock();
}

/* Why did it die? Reads ffmpeg's log and turns it into something displayable. */
static rs_spawn_result_t rs_classify_exit(char *err, size_t err_cap)
{
    /* Classify over the WHOLE log, not just its last line. ffmpeg's final line
     * is a generic summary -- "Error opening input files: Input/output error"
     * -- while the line that says which input and why is several above it. The
     * last line is still the best thing to SHOW; it is just useless to match
     * on. */
    char log_text[4096] = {0};
    char reason[256] = {0};
    FILE *lf = fopen(rs.ffmpeg_log, "r");
    if (lf) {
        size_t used = fread(log_text, 1, sizeof(log_text) - 1, lf);
        log_text[used] = '\0';
        fclose(lf);

        const char *line = log_text;
        while (line && *line) {
            const char *nl = strchr(line, '\n');
            size_t len = nl ? (size_t)(nl - line) : strlen(line);
            if (len > 0 && len < sizeof(reason)) {
                memcpy(reason, line, len);
                reason[len] = '\0';
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    if (strstr(log_text, "Connection refused")) {
        snprintf(err, err_cap, "mediamtx is not accepting publishers on the RTSP port");
        return RS_SPAWN_FAIL_OTHER;
    }
    if (strstr(log_text, "CUDA") || strstr(log_text, "nvenc")) {
        snprintf(err, err_cap, "NVENC is unavailable: %s", reason);
        return RS_SPAWN_FAIL_OTHER;
    }
    if (strstr(log_text, "cannot open audio device") || strstr(log_text, "ALSA lib") ||
        strstr(log_text, "Device or resource busy") || strstr(log_text, "snd_config") ||
        strstr(log_text, "Cannot get card index")) {
        snprintf(err, err_cap, "the dongle's audio device could not be opened: %s", reason);
        return RS_SPAWN_FAIL_AUDIO;
    }
    snprintf(err, err_cap, "ffmpeg exited at startup: %s", reason[0] ? reason : "see the log");
    return RS_SPAWN_FAIL_OTHER;
}

int gui_rtsp_stream_start(const gui_rtsp_stream_opts_t *opts, char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (rs.thread_running) return 0;
    if (opts == NULL) {
        snprintf(err, err_cap, "no stream options supplied");
        return -1;
    }
    if (!gui_rtsp_stream_probe()) {
        snprintf(err, err_cap, "ffmpeg was not found, or has neither h264_nvenc nor libx264");
        return -1;
    }

    bool use_nvenc = (opts->encoder == RTSP_ENCODER_NVENC) ||
                     (opts->encoder == RTSP_ENCODER_AUTO && enc.has_nvenc);
    if (use_nvenc && !enc.has_nvenc) {
        snprintf(err, err_cap, "%s cannot encode with h264_nvenc",
                 gui_video_record_ffmpeg_path());
        return -1;
    }
    if (!use_nvenc && !enc.has_x264) {
        snprintf(err, err_cap, "%s cannot encode with libx264",
                 gui_video_record_ffmpeg_path());
        return -1;
    }

    /* The supervisor must be up before the first publisher. Idempotent, and
     * deliberately NOT stopped when a stream stops: restarting mediamtx per
     * stream would drop every viewer that was reconnecting. App exit stops it. */
    if (gui_mediamtx_start(&opts->ports, err, err_cap) != 0) {
        return -1;
    }

    memset(&rs.status, 0, sizeof(rs.status));
    atomic_flag_clear(&rs.lock);
    rs.width = opts->width; rs.height = opts->height; rs.pitch = opts->pitch;
    rs.fps_num = opts->fps_num ? opts->fps_num : 25;
    rs.fps_den = opts->fps_den ? opts->fps_den : 1;
    rs.frame_bytes = (size_t)opts->pitch * opts->height;
    rs.sock = -1;
    rs.wake_fd = -1;
    rs.child_pid = 0;
    rs.stop_at = 0.0;
    rs.holding_preview = false;
    atomic_store(&rs.head, 0);
    atomic_store(&rs.tail, 0);

    char audio_device[128];
    rs_resolve_audio(opts, audio_device, sizeof(audio_device));
    if (audio_device[0] && !enc.has_opus) {
        audio_device[0] = '\0';
        rs.status.audio_active = false;
        snprintf(rs.status.audio_note, sizeof(rs.status.audio_note),
                 "ffmpeg has no libopus; streaming video-only");
    }
    rs_fill_urls(opts);

    for (int i = 0; i < RS_RING_FRAMES; i++) {
        rs.slots[i] = malloc(rs.frame_bytes);
        if (!rs.slots[i]) {
            rs_free_ring();
            snprintf(err, err_cap, "out of memory for the stream ring");
            return -1;
        }
    }

    /* Keep everything a retry will need. The caller's opts may be a stack
     * temporary and its string fields may not outlive this call. */
    rs.opts = *opts;
    snprintf(rs.opt_video_device, sizeof(rs.opt_video_device), "%s",
             opts->video_device ? opts->video_device : "");
    snprintf(rs.opt_audio_device, sizeof(rs.opt_audio_device), "%s",
             opts->audio_device ? opts->audio_device : "");
    snprintf(rs.opt_reader_host, sizeof(rs.opt_reader_host), "%s",
             opts->reader_host ? opts->reader_host : "");
    rs.opts.video_device = rs.opt_video_device;
    rs.opts.audio_device = rs.opt_audio_device;
    rs.opts.reader_host = rs.opt_reader_host;
    rs.use_nvenc = use_nvenc;
    rs.retried_video_only = false;
    snprintf(rs.audio_device, sizeof(rs.audio_device), "%s", audio_device);

    int sock = -1;
    pid_t pid = 0;
    if (rs_spawn_attempt(&rs.opts, use_nvenc, rs.audio_device,
                         &sock, &pid, err, err_cap) != RS_SPAWN_OK) {
        rs_free_ring();
        return -1;
    }
    (void)sock;

    /* Long enough for a frame or two to reach ffmpeg and for it to fail on
     * them. Checked by gui_rtsp_stream_poll(), not slept for here: this runs on
     * the render thread, and a second and a half of sleeping is a frozen
     * window. Until then the stream reports itself as starting. */
    rs.verify_at = rs_now() + 1.5;
    rs_lock();
    rs.status.starting = true;
    rs_unlock();

    return 0;
}

void gui_rtsp_stream_poll(void)
{
    if (rs.verify_at == 0.0) return;              /* nothing pending */
    if (rs_now() < rs.verify_at) return;          /* still inside the window */

    int wstatus = 0;
    if (rs.child_pid > 0 &&
        waitpid((pid_t)rs.child_pid, &wstatus, WNOHANG) != (pid_t)rs.child_pid) {
        rs.verify_at = 0.0;                       /* survived: it is a stream */
        rs_lock();
        rs.status.starting = false;
        rs_unlock();
        /* KILL and HANG model a fault in a stream that was already running, so
         * they fire here rather than at spawn -- injected during the startup
         * window they would merely look like a start that failed. */
        if (s_inject == RS_INJECT_KILL) kill((pid_t)rs.child_pid, SIGKILL);
        if (s_inject == RS_INJECT_HANG) kill((pid_t)rs.child_pid, SIGSTOP);
        return;
    }

    char err[192] = {0};
    rs_spawn_result_t why = rs_classify_exit(err, sizeof(err));
    rs_unwind_attempt();

    /* One retry, video-only, per the failure-mode table: a sound card that
     * cannot be opened must not cost the picture. Exactly one -- retrying a
     * genuinely broken device in a loop burns CPU and fills logs, which is why
     * capture-node leaves restarts to the operator. */
    if (why == RS_SPAWN_FAIL_AUDIO && !rs.retried_video_only && rs.audio_device[0]) {
        rs.retried_video_only = true;
        rs.audio_device[0] = '\0';
        rs_lock();
        rs.status.audio_active = false;
        snprintf(rs.status.audio_note, sizeof(rs.status.audio_note),
                 "audio device could not be opened; streaming video-only");
        rs_unlock();

        char retry_err[192] = {0};
        if (rs_spawn_attempt(&rs.opts, rs.use_nvenc, rs.audio_device,
                             NULL, NULL, retry_err, sizeof(retry_err)) == RS_SPAWN_OK) {
            rs.verify_at = rs_now() + 1.5;
            return;
        }
        snprintf(err, sizeof(err), "%s", retry_err[0] ? retry_err : err);
    }

    rs.verify_at = 0.0;
    rs_free_ring();
    rs_lock();
    rs.status.starting = false;
    rs.status.running = false;
    rs.status.error = true;
    snprintf(rs.status.err_text, sizeof(rs.status.err_text), "%s",
             err[0] ? err : "the stream could not be started");
    rs_unlock();
}

void gui_rtsp_stream_request_stop(void)
{
    rs.verify_at = 0.0;
    if (!rs.thread_running) return;
    gui_preview_mux_remove(&rs.tap);
    if (rs.stop_at == 0.0) rs.stop_at = rs_now();
    atomic_store_explicit(&rs.run, false, memory_order_release);
    uint64_t one = 1;
    ssize_t w = write(rs.wake_fd, &one, sizeof(one));
    (void)w;
}

static void rs_reap_child(void)
{
    if (rs.child_pid <= 0) return;
    pid_t pid = (pid_t)rs.child_pid;

    /* A SIGSTOPped child never reacts to SIGTERM; continue it first so the
     * ladder can actually run. */
    kill(pid, SIGCONT);
    kill(pid, SIGTERM);
    double deadline = rs_now() + RS_REAP_LIMIT_S;
    while (rs_now() < deadline) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { rs.child_pid = 0; return; }
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    rs.child_pid = 0;
}

void gui_rtsp_stream_finish(void)
{
    rs.verify_at = 0.0;
    if (!rs.thread_running) return;

    gui_preview_mux_remove(&rs.tap);
    if (rs.stop_at == 0.0) rs.stop_at = rs_now();
    atomic_store_explicit(&rs.run, false, memory_order_release);
    uint64_t one = 1;
    ssize_t w = write(rs.wake_fd, &one, sizeof(one));
    (void)w;

    pthread_join(rs.thread, NULL);
    rs.thread_running = false;

    if (rs.sock >= 0) { close(rs.sock); rs.sock = -1; }
    rs_reap_child();
    if (rs.wake_fd >= 0) { close(rs.wake_fd); rs.wake_fd = -1; }
    rs_free_ring();

    if (rs.holding_preview) {
        gui_preview_hold_release();
        rs.holding_preview = false;
    }

    rs_lock();
    rs.status.running = false;
    rs.status.child_pid = 0;
    rs_unlock();
}

void gui_rtsp_stream_shutdown(void)
{
    if (rs.thread_running) {
        gui_rtsp_stream_request_stop();
        gui_rtsp_stream_finish();
    }
}

/* ---- headless harness ------------------------------------------------------ */

/* The app drives gui_rtsp_stream_poll() from its frame loop; a headless harness
 * has no frame loop, so it has to drive it too. Returns once the start has
 * resolved one way or the other. */
static void rs_settle_start(double limit_s)
{
    double deadline = rs_now() + limit_s;
    while (rs_now() < deadline) {
        gui_rtsp_stream_poll();
        if (!gui_rtsp_stream_get_status().starting) return;
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

int gui_rtsp_stream_test_main(const char *device, int seconds)
{
    if (!device || !device[0]) device = "/dev/video0";
    if (seconds <= 0) seconds = 10;

    gui_mediamtx_config_t ports = gui_mediamtx_default_config();
    char err[256];
    if (gui_mediamtx_start(&ports, err, sizeof(err)) != 0) {
        fprintf(stderr, "mediamtx: %s\n", err);
        return 1;
    }

    /* Same sequence as gui_video_record_test_main: geometry only exists once a
     * device and mode have been selected, so connecting without selecting
     * yields a 0x0 stream that ffmpeg rejects at startup. */
    gui_preview_init(NULL);
    gui_preview_refresh_devices();
    size_t n_devs = 0;
    const preview_device_t *devs = gui_preview_devices(&n_devs);
    if (n_devs == 0) {
        fprintf(stderr, "no preview devices\n");
        gui_mediamtx_stop();
        return 1;
    }
    int idx = -1;
    for (size_t i = 0; i < n_devs; i++) {
        if (strcmp(devs[i].path, device) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) {
        fprintf(stderr, "no such device: %s\n", device);
        gui_mediamtx_stop();
        return 1;
    }
    gui_preview_select(idx, 0);

    if (gui_preview_connect() != 0) {
        preview_status_t ps = gui_preview_get_status();
        fprintf(stderr, "preview: %s\n", ps.err_text);
        gui_preview_shutdown();
        gui_mediamtx_stop();
        return 1;
    }
    preview_status_t st = gui_preview_get_status();

    gui_rtsp_stream_opts_t opts = {0};
    opts.width = st.width;
    opts.height = st.height;
    opts.pitch = gui_preview_negotiated_pitch();
    if (opts.pitch == 0) opts.pitch = st.width * 2;
    opts.fps_num = st.fps_num ? st.fps_num : 25;
    opts.fps_den = st.fps_den ? st.fps_den : 1;
    opts.video_device = device;
    opts.audio_device = "";
    opts.encoder = RTSP_ENCODER_AUTO;
    opts.ports = ports;

    if (gui_rtsp_stream_start(&opts, err, sizeof(err)) != 0) {
        fprintf(stderr, "stream: %s\n", err);
        gui_preview_disconnect();
        gui_preview_shutdown();
        gui_mediamtx_stop();
        return 1;
    }

    rs_settle_start(6.0);
    gui_rtsp_stream_status_t s0 = gui_rtsp_stream_get_status();
    if (s0.error) {
        fprintf(stderr, "stream: %s\n", s0.err_text);
        gui_rtsp_stream_finish();
        gui_preview_disconnect();
        gui_preview_shutdown();
        gui_mediamtx_stop();
        return 1;
    }
    printf("streaming %ux%u @ %u/%u -> %s\n", opts.width, opts.height,
           opts.fps_num, opts.fps_den, s0.url_rtsp);
    printf("  audio: %s%s\n", s0.audio_active ? s0.audio_device : "off",
           s0.audio_active ? "" : s0.audio_note);

    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        gui_rtsp_stream_poll();
        gui_rtsp_stream_status_t s = gui_rtsp_stream_get_status();
        printf("  t=%2ds submitted=%llu written=%llu dropped=%llu%s\n", i + 1,
               (unsigned long long)s.frames_submitted,
               (unsigned long long)s.frames_written,
               (unsigned long long)s.frames_dropped,
               s.error ? " ERROR" : "");
    }

    gui_rtsp_stream_status_t s = gui_rtsp_stream_get_status();
    gui_rtsp_stream_request_stop();
    gui_rtsp_stream_finish();
    gui_preview_disconnect();
    gui_preview_shutdown();
    gui_mediamtx_stop();

    if (s.error) {
        fprintf(stderr, "stream error: %s\n", s.err_text);
        return 1;
    }
    if (s.frames_written == 0) {
        fprintf(stderr, "no frames reached the encoder\n");
        return 1;
    }
    puts("rtsp stream test passed");
    return 0;
}


/* ---- fault injection harness ---------------------------------------------- */

static int fault_expect(bool ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok:  " : "FAIL:", what);
    return ok ? 0 : 1;
}

/* Runs the recorder and the stream against the same tap, injects one fault, and
 * asserts both the documented stream behaviour AND that the recorder never
 * noticed. The second half is the point: a monitoring feature that can cost the
 * QC artifact a frame is not worth having. */
int gui_rtsp_stream_fault_test_main(const char *device, const char *fault, int seconds)
{
    if (!device || !device[0]) device = "/dev/video0";
    if (!fault || !fault[0]) fault = "none";
    if (seconds <= 0) seconds = 6;

    rs_inject_t inject = RS_INJECT_NONE;
    if      (strcmp(fault, "none") == 0)       inject = RS_INJECT_NONE;
    else if (strcmp(fault, "kill") == 0)       inject = RS_INJECT_KILL;
    else if (strcmp(fault, "hang") == 0)       inject = RS_INJECT_HANG;
    else if (strcmp(fault, "bad-args") == 0)   inject = RS_INJECT_BAD_ARGS;
    else if (strcmp(fault, "no-audio") == 0)   inject = RS_INJECT_NO_AUDIO;
    else if (strcmp(fault, "busy-audio") == 0) inject = RS_INJECT_BUSY_AUDIO;
    else {
        fprintf(stderr, "unknown fault: %s "
                        "(none|kill|hang|bad-args|no-audio|busy-audio)\n", fault);
        return 2;
    }

    gui_mediamtx_config_t ports = gui_mediamtx_default_config();
    char err[256] = {0};

    gui_preview_init(NULL);
    gui_preview_refresh_devices();
    size_t n_devs = 0;
    const preview_device_t *devs = gui_preview_devices(&n_devs);
    int idx = -1;
    for (size_t i = 0; i < n_devs; i++) {
        if (strcmp(devs[i].path, device) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) { fprintf(stderr, "no such device: %s\n", device); return 1; }
    gui_preview_select(idx, 0);
    if (gui_preview_connect() != 0) {
        preview_status_t ps = gui_preview_get_status();
        fprintf(stderr, "preview: %s\n", ps.err_text);
        gui_preview_shutdown();
        return 1;
    }
    preview_status_t st = gui_preview_get_status();
    uint32_t pitch = gui_preview_negotiated_pitch();
    if (pitch == 0) pitch = st.width * 2;

    /* The recorder is the control: it shares the tap through the mux, and its
     * counters are what must not move when the stream misbehaves. */
    char mkv[256];
    snprintf(mkv, sizeof(mkv), "%s/misrc-fault-control.mkv",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
    if (gui_video_record_start(mkv, VIDEO_CODEC_H264, st.width, st.height, pitch,
                               st.fps_num, st.fps_den, err, sizeof(err)) != 0) {
        fprintf(stderr, "control recorder: %s\n", err);
        gui_preview_disconnect();
        gui_preview_shutdown();
        return 1;
    }

    gui_rtsp_stream_opts_t opts = {0};
    opts.width = st.width; opts.height = st.height; opts.pitch = pitch;
    opts.fps_num = st.fps_num ? st.fps_num : 25;
    opts.fps_den = st.fps_den ? st.fps_den : 1;
    opts.video_device = device;
    opts.audio_device = "";
    opts.encoder = RTSP_ENCODER_AUTO;
    opts.ports = ports;

    gui_rtsp_stream_set_inject(inject);
    /* The whole point of the async start: this call must not block the caller,
     * because in the app the caller is the render thread. */
    (void)gui_rtsp_stream_probe();   /* the panel has already done this */
    double t_start = rs_now();
    int start_rc = gui_rtsp_stream_start(&opts, err, sizeof(err));
    double start_ms = (rs_now() - t_start) * 1000.0;
    printf("  gui_rtsp_stream_start() returned in %.1f ms\n", start_ms);
    /* start() only launches now. A failure that used to come back in start_rc
     * arrives through the status instead, once the poll has run. */
    if (start_rc == 0) rs_settle_start(8.0);
    gui_rtsp_stream_status_t s0 = gui_rtsp_stream_get_status();
    if (start_rc == 0 && s0.error) {
        start_rc = -1;
        snprintf(err, sizeof(err), "%s", s0.err_text);
    }

    printf("fault=%s start_rc=%d%s%s\n", fault, start_rc,
           start_rc != 0 ? " err=" : "", start_rc != 0 ? err : "");
    if (start_rc == 0) {
        if (s0.audio_active) {
            printf("  audio: %s\n", s0.audio_device);
        } else {
            printf("  audio: off (%s)\n",
                   s0.audio_note[0] ? s0.audio_note : "no reason recorded");
        }
    }

    gui_video_record_status_t r0 = gui_video_record_get_status();
    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        gui_rtsp_stream_poll();
    }
    gui_rtsp_stream_status_t s1 = gui_rtsp_stream_get_status();
    gui_video_record_status_t r1 = gui_video_record_get_status();

    int bad = 0;
    switch (inject) {
    case RS_INJECT_NONE:
        bad |= fault_expect(start_rc == 0, "stream started");
        bad |= fault_expect(s1.frames_written > 0, "frames reached the encoder");
        bad |= fault_expect(!s1.error, "no error reported");
        break;
    case RS_INJECT_KILL:
        bad |= fault_expect(start_rc == 0, "stream started before the kill");
        bad |= fault_expect(s1.error, "a killed encoder is reported, not hidden");
        break;
    case RS_INJECT_HANG:
        bad |= fault_expect(start_rc == 0, "stream started before the stop");
        bad |= fault_expect(s1.frames_dropped > 0, "a wedged encoder drops frames");
        break;
    case RS_INJECT_BAD_ARGS:
        bad |= fault_expect(start_rc != 0, "an unusable encoder fails the start");
        bad |= fault_expect(err[0] != '\0', "the failure names a reason");
        break;
    case RS_INJECT_NO_AUDIO:
        bad |= fault_expect(start_rc == 0, "an absent audio device still streams video");
        bad |= fault_expect(!s0.audio_active, "audio reported inactive");
        bad |= fault_expect(s0.audio_note[0] != '\0', "the reason is displayable");
        bad |= fault_expect(s1.frames_written > 0, "video kept flowing");
        break;
    case RS_INJECT_BUSY_AUDIO:
        bad |= fault_expect(start_rc == 0, "an unopenable audio device still streams video");
        bad |= fault_expect(!gui_rtsp_stream_get_status().audio_active,
                            "audio dropped after the retry");
        bad |= fault_expect(s1.frames_written > 0, "video kept flowing");
        break;
    }

    /* The control, in every case. */
    bad |= fault_expect(r1.frames_written > r0.frames_written,
                        "the recorder kept writing throughout");
    bad |= fault_expect(r1.frames_dropped == r0.frames_dropped,
                        "the recorder dropped nothing");
    bad |= fault_expect(!r1.error, "the recorder reported no error");

    double t0 = rs_now();
    gui_rtsp_stream_request_stop();
    gui_rtsp_stream_finish();
    double teardown = rs_now() - t0;
    bad |= fault_expect(teardown < (double)RS_REAP_LIMIT_S + 3.0,
                        "teardown completed within its deadline");
    printf("  teardown %.2fs\n", teardown);

    gui_video_record_request_stop();
    gui_video_record_finish();
    gui_preview_disconnect();
    gui_preview_shutdown();
    gui_mediamtx_stop();
    remove(mkv);
    gui_rtsp_stream_set_inject(RS_INJECT_NONE);

    if (bad) { fprintf(stderr, "fault test FAILED (%s)\n", fault); return 1; }
    printf("fault test passed (%s)\n", fault);
    return 0;
}

#else /* not Linux */

bool gui_rtsp_stream_probe(void) { return false; }
bool gui_rtsp_stream_has_nvenc(void) { return false; }
int gui_rtsp_stream_start(const gui_rtsp_stream_opts_t *opts, char *err, size_t err_cap)
{
    (void)opts;
    if (err && err_cap) snprintf(err, err_cap, "RTSP streaming requires Linux");
    return -1;
}
void gui_rtsp_stream_request_stop(void) { }
void gui_rtsp_stream_finish(void) { }
void gui_rtsp_stream_shutdown(void) { }
gui_rtsp_stream_status_t gui_rtsp_stream_get_status(void)
{
    gui_rtsp_stream_status_t st = {0};
    return st;
}
bool gui_rtsp_stream_is_running(void) { return false; }
void gui_rtsp_stream_set_inject(rs_inject_t what) { (void)what; }
int gui_rtsp_stream_test_main(const char *device, int seconds)
{
    (void)device; (void)seconds;
    fprintf(stderr, "requires Linux\n");
    return 2;
}
int gui_rtsp_stream_fault_test_main(const char *device, const char *fault, int seconds)
{
    (void)device; (void)fault; (void)seconds;
    fprintf(stderr, "requires Linux\n");
    return 2;
}

#endif /* __linux__ */
