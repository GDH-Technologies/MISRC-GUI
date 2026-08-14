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

        if (usable) {
            if (have_seq && buf.sequence > last_seq + 1) {
                status_lock();
                g.status.seq_gaps += buf.sequence - last_seq - 1;
                status_unlock();
            }
            last_seq = buf.sequence;
            have_seq = true;
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
    if (g.want_auto_stop) {
        g.want_auto_stop = false;
        preview_status_t st = gui_preview_get_status();
        if (st.viewers == 0 && g.fd >= 0) {
            gui_preview_disconnect();
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
    preview_release_device();
    memset(&g.status, 0, sizeof(g.status));
    g.status.state = PREVIEW_STATE_NO_DEVICE;
}

/* ---------------------------------------------------- headless diagnostics */

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

/* Popout and the child entry point land in a later milestone. */
int  gui_preview_popout(void)  { return -1; }
void gui_preview_reclaim(void) { }
int  gui_preview_child_main(const char *device, const char *fmt_spec, int parent_pid)
{
    (void)device; (void)fmt_spec; (void)parent_pid;
    fprintf(stderr, "--preview-only is not implemented yet\n");
    return 2;
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

#endif /* __linux__ */
