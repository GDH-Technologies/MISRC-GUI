/*
 * MISRC GUI - reference video recording. See gui_video_record.h for the shape.
 */

#include "gui_video_record.h"
#include "../input/gui_preview_v4l2.h"
#include "../streaming/gui_alsa_device.h"
#include "../streaming/gui_preview_tap_mux.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- timeline
 *
 * Where a frame goes on the recording's fixed-rate timeline. Slot k starts at
 * origin + k*P. A frame is placed by its own start time, so the file follows
 * what really happened:
 *
 *   - a frame >= 0.75 slot late is preceded by repeats of the previous frame
 *     (the source ran slow, or the dongle lost fields -- fed a blanked
 *     composite input the StarTech delivered 0.6% fewer frames than nominal);
 *   - a frame >= 0.75 slot early is skipped (the source ran fast);
 *   - otherwise it takes the next slot.
 *
 * After a correction the frame sits ~0.25 slot on the other side, half a slot
 * from either limit, so USB timestamp jitter (a few ms) cannot flip-flop it.
 * The worst error between a frame and its slot is under one frame, which is
 * what keeps the picture on the audio. Pure and platform-neutral so the
 * selftest runs everywhere. Returns 1 to submit (after *repeats repeats), 0 to
 * skip. */
#define VR_TL_LIMIT 0.75

static int vr_timeline_place(uint64_t ts_ns, uint64_t origin_ns,
                             uint32_t fps_num, uint32_t fps_den,
                             uint64_t *next_slot, uint32_t *repeats)
{
    *repeats = 0;
    /* double is exact here: 2^53 ns is 104 days. */
    double pos = ((double)ts_ns - (double)origin_ns) * (double)fps_num /
                 ((double)(fps_den ? fps_den : 1) * 1e9);
    double e = pos - (double)*next_slot;
    if (e <= -VR_TL_LIMIT) return 0;
    if (e >= VR_TL_LIMIT) {
        double r = floor(e + (1.0 - VR_TL_LIMIT));
        *repeats = (uint32_t)(r > 4.0e9 ? 4.0e9 : r);
    }
    *next_slot += (uint64_t)*repeats + 1;
    return 1;
}

int gui_video_record_timeline_selftest_main(void)
{
    const uint32_t num = 30000, den = 1001;
    const double P = 1e9 * den / num;
    int fails = 0;

    struct {
        const char *name;
        double ppm;          /* source rate error: + = frames arrive faster */
        double jitter_ms;    /* +- uniform */
        int    lose_every;   /* drop one frame every N (0 = never) */
        double first_ms;     /* first frame's start after the origin */
        int    seconds;
    } cases[] = {
        { "exact rate, no jitter",            0.0, 0.0,    0,  5.0, 7200 },
        { "camcorder -50 ppm, +-3 ms jitter", -50.0, 3.0,  0, 12.0, 7200 },
        { "camcorder +50 ppm, +-3 ms jitter", 50.0, 3.0,   0, 12.0, 7200 },
        { "lost fields (0.6%), +-3 ms",      0.0, 3.0,  170, 40.0, 600 },
        { "first frame 100 ms after spawn",   0.0, 1.0,    0, 100.0, 60 },
    };
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint64_t origin = 1000000000000ull;
        uint64_t next = 0, written = 0, repeats_total = 0, skips = 0, flaps = 0;
        double worst = 0.0;
        int last_kind = 0;            /* +1 repeat, -1 skip */
        uint64_t last_kind_slot = 0;
        double period = P / (1.0 + cases[c].ppm * 1e-6);
        uint64_t frames = (uint64_t)(cases[c].seconds * 1e9 / period);
        for (uint64_t i = 0; i < frames; i++) {
            if (cases[c].lose_every && i % (uint64_t)cases[c].lose_every == 7) continue;
            rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
            double j = ((double)(rng % 2001) / 1000.0 - 1.0) * cases[c].jitter_ms * 1e6;
            double t = cases[c].first_ms * 1e6 + (double)i * period + j;
            uint64_t ts = origin + (uint64_t)(t > 0 ? t : 0);
            uint32_t rep = 0;
            uint64_t before = next;
            if (!vr_timeline_place(ts, origin, num, den, &next, &rep)) {
                skips++;
                if (last_kind == +1 && before - last_kind_slot < 30) flaps++;
                last_kind = -1; last_kind_slot = before;
                continue;
            }
            if (rep && i > 0) {
                if (last_kind == -1 && before - last_kind_slot < 30) flaps++;
                last_kind = +1; last_kind_slot = before;
            }
            repeats_total += rep;
            written += rep + 1;
            /* Error between the frame's real start and its slot, in frames. */
            double err = ((double)(ts - origin) - (double)(next - 1) * P) / P;
            if (fabs(err) > worst) worst = fabs(err);
        }
        double ideal = (cases[c].first_ms * 1e6 + (double)frames * period) / P;
        double slot_err = (double)written - ideal;
        bool ok = fabs(slot_err) <= 1.0 && worst < 1.0 && flaps == 0;
        printf("%s: %s -- slots %" PRIu64 " vs ideal %.1f (%+.2f), repeats %" PRIu64
               ", skips %" PRIu64 ", worst offset %.2f frame, flip-flops %" PRIu64 "\n",
               ok ? "PASS" : "FAIL", cases[c].name, written, ideal, slot_err,
               repeats_total, skips, worst, flaps);
        if (!ok) fails++;
    }
    printf(fails ? "video timeline selftest: FAIL (%d)\n" : "video timeline selftest: PASS\n", fails);
    return fails ? 1 : 0;
}

/* Android is __linux__ too, but has no V4L2 access, no posix_spawn/environ
 * ffmpeg subprocess and no writable /dev/video* -- and upstream now builds
 * this same source list into its Android target. Route Android to the
 * non-Linux stubs below instead of compiling this in. */
#if defined(__linux__) && !defined(__ANDROID__)

#include <linux/videodev2.h>
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
#include <sys/syscall.h>
#include <sys/wait.h>

/* The reference-video ffmpeg records too, but RF comes first: it runs one I/O
 * step below the RF writers (best-effort level 1 against their 0). Set on the
 * spawning thread around posix_spawn, so the child inherits it at fork. */
#define VR_CHILD_IOPRIO ((2 << 13) | 1)

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

    /* --- timeline: written by the tap (capture thread) only --- */
    uint64_t  mono0_ns;            /* slot 0 starts here, CLOCK_MONOTONIC */
    int64_t   real0_us;            /* the same instant on CLOCK_REALTIME (ffmpeg's ALSA clock) */
    uint64_t  tl_next_slot;
    uint64_t  tl_first_ts, tl_frames_seen;
    /* Repeats of the previous frame that go before ring slot i. */
    uint32_t  slot_repeats[VR_RING_FRAMES];

    /* --- spawn parameters, kept for the video-only respawn --- */
    video_codec_t codec;
    char      out_path[512];
    char      audio_device[128];   /* "" once running video-only */
    double    spawned_at;
    bool      audio_retry_used;
    atomic_bool drain_abandoned;   /* the child stopped reading; do not wait on it */
    double      stop_at;           /* when a stop was asked for; published before run=false */
} vr;

static vr_inject_t s_inject = VR_INJECT_NONE;
void gui_video_record_set_inject(vr_inject_t what) { s_inject = what; }

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
static bool vr_find_in_path(const char *name, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    if (!path || !path[0]) return false;
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0 && len < cap - 16) {
            char cand[512];
            snprintf(cand, sizeof(cand), "%.*s/%s", (int)len, p, name);
            if (vr_is_executable(cand)) { snprintf(out, cap, "%s", cand); return true; }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return false;
}

/* Set from the settings file before probing, so the user's explicit choice
 * wins over anything found on PATH. Kept as a copy rather than a pointer into
 * gui_settings_t, because this module deliberately knows nothing about it. */
static char s_ffmpeg_override[512];

/* The caption recorder's binary, resolved by gui_video_record_cc_ffmpeg_path()
 * and cached here: the main loop asks for it every frame, and a PATH walk per
 * frame would be a stat() per PATH entry at 60 Hz. */
static char s_cc_ffmpeg[512];
static bool s_cc_resolved;

void gui_video_record_set_ffmpeg_path(const char *path)
{
    if (path && path[0]) snprintf(s_ffmpeg_override, sizeof(s_ffmpeg_override), "%s", path);
    else                 s_ffmpeg_override[0] = '\0';
    ff.probed = false;   /* a new path deserves a fresh probe */
    s_cc_resolved = false;
}

static bool vr_resolve_ffmpeg(char *out, size_t cap)
{
    if (vr_is_executable(s_ffmpeg_override)) {
        snprintf(out, cap, "%s", s_ffmpeg_override);
        return true;
    }
    const char *env = getenv("MISRC_FFMPEG");
    if (vr_is_executable(env)) { snprintf(out, cap, "%s", env); return true; }
    if (vr_find_in_path("ffmpeg", out, cap)) return true;

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

const char *gui_video_record_cc_ffmpeg_path(void)
{
    if (s_cc_resolved) return s_cc_ffmpeg;
    s_cc_resolved = true;

    /* An explicit choice governs captions and video alike, as before. */
    const char *env = getenv("MISRC_FFMPEG");
    if (vr_is_executable(s_ffmpeg_override)) {
        snprintf(s_cc_ffmpeg, sizeof(s_cc_ffmpeg), "%s", s_ffmpeg_override);
    } else if (vr_is_executable(env)) {
        snprintf(s_cc_ffmpeg, sizeof(s_cc_ffmpeg), "%s", env);
    } else if (!vr_find_in_path(GUI_CC_FFMPEG_NAME, s_cc_ffmpeg, sizeof(s_cc_ffmpeg))) {
        /* No v4l2vbi build installed under its own name: the caption probe
         * then asks the video ffmpeg, and says so if it lacks the device. */
        snprintf(s_cc_ffmpeg, sizeof(s_cc_ffmpeg), "%s", gui_video_record_ffmpeg_path());
    }
    return s_cc_ffmpeg;
}

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
static void vr_tap_cb(const uint8_t *yuyv, size_t pitch, uint32_t w, uint32_t h,
                      uint64_t ts_ns, void *user)
{
    (void)user;
    size_t want = pitch * (size_t)h;
    if (w != vr.width || h != vr.height || want != vr.frame_bytes) {
        return;   /* geometry changed under us; drop rather than write short */
    }

    /* A frame cannot have started after now. Clamping to the clock bounds the
     * repeats a driver timestamp glitch could ask for by real elapsed time --
     * a wild stamp would otherwise queue billions of them for the writer. */
    struct timespec now_ts;
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    uint64_t now_ns = (uint64_t)now_ts.tv_sec * 1000000000ull + (uint64_t)now_ts.tv_nsec;
    if (ts_ns > now_ns) ts_ns = now_ns;

    /* The source's real rate, from first and latest start times. */
    if (vr.tl_frames_seen++ == 0) vr.tl_first_ts = ts_ns;
    double ppm = 0.0;
    if (vr.tl_frames_seen > 1 && ts_ns > vr.tl_first_ts) {
        double secs = (double)(ts_ns - vr.tl_first_ts) / 1e9;
        double nominal = (double)vr.fps_num / (double)vr.fps_den;
        ppm = ((double)(vr.tl_frames_seen - 1) / secs / nominal - 1.0) * 1e6;
    }

    uint32_t repeats = 0;
    if (!vr_timeline_place(ts_ns, vr.mono0_ns, vr.fps_num, vr.fps_den,
                           &vr.tl_next_slot, &repeats)) {
        vr_lock();
        vr.status.frames_skipped++;
        vr.status.source_rate_ppm = ppm;
        vr_unlock();
        return;
    }

    uint_fast32_t head = atomic_load_explicit(&vr.head, memory_order_relaxed);
    uint_fast32_t tail = atomic_load_explicit(&vr.tail, memory_order_acquire);

    if (head - tail >= VR_RING_FRAMES) {
        /* Count a duplicate rather than just a loss: ffmpeg assigns PTS by
         * frames read, so a silently missing frame shortens the file and
         * slides it out of step with the RF timeline. The repeats this frame
         * was owed go the same way. */
        atomic_fetch_add_explicit(&vr.pending_dupes, 1 + repeats, memory_order_relaxed);
        vr_lock();
        vr.status.frames_dropped++;
        vr.status.source_rate_ppm = ppm;
        vr_unlock();
        return;
    }

    vr.slot_repeats[head % VR_RING_FRAMES] = repeats;
    memcpy(vr.slots[head % VR_RING_FRAMES], yuyv, vr.frame_bytes);
    atomic_store_explicit(&vr.head, head + 1, memory_order_release);

    vr_lock();
    vr.status.frames_submitted++;
    vr.status.source_rate_ppm = ppm;
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
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* SO_SNDTIMEO fired: the encoder is not draining. While the
             * recording is live that is ordinary backpressure, so keep trying.
             *
             * The stop state has to be read HERE, live, on every timeout. A
             * deadline captured before the send began is useless: a send that
             * was already blocked when the stop arrived would never learn one
             * exists, and the writer would loop on 200ms timeouts forever
             * while finalize waited on its join. */
            if (!atomic_load_explicit(&vr.run, memory_order_acquire) &&
                vr_now() > vr.stop_at + VR_DRAIN_LIMIT_S) {
                atomic_store(&vr.drain_abandoned, true);
                vr_set_error("reference video encoder stopped reading; gave up draining", 0);
                return false;
            }
            continue;
        }
        vr_set_error("reference video encoder stopped reading", errno);
        return false;
    }
    return true;
}

static bool vr_audio_fallback(void);

/* Send one frame. If that fails because ffmpeg could not open the audio and
 * exited, respawn it video-only once and send again: the reference video goes
 * on without its sound rather than not at all. */
static bool vr_send_frame(const uint8_t *buf)
{
    if (vr_send_all(buf, vr.frame_bytes)) return true;
    if (!vr_audio_fallback()) return false;
    return vr_send_all(buf, vr.frame_bytes);
}

static void vr_count_written(bool dupe)
{
    vr_lock();
    if (dupe) vr.status.frames_duped++;
    vr.status.frames_written++;
    vr_unlock();
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

        /* Bound the drain so a stalled encoder cannot hold finalize open. The
         * same deadline is re-checked inside vr_send_all on every send
         * timeout, which is what covers a send that was already blocked when
         * the stop arrived. */
        if (!running && vr_now() > vr.stop_at + VR_DRAIN_LIMIT_S) break;
        (void)drain_started;

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

        /* Repeats first, so they occupy the timeline slots before this frame:
         * frames the ring dropped, and slots the timeline says the source
         * never filled. Before the first frame there is nothing to repeat, so
         * the leading slots get the first frame itself, after it -- the same
         * picture either way. */
        uint_fast32_t dupes = atomic_exchange_explicit(&vr.pending_dupes, 0, memory_order_relaxed);
        dupes += vr.slot_repeats[tail % VR_RING_FRAMES];
        if (dupes && have_last) {
            for (uint_fast32_t i = 0; i < dupes; i++) {
                if (!vr_send_frame(vr.last_frame)) goto out;
                vr_count_written(true);
            }
            dupes = 0;
        }

        const uint8_t *slot = vr.slots[tail % VR_RING_FRAMES];
        if (!vr_send_frame(slot)) goto out;

        memcpy(vr.last_frame, slot, vr.frame_bytes);
        have_last = true;
        atomic_store_explicit(&vr.tail, tail + 1, memory_order_release);
        vr_count_written(false);

        for (uint_fast32_t i = 0; i < dupes; i++) {
            if (!vr_send_frame(vr.last_frame)) goto out;
            vr_count_written(true);
        }
    }

out:
    /* Closing our end delivers EOF, so ffmpeg flushes its encoder and writes a
     * real trailer instead of being killed mid-file. */
    if (vr.sock >= 0) { close(vr.sock); vr.sock = -1; }
    return NULL;
}

/* ----------------------------------------------------------------- spawn */

/* The track title, so this audio cannot be mistaken for an RF-decoded track
 * muxed into the same file later. */
#define VR_AUDIO_TITLE "title=Camcorder line out (USB reference)"

/* Whether the source frames are two woven fields. Every SD analog raster is
 * (uvcvideo reports V4L2_FIELD_NONE for the MS210x's 720x480 all the same);
 * anything else only when the driver says so. */
static bool vr_source_interlaced(void)
{
    if (vr.height == 480 || vr.height == 486 || vr.height == 576) return true;
    preview_status_t ps = gui_preview_get_status();
    return ps.field == V4L2_FIELD_INTERLACED || ps.field == V4L2_FIELD_INTERLACED_TB ||
           ps.field == V4L2_FIELD_INTERLACED_BT;
}

static void vr_build_argv(char *argv[], int *argc_out, video_codec_t codec,
                          char *size_buf, size_t size_cap,
                          char *rate_buf, size_t rate_cap,
                          char *aspect_buf, size_t aspect_cap,
                          char *offset_buf, size_t offset_cap,
                          const char *audio_device,
                          const char *out_path)
{
    bool audio = audio_device && audio_device[0];
    snprintf(size_buf, size_cap, "%ux%u", vr.width, vr.height);
    if (vr.fps_den <= 1) snprintf(rate_buf, rate_cap, "%u", vr.fps_num);
    else                 snprintf(rate_buf, rate_cap, "%u/%u", vr.fps_num, vr.fps_den);
    gui_preview_aspect_arg(aspect_buf, aspect_cap);

    int n = 0;
    argv[n++] = ff.path;
    argv[n++] = (char *)"-hide_banner";
    argv[n++] = (char *)"-nostdin";
    argv[n++] = (char *)"-nostats";
    argv[n++] = (char *)"-loglevel"; argv[n++] = (char *)"+time+warning";   /* time-stamped, to place an xrun */
    argv[n++] = (char *)"-y";
    if (audio) {
        /* The dongle's audio, opened by ffmpeg itself, and FIRST: inputs open
         * in order and the raw video waits for its first frame, so a busy or
         * missing device fails at once, before any frame is lost to it.
         *
         * Timing (verified against FFmpeg's sources, 2026-11-16):
         *  - alsa_dec stamps each packet with its first sample's capture
         *    time, smoothed, on CLOCK_REALTIME (av_gettime);
         *  - -copyts keeps those stamps -- without it every input is shifted
         *    to its own zero and the A/V offset is thrown away;
         *  - -itsoffset -<real0> makes them relative to the spawn instant,
         *    which is also video slot 0 (mono0, sampled back to back). */
        int64_t r = vr.real0_us;
        snprintf(offset_buf, offset_cap, "-%" PRId64 ".%06" PRId64,
                 r / 1000000, r % 1000000);
        argv[n++] = (char *)"-copyts";
        argv[n++] = (char *)"-f";                 argv[n++] = (char *)"alsa";
        argv[n++] = (char *)"-sample_rate";       argv[n++] = (char *)"48000";
        argv[n++] = (char *)"-ch_layout";         argv[n++] = (char *)"stereo";
        argv[n++] = (char *)"-itsoffset";         argv[n++] = offset_buf;
        argv[n++] = (char *)"-i";                 argv[n++] = (char *)audio_device;
        /* No probing of the raw video: its format is fully given below, and
         * while ffmpeg probes the pipe nobody reads the ALSA input, whose
         * dsnoop buffer holds 341 ms -- measured on the live StarTech, the
         * default probe cost 13 xruns at startup and 7 ring drops. */
        argv[n++] = (char *)"-probesize";         argv[n++] = (char *)"32";
        argv[n++] = (char *)"-analyzeduration";   argv[n++] = (char *)"0";
    }
    argv[n++] = (char *)"-f";             argv[n++] = (char *)"rawvideo";
    argv[n++] = (char *)"-pixel_format";  argv[n++] = (char *)"yuyv422";
    argv[n++] = (char *)"-video_size";    argv[n++] = size_buf;
    argv[n++] = (char *)"-framerate";     argv[n++] = rate_buf;
    argv[n++] = (char *)"-i";             argv[n++] = (char *)"-";
    if (audio) {
        argv[n++] = (char *)"-map"; argv[n++] = (char *)"1:v";
        argv[n++] = (char *)"-map"; argv[n++] = (char *)"0:a";
        /* The writer is the only place a frame is repeated or skipped; auto
         * would pick vfr for Matroska and start deciding too. */
        argv[n++] = (char *)"-fps_mode"; argv[n++] = (char *)"passthrough";
        /* Keep the sample count on the arrival stamps: async > 1 enables
         * stretching up to async samples/s (96 = 2000 ppm, inaudible; a
         * crystal needs < 100) and min_comp 0.001; first_pts pads the start
         * to the origin. Without it a player clocks the audio by its sample
         * count -- the dongle's crystal -- and the drift comes back. */
        argv[n++] = (char *)"-af"; argv[n++] = (char *)"aresample=async=96:first_pts=0";
        argv[n++] = (char *)"-c:a"; argv[n++] = (char *)"flac";
        argv[n++] = (char *)"-metadata:s:a:0"; argv[n++] = (char *)VR_AUDIO_TITLE;
        /* The ALSA input never ends by itself: stop with the video. */
        argv[n++] = (char *)"-shortest";
    } else {
        argv[n++] = (char *)"-an";
    }
    argv[n++] = (char *)"-sn";
    /* SD rasters are not square-pixel: 720x576 without this shows a 5:4
     * stretch and 720x480 a 3:2 one. The value is computed from the negotiated
     * geometry and the user's aspect setting rather than hard-coded, so the
     * MKV, the RTSP stream and the on-screen preview cannot disagree -- and so
     * a non-SDTV source is not forced into 4:3 it never was.
     *
     * Note there is deliberately no crop filter here. The preview crop is a
     * viewing aid; this file must keep the full active raster so it stays
     * frame-comparable with a tbc-video-export of the same tape. */
    argv[n++] = (char *)"-aspect";        argv[n++] = aspect_buf;

    if (codec == VIDEO_CODEC_FFV1) {
        argv[n++] = (char *)"-c:v";     argv[n++] = (char *)"ffv1";
        argv[n++] = (char *)"-level";   argv[n++] = (char *)"3";
        argv[n++] = (char *)"-coder";   argv[n++] = (char *)"1";
        argv[n++] = (char *)"-context"; argv[n++] = (char *)"1";
        argv[n++] = (char *)"-g";       argv[n++] = (char *)"1";
        argv[n++] = (char *)"-slices";  argv[n++] = (char *)"4";
        argv[n++] = (char *)"-slicecrc";argv[n++] = (char *)"1";
        /* The lossless reference keeps the dongle's full 4:2:2 chroma; no
         * GPU decodes FFV1, so every player decodes it in software anyway. */
        argv[n++] = (char *)"-pix_fmt"; argv[n++] = (char *)"yuv422p";
    } else if (s_inject == VR_INJECT_BAD_ARGS) {
        /* A codec ffmpeg does not have: it must fail fast, with the reason in
         * its own log, rather than hanging or half-writing a file. */
        argv[n++] = (char *)"-c:v";     argv[n++] = (char *)"no_such_codec_xyz";
    } else {
        argv[n++] = (char *)"-c:v";     argv[n++] = (char *)"libx264";
        argv[n++] = (char *)"-preset";  argv[n++] = (char *)"veryfast";
        argv[n++] = (char *)"-crf";     argv[n++] = (char *)"18";
        argv[n++] = (char *)"-g";       argv[n++] = (char *)"25";
        /* 4:2:0 High profile, not 4:2:2 (High 4:2:2): no common GPU decodes
         * H.264 4:2:2 (an RTX 3080's NVDEC does not), and VLC 3.0 hands the
         * stream to VDPAU anyway and shows no picture -- audio over an empty
         * window (wm, 2026-11-17). The chroma is halved per FIELD on an
         * interlaced source: a progressive conversion averages the two
         * fields' colour, which smears it on motion. -profile:v makes x264
         * refuse rather than silently write another profile. */
        argv[n++] = (char *)"-vf";
        argv[n++] = (char *)(vr_source_interlaced() ? "scale=interl=1,format=yuv420p"
                                                    : "format=yuv420p");
        argv[n++] = (char *)"-pix_fmt";   argv[n++] = (char *)"yuv420p";
        argv[n++] = (char *)"-profile:v"; argv[n++] = (char *)"high";
        if (audio) {
            /* A live sound card cannot be paused, and ffmpeg's scheduler
             * pauses ("chokes") any input whose output runs >= 100 ms ahead
             * of the slowest muxed stream (SCHEDULE_TOLERANCE in
             * ffmpeg_sched.c; no exemption for live devices). veryfast holds
             * frames back for lookahead, B-frames and frame threads -- about
             * half a second -- which outran the 100 ms plus dsnoop's 341 ms
             * buffer: measured on the live StarTech, ALSA xruns from ~12 s in.
             * zerolatency takes that delay out (no lookahead, no B-frames,
             * sliced threads). FFV1 is intra-only with slice threads and has
             * none to take out. */
            argv[n++] = (char *)"-tune"; argv[n++] = (char *)"zerolatency";
        }
    }
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

/* Socket, argv, posix_spawn. Sets vr.sock and vr.child_pid. append_log keeps
 * the previous attempt's ffmpeg log (the audio failure) above the next one. */
static int vr_spawn(const char *audio_device, bool append_log, char *err, size_t err_cap)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        snprintf(err, err_cap, "could not create the encoder socket: %s", strerror(errno));
        return -1;
    }
    /* The default 208 KB is a quarter of one frame; without this every send
     * round-trips to ffmpeg's read loop. */
    int want = VR_SNDBUF_BYTES;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
    /* Without a send timeout a stopped or wedged ffmpeg blocks the writer
     * inside send() forever, where it can never observe the stop flag -- the
     * drain deadline is checked at the top of the loop, which such a thread
     * never reaches again. This is what makes the deadline reachable. */
    struct timeval sndto = { 0, 200 * 1000 };
    setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));

    char size_buf[32], rate_buf[32], aspect_buf[16], offset_buf[40];
    char *argv[80];
    int argc = 0;
    vr_build_argv(argv, &argc, vr.codec, size_buf, sizeof(size_buf),
                  rate_buf, sizeof(rate_buf), aspect_buf, sizeof(aspect_buf),
                  offset_buf, sizeof(offset_buf), audio_device, vr.out_path);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, sv[1], 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    /* Keeps the terminal clean while leaving a failure diagnosable -- the
     * difference between "video failed" and "video failed: No space left". */
    posix_spawn_file_actions_addopen(&fa, 2, vr.ffmpeg_log,
                                     O_WRONLY | O_CREAT | (append_log ? O_APPEND : O_TRUNC),
                                     0644);

    pid_t pid = 0;
    /* posix_spawn for the same reason as the preview popout: this process has
     * display, audio, capture and libusb threads plus a live GL context, and
     * only async-signal-safe calls are legal between fork and exec. Note it
     * returns the error directly and does NOT set errno. */
    int saved_io = (int)syscall(SYS_ioprio_get, 1 /* IOPRIO_WHO_PROCESS */, 0);
    (void)syscall(SYS_ioprio_set, 1, 0, VR_CHILD_IOPRIO);
    int rc = posix_spawn(&pid, ff.path, &fa, NULL, argv, environ);
    (void)syscall(SYS_ioprio_set, 1, 0, saved_io);
    posix_spawn_file_actions_destroy(&fa);
    close(sv[1]);   /* mandatory: otherwise closing our end never delivers EOF */

    if (rc != 0) {
        close(sv[0]);
        snprintf(err, err_cap, "could not start ffmpeg: %s", strerror(rc));
        return -1;
    }
    vr.sock = sv[0];
    vr.child_pid = (int)pid;
    vr.spawned_at = vr_now();
    vr_lock();
    vr.status.child_pid = (int)pid;
    vr_unlock();
    return 0;
}

/* Called by the writer when a send failed. If ffmpeg died within its first
 * seconds because it could not open the audio, respawn it video-only, once,
 * and resend what the dead one swallowed so the timeline stays whole. Any
 * other failure is left to the normal error path. */
static bool vr_audio_fallback(void)
{
    if (!vr.audio_device[0] || vr.audio_retry_used) return false;
    if (!atomic_load_explicit(&vr.run, memory_order_acquire)) return false;
    if (vr_now() - vr.spawned_at > 10.0) return false;
    vr.audio_retry_used = true;

    /* It has exited, or is about to: our send failed because it closed. */
    int wstatus = 0;
    double deadline = vr_now() + 2.0;
    pid_t r = 0;
    while ((r = waitpid(vr.child_pid, &wstatus, WNOHANG)) == 0 && vr_now() < deadline) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (r != vr.child_pid) return false;   /* alive: not an open failure */

    char log[2048] = "";
    FILE *f = fopen(vr.ffmpeg_log, "r");
    if (f) { size_t n = fread(log, 1, sizeof(log) - 1, f); log[n] = '\0'; fclose(f); }
    if (!strstr(log, vr.audio_device) && !strstr(log, "alsa") && !strstr(log, "ALSA")) {
        /* Died of something else. Already reaped here, so say how and clear
         * the pid, or finish() would wait out its deadline on a dead one. */
        char msg[96];
        if (WIFEXITED(wstatus)) snprintf(msg, sizeof(msg), "ffmpeg exited with code %d", WEXITSTATUS(wstatus));
        else                    snprintf(msg, sizeof(msg), "ffmpeg terminated by signal %d", WTERMSIG(wstatus));
        vr_set_error(msg, 0);
        vr.child_pid = 0;
        vr_lock();
        vr.status.child_pid = 0;
        vr_unlock();
        return false;
    }

    /* The line that says why, and who holds the device if anyone does. */
    /* ffmpeg's own verdict: "cannot open audio device <dev> (<errno text>)". */
    char reason[96] = "could not be opened";
    const char *line = strstr(log, "cannot open audio device");
    if (line) {
        size_t line_len = strcspn(line, "\n");
        const char *open = memchr(line, '(', line_len);
        const char *close = open ? memchr(open, ')', line_len - (size_t)(open - line)) : NULL;
        if (open && close && close > open + 1 && (size_t)(close - open - 1) < sizeof(reason) - 16) {
            snprintf(reason, sizeof(reason), "could not be opened (%.*s)",
                     (int)(close - open - 1), open + 1);
        }
    }
    char holder[96] = "";
    gui_alsa_capture_holder(vr.audio_device, holder, sizeof(holder));

    if (vr.sock >= 0) { close(vr.sock); vr.sock = -1; }
    char err[192];
    if (vr_spawn("", true, err, sizeof(err)) != 0) {
        vr_set_error(err, 0);
        return false;
    }

    vr_lock();
    vr.status.error = false;
    vr.status.err_text[0] = '\0';
    vr.status.audio_active = false;
    snprintf(vr.status.audio_note, sizeof(vr.status.audio_note),
             "audio device %s: %s%s%s; recording video-only",
             vr.audio_device, reason, holder[0] ? "; held by " : "", holder);
    uint64_t lost = vr.status.frames_written;
    vr_unlock();
    vr.audio_device[0] = '\0';

    /* The dead ffmpeg took the first frames with it. Fill their slots so the
     * new file's frame 0 is still the origin. */
    for (uint64_t i = 0; i < lost; i++) {
        if (!vr_send_all(vr.last_frame, vr.frame_bytes)) return false;
        vr_count_written(true);
    }
    return true;
}

int gui_video_record_start(const char *out_path, video_codec_t codec,
                           uint32_t width, uint32_t height, uint32_t pitch,
                           uint32_t fps_num, uint32_t fps_den,
                           const char *audio_device,
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
    atomic_store(&vr.drain_abandoned, false);
    vr.stop_at = 0.0;
    atomic_store(&vr.pending_dupes, 0);
    memset(vr.slot_repeats, 0, sizeof(vr.slot_repeats));
    vr.tl_next_slot = 0;
    vr.tl_first_ts = 0;
    vr.tl_frames_seen = 0;
    vr.codec = codec;
    vr.audio_retry_used = false;
    snprintf(vr.out_path, sizeof(vr.out_path), "%s", out_path);
    snprintf(vr.audio_device, sizeof(vr.audio_device), "%s", audio_device ? audio_device : "");
    snprintf(vr.status.path, sizeof(vr.status.path), "%s", out_path);
    snprintf(vr.ffmpeg_log, sizeof(vr.ffmpeg_log), "%s.ffmpeg.log", out_path);
    if (vr.audio_device[0]) {
        vr.status.audio_active = true;
        snprintf(vr.status.audio_device, sizeof(vr.status.audio_device), "%s", vr.audio_device);
    }

    for (int i = 0; i < VR_RING_FRAMES; i++) {
        vr.slots[i] = malloc(vr.frame_bytes);
        if (!vr.slots[i]) { vr_free_ring(); snprintf(err, err_cap, "out of memory for the video ring"); return -1; }
    }
    vr.last_frame = calloc(1, vr.frame_bytes);
    if (!vr.last_frame) { vr_free_ring(); snprintf(err, err_cap, "out of memory for the video ring"); return -1; }

    vr.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (vr.wake_fd < 0) {
        vr_free_ring();
        snprintf(err, err_cap, "could not create the video wake eventfd: %s", strerror(errno));
        return -1;
    }

    /* The shared origin: video slot 0 and audio pts 0. Sampled back to back;
     * NTP slews both clocks alike, so only a step of the realtime clock
     * mid-recording could move one against the other (it would shift the
     * audio by the step; chrony slews by default). */
    struct timespec m, rt;
    clock_gettime(CLOCK_MONOTONIC, &m);
    clock_gettime(CLOCK_REALTIME, &rt);
    vr.mono0_ns = (uint64_t)m.tv_sec * 1000000000ull + (uint64_t)m.tv_nsec;
    vr.real0_us = (int64_t)rt.tv_sec * 1000000 + (int64_t)rt.tv_nsec / 1000;

    if (vr_spawn(vr.audio_device, false, err, err_cap) != 0) {
        close(vr.wake_fd); vr.wake_fd = -1;
        vr_free_ring();
        return -1;
    }

    vr.status.running = true;
    atomic_store(&vr.run, true);

    /* Normal priority, deliberately: the RF writers run SCHED_FIFO, and a
     * blocking send() to a possibly-hung child must never sit in that class. */
    if (pthread_create(&vr.thread, NULL, vr_writer_thread, NULL) != 0) {
        close(vr.sock); vr.sock = -1;
        close(vr.wake_fd); vr.wake_fd = -1;
        kill(vr.child_pid, SIGKILL);
        waitpid(vr.child_pid, NULL, 0);
        vr.child_pid = 0;
        vr.status.running = false;
        vr_free_ring();
        snprintf(err, err_cap, "could not start the video writer thread");
        return -1;
    }
    vr.thread_running = true;

    vr.tap.fn = vr_tap_cb;
    vr.tap.user = NULL;
    gui_preview_mux_add(&vr.tap);
    return 0;
}

void gui_video_record_request_stop(void)
{
    if (!vr.thread_running) return;
    gui_preview_mux_remove(&vr.tap);   /* returns once no tap call is in flight */
    vr.stop_at = vr_now();             /* published before the flag the writer reads */
    atomic_store_explicit(&vr.run, false, memory_order_release);
    uint64_t one = 1;
    ssize_t w = write(vr.wake_fd, &one, sizeof(one));
    (void)w;
}

static void vr_reap_child(void)
{
    if (vr.child_pid <= 0) return;
    /* If the writer gave up draining, the child demonstrably is not reading, so
     * there is nothing to wait politely for. */
    double deadline = vr_now() +
        (atomic_load(&vr.drain_abandoned) ? 0.0 : VR_REAP_LIMIT_S);
    int wstatus = 0;

    while (vr_now() < deadline) {
        pid_t r = waitpid(vr.child_pid, &wstatus, WNOHANG);
        if (r == vr.child_pid) goto reaped;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    /* SIGCONT first: a SIGSTOPped child queues SIGTERM and never acts on it,
     * so without this the escalation would always fall through to SIGKILL. */
    kill(vr.child_pid, SIGCONT);
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

    /* Idempotent: request_stop() usually got here first. Must go through the
     * mux -- a raw gui_preview_tap_remove() would tear down the shared tap and
     * take every other consumer (the RTSP stream) offline with it. */
    gui_preview_mux_remove(&vr.tap);
    if (vr.stop_at == 0.0) vr.stop_at = vr_now();
    atomic_store_explicit(&vr.run, false, memory_order_release);
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

static int s_inject_at = 0;

/* Probe the finished MKV with the ffprobe next to the ffmpeg in use (else the
 * one on PATH): stream kinds, codecs, start times and durations. Returns the
 * number of streams read, -1 if ffprobe could not run. */
typedef struct { char type[16]; char codec[24]; char pix_fmt[24]; double start; double duration; } vr_probe_stream_t;

static int vr_ffprobe_streams(const char *path, vr_probe_stream_t *out, int cap)
{
    char probe[600] = "ffprobe";
    const char *slash = strrchr(ff.path, '/');
    if (slash) {
        char cand[600];
        snprintf(cand, sizeof(cand), "%.*s/ffprobe", (int)(slash - ff.path), ff.path);
        if (vr_is_executable(cand)) snprintf(probe, sizeof(probe), "%s", cand);
    }
    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
             "\"%s\" -v error -show_entries stream=codec_type,codec_name,pix_fmt,start_time:stream_tags=DURATION "
             "-of compact=p=0:nk=0 \"%s\" 2>/dev/null", probe, path);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    int n = 0;
    char line[512];
    while (n < cap && fgets(line, sizeof(line), p)) {
        vr_probe_stream_t *st = &out[n];
        memset(st, 0, sizeof(*st));
        st->start = st->duration = -1.0;
        for (char *tok = strtok(line, "|\n"); tok; tok = strtok(NULL, "|\n")) {
            if (!strncmp(tok, "codec_name=", 11)) snprintf(st->codec, sizeof(st->codec), "%s", tok + 11);
            else if (!strncmp(tok, "pix_fmt=", 8)) snprintf(st->pix_fmt, sizeof(st->pix_fmt), "%s", tok + 8);
            else if (!strncmp(tok, "codec_type=", 11)) snprintf(st->type, sizeof(st->type), "%s", tok + 11);
            else if (!strncmp(tok, "start_time=", 11)) st->start = atof(tok + 11);
            else if (!strncmp(tok, "tag:DURATION=", 13)) {
                int hh = 0, mm = 0; double ss = 0.0;
                if (sscanf(tok + 13, "%d:%d:%lf", &hh, &mm, &ss) == 3) st->duration = hh * 3600.0 + mm * 60.0 + ss;
            }
        }
        if (st->type[0]) n++;
    }
    pclose(p);
    return n;
}

int gui_video_record_test_main(const char *device, const char *out_path,
                              int seconds, const char *codec_name,
                              const char *audio_mode)
{
    bool want_audio = false;
    if (audio_mode && strcmp(audio_mode, "audio") == 0) want_audio = true;
    else if (audio_mode && strcmp(audio_mode, "noaudio") != 0) {
        fprintf(stderr, "unknown audio mode \"%s\"; accepted: audio, noaudio\n", audio_mode);
        return 2;
    }

    if (!out_path) { fprintf(stderr, "usage: --video-record-test <dev> <out.mkv> <secs> [x264|ffv1]\n"); return 2; }
    char codec_buf[64] = {0};
    if (codec_name) snprintf(codec_buf, sizeof(codec_buf), "%s", codec_name);
    /* "x264", "ffv1", or "<codec>:kill@5" / ":hang@5" / ":bad-args" */
    char *spec = strchr(codec_buf, ':');
    if (spec) {
        *spec++ = '\0';
        char *at = strchr(spec, '@');
        if (at) { *at++ = '\0'; s_inject_at = atoi(at); }
        if      (strcmp(spec, "kill") == 0)     s_inject = VR_INJECT_KILL;
        else if (strcmp(spec, "hang") == 0)     s_inject = VR_INJECT_HANG;
        else if (strcmp(spec, "bad-args") == 0) s_inject = VR_INJECT_BAD_ARGS;
        if (s_inject_at <= 0) s_inject_at = 5;
    }
    video_codec_t codec = (codec_buf[0] && strcmp(codec_buf, "ffv1") == 0)
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
    /* The dongle's own ALSA card, found the way the recording finds it:
     * MISRC_USBREF_AUDIO overrides (the settings override, verbatim). */
    char audio_dev[128] = "";
    if (want_audio) {
        const char *ov = getenv("MISRC_USBREF_AUDIO");
        if (ov && ov[0]) snprintf(audio_dev, sizeof(audio_dev), "%s", ov);
        else if (gui_alsa_resolve_for_video_device(devs[idx].path, audio_dev, sizeof(audio_dev)) != 0) {
            fprintf(stderr, "no ALSA card shares %s's USB port; recording video-only\n", devs[idx].path);
        }
    }
    if (gui_video_record_start(out_path, codec, ps.width, ps.height, pitch,
                               ps.fps_num, ps.fps_den, audio_dev, err, sizeof(err)) != 0) {
        fprintf(stderr, "video record start failed: %s\n", err);
        gui_preview_disconnect();
        gui_preview_shutdown();
        return 2;
    }

    printf("recording %ux%u @ %u/%u -> %s (%s), ffmpeg pid %d, audio %s\n",
           ps.width, ps.height, ps.fps_num, ps.fps_den, out_path,
           codec == VIDEO_CODEC_FFV1 ? "ffv1" : "x264",
           gui_video_record_get_status().child_pid,
           audio_dev[0] ? audio_dev : "off");

    if (seconds <= 0) seconds = 10;
    int child = gui_video_record_get_status().child_pid;
    /* Deliberately not a whole number of seconds: the frame period is 40ms and
     * sampling at an exact multiple aliases against it. */
    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 7 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (s_inject_at > 0 && i + 1 == s_inject_at && child > 0) {
            if (s_inject == VR_INJECT_KILL) {
                printf("  >> SIGKILL ffmpeg (pid %d)\n", child);
                kill(child, SIGKILL);
            } else if (s_inject == VR_INJECT_HANG) {
                printf("  >> SIGSTOP ffmpeg (pid %d) - stops reading from here on\n", child);
                kill(child, SIGSTOP);
            }
        }
        gui_video_record_status_t vs = gui_video_record_get_status();
        preview_status_t live = gui_preview_get_status();
        /* The preview counter is the point of this line under injection: with
         * ffmpeg hung it must keep climbing at 25fps while the video counters
         * stall, which is what proves the capture thread is not blocked. */
        printf("  t=%2ds preview=%llu @%.1ffps | submitted=%llu written=%llu dropped=%llu duped=%llu skipped=%llu src=%+.0fppm bytes=%llu%s%s\n",
               i + 1, (unsigned long long)live.frames, live.fps_measured,
               (unsigned long long)vs.frames_submitted, (unsigned long long)vs.frames_written,
               (unsigned long long)vs.frames_dropped, (unsigned long long)vs.frames_duped,
               (unsigned long long)vs.frames_skipped, vs.source_rate_ppm,
               (unsigned long long)vs.output_bytes,
               vs.error ? "  err=" : "", vs.error ? vs.err_text : "");
        fflush(stdout);
        if (vs.error && s_inject == VR_INJECT_NONE) break;
    }

    double t0 = vr_now();
    gui_video_record_request_stop();
    gui_video_record_finish();
    double stop_s = vr_now() - t0;

    gui_video_record_status_t vs = gui_video_record_get_status();
    /* Sample the preview BEFORE tearing it down: the whole point under
     * injection is whether the capture thread survived the encoder dying. */
    preview_status_t final_preview = gui_preview_get_status();
    gui_preview_disconnect();
    gui_preview_shutdown();

    printf("stopped in %.2fs: submitted=%llu written=%llu dropped=%llu duped=%llu skipped=%llu "
           "source %+.0f ppm bytes=%llu\n",
           stop_s,
           (unsigned long long)vs.frames_submitted, (unsigned long long)vs.frames_written,
           (unsigned long long)vs.frames_dropped, (unsigned long long)vs.frames_duped,
           (unsigned long long)vs.frames_skipped, vs.source_rate_ppm,
           (unsigned long long)vs.output_bytes);
    printf("audio: %s\n", vs.audio_active ? vs.audio_device : "off");
    if (!vs.audio_active && vs.audio_note[0]) printf("audio note: %s\n", vs.audio_note);
    if (vs.error) printf("error: %s\n", vs.err_text);

    if (s_inject != VR_INJECT_NONE) {
        /* Under injection the interesting question is not "did it succeed" but
         * "did it notice, did the capture thread survive, and is what landed on
         * disk still usable". The normal assertions below would all fire by
         * design, so they are skipped rather than reported as failures. */
        const char *name = s_inject == VR_INJECT_KILL ? "kill"
                         : s_inject == VR_INJECT_HANG ? "hang" : "bad-args";
        printf("inject=%s detected=%s preview_state=%d preview_frames=%llu fps=%.1f\n",
               name, vs.error ? "yes" : "no",
               (int)final_preview.state,
               (unsigned long long)final_preview.frames,
               final_preview.fps_measured);
        bool preview_survived = (final_preview.state == PREVIEW_STATE_STREAMING ||
                                 final_preview.state == PREVIEW_STATE_STALLED);
        if (!preview_survived) {
            printf("FAIL: the capture thread did not survive the encoder fault\n");
            return 1;
        }
        if (!vs.error) {
            printf("FAIL: the fault went undetected\n");
            return 1;
        }
        printf("injection test passed\n");
        return 0;
    }

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

    /* The chroma layout each codec promises: H.264 4:2:0 so a GPU decodes
     * it, FFV1 the dongle's full 4:2:2. */
    if (s_inject == VR_INJECT_NONE && !vs.error && vs.output_bytes > 0) {
        vr_probe_stream_t st[4];
        int ns = vr_ffprobe_streams(out_path, st, 4);
        const vr_probe_stream_t *v = NULL;
        for (int k = 0; k < ns; k++) if (!strcmp(st[k].type, "video")) { v = &st[k]; break; }
        const char *want = codec == VIDEO_CODEC_FFV1 ? "yuv422p" : "yuv420p";
        if (ns < 0) {
            printf("note: ffprobe could not run; pixel format not checked\n");
        } else if (!v) {
            printf("FAIL: ffprobe found no video stream\n");
            rc = 1;
        } else {
            printf("video stream: %s %s (interlaced-aware chroma: %s)\n", v->codec, v->pix_fmt,
                   codec == VIDEO_CODEC_H264 && vr_source_interlaced() ? "yes" : "n/a");
            if (strcmp(v->pix_fmt, want) != 0) {
                printf("FAIL: video is %s, expected %s\n", v->pix_fmt, want);
                rc = 1;
            }
        }
    }

    /* With audio, what landed in the file: one FLAC track, and both tracks
     * starting and ending together -- within one frame. */
    if (want_audio && vs.audio_active) {
        vr_probe_stream_t st[4];
        int ns = vr_ffprobe_streams(out_path, st, 4);
        const vr_probe_stream_t *v = NULL, *a = NULL;
        int n_audio = 0;
        for (int k = 0; k < ns; k++) {
            if (!strcmp(st[k].type, "video") && !v) v = &st[k];
            if (!strcmp(st[k].type, "audio")) { n_audio++; if (!a) a = &st[k]; }
        }
        double frame = (double)ps.fps_den / (double)(ps.fps_num ? ps.fps_num : 25);
        if (!v || !a || n_audio != 1 || strcmp(a->codec, "flac") != 0) {
            printf("FAIL: expected one video and exactly one FLAC audio stream (ffprobe saw %d stream(s))\n", ns);
            rc = 1;
        } else {
            double v_end = v->start + v->duration, a_end = a->start + a->duration;
            printf("streams: video %s start %.3f s dur %.3f s | audio %s start %.3f s dur %.3f s "
                   "| start diff %+.1f ms, end diff %+.1f ms (one frame = %.1f ms)\n",
                   v->codec, v->start, v->duration, a->codec, a->start, a->duration,
                   (a->start - v->start) * 1e3, (a_end - v_end) * 1e3, frame * 1e3);
            if (fabs(a->start - v->start) > frame) {
                printf("FAIL: the tracks start more than one frame apart\n");
                rc = 1;
            }
            if (v->duration < 0 || a->duration < 0 || fabs(a_end - v_end) > frame) {
                printf("FAIL: the tracks end more than one frame apart\n");
                rc = 1;
            }
        }
    } else if (want_audio) {
        printf("note: audio was requested but the recording ran video-only\n");
    }

    printf("%s\n", rc ? "VIDEO RECORD TEST FAILED" : "video record test passed");
    return rc;
}

#else  /* !__linux__ */

bool gui_video_record_probe(void) { return false; }
const char *gui_video_record_ffmpeg_path(void) { return ""; }
const char *gui_video_record_cc_ffmpeg_path(void) { return ""; }
const char *gui_video_record_ffmpeg_version(void) { return ""; }
bool gui_video_record_codec_available(video_codec_t c) { (void)c; return false; }
int  gui_video_record_start(const char *o, video_codec_t c, uint32_t w, uint32_t h,
                            uint32_t p, uint32_t fn, uint32_t fd, const char *a,
                            char *err, size_t cap)
{ (void)o;(void)c;(void)w;(void)h;(void)p;(void)fn;(void)fd;(void)a;
  snprintf(err, cap, "reference video recording requires Linux"); return -1; }
void gui_video_record_request_stop(void) { }
void gui_video_record_finish(void) { }
void gui_video_record_shutdown(void) { }
gui_video_record_status_t gui_video_record_get_status(void)
{ gui_video_record_status_t s; memset(&s, 0, sizeof(s)); return s; }
uint64_t gui_video_record_output_bytes(void) { return 0; }
bool gui_video_record_is_running(void) { return false; }
int gui_video_record_test_main(const char *d, const char *o, int s, const char *c, const char *a)
{ (void)d;(void)o;(void)s;(void)c;(void)a; fprintf(stderr, "requires Linux\n"); return 2; }
int gui_video_record_probe_main(void) { fprintf(stderr, "requires Linux\n"); return 2; }
void gui_video_record_set_ffmpeg_path(const char *p) { (void)p; }
int gui_video_record_name_test_main(void) { fprintf(stderr, "requires Linux\n"); return 2; }

#endif /* __linux__ */
