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

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        rs_free_ring();
        snprintf(err, err_cap, "could not create the encoder socket: %s", strerror(errno));
        return -1;
    }
    int want = RS_SNDBUF_BYTES;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
    /* Without a send timeout a wedged ffmpeg blocks the writer inside send()
     * forever, where it can never observe the stop flag. */
    struct timeval sndto = { 0, 200 * 1000 };
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

    rs.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (rs.wake_fd < 0) {
        close(sv[0]); close(sv[1]); rs_free_ring();
        snprintf(err, err_cap, "could not create the stream wake eventfd: %s", strerror(errno));
        return -1;
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
        rs_free_ring();
        snprintf(err, err_cap, "could not start ffmpeg: %s", strerror(rc));
        return -1;
    }

    /* capture-node's trick: an ffmpeg that cannot reach mediamtx, or cannot
     * open the ALSA device, exits within a moment. Catching it here turns
     * "the stream is dead and nobody knows" into a message naming the cause. */
    struct timespec settle = { 1, 0 };
    nanosleep(&settle, NULL);
    int wstatus = 0;
    if (waitpid(pid, &wstatus, WNOHANG) == pid) {
        char reason[256] = {0};
        FILE *lf = fopen(rs.ffmpeg_log, "r");
        if (lf) {
            char line[256];
            while (fgets(line, sizeof(line), lf)) {
                line[strcspn(line, "\r\n")] = '\0';
                if (line[0]) snprintf(reason, sizeof(reason), "%s", line);
            }
            fclose(lf);
        }
        close(sv[0]); close(rs.wake_fd); rs.wake_fd = -1;
        rs_free_ring();
        if (strstr(reason, "Connection refused")) {
            snprintf(err, err_cap, "mediamtx is not accepting publishers on %s", url_buf);
        } else if (strstr(reason, "CUDA") || strstr(reason, "nvenc")) {
            snprintf(err, err_cap, "NVENC is unavailable: %s", reason);
        } else if (strstr(reason, "alsa") || strstr(reason, "ALSA") ||
                   strstr(reason, "Device or resource busy")) {
            snprintf(err, err_cap, "the dongle's audio device could not be opened: %s", reason);
        } else {
            snprintf(err, err_cap, "ffmpeg exited at startup: %s",
                     reason[0] ? reason : "see the log");
        }
        return -1;
    }

    rs.sock = sv[0];
    rs.child_pid = (int)pid;
    rs.status.child_pid = (int)pid;
    rs.status.running = true;
    atomic_store(&rs.run, true);

    if (pthread_create(&rs.thread, NULL, rs_writer_thread, NULL) != 0) {
        close(rs.sock); rs.sock = -1;
        close(rs.wake_fd); rs.wake_fd = -1;
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        rs.child_pid = 0;
        rs.status.running = false;
        rs_free_ring();
        snprintf(err, err_cap, "could not start the stream writer thread");
        return -1;
    }
    rs.thread_running = true;

    /* Hold the preview open for the life of the stream, so closing the panel
     * does not silently end the broadcast. */
    gui_preview_hold_acquire();
    rs.holding_preview = true;

    rs.tap.fn = rs_tap_cb;
    rs.tap.user = NULL;
    gui_preview_mux_add(&rs.tap);

    if (s_inject == RS_INJECT_KILL) kill(pid, SIGKILL);
    if (s_inject == RS_INJECT_HANG) kill(pid, SIGSTOP);
    return 0;
}

void gui_rtsp_stream_request_stop(void)
{
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

    gui_rtsp_stream_status_t s0 = gui_rtsp_stream_get_status();
    printf("streaming %ux%u @ %u/%u -> %s\n", opts.width, opts.height,
           opts.fps_num, opts.fps_den, s0.url_rtsp);
    printf("  audio: %s%s\n", s0.audio_active ? s0.audio_device : "off",
           s0.audio_active ? "" : s0.audio_note);

    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
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

#endif /* __linux__ */
