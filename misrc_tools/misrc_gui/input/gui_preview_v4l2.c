/*
 * MISRC GUI - USB video preview (V4L2). See gui_preview_v4l2.h for why this
 * does not reuse misrc_capture/simple_capture.
 */

#include "gui_preview_v4l2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <linux/videodev2.h>

#define PREVIEW_MAX_NODES     64
#define PREVIEW_REQ_BUFFERS   4      /* 160ms of slack at 25fps */
#define PREVIEW_STALL_SECS    3      /* soft: show "no frames", keep streaming */
#define PREVIEW_DEAD_SECS     30     /* hard: give up */
#define PREVIEW_EIO_TOLERANCE 8      /* uvc permits transient EIO */

/* Triple-buffer publish word: low 2 bits = slot index, bit 2 = new-frame. */
#define PV_IDX 0x3u
#define PV_NEW 0x4u

typedef struct {
    void  *start;
    size_t length;
} preview_buf_t;

static struct {
    /* --- enumeration (render thread only) --- */
    preview_device_t devices[PREVIEW_MAX_DEVICES];
    size_t           n_devices;
    int              sel_device;
    int              sel_mode;
    bool             enumerated;

    /* --- device (render thread owns open/close) --- */
    int           fd;
    int           wake_fd;
    preview_buf_t bufs[8];
    unsigned      n_bufs;
    uint32_t      width, height;
    uint32_t      pitch;        /* bytesperline; may exceed width*2 */
    uint32_t      frame_bytes;  /* sizeimage */

    /* --- frame slots (capture thread writes, render thread reads) --- */
    uint32_t     *slots[3];
    size_t        slot_bytes;
    atomic_uint   pub;          /* see PV_* above */
    int           write_idx;    /* capture thread only */
    int           read_idx;     /* render thread only */

    /* --- GL, render thread only --- */
    Texture2D tex;
    bool      tex_valid;

    /* --- thread --- */
    pthread_t thread;
    bool      thread_running;

    /* --- status, guarded by status_lock --- */
    atomic_flag      status_lock;
    preview_status_t status;

    /* --- fps measurement (capture thread) --- */
    double   fps_window_start;
    uint64_t fps_window_frames;

    /* --- deferred work for tick() --- */
    bool want_auto_stop;

    /* --- popout --- */
    int            child_pid;
    double         reclaim_deadline;   /* 0 = not reclaiming */
    bool           auto_reconnect;     /* child exited cleanly; go back in-app */
    char           popout_path[32];    /* device to restore when it comes back */
    preview_mode_t popout_mode;

    char argv0[512];
} g;

/* raylib's GetTime() needs an initialised window, and the headless probes below
 * have none. A monotonic clock also keeps the capture thread independent of the
 * render loop entirely. */
static double preview_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Frame tap. A single atomic pointer publishes fn and user together, so a
 * concurrent install can never be seen half-applied. The in-flight counter
 * lets remove() wait out a callback that is already running -- a minimal RCU
 * quiescence, which is what makes it safe for the tap owner to free its
 * buffers immediately after remove() returns. */
static _Atomic(const preview_tap_t *) g_tap;
static atomic_int g_tap_inflight;

void gui_preview_tap_install(const preview_tap_t *tap)
{
    atomic_store_explicit(&g_tap, tap, memory_order_release);
}

void gui_preview_tap_remove(void)
{
    atomic_store_explicit(&g_tap, NULL, memory_order_release);
    while (atomic_load_explicit(&g_tap_inflight, memory_order_acquire) != 0) {
        struct timespec ts = { 0, 200 * 1000 };   /* 200us */
        nanosleep(&ts, NULL);
    }
}

/* Defined further down with the rest of the popout machinery, but needed by
 * the per-frame tick above them. */
static void preview_poll_child(void);
static void preview_restore_popout_selection(void);
static void preview_kill_child(void);

/* ------------------------------------------------------------------ status */

static void status_lock(void)   { while (atomic_flag_test_and_set(&g.status_lock)) {} }
static void status_unlock(void) { atomic_flag_clear(&g.status_lock); }

static void preview_set_state(preview_state_t st)
{
    status_lock();
    g.status.state = st;
    status_unlock();
}

/* Called from the capture thread on any fatal condition. Stores a message the
 * panel can paint verbatim -- the whole point of not reusing simple_capture,
 * whose data_info has nowhere to put this. */
static void preview_fail(const char *what, int err)
{
    status_lock();
    g.status.state = PREVIEW_STATE_ERROR;
    g.status.err_errno = err;
    if (err) {
        snprintf(g.status.err_text, sizeof(g.status.err_text), "%s: %s", what, strerror(err));
    } else {
        snprintf(g.status.err_text, sizeof(g.status.err_text), "%s", what);
    }
    status_unlock();
}

preview_status_t gui_preview_get_status(void)
{
    status_lock();
    preview_status_t copy = g.status;
    status_unlock();
    return copy;
}

bool gui_preview_supported(void) { return true; }

/* ------------------------------------------------------------- enumeration */

static int mode_cmp(const void *a, const void *b)
{
    const preview_mode_t *x = a, *y = b;
    long ax = (long)x->w * x->h, ay = (long)y->w * y->h;
    if (ax != ay) return (ay > ax) ? 1 : -1;               /* larger first */
    double fx = (double)x->fps_num / (x->fps_den ? x->fps_den : 1);
    double fy = (double)y->fps_num / (y->fps_den ? y->fps_den : 1);
    if (fx != fy) return (fy > fx) ? 1 : -1;               /* faster first */
    return 0;
}

static void enumerate_modes(int fd, preview_device_t *dev)
{
    struct v4l2_fmtdesc fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; fmt.index++) {
        /* Allowlist, not an MJPG denylist: we only have a YUYV converter, and
         * a device offering H264 or NV12 must be excluded just as firmly.
         * Filtering on codec first is also what stops the picker showing the
         * five resolutions that both format lists happen to share. */
        if (fmt.pixelformat != V4L2_PIX_FMT_YUYV) continue;

        struct v4l2_frmsizeenum fse;
        memset(&fse, 0, sizeof(fse));
        fse.pixel_format = fmt.pixelformat;

        for (fse.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fse) == 0; fse.index++) {
            if (fse.type != V4L2_FRMSIZE_TYPE_DISCRETE) continue;
            if (dev->n_modes >= PREVIEW_MAX_MODES) return;

            struct v4l2_frmivalenum fie;
            memset(&fie, 0, sizeof(fie));
            fie.pixel_format = fmt.pixelformat;
            fie.width = fse.discrete.width;
            fie.height = fse.discrete.height;

            for (fie.index = 0; ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fie) == 0; fie.index++) {
                if (dev->n_modes >= PREVIEW_MAX_MODES) return;
                uint32_t num, den;
                if (fie.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
                    /* V4L2 gives an interval; we want a rate. */
                    num = fie.discrete.denominator;
                    den = fie.discrete.numerator ? fie.discrete.numerator : 1;
                } else {
                    num = fie.stepwise.min.denominator;
                    den = fie.stepwise.min.numerator ? fie.stepwise.min.numerator : 1;
                }
                if (num == 0) continue;

                preview_mode_t *m = &dev->modes[dev->n_modes++];
                m->w = fse.discrete.width;
                m->h = fse.discrete.height;
                m->fps_num = num;
                m->fps_den = den;
                if (den == 1) {
                    snprintf(m->label, sizeof(m->label), "%ux%u @ %u", m->w, m->h, num);
                } else {
                    snprintf(m->label, sizeof(m->label), "%ux%u @ %.1f",
                             m->w, m->h, (double)num / (double)den);
                }
                if (fie.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;  /* one synthetic entry */
            }
        }
    }
    if (dev->n_modes > 1) {
        qsort(dev->modes, (size_t)dev->n_modes, sizeof(dev->modes[0]), mode_cmp);
    }
}

void gui_preview_refresh_devices(void)
{
    g.n_devices = 0;

    for (int n = 0; n < PREVIEW_MAX_NODES && g.n_devices < PREVIEW_MAX_DEVICES; n++) {
        char node[32];
        snprintf(node, sizeof(node), "/dev/video%d", n);

        int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        struct v4l2_capability cap;
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { close(fd); continue; }

        /* device_caps describes this node; capabilities describes the whole
         * device. Using the latter would accept a metadata-only node that
         * shares a physical device with a capture node. */
        uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                  : cap.capabilities;
        /* Single-planar only: that is all the streaming path below drives. */
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
            close(fd);
            continue;
        }

        preview_device_t *dev = &g.devices[g.n_devices];
        memset(dev, 0, sizeof(*dev));
        snprintf(dev->path, sizeof(dev->path), "%s", node);
        snprintf(dev->card, sizeof(dev->card), "%s", (const char *)cap.card);
        enumerate_modes(fd, dev);
        close(fd);

        /* A capture node with no YUYV mode can only ever produce a Connect
         * button that fails, so drop it rather than list it. */
        if (dev->n_modes > 0) g.n_devices++;
    }

    g.enumerated = true;
    if (g.sel_device >= (int)g.n_devices) g.sel_device = 0;
    if (g.n_devices > 0 && g.sel_mode >= g.devices[g.sel_device].n_modes) g.sel_mode = 0;

    status_lock();
    if (g.status.state == PREVIEW_STATE_NO_DEVICE ||
        (g.status.state == PREVIEW_STATE_DISCONNECTED && g.n_devices == 0)) {
        g.status.state = (g.n_devices > 0) ? PREVIEW_STATE_DISCONNECTED
                                           : PREVIEW_STATE_NO_DEVICE;
    }
    status_unlock();
}

const preview_device_t *gui_preview_devices(size_t *count)
{
    if (!g.enumerated) gui_preview_refresh_devices();
    if (count) *count = g.n_devices;
    return g.devices;
}

int  gui_preview_selected_device(void) { return g.sel_device; }
int  gui_preview_selected_mode(void)   { return g.sel_mode; }

void gui_preview_select(int device_index, int mode_index)
{
    if (device_index >= 0 && device_index < (int)g.n_devices) g.sel_device = device_index;
    if (g.n_devices == 0) return;
    if (mode_index >= 0 && mode_index < g.devices[g.sel_device].n_modes) g.sel_mode = mode_index;
}

/* ------------------------------------------------------------- conversion */

static inline uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/* YUYV (YUY2) is [Y0][Cb][Y1][Cr] per two pixels. BT.601 limited range, 8-bit
 * fixed point. Integer only, so the build's -ffast-math cannot perturb it.
 *
 * src_pitch comes from the negotiated bytesperline rather than width*2: a
 * padded stride would otherwise shear the picture, the same class of bug as
 * trusting a requested frame size instead of the negotiated one.
 *
 * Output byte order is R,G,B,A to match PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
 * which on little-endian means (A<<24)|(B<<16)|(G<<8)|R.
 */
static void yuyv_to_rgba(const uint8_t *src, size_t src_pitch,
                         uint32_t *dst, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *sp = src + (size_t)y * src_pitch;
        uint32_t      *dp = dst + (size_t)y * w;

        for (uint32_t x = 0; x + 1 < w; x += 2) {
            int y0 = sp[0], cb = sp[1], y1 = sp[2], cr = sp[3];
            sp += 4;

            int d = cb - 128, e = cr - 128;
            int rd =  409 * e + 128;
            int gd = -100 * d - 208 * e + 128;
            int bd =  516 * d + 128;

            int c0 = 298 * (y0 - 16);
            int c1 = 298 * (y1 - 16);

            dp[0] = 0xFF000000u
                  | ((uint32_t)clamp8((c0 + bd) >> 8) << 16)
                  | ((uint32_t)clamp8((c0 + gd) >> 8) << 8)
                  |  (uint32_t)clamp8((c0 + rd) >> 8);
            dp[1] = 0xFF000000u
                  | ((uint32_t)clamp8((c1 + bd) >> 8) << 16)
                  | ((uint32_t)clamp8((c1 + gd) >> 8) << 8)
                  |  (uint32_t)clamp8((c1 + rd) >> 8);
            dp += 2;
        }
    }
}

/* --------------------------------------------------------- capture thread */

static void preview_publish(const uint8_t *src)
{
    yuyv_to_rgba(src, g.pitch, g.slots[g.write_idx], g.width, g.height);

    unsigned prev = atomic_exchange_explicit(&g.pub,
                                             (unsigned)g.write_idx | PV_NEW,
                                             memory_order_acq_rel);
    g.write_idx = (int)(prev & PV_IDX);

    double now = preview_now();
    status_lock();
    g.status.frames++;
    g.status.last_frame_time = now;
    if (g.status.state == PREVIEW_STATE_CONNECTING || g.status.state == PREVIEW_STATE_STALLED) {
        g.status.state = PREVIEW_STATE_STREAMING;
    }
    g.fps_window_frames++;
    if (now - g.fps_window_start >= 1.0) {
        g.status.fps_measured = (double)g.fps_window_frames / (now - g.fps_window_start);
        g.fps_window_start = now;
        g.fps_window_frames = 0;
    }
    status_unlock();
}

static void *preview_thread(void *arg)
{
    (void)arg;
    int stalls = 0;
    int io_errors = 0;
    uint32_t last_seq = 0;
    bool have_seq = false;
    bool reported_first_drop = false;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g.fd, &rfds);
        FD_SET(g.wake_fd, &rfds);
        int nfds = (g.fd > g.wake_fd ? g.fd : g.wake_fd) + 1;

        /* A watchdog, never the stop path -- stopping comes through wake_fd,
         * so teardown does not wait for this to expire. */
        struct timeval tv = { 1, 0 };

        errno = 0;
        int r = select(nfds, &rfds, NULL, NULL, &tv);

        if (r < 0) {
            if (errno == EINTR) continue;
            preview_fail("select failed", errno);
            break;
        }
        if (r == 0) {
            stalls++;
            if (stalls >= PREVIEW_STALL_SECS) {
                status_lock();
                if (g.status.state == PREVIEW_STATE_STREAMING ||
                    g.status.state == PREVIEW_STATE_CONNECTING) {
                    g.status.state = PREVIEW_STATE_STALLED;
                }
                status_unlock();
            }
            if (stalls >= PREVIEW_DEAD_SECS) {
                preview_fail("no frames", ETIMEDOUT);
                break;
            }
            continue;
        }
        if (FD_ISSET(g.wake_fd, &rfds)) break;      /* stop requested */
        if (!FD_ISSET(g.fd, &rfds)) continue;

        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        errno = 0;
        if (ioctl(g.fd, VIDIOC_DQBUF, &buf) < 0) {
            int e = errno;
            if (e == EAGAIN || e == EINTR) continue;
            if (e == EIO && ++io_errors < PREVIEW_EIO_TOLERANCE) continue;
            /* ENODEV/ENXIO/EPIPE/EBUSY and a persistent EIO all mean the
             * device is gone. Exiting here is the whole difference from the
             * shared reader, which would spin on a readable-but-dead fd. */
            preview_fail("device lost", e);
            break;
        }
        stalls = 0;
        io_errors = 0;

        bool usable = true;
        const char *drop_reason = NULL;
        if (buf.flags & V4L2_BUF_FLAG_ERROR)     { usable = false; drop_reason = "BUF_FLAG_ERROR"; }
        else if (buf.index >= g.n_bufs)          { usable = false; drop_reason = "bad buffer index"; }
        else if (buf.bytesused < g.frame_bytes)  { usable = false; drop_reason = "short frame"; }

        /* Report the first drop of each session. uvc devices routinely discard
         * one buffer right after STREAMON, and a silent counter that always
         * reads 1 looks like a defect; naming the reason once settles it. */
        if (!usable && !reported_first_drop) {
            reported_first_drop = true;
            fprintf(stderr, "[preview] dropped frame (%s): bytesused=%u expected=%u flags=0x%x\n",
                    drop_reason, buf.bytesused, g.frame_bytes, buf.flags);
        }

        /* Sequence continuity describes what the driver delivered, so track it
         * for every dequeued buffer including the ones we discard. Advancing
         * last_seq only on usable frames made our own drop show up as a
         * driver-lost frame on the following iteration. */
        if (have_seq && buf.sequence > last_seq + 1) {
            status_lock();
            g.status.seq_gaps += buf.sequence - last_seq - 1;
            status_unlock();
        }
        last_seq = buf.sequence;
        have_seq = true;

        if (usable) {
            /* Tap before publish: publish converts to RGBA and drops frames
             * when the renderer is slow, neither of which suits a recording. */
            const preview_tap_t *t = atomic_load_explicit(&g_tap, memory_order_acquire);
            if (t) {
                atomic_fetch_add_explicit(&g_tap_inflight, 1, memory_order_acquire);
                /* Re-load inside the counter: install/remove is not atomic with
                 * it, so the pointer may have been cleared just now. */
                const preview_tap_t *t2 = atomic_load_explicit(&g_tap, memory_order_acquire);
                if (t2 && t2->fn) {
                    t2->fn((const uint8_t *)g.bufs[buf.index].start, g.pitch,
                           g.width, g.height, t2->user);
                }
                atomic_fetch_sub_explicit(&g_tap_inflight, 1, memory_order_release);
            }
            preview_publish((const uint8_t *)g.bufs[buf.index].start);
        } else {
            status_lock();
            g.status.drops++;
            status_unlock();
        }

        if (ioctl(g.fd, VIDIOC_QBUF, &buf) < 0) {
            preview_fail("requeue failed", errno);
            break;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------- open / close */

static void preview_release_device(void)
{
    if (g.thread_running) {
        uint64_t one = 1;
        ssize_t wr = write(g.wake_fd, &one, sizeof(one));
        (void)wr;
        pthread_join(g.thread, NULL);
        g.thread_running = false;
    }
    if (g.fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(g.fd, VIDIOC_STREAMOFF, &t);
    }
    for (unsigned i = 0; i < g.n_bufs; i++) {
        if (g.bufs[i].start && g.bufs[i].start != MAP_FAILED) {
            munmap(g.bufs[i].start, g.bufs[i].length);
        }
        g.bufs[i].start = NULL;
        g.bufs[i].length = 0;
    }
    g.n_bufs = 0;
    if (g.fd >= 0)      { close(g.fd);      g.fd = -1; }
    if (g.wake_fd >= 0) { close(g.wake_fd); g.wake_fd = -1; }

    if (g.tex_valid) { UnloadTexture(g.tex); g.tex_valid = false; }

    for (int i = 0; i < 3; i++) { free(g.slots[i]); g.slots[i] = NULL; }
    g.slot_bytes = 0;
    atomic_store(&g.pub, 2u);
    g.write_idx = 0;
    g.read_idx = 1;
}

static int preview_open_fail(const char *what, int err)
{
    preview_fail(what, err);
    preview_release_device();
    return -1;
}

int gui_preview_connect(void)
{
    if (!g.enumerated) gui_preview_refresh_devices();
    if (g.n_devices == 0) {
        preview_set_state(PREVIEW_STATE_NO_DEVICE);
        return -1;
    }
    if (g.fd >= 0) return 0;   /* already streaming */

    const preview_device_t *dev = &g.devices[g.sel_device];
    const preview_mode_t *mode = &dev->modes[g.sel_mode];

    status_lock();
    memset(&g.status, 0, sizeof(g.status));
    g.status.state = PREVIEW_STATE_CONNECTING;
    snprintf(g.status.device_path, sizeof(g.status.device_path), "%s", dev->path);
    snprintf(g.status.device_card, sizeof(g.status.device_card), "%s", dev->card);
    status_unlock();

    g.fd = open(dev->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (g.fd < 0) {
        int e = errno;
        if (e == EACCES || e == EPERM) {
            return preview_open_fail("Permission denied - add your user to the 'video' group", 0);
        }
        return preview_open_fail("cannot open device", e);
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = mode->w;
    fmt.fmt.pix.height = mode->h;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl(g.fd, VIDIOC_S_FMT, &fmt) < 0) {
        return preview_open_fail("cannot set format", errno);
    }
    /* S_FMT is _IOWR and clamps silently, so the readback is not optional. A
     * substituted codec would feed the converter garbage, so fail on it. */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        return preview_open_fail("device substituted a different pixel format", 0);
    }
    g.width  = fmt.fmt.pix.width;
    g.height = fmt.fmt.pix.height;
    g.pitch  = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : g.width * 2;
    g.frame_bytes = fmt.fmt.pix.sizeimage ? fmt.fmt.pix.sizeimage : g.pitch * g.height;
    if (g.width == 0 || g.height == 0) {
        return preview_open_fail("device negotiated a zero-sized frame", 0);
    }

    /* Report the achieved rate, never the requested one. */
    uint32_t got_num = mode->fps_num, got_den = mode->fps_den;
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g.fd, VIDIOC_G_PARM, &parm) == 0 &&
        (parm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME)) {
        parm.parm.capture.timeperframe.numerator   = mode->fps_den;
        parm.parm.capture.timeperframe.denominator = mode->fps_num;
        ioctl(g.fd, VIDIOC_S_PARM, &parm);         /* best effort */
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(g.fd, VIDIOC_G_PARM, &parm) == 0 &&
            parm.parm.capture.timeperframe.numerator != 0) {
            got_num = parm.parm.capture.timeperframe.denominator;
            got_den = parm.parm.capture.timeperframe.numerator;
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = PREVIEW_REQ_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(g.fd, VIDIOC_REQBUFS, &req) < 0) {
        return preview_open_fail("cannot request buffers", errno);
    }
    if (req.count < 2) {
        return preview_open_fail("device granted too few buffers", 0);
    }
    g.n_bufs = req.count;   /* the granted count, not the requested one */

    for (unsigned i = 0; i < g.n_bufs; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (ioctl(g.fd, VIDIOC_QUERYBUF, &b) < 0) {
            return preview_open_fail("cannot query buffer", errno);
        }
        g.bufs[i].length = b.length;
        g.bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               g.fd, b.m.offset);
        if (g.bufs[i].start == MAP_FAILED) {
            g.bufs[i].start = NULL;
            return preview_open_fail("cannot map buffer", errno);
        }
        if (ioctl(g.fd, VIDIOC_QBUF, &b) < 0) {
            return preview_open_fail("cannot queue buffer", errno);
        }
    }

    g.slot_bytes = (size_t)g.width * g.height * 4u;
    for (int i = 0; i < 3; i++) {
        g.slots[i] = calloc(1, g.slot_bytes);
        if (!g.slots[i]) return preview_open_fail("out of memory for frame slots", ENOMEM);
    }
    atomic_store(&g.pub, 2u);
    g.write_idx = 0;
    g.read_idx = 1;

    g.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g.wake_fd < 0) return preview_open_fail("cannot create wake eventfd", errno);

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(g.fd, VIDIOC_STREAMON, &t) < 0) {
        return preview_open_fail("cannot start streaming", errno);
    }

    status_lock();
    g.status.width = g.width;
    g.status.height = g.height;
    g.status.fps_num = got_num;
    g.status.fps_den = got_den;
    status_unlock();
    g.fps_window_start = preview_now();
    g.fps_window_frames = 0;

    /* No thrd_set_priority call, deliberately: a 25fps preview has no realtime
     * requirement and must never share a scheduling class with RF ingest. */
    if (pthread_create(&g.thread, NULL, preview_thread, NULL) != 0) {
        return preview_open_fail("cannot start capture thread", errno);
    }
    g.thread_running = true;
    return 0;
}

void gui_preview_disconnect(void)
{
    preview_state_t prev = gui_preview_get_status().state;
    preview_release_device();
    status_lock();
    /* Keep an error visible so the panel can explain why we stopped. */
    if (prev != PREVIEW_STATE_ERROR) {
        g.status.state = (g.n_devices > 0) ? PREVIEW_STATE_DISCONNECTED
                                           : PREVIEW_STATE_NO_DEVICE;
        g.status.err_text[0] = '\0';
        g.status.err_errno = 0;
    }
    status_unlock();
}

/* --------------------------------------------------------- frame delivery */

bool gui_preview_frame_sync(void)
{
    if (!g.slots[0]) return false;

    unsigned cur = atomic_load_explicit(&g.pub, memory_order_acquire);
    if (!(cur & PV_NEW)) return false;

    unsigned prev = atomic_exchange_explicit(&g.pub, (unsigned)g.read_idx,
                                             memory_order_acq_rel);
    g.read_idx = (int)(prev & PV_IDX);

    if (!g.tex_valid) {
        Image img;
        memset(&img, 0, sizeof(img));
        img.data = g.slots[g.read_idx];
        img.width = (int)g.width;
        img.height = (int)g.height;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        /* LoadTextureFromImage copies; the Image descriptor is a view onto a
         * slot we own, so it must never be passed to UnloadImage. */
        g.tex = LoadTextureFromImage(img);
        if (g.tex.id == 0) return false;
        SetTextureFilter(g.tex, TEXTURE_FILTER_BILINEAR);
        g.tex_valid = true;
    }
    UpdateTexture(g.tex, g.slots[g.read_idx]);
    return true;
}

/* ------------------------------------------------------------------ panel */

void gui_preview_panel_attach(void)
{
    status_lock();
    g.status.viewers++;
    status_unlock();
}

/* Runs under panel_config_lock, so it must not join, ioctl or touch GL. It
 * only records the intent; gui_preview_tick() performs the teardown. */
void gui_preview_panel_detach(void)
{
    status_lock();
    if (g.status.viewers > 0) g.status.viewers--;
    bool last = (g.status.viewers == 0);
    status_unlock();
    if (last) g.want_auto_stop = true;
}

void gui_preview_tick(void)
{
    preview_poll_child();

    if (g.auto_reconnect) {
        g.auto_reconnect = false;
        preview_status_t st = gui_preview_get_status();
        if (st.viewers > 0) {
            preview_restore_popout_selection();
            gui_preview_connect();
        }
    }

    if (g.want_auto_stop) {
        g.want_auto_stop = false;
        preview_status_t st = gui_preview_get_status();
        if (st.viewers == 0) {
            /* The last preview panel went away. Take the child with it: an
             * orphan window with no route back into the app is worse than
             * losing it. */
            if (g.child_pid > 0) preview_kill_child();
            if (g.fd >= 0) gui_preview_disconnect();
        }
    }
}

/* ---------------------------------------------------------------- drawing */

static Rectangle preview_aspect_fit(Rectangle bounds, float aspect)
{
    float display_aspect = bounds.width / bounds.height;
    float draw_w, draw_h;
    if (display_aspect > aspect) { draw_h = bounds.height; draw_w = bounds.height * aspect; }
    else                         { draw_w = bounds.width;  draw_h = bounds.width / aspect; }
    return (Rectangle){ bounds.x + (bounds.width - draw_w) / 2,
                        bounds.y + (bounds.height - draw_h) / 2,
                        draw_w, draw_h };
}

/* Uses only raylib core and DrawText: the popout child has no Clay, no fonts
 * and no gui_app_t. */
static void preview_center_text(Rectangle b, const char *line1, const char *line2)
{
    int w1 = MeasureText(line1, 20);
    DrawText(line1, (int)(b.x + b.width / 2 - w1 / 2), (int)(b.y + b.height / 2 - 16),
             20, (Color){ 150, 150, 155, 255 });
    if (line2 && line2[0]) {
        int w2 = MeasureText(line2, 14);
        DrawText(line2, (int)(b.x + b.width / 2 - w2 / 2), (int)(b.y + b.height / 2 + 10),
                 14, (Color){ 100, 100, 105, 255 });
    }
}

void gui_preview_draw(Rectangle bounds, bool with_status)
{
    preview_status_t st = gui_preview_get_status();

    DrawRectangleRec(bounds, (Color){ 20, 20, 30, 255 });

    bool have_picture = g.tex_valid && st.frames > 0;
    unsigned char alpha = 255;
    if (st.state == PREVIEW_STATE_STALLED) alpha = 128;
    else if (st.state == PREVIEW_STATE_ERROR) alpha = 90;

    if (have_picture) {
        Rectangle dst = preview_aspect_fit(bounds, 4.0f / 3.0f);
        Rectangle src = { 0, 0, (float)g.tex.width, (float)g.tex.height };
        DrawTexturePro(g.tex, src, dst, (Vector2){ 0, 0 }, 0.0f,
                       (Color){ 255, 255, 255, alpha });
    }

    char detail[96];
    switch (st.state) {
        case PREVIEW_STATE_UNSUPPORTED:
            preview_center_text(bounds, "Preview requires Linux / V4L2", NULL);
            break;
        case PREVIEW_STATE_NO_DEVICE:
            preview_center_text(bounds, "No V4L2 capture device found",
                                "Connect a USB capture device and rescan");
            break;
        case PREVIEW_STATE_DISCONNECTED:
            snprintf(detail, sizeof(detail), "%s  -  %s",
                     st.device_path[0] ? st.device_path : "(no device)", st.device_card);
            preview_center_text(bounds, "Preview disconnected", detail);
            break;
        case PREVIEW_STATE_CONNECTING:
            snprintf(detail, sizeof(detail), "Opening %s", st.device_path);
            preview_center_text(bounds, "Waiting for first frame...", detail);
            break;
        case PREVIEW_STATE_POPPED_OUT:
            snprintf(detail, sizeof(detail), "pid %d", st.child_pid);
            preview_center_text(bounds, "Preview is in a separate window", detail);
            break;
        case PREVIEW_STATE_STALLED:
            if (!have_picture) preview_center_text(bounds, "No signal", st.device_path);
            break;
        case PREVIEW_STATE_ERROR:
            if (!have_picture) preview_center_text(bounds, "Preview error", NULL);
            break;
        case PREVIEW_STATE_STREAMING:
        default:
            break;
    }

    if (st.state == PREVIEW_STATE_ERROR && st.err_text[0]) {
        float bh = 22.0f;
        DrawRectangle((int)bounds.x, (int)(bounds.y + bounds.height - bh),
                      (int)bounds.width, (int)bh, (Color){ 120, 20, 20, 220 });
        DrawText(st.err_text, (int)(bounds.x + 6),
                 (int)(bounds.y + bounds.height - bh + 5), 12,
                 (Color){ 255, 220, 220, 255 });
    } else if (with_status && st.state == PREVIEW_STATE_STREAMING) {
        snprintf(detail, sizeof(detail), "%ux%u  %.1f fps  drops %llu",
                 st.width, st.height, st.fps_measured,
                 (unsigned long long)st.drops);
        DrawText(detail, (int)(bounds.x + 6), (int)(bounds.y + bounds.height - 18), 12,
                 (Color){ 200, 200, 200, 160 });
    } else if (st.state == PREVIEW_STATE_STALLED) {
        double age = preview_now() - st.last_frame_time;
        snprintf(detail, sizeof(detail), "No frames for %.1fs", age);
        DrawText(detail, (int)(bounds.x + 6), (int)(bounds.y + bounds.height - 18), 12,
                 (Color){ 255, 200, 120, 220 });
    }
}

/* ----------------------------------------------------------------- init */

void gui_preview_init(const char *argv0)
{
    memset(&g, 0, sizeof(g));
    g.fd = -1;
    g.wake_fd = -1;
    g.read_idx = 1;
    atomic_store(&g.pub, 2u);
    atomic_flag_clear(&g.status_lock);
    g.status.state = PREVIEW_STATE_NO_DEVICE;
    if (argv0) snprintf(g.argv0, sizeof(g.argv0), "%s", argv0);
}

void gui_preview_shutdown(void)
{
    preview_kill_child();
    preview_release_device();
    memset(&g.status, 0, sizeof(g.status));
    g.status.state = PREVIEW_STATE_NO_DEVICE;
}

/* ---------------------------------------------------- headless diagnostics */

/* Exercises the frame tap with no encoder attached: counts what the capture
 * thread hands over and checksums line 0 so a stride or ordering mistake shows
 * up as a constant or wildly varying digest rather than silently. */
typedef struct {
    _Atomic uint64_t frames;
    _Atomic uint64_t bytes;
    _Atomic uint32_t last_digest;
    _Atomic uint32_t distinct_digests;
    uint32_t         prev_digest;
} tap_probe_t;

static void tap_probe_cb(const uint8_t *yuyv, size_t pitch,
                         uint32_t w, uint32_t h, void *user)
{
    tap_probe_t *tp = user;
    /* FNV-1a over the first line only -- cheap enough for the capture thread. */
    uint32_t d = 2166136261u;
    size_t n = (size_t)w * 2;
    if (n > pitch) n = pitch;
    for (size_t i = 0; i < n; i++) { d ^= yuyv[i]; d *= 16777619u; }

    atomic_fetch_add_explicit(&tp->frames, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&tp->bytes, (uint64_t)pitch * h, memory_order_relaxed);
    if (d != tp->prev_digest) {
        tp->prev_digest = d;
        atomic_fetch_add_explicit(&tp->distinct_digests, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&tp->last_digest, d, memory_order_relaxed);
}

static tap_probe_t s_tap_probe;
static const preview_tap_t s_tap_probe_tap = { tap_probe_cb, &s_tap_probe };

int gui_preview_tap_test_main(const char *device, int seconds)
{
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
        preview_status_t st = gui_preview_get_status();
        fprintf(stderr, "connect failed: %s\n", st.err_text);
        return 2;
    }

    memset(&s_tap_probe, 0, sizeof(s_tap_probe));
    /* Latch the publish count at install so the comparison is over the tap's
     * own window, not the whole session. */
    uint64_t base_published = gui_preview_get_status().frames;
    gui_preview_tap_install(&s_tap_probe_tap);

    if (seconds <= 0) seconds = 10;
    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        preview_status_t st = gui_preview_get_status();
        printf("  t=%2ds tapped=%llu published=%llu drops=%llu gaps=%llu fps=%.2f\n",
               i + 1,
               (unsigned long long)atomic_load(&s_tap_probe.frames),
               (unsigned long long)st.frames,
               (unsigned long long)st.drops,
               (unsigned long long)st.seq_gaps, st.fps_measured);
        fflush(stdout);
        if (st.state == PREVIEW_STATE_ERROR) { printf("  device lost: %s\n", st.err_text); break; }
    }

    double t0 = preview_now();
    gui_preview_tap_remove();
    double remove_us = (preview_now() - t0) * 1e6;

    /* Let a frame that was mid-publish when the tap was removed finish. This
     * only has to outlast one YUYV->RGBA conversion (~0.5ms), NOT one frame
     * period (40ms) -- sleeping a whole period would admit several untapped
     * publishes and manufacture the very skew being measured. */
    struct timespec settle = { 0, 5 * 1000 * 1000 };
    nanosleep(&settle, NULL);

    preview_status_t st = gui_preview_get_status();
    uint64_t tapped = atomic_load(&s_tap_probe.frames);
    uint64_t published_in_window = st.frames - base_published;
    uint32_t distinct = atomic_load(&s_tap_probe.distinct_digests);

    printf("tapped=%llu published_in_window=%llu bytes=%llu distinct_line0=%u tap_remove=%.0fus\n",
           (unsigned long long)tapped, (unsigned long long)published_in_window,
           (unsigned long long)atomic_load(&s_tap_probe.bytes), distinct, remove_us);

    /* The tap sits immediately before publish on the same branch, so every
     * good frame inside the window must reach both. A tolerance of 2 covers
     * the frames that can slip in between latching the base count and the
     * install, and between the remove and the settle. A tap on the wrong side
     * of a drop, or one that is skipped, shows up as a large deficit. */
    int rc = 0;
    long long skew = (long long)published_in_window - (long long)tapped;
    if (skew < -2 || skew > 2) {
        printf("FAIL: tapped vs published skew %lld (expected within 2)\n", skew);
        rc = 1;
    }
    if (tapped > 0 && distinct < 2) {
        printf("FAIL: line 0 never changed across %llu frames - stale or wrong pointer\n",
               (unsigned long long)tapped);
        rc = 1;
    }
    printf("%s\n", rc ? "TAP TEST FAILED" : "tap test passed");

    gui_preview_disconnect();
    gui_preview_shutdown();
    return rc;
}

/* Numerical proof of the colour matrix and byte order, with no device and no
 * window. Eyeballing a picture cannot distinguish BT.601 from BT.709, nor a
 * Cb/Cr swap on a low-saturation source; these vectors can. */
int gui_preview_selftest_main(void)
{
    struct {
        const char *name;
        uint8_t y, cb, cr;
        uint8_t r, g, b;
    } vectors[] = {
        /* BT.601 limited range: Y 16..235, Cb/Cr centred on 128. */
        { "black",   16, 128, 128,   0,   0,   0 },
        { "white",  235, 128, 128, 255, 255, 255 },
        { "red",     81,  90, 240, 255,   0,   0 },
        { "green",  145,  54,  34,   0, 255,   0 },
        { "blue",    41, 240, 110,   0,   0, 255 },
        { "mid grey",126, 128, 128, 128, 128, 128 },
    };
    const int tolerance = 2;
    int failures = 0;

    printf("YUYV -> RGBA conversion self-test (BT.601 limited range)\n");
    printf("  %-9s %-14s %-14s %-14s\n", "vector", "input YCbCr", "expected RGB", "got RGB");

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        /* Two pixels of the same colour: [Y0][Cb][Y1][Cr]. */
        uint8_t src[4] = { vectors[i].y, vectors[i].cb, vectors[i].y, vectors[i].cr };
        uint32_t dst[2] = { 0, 0 };
        yuyv_to_rgba(src, sizeof(src), dst, 2, 1);

        /* Read back through a byte view: PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
         * means the bytes in memory must be R,G,B,A in that order. */
        const uint8_t *px = (const uint8_t *)&dst[0];
        int dr = abs((int)px[0] - vectors[i].r);
        int dg = abs((int)px[1] - vectors[i].g);
        int db = abs((int)px[2] - vectors[i].b);
        bool ok = (dr <= tolerance && dg <= tolerance && db <= tolerance && px[3] == 255);
        if (!ok) failures++;

        printf("  %-9s %3u,%3u,%3u    %3u,%3u,%3u    %3u,%3u,%3u  a=%u  %s\n",
               vectors[i].name, vectors[i].y, vectors[i].cb, vectors[i].cr,
               vectors[i].r, vectors[i].g, vectors[i].b,
               px[0], px[1], px[2], px[3], ok ? "ok" : "FAIL");

        /* Both pixels of the macropixel must decode identically. */
        if (dst[0] != dst[1]) {
            printf("      FAIL: the two pixels of the macropixel differ\n");
            failures++;
        }
    }

    /* A padded stride must not shear the image: give a 2-pixel-wide frame a
     * pitch of 8 bytes (4 bytes of padding) and check row 1 is unaffected. */
    {
        uint8_t src[16];
        memset(src, 0, sizeof(src));
        src[0] = 235; src[1] = 128; src[2] = 235; src[3] = 128;   /* row 0: white */
        /* bytes 4..7 are padding and must be skipped */
        src[8] = 16;  src[9] = 128; src[10] = 16; src[11] = 128;  /* row 1: black */
        uint32_t dst[4] = { 0, 0, 0, 0 };
        yuyv_to_rgba(src, 8, dst, 2, 2);
        const uint8_t *r0 = (const uint8_t *)&dst[0];
        const uint8_t *r1 = (const uint8_t *)&dst[2];
        bool ok = (r0[0] > 250 && r1[0] < 5);
        printf("  %-9s pitch=8 w=2       row0 white, row1 black    row0=%u row1=%u  %s\n",
               "stride", r0[0], r1[0], ok ? "ok" : "FAIL");
        if (!ok) failures++;
    }

    printf("%s\n", failures ? "SELF-TEST FAILED" : "self-test passed");
    return failures ? 1 : 0;
}

int gui_preview_probe_main(void)
{
    gui_preview_init(NULL);
    gui_preview_refresh_devices();

    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    printf("preview devices: %zu\n", n);
    for (size_t i = 0; i < n; i++) {
        printf("  %s  \"%s\"  (%d YUYV modes)\n", devs[i].path, devs[i].card, devs[i].n_modes);
        for (int m = 0; m < devs[i].n_modes; m++) {
            printf("      %-16s %s\n", devs[i].modes[m].label, m == 0 ? "<- default" : "");
        }
    }
    gui_preview_shutdown();
    return n > 0 ? 0 : 1;
}

/* Write one converted frame to a PNG, with no window and no GL: it reads the
 * published slot directly rather than going through frame_sync. Lets the real
 * device's output be checked numerically instead of by eye. */
int gui_preview_dump_frame_main(const char *device, const char *out_png)
{
    if (!out_png) { fprintf(stderr, "usage: --preview-dump-frame <device> <out.png>\n"); return 2; }

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
        preview_status_t st = gui_preview_get_status();
        fprintf(stderr, "connect failed: %s\n", st.err_text);
        return 2;
    }

    /* Wait up to 5s for a published frame. */
    int rc = 2;
    for (int i = 0; i < 500; i++) {
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        unsigned cur = atomic_load_explicit(&g.pub, memory_order_acquire);
        if (cur & PV_NEW) {
            unsigned prev = atomic_exchange_explicit(&g.pub, (unsigned)g.read_idx,
                                                     memory_order_acq_rel);
            g.read_idx = (int)(prev & PV_IDX);

            Image img;
            memset(&img, 0, sizeof(img));
            img.data = g.slots[g.read_idx];
            img.width = (int)g.width;
            img.height = (int)g.height;
            img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            /* ExportImage is pure CPU; the Image is a view onto a slot we own,
             * so it must never be handed to UnloadImage. */
            rc = ExportImage(img, out_png) ? 0 : 2;
            printf("%s %ux%u -> %s (%s)\n", devs[idx].path, g.width, g.height, out_png,
                   rc == 0 ? "ok" : "export failed");
            break;
        }
    }
    if (rc == 2) fprintf(stderr, "no frame published within 5s\n");

    gui_preview_disconnect();
    gui_preview_shutdown();
    return rc;
}

int gui_preview_probe_stream_main(const char *device, int seconds)
{
    gui_preview_init(NULL);
    gui_preview_refresh_devices();

    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    if (n == 0) { fprintf(stderr, "no preview devices\n"); return 1; }

    int idx = 0;
    if (device) {
        idx = -1;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(devs[i].path, device) == 0) { idx = (int)i; break; }
        }
        if (idx < 0) { fprintf(stderr, "no such preview device: %s\n", device); return 1; }
    }
    gui_preview_select(idx, 0);

    if (gui_preview_connect() != 0) {
        preview_status_t st = gui_preview_get_status();
        fprintf(stderr, "connect failed: %s\n", st.err_text[0] ? st.err_text : "(no detail)");
        gui_preview_shutdown();
        return 2;
    }

    preview_status_t st = gui_preview_get_status();
    printf("streaming %s %ux%u @ %.2f fps (negotiated)\n",
           st.device_path, st.width, st.height,
           st.fps_den ? (double)st.fps_num / st.fps_den : 0.0);
    printf("capture thread tid is a child of pid %d; check with: chrt -p <tid>\n", (int)getpid());

    if (seconds <= 0) seconds = 10;
    for (int s = 0; s < seconds; s++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        st = gui_preview_get_status();
        printf("  t=%2ds state=%d frames=%llu drops=%llu seq_gaps=%llu fps=%.2f%s%s\n",
               s + 1, (int)st.state,
               (unsigned long long)st.frames, (unsigned long long)st.drops,
               (unsigned long long)st.seq_gaps, st.fps_measured,
               st.err_text[0] ? "  err=" : "", st.err_text);
        fflush(stdout);
        if (st.state == PREVIEW_STATE_ERROR) {
            printf("  -> device reported lost; stopping early\n");
            break;
        }
    }

    double t0 = preview_now();
    gui_preview_disconnect();
    double teardown_us = (preview_now() - t0) * 1e6;
    printf("teardown: %.0f us\n", teardown_us);

    gui_preview_shutdown();
    return 0;
}

/* ------------------------------------------------------------ popout child */

/* Close every descriptor above stderr. The child inherits whatever the parent
 * had open -- X11 sockets, ALSA, libusb, and any recording file -- and holding
 * a recording file open would keep its inode alive after the parent closed it.
 * Safe here because this runs before the child opens anything of its own. */
static void preview_close_inherited_fds(void)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    closefrom(3);
#else
    DIR *d = opendir("/proc/self/fd");
    if (d) {
        int dfd = dirfd(d);
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int fd = atoi(e->d_name);
            if (fd > 2 && fd != dfd) close(fd);
        }
        closedir(d);
    }
#endif
}

/* "YUYV:720x576@25", "720x576@25" or "720x576@25/1". Returns the matching mode
 * index on the selected device, or -1 to mean "leave the default". */
static int preview_parse_mode_spec(const preview_device_t *dev, const char *spec)
{
    if (!dev || !spec || !spec[0]) return -1;
    const char *p = strchr(spec, ':');
    p = p ? p + 1 : spec;

    unsigned w = 0, h = 0, num = 0, den = 1;
    if (sscanf(p, "%ux%u@%u/%u", &w, &h, &num, &den) < 3) {
        den = 1;
        if (sscanf(p, "%ux%u@%u", &w, &h, &num) < 3) return -1;
    }
    if (den == 0) den = 1;

    for (int i = 0; i < dev->n_modes; i++) {
        if (dev->modes[i].w == w && dev->modes[i].h == h &&
            dev->modes[i].fps_num == num && dev->modes[i].fps_den == den) {
            return i;
        }
    }
    /* Fall back to the same geometry at whatever rate it offers. */
    for (int i = 0; i < dev->n_modes; i++) {
        if (dev->modes[i].w == w && dev->modes[i].h == h) return i;
    }
    return -1;
}

int gui_preview_child_main(const char *device, const char *fmt_spec, int parent_pid)
{
    preview_close_inherited_fds();

    /* Die with the parent. The getppid check closes the window where the parent
     * exits between posix_spawn returning and this prctl landing -- in that case
     * the death signal is already lost, and an orphan would hold the device
     * forever with no route back into the app. */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (parent_pid > 0 && getppid() != parent_pid) {
        _exit(0);
    }

    gui_preview_init(NULL);
    gui_preview_refresh_devices();

    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    if (n == 0) {
        fprintf(stderr, "preview: no V4L2 capture device found\n");
        return 2;
    }

    int dev_idx = 0;
    if (device && device[0]) {
        dev_idx = -1;
        for (size_t i = 0; i < n; i++) {
            if (strcmp(devs[i].path, device) == 0) { dev_idx = (int)i; break; }
        }
        if (dev_idx < 0) {
            fprintf(stderr, "preview: no such device: %s\n", device);
            return 2;
        }
    }
    int mode_idx = preview_parse_mode_spec(&devs[dev_idx], fmt_spec);
    gui_preview_select(dev_idx, mode_idx >= 0 ? mode_idx : 0);

    char title[128];
    snprintf(title, sizeof(title), "MISRC Preview - %s", devs[dev_idx].path);

    /* No MSAA: nothing here is antialiased. No icon and no fonts either -- the
     * child deliberately loads none of the app's assets, and gui_preview_draw
     * uses only raylib's built-in DrawText. */
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(720, 576, title);
    SetWindowMinSize(320, 240);
    SetTargetFPS(60);
    SetExitKey(KEY_ESCAPE);

    int rc = 0;
    if (gui_preview_connect() != 0) {
        preview_status_t st = gui_preview_get_status();
        fprintf(stderr, "preview: %s\n", st.err_text[0] ? st.err_text : "connect failed");
        rc = 2;
    }

    while (!WindowShouldClose()) {
        gui_preview_tick();
        gui_preview_frame_sync();

        preview_status_t st = gui_preview_get_status();
        if (st.state == PREVIEW_STATE_ERROR && rc == 0) {
            rc = 3;   /* tell the parent the device went away */
        }

        BeginDrawing();
        ClearBackground(BLACK);
        gui_preview_draw((Rectangle){ 0, 0, (float)GetRenderWidth(), (float)GetRenderHeight() },
                         true);
        EndDrawing();
    }

    gui_preview_shutdown();
    CloseWindow();
    return rc;
}

/* --------------------------------------------------------- popout (parent) */

extern char **environ;

/* Locate an executable to spawn the popout window from.
 *
 * $APPIMAGE comes first deliberately. Under AppImage the running binary lives
 * in a squashfuse mount owned by this process's AppImage runtime, which is
 * unmounted when we exit -- an orphaned child's file-backed text pages would
 * then fault with SIGBUS rather than any interpretable error. Re-launching the
 * AppImage gives the child its own mount and removes that failure entirely.
 * AppRun passes unrecognised arguments straight through to misrc_gui. */
static bool preview_resolve_exe(char *out, size_t cap)
{
    const char *appimage = getenv("APPIMAGE");
    if (appimage && appimage[0] && access(appimage, X_OK) == 0) {
        snprintf(out, cap, "%s", appimage);
        return true;
    }

    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        /* A deleted image cannot be re-executed. */
        if (!strstr(buf, " (deleted)") && access(buf, X_OK) == 0) {
            snprintf(out, cap, "%s", buf);
            return true;
        }
    }

    if (g.argv0[0]) {
        char resolved[512];
        if (realpath(g.argv0, resolved) && access(resolved, X_OK) == 0) {
            snprintf(out, cap, "%s", resolved);
            return true;
        }
    }
    return false;
}

int gui_preview_popout(void)
{
    if (g.child_pid > 0) return 0;            /* already out */
    if (g.n_devices == 0) return -1;

    /* Resolve before releasing anything: if we cannot find our own executable
     * the user should be no worse off than before they clicked. */
    char exe[512];
    if (!preview_resolve_exe(exe, sizeof(exe))) {
        preview_fail("cannot locate the misrc_gui executable to pop out", 0);
        return -1;
    }

    const preview_device_t *dev = &g.devices[g.sel_device];
    const preview_mode_t *mode = &dev->modes[g.sel_mode];
    snprintf(g.popout_path, sizeof(g.popout_path), "%s", dev->path);
    g.popout_mode = *mode;

    char fmt_spec[48];
    snprintf(fmt_spec, sizeof(fmt_spec), "YUYV:%ux%u@%u/%u",
             mode->w, mode->h, mode->fps_num, mode->fps_den ? mode->fps_den : 1);
    char ppid_str[16];
    snprintf(ppid_str, sizeof(ppid_str), "%d", (int)getpid());

    /* The child's REQBUFS/STREAMON would fail EBUSY while we still hold
     * buffers, so release fully first. */
    gui_preview_disconnect();

    char *argv[] = {
        exe,
        (char *)"--preview-only",       g.popout_path,
        (char *)"--preview-format",     fmt_spec,
        (char *)"--preview-parent-pid", ppid_str,
        NULL
    };

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    /* Give the child its own stdin; keep stdout/stderr so its diagnostics land
     * wherever ours do -- indispensable when the window fails to appear. */
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);

    pid_t pid = 0;
    /* posix_spawn, not fork+exec: this process has display, audio, capture and
     * libusb threads plus a live GL context, and only async-signal-safe calls
     * are legal between fork and exec. glibc implements posix_spawn with
     * CLONE_VM|CLONE_VFORK, so no user code runs in that window at all.
     * Note it returns the error directly and does NOT set errno. */
    int rc = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        preview_fail("could not start the preview window", rc);
        /* We already released the device; put it back the way it was. */
        gui_preview_connect();
        return -1;
    }

    g.child_pid = (int)pid;
    status_lock();
    g.status.state = PREVIEW_STATE_POPPED_OUT;
    g.status.child_pid = (int)pid;
    snprintf(g.status.device_path, sizeof(g.status.device_path), "%s", g.popout_path);
    status_unlock();
    return 0;
}

void gui_preview_reclaim(void)
{
    if (g.child_pid <= 0) return;
    kill(g.child_pid, SIGTERM);
    g.reclaim_deadline = preview_now() + 2.0;
}

/* Restore the selection the popout was using, so coming back lands on the same
 * device and mode even if the picker moved meanwhile. */
static void preview_restore_popout_selection(void)
{
    if (!g.popout_path[0]) return;
    for (size_t i = 0; i < g.n_devices; i++) {
        if (strcmp(g.devices[i].path, g.popout_path) != 0) continue;
        g.sel_device = (int)i;
        for (int m = 0; m < g.devices[i].n_modes; m++) {
            if (g.devices[i].modes[m].w == g.popout_mode.w &&
                g.devices[i].modes[m].h == g.popout_mode.h &&
                g.devices[i].modes[m].fps_num == g.popout_mode.fps_num) {
                g.sel_mode = m;
                break;
            }
        }
        return;
    }
}

/* Polled from tick() rather than handled with SIGCHLD: a signal handler lands
 * on an arbitrary thread and may only use async-signal-safe calls, so it would
 * have to set a flag and be polled from here anyway -- while adding a
 * process-wide disposition change to a process that links libusb and ALSA and
 * currently installs no handlers at all. */
static void preview_poll_child(void)
{
    if (g.child_pid <= 0) return;

    if (g.reclaim_deadline > 0.0 && preview_now() > g.reclaim_deadline) {
        kill(g.child_pid, SIGKILL);
        g.reclaim_deadline = 0.0;
    }

    int wstatus = 0;
    pid_t r = waitpid(g.child_pid, &wstatus, WNOHANG);
    if (r != g.child_pid) return;

    int pid = g.child_pid;
    g.child_pid = 0;
    g.reclaim_deadline = 0.0;
    status_lock();
    g.status.child_pid = 0;
    status_unlock();

    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
        /* User closed the window: make pop-out/close feel like a round trip. */
        g.auto_reconnect = true;
        preview_set_state(PREVIEW_STATE_DISCONNECTED);
    } else if (WIFEXITED(wstatus)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "preview window exited (code %d)", WEXITSTATUS(wstatus));
        preview_fail(msg, 0);
    } else if (WIFSIGNALED(wstatus)) {
        char msg[96];
        int sig = WTERMSIG(wstatus);
        snprintf(msg, sizeof(msg), "preview window terminated by signal %d (%s)",
                 sig, strsignal(sig));
        preview_fail(msg, 0);
    } else {
        preview_set_state(PREVIEW_STATE_DISCONNECTED);
    }
    (void)pid;
}

/* Kill any popout child before we go. Mandatory, for two reasons: an orphan
 * holds /dev/video0 open forever with no route back into the app, and under
 * AppImage its text pages are backed by a mount that disappears with us. */
static void preview_kill_child(void)
{
    if (g.child_pid <= 0) return;
    kill(g.child_pid, SIGTERM);
    for (int i = 0; i < 200; i++) {           /* up to ~2s */
        if (waitpid(g.child_pid, NULL, WNOHANG) == g.child_pid) {
            g.child_pid = 0;
            return;
        }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    kill(g.child_pid, SIGKILL);
    waitpid(g.child_pid, NULL, 0);
    g.child_pid = 0;
}

#else  /* !__linux__ */

/* cxadc and V4L2 are Linux-only; the panel and the argv branch exist on every
 * platform and rely on these stubs so nothing else needs an #ifdef. */

static preview_status_t s_unsupported = { .state = PREVIEW_STATE_UNSUPPORTED };

void gui_preview_init(const char *argv0) { (void)argv0; }
void gui_preview_shutdown(void) { }
void gui_preview_tick(void) { }
bool gui_preview_supported(void) { return false; }

void gui_preview_refresh_devices(void) { }
const preview_device_t *gui_preview_devices(size_t *count) { if (count) *count = 0; return NULL; }
int  gui_preview_selected_device(void) { return 0; }
int  gui_preview_selected_mode(void) { return 0; }
void gui_preview_select(int d, int m) { (void)d; (void)m; }

int  gui_preview_connect(void) { return -1; }
void gui_preview_disconnect(void) { }
preview_status_t gui_preview_get_status(void) { return s_unsupported; }

void gui_preview_panel_attach(void) { }
void gui_preview_panel_detach(void) { }
bool gui_preview_frame_sync(void) { return false; }

void gui_preview_draw(Rectangle bounds, bool with_status)
{
    (void)with_status;
    DrawRectangleRec(bounds, (Color){ 20, 20, 30, 255 });
    const char *msg = "Preview requires Linux / V4L2";
    int w = MeasureText(msg, 20);
    DrawText(msg, (int)(bounds.x + bounds.width / 2 - w / 2),
             (int)(bounds.y + bounds.height / 2 - 10), 20, (Color){ 150, 150, 155, 255 });
}

int  gui_preview_popout(void) { return -1; }
void gui_preview_reclaim(void) { }
int  gui_preview_child_main(const char *device, const char *fmt_spec, int parent_pid)
{
    (void)device; (void)fmt_spec; (void)parent_pid;
    fprintf(stderr, "--preview-only is not supported on this platform\n");
    return 2;
}
int gui_preview_probe_main(void) { fprintf(stderr, "preview requires Linux\n"); return 2; }
int gui_preview_probe_stream_main(const char *d, int s) { (void)d; (void)s; return 2; }
int gui_preview_selftest_main(void) { fprintf(stderr, "preview requires Linux\n"); return 2; }
int gui_preview_dump_frame_main(const char *d, const char *o) { (void)d; (void)o; return 2; }
int gui_preview_tap_test_main(const char *d, int s) { (void)d; (void)s; return 2; }
void gui_preview_tap_install(const preview_tap_t *t) { (void)t; }
void gui_preview_tap_remove(void) { }

#endif /* __linux__ */
