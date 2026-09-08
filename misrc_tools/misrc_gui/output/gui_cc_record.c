/*
 * MISRC GUI - EIA-608 closed-caption sidecars. See gui_cc_record.h for the shape.
 *
 * This file deliberately includes nothing from the rest of the project. The
 * ffmpeg path and the preview device are injected instead, which is what lets
 * the argv guard compile it standalone -- a guard that had to spawn ffmpeg to
 * inspect a command line could not run in CI.
 */

#include "gui_cc_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Android is __linux__ too, but has no V4L2 access and no posix_spawn/environ
 * ffmpeg subprocess -- and upstream builds this same source list into its
 * Android target. Route Android to the non-Linux stubs below. */
#if defined(__linux__) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

#define CC_PROBE_TTL_S   2.0
#define CC_MAX_VBI_NODES 8

/* Wall-clock ceiling for the --cc-probe --live self-test. See cc_live_selftest()
 * for why a stream-time -t cannot bound it on its own. */
#define CC_LIVE_WALL_LIMIT_S 12

/* Long enough for ffmpeg to open the node, fail, and say why; short enough
 * that a refusal still feels immediate. Nothing is learned synchronously --
 * every interesting failure (EBUSY, unknown format, unwritable output) happens
 * after exec -- and sleeping this long on the render thread IS a frozen
 * window, so the verdict is resolved from the frame loop instead. */
#define CC_VERIFY_DELAY_S   1.5
#define CC_STAT_INTERVAL_S  1.0
#define CC_REAP_LIMIT_S     8.0
#define CC_TERM_LIMIT_S     2.0

/* The child is tiny -- 30 packets of 6 bytes a second -- so this is about the
 * house rule that no ffmpeg child competes with the RF writers, not about load. */
#define CC_CHILD_NICE 5

static struct {
    /* --- tier A: per resolved ffmpeg binary. Two popens, then never again. --- */
    char   ffmpeg[512];        /* the path tier A was computed for */
    bool   ff_probed;
    bool   has_v4l2vbi;
    bool   has_scc;

    /* --- tier B: per TTL. A few opens. --- */
    double dev_checked_at;     /* 0 = never */
    char   dev_path[64];       /* resolved node, "" if none */
    int    dev_errno;          /* 0, EBUSY, EACCES, ENOENT */
    bool   dev_uncorrelated;   /* usable, but not on the previewed dongle */

    /* --- injected --- */
    char   override_dev[64];   /* settings cc_vbi_device; "" = auto */
    char   preview_dev[64];    /* settings preview_device_path; "" = no correlation */

    /* --- derived --- */
    cc_probe_state_t state;
    char   hint[240];

    /* --- child, so the probe can decline to fight its own recording --- */
    int    child_pid;
    char   ffmpeg_log[600];
    bool   stop_requested;      /* we asked; a 255 exit is therefore expected */
    double verify_at;           /* 0 = no startup verdict pending */
    double next_stat_at;
    double spawn_wall;          /* for start_offset_s */
    cc_inject_t inject;
    bool   inited;
    int    busy_fd;             /* CC_INJECT_BUSY_DEVICE holds the node here */

    gui_cc_record_status_t status;
} cc;

/* The struct is static, so every field starts at zero -- and zero is a VALID
 * file descriptor. Without this, releasing an unheld busy_fd would close
 * stdin. */
static void cc_init(void)
{
    if (cc.inited) return;
    cc.inited = true;
    cc.busy_fd = -1;
}

static double cc_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool cc_is_executable(const char *p)
{
    return p && p[0] && access(p, X_OK) == 0;
}

/* ------------------------------------------------------------- injection */

void gui_cc_record_set_ffmpeg(const char *path)
{
    const char *p = path ? path : "";
    if (strcmp(p, cc.ffmpeg) == 0) return;      /* unchanged: leave the cache alone */
    snprintf(cc.ffmpeg, sizeof(cc.ffmpeg), "%s", p);
    cc.ff_probed = false;                       /* a new binary deserves a fresh probe */
}

void gui_cc_record_set_preview_device(const char *path)
{
    const char *p = path ? path : "";
    if (strcmp(p, cc.preview_dev) == 0) return;
    snprintf(cc.preview_dev, sizeof(cc.preview_dev), "%s", p);
    cc.dev_checked_at = 0.0;                    /* correlation target moved */
}

void gui_cc_record_set_device(const char *path)
{
    const char *p = path ? path : "";
    if (strcmp(p, cc.override_dev) == 0) return;
    snprintf(cc.override_dev, sizeof(cc.override_dev), "%s", p);
    cc.dev_checked_at = 0.0;
}

void gui_cc_record_invalidate_probe(void)
{
    cc.ff_probed = false;
    cc.dev_checked_at = 0.0;
}

/* --------------------------------------------------------- tier A: ffmpeg */

/* Interrogate the binary once. -devices is the ONLY sound test for the input
 * device: `ffmpeg -h demuxer=v4l2vbi` exits 0 even for a format that does not
 * exist (checked -- `-h demuxer=nosuchdev` also returns 0, printing "Unknown
 * format" to stdout), so using it would arm the toggle on every host where
 * the device is absent. */
static void cc_probe_ffmpeg(void)
{
    if (cc.ff_probed) return;
    cc.ff_probed = true;
    cc.has_v4l2vbi = false;
    cc.has_scc = false;

    if (!cc_is_executable(cc.ffmpeg)) return;

    char cmd[700];
    char line[256];

    snprintf(cmd, sizeof(cmd), "\"%s\" -hide_banner -devices 2>/dev/null", cc.ffmpeg);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        /* The surrounding spaces matter: they stop a future "v4l2vbi_something"
         * from matching a device we have not been written against. */
        while (fgets(line, sizeof(line), fp))
            if (strstr(line, " v4l2vbi ")) cc.has_v4l2vbi = true;
        pclose(fp);
    }

    snprintf(cmd, sizeof(cmd), "\"%s\" -hide_banner -muxers 2>/dev/null", cc.ffmpeg);
    fp = popen(cmd, "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp))
            if (strstr(line, " scc ")) cc.has_scc = true;
        pclose(fp);
    }
}

/* --------------------------------------------------------- tier B: device */

/* The sysfs parent of a /dev/videoN or /dev/vbiN, resolved. Two nodes on the
 * same USB interface share this string, which is how the VBI node belonging
 * to the dongle the preview is watching gets picked out from any other VBI
 * node on the machine (a tuner card, say). No device open is required, which
 * matters because the video node is already held by the preview. */
static bool cc_sysfs_parent(const char *devnode, char *out, size_t cap)
{
    const char *base = strrchr(devnode, '/');
    base = base ? base + 1 : devnode;
    if (!base[0]) return false;

    char link[300];
    snprintf(link, sizeof(link), "/sys/class/video4linux/%s/device", base);

    char resolved[PATH_MAX];
    if (!realpath(link, resolved)) return false;
    snprintf(out, cap, "%s", resolved);
    return true;
}

/* The availability test. Returns 0 if the node is usable RIGHT NOW, or the
 * errno explaining why not.
 *
 * Opening is NOT sufficient, and assuming it was is a trap this code fell into
 * once: open() succeeds perfectly well while another process is streaming from
 * the node. Exclusivity is enforced when buffers are allocated -- measured, a
 * second reader fails with "ioctl(VIDIOC_REQBUFS): Device or resource busy"
 * while its open() had already returned a valid fd. So the test has to go as
 * far as REQBUFS, or the probe reports a healthy device, the preflight passes,
 * and ffmpeg fails at record time instead: precisely the failure the preflight
 * exists to prevent.
 *
 * Asking for one buffer and immediately releasing it is the smallest thing
 * that actually answers the question. When keep_fd is non-NULL the claim is
 * HELD and the fd returned instead, which is how the busy-device fault
 * injection makes a real EBUSY rather than a simulated one. */
static int cc_try_claim(const char *node, int *keep_fd)
{
    if (keep_fd) *keep_fd = -1;

    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno ? errno : ENOENT;

    struct v4l2_capability capbuf;
    memset(&capbuf, 0, sizeof(capbuf));
    if (ioctl(fd, VIDIOC_QUERYCAP, &capbuf) < 0) { close(fd); return ENOTTY; }

    /* device_caps, NOT capabilities: this node's device-wide capabilities
     * field also advertises VIDEO_CAPTURE on behalf of the sibling video node,
     * so testing the wrong field accepts anything the dongle exposes. A
     * sliced-only node is also not what the demuxer wants. */
    if (!(capbuf.device_caps & V4L2_CAP_VBI_CAPTURE)) { close(fd); return ENODEV; }

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof(rb));
    rb.count = 1;
    rb.type = V4L2_BUF_TYPE_VBI_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) < 0) {
        int rc = errno ? errno : EBUSY;
        close(fd);
        return rc;
    }

    if (keep_fd) { *keep_fd = fd; return 0; }

    /* Hand the buffers back, or we become the thing that makes the node busy. */
    memset(&rb, 0, sizeof(rb));
    rb.type = V4L2_BUF_TYPE_VBI_CAPTURE;
    rb.memory = V4L2_MEMORY_MMAP;
    ioctl(fd, VIDIOC_REQBUFS, &rb);
    close(fd);
    return 0;
}

static void cc_resolve_device(void)
{
    double now = cc_now();
    if (cc.dev_checked_at > 0.0 && (now - cc.dev_checked_at) < CC_PROBE_TTL_S) return;
    cc.dev_checked_at = now;
    cc.dev_path[0] = '\0';
    cc.dev_errno = ENOENT;
    cc.dev_uncorrelated = false;

    /* An explicit setting is the only candidate: if the operator named a node,
     * silently using a different one would be worse than failing. */
    if (cc.override_dev[0]) {
        cc.dev_errno = cc_try_claim(cc.override_dev, NULL);
        if (cc.dev_errno == 0) snprintf(cc.dev_path, sizeof(cc.dev_path), "%s", cc.override_dev);
        return;
    }

    char want_parent[PATH_MAX];
    bool have_parent = cc.preview_dev[0] &&
                       cc_sysfs_parent(cc.preview_dev, want_parent, sizeof(want_parent));

    char first_ok[64];
    first_ok[0] = '\0';
    int first_err = ENOENT;

    for (int i = 0; i < CC_MAX_VBI_NODES; i++) {
        char node[64];
        snprintf(node, sizeof(node), "/dev/vbi%d", i);
        if (access(node, F_OK) != 0) continue;

        int err = cc_try_claim(node, NULL);
        if (err != 0) {
            /* Remember the most interesting refusal: a node that exists but is
             * busy or forbidden explains far more than "nothing found". */
            if (first_err == ENOENT) first_err = err;
            continue;
        }
        if (!first_ok[0]) snprintf(first_ok, sizeof(first_ok), "%s", node);

        if (have_parent) {
            char parent[PATH_MAX];
            if (cc_sysfs_parent(node, parent, sizeof(parent)) &&
                strcmp(parent, want_parent) == 0) {
                snprintf(cc.dev_path, sizeof(cc.dev_path), "%s", node);
                cc.dev_errno = 0;
                return;
            }
            continue;   /* usable, but on a different dongle */
        }

        snprintf(cc.dev_path, sizeof(cc.dev_path), "%s", node);
        cc.dev_errno = 0;
        return;
    }

    /* Correlation found no VBI node on the previewed dongle. Fall back to a
     * usable one elsewhere rather than refusing.
     *
     * Refusing was the first behaviour here and it was wrong: a rig can quite
     * reasonably take its picture from one capture device and its captions
     * from another -- composite into a dongle with no VBI transport at all,
     * S-Video into one that has it -- and both come off the same tape. Worse,
     * a device whose video node this program cannot open is still invisible to
     * the correlation while its VBI node works perfectly, so refusing greys the
     * toggle out with a node sitting right there.
     *
     * The fallback is flagged, not hidden: the hint says the captions come from
     * a different device than the picture, so an operator who did NOT intend
     * that can see it. */
    if (first_ok[0]) {
        snprintf(cc.dev_path, sizeof(cc.dev_path), "%s", first_ok);
        cc.dev_errno = 0;
        cc.dev_uncorrelated = have_parent;
        return;
    }
    cc.dev_errno = first_err;
}

/* ------------------------------------------------------------------ probe */

static void cc_set_hint(cc_probe_state_t st)
{
    const char *dev = cc.dev_path[0] ? cc.dev_path : "/dev/vbi0";
    switch (st) {
    case CC_PROBE_OK:
        if (cc.child_pid > 0)
            snprintf(cc.hint, sizeof(cc.hint), "recording captions from %s", dev);
        else if (cc.dev_uncorrelated)
            snprintf(cc.hint, sizeof(cc.hint),
                     "captions: %s -- a different device than the preview; "
                     "NTSC/525 only, about one frame late", dev);
        else
            snprintf(cc.hint, sizeof(cc.hint),
                     "captions: %s -- NTSC/525 only, about one frame late", dev);
        break;
    case CC_PROBE_NO_FFMPEG:
        snprintf(cc.hint, sizeof(cc.hint),
                 "ffmpeg not found - install it or set ffmpeg_path in the settings file");
        break;
    case CC_PROBE_NO_V4L2VBI:
        snprintf(cc.hint, sizeof(cc.hint),
                 "this ffmpeg has no v4l2vbi input device - closed captions need an ffmpeg built with it");
        break;
    case CC_PROBE_NO_MUXER:
        snprintf(cc.hint, sizeof(cc.hint), "this ffmpeg cannot write scc subtitles");
        break;
    case CC_PROBE_NO_DEVICE:
        snprintf(cc.hint, sizeof(cc.hint),
                 "no raw VBI device - captions come from the capture dongle's VBI node, not from the RF");
        break;
    case CC_PROBE_DEVICE_BUSY:
        snprintf(cc.hint, sizeof(cc.hint),
                 "%s is held by another program - the node is exclusive; close the other reader", dev);
        break;
    case CC_PROBE_DEVICE_DENIED:
        snprintf(cc.hint, sizeof(cc.hint),
                 "%s: permission denied - add your user to the 'video' group and log back in", dev);
        break;
    case CC_PROBE_UNSUPPORTED:
    default:
        snprintf(cc.hint, sizeof(cc.hint), "closed captions require Linux and a raw VBI device");
        break;
    }
}

cc_probe_state_t gui_cc_record_probe(void)
{
    cc_init();
    cc_probe_ffmpeg();

    cc_probe_state_t st;
    if (!cc_is_executable(cc.ffmpeg))  st = CC_PROBE_NO_FFMPEG;
    else if (!cc.has_v4l2vbi)          st = CC_PROBE_NO_V4L2VBI;
    else if (!cc.has_scc)              st = CC_PROBE_NO_MUXER;
    else if (cc.child_pid > 0) {
        /* Our own ffmpeg holds the node. Re-probing here would open it, get
         * EBUSY, and grey the toggle out in the middle of the recording it is
         * describing. */
        st = CC_PROBE_OK;
    } else {
        cc_resolve_device();
        switch (cc.dev_errno) {
        case 0:      st = CC_PROBE_OK;                break;
        case EBUSY:  st = CC_PROBE_DEVICE_BUSY;       break;
        case EACCES:
        case EPERM:  st = CC_PROBE_DEVICE_DENIED;     break;
        default:     st = CC_PROBE_NO_DEVICE;         break;
        }
    }

    cc.state = st;
    cc_set_hint(st);
    return st;
}

bool gui_cc_record_available(void)      { return gui_cc_record_probe() == CC_PROBE_OK; }
const char *gui_cc_record_probe_hint(void) { return cc.hint; }
const char *gui_cc_record_device(void)  { return cc.dev_path; }

/* ----------------------------------------------------------- argv builder */

/* One input, one output, one process -- the VBI node is exclusive, so a second
 * process could not open it at all.
 *
 * Every token here is deliberate:
 *   -nostdin   the child must never eat the terminal's input, which is why
 *              stopping is a signal rather than ffmpeg's own 'q'.
 *   -y         the overwrite decision was already made by the record prompt;
 *              without it ffmpeg blocks on its own interactive question.
 *   -loglevel warning -nostats
 *              keeps the log to a few lines over a multi-hour capture, which
 *              is what makes reading only its tail honest.
 *   -fill_nulls 1
 *              measured to make no difference to a .scc (the muxer drops null
 *              pairs), but it is the device default and stays correct if this
 *              stream is ever muxed against video. Passed explicitly so it is
 *              visible and assertable.
 *   -flush_packets 1
 *              SCC has no trailer, so a killed child leaves a valid file
 *              truncated at the last flush. This bounds that loss.
 *
 * -raw_timestamps is deliberately absent: it needs -copyts and would leave the
 * sidecar on kernel-clock timestamps instead of rebased-to-zero, so it would
 * not line up with a recording that starts at t=0. Do not add it to "fix" the
 * ~1-frame offset.
 */
static int cc_build_argv(const char *device, const char *out_path,
                         char *buf, size_t buf_cap,
                         const char *argv_out[], size_t argv_cap,
                         bool bad_args)
{
    if (!device || !device[0] || !out_path || !out_path[0]) return -1;
    if (!buf || buf_cap == 0 || !argv_out || argv_cap < 24) return -1;

    size_t used = 0;
    size_t n = 0;

    /* Copy a token into caller storage and record its pointer. */
    #define CC_TOK(s)                                                     \
        do {                                                              \
            const char *_s = (s);                                         \
            size_t _l = strlen(_s) + 1;                                   \
            if (used + _l > buf_cap || n + 2 > argv_cap) return -1;       \
            memcpy(buf + used, _s, _l);                                   \
            argv_out[n++] = buf + used;                                   \
            used += _l;                                                   \
        } while (0)

    CC_TOK(cc.ffmpeg[0] ? cc.ffmpeg : "ffmpeg");
    CC_TOK("-hide_banner");
    CC_TOK("-nostdin");
    CC_TOK("-nostats");
    CC_TOK("-loglevel"); CC_TOK("warning");
    CC_TOK("-y");
    CC_TOK("-f");           CC_TOK("v4l2vbi");
    CC_TOK("-fill_nulls");  CC_TOK("1");
    CC_TOK("-i");           CC_TOK(device);
    CC_TOK("-map");         CC_TOK("0:s:0");
    CC_TOK("-c:s");         CC_TOK("copy");
    CC_TOK("-f");           CC_TOK(bad_args ? "no_such_muxer" : "scc");
    CC_TOK("-flush_packets"); CC_TOK("1");
    CC_TOK(out_path);

    #undef CC_TOK

    argv_out[n] = NULL;
    return (int)n;
}

int gui_cc_record_build_argv_test(const char *device, const char *out_path,
                                  char *buf, size_t buf_cap,
                                  const char *argv_out[], size_t argv_cap)
{
    return cc_build_argv(device, out_path, buf, buf_cap, argv_out, argv_cap, false);
}

/* -------------------------------------------------------------- lifecycle */

void gui_cc_record_set_inject(cc_inject_t what) { cc.inject = what; }

/* Read only the TAIL of the log. gui_rtsp_stream.c reads the head, which on a
 * long-running child would miss the failure entirely; seeking to the end is
 * the fix. Classify over the whole tail, but SHOW the last non-empty line --
 * ffmpeg's final line is a generic summary while the line naming the cause
 * sits several above it. */
static void cc_log_tail(char *out, size_t cap, char *last_line, size_t line_cap)
{
    out[0] = '\0';
    if (last_line && line_cap) last_line[0] = '\0';

    FILE *f = fopen(cc.ffmpeg_log, "r");
    if (!f) return;
    if (fseek(f, -(long)(cap - 1), SEEK_END) != 0) fseek(f, 0, SEEK_SET);
    size_t used = fread(out, 1, cap - 1, f);
    out[used] = '\0';
    fclose(f);

    if (!last_line || !line_cap) return;
    const char *line = out;
    while (line && *line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > 0 && len < line_cap) {
            memcpy(last_line, line, len);
            last_line[len] = '\0';
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void cc_classify_exit(char *err, size_t err_cap)
{
    char tail[4096];
    char reason[256];
    cc_log_tail(tail, sizeof(tail), reason, sizeof(reason));

    if (strstr(tail, "Device or resource busy")) {
        snprintf(err, err_cap,
                 "%s is held by another program; captions were not recorded", cc.status.device);
    } else if (strstr(tail, "Permission denied")) {
        snprintf(err, err_cap,
                 "%s: permission denied (add your user to the 'video' group)", cc.status.device);
    } else if (strstr(tail, "Unknown input format")) {
        snprintf(err, err_cap,
                 "this ffmpeg has no v4l2vbi input device; captions cannot be recorded with it");
    } else if (strstr(tail, "No such file or directory")) {
        snprintf(err, err_cap, "no VBI device at %s", cc.status.device);
    } else if (strstr(tail, "No space left")) {
        snprintf(err, err_cap, "out of disk space writing the caption sidecar");
    } else {
        snprintf(err, err_cap, "ffmpeg exited at startup: %s",
                 reason[0] ? reason : "see the log beside the sidecar");
    }
}

static void cc_set_error(const char *text)
{
    cc.status.error = true;
    snprintf(cc.status.err_text, sizeof(cc.status.err_text), "%s", text);
}

int gui_cc_record_start(const char *device, const char *out_path,
                        char *err, size_t err_cap)
{
    cc_init();
    double entry_t0 = cc_now();
    if (err && err_cap) err[0] = '\0';
    if (cc.child_pid > 0) return 0;             /* already running */
    if (!out_path || !out_path[0]) {
        snprintf(err, err_cap, "no caption output path");
        return -1;
    }

    const char *dev = (device && device[0]) ? device : gui_cc_record_device();
    if (!dev || !dev[0]) {
        snprintf(err, err_cap, "no raw VBI device to record captions from");
        return -1;
    }

    memset(&cc.status, 0, sizeof(cc.status));
    snprintf(cc.status.device, sizeof(cc.status.device), "%s", dev);
    snprintf(cc.status.path, sizeof(cc.status.path), "%s", out_path);
    snprintf(cc.ffmpeg_log, sizeof(cc.ffmpeg_log), "%s.ffmpeg.log", out_path);
    cc.stop_requested = false;

    /* Claim the node's BUFFERS, not merely an fd on it: holding an open fd
     * does not make the device busy -- another process opens it happily and
     * only fails at REQBUFS. Claiming for real is what makes ffmpeg hit the
     * same EBUSY a competing reader would cause. */
    if (cc.inject == CC_INJECT_BUSY_DEVICE && cc.busy_fd < 0)
        cc_try_claim(dev, &cc.busy_fd);

    char buf[2048];
    const char *argv_c[48];
    int n = cc_build_argv(dev, out_path, buf, sizeof(buf), argv_c, 48,
                          cc.inject == CC_INJECT_BAD_ARGS);
    if (n < 0) {
        snprintf(err, err_cap, "could not build the caption command line");
        return -1;
    }
    /* posix_spawn's prototype is char *const[], and it does not modify these. */
    char *argv[48];
    for (int i = 0; i < n; i++) argv[i] = (char *)argv_c[i];
    argv[n] = NULL;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    /* stdin from /dev/null, not merely -nostdin: the child must not be able to
     * consume the terminal's input under any circumstances. */
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    /* O_NOFOLLOW and 0600: this lands in a user-chosen output directory, so it
     * costs nothing to refuse to follow a symlink into somewhere else. */
    posix_spawn_file_actions_addopen(&fa, 2, cc.ffmpeg_log,
                                     O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                                     S_IRUSR | S_IWUSR);

    pid_t pid = 0;
    /* posix_spawn because this process has display, audio, capture and libusb
     * threads plus a live GL context: only async-signal-safe calls are legal
     * between fork and exec. It returns the error directly, and does NOT set
     * errno. */
    int rc = posix_spawn(&pid, cc.ffmpeg, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        /* Distinguish the common case from the confusing one: if the log could
         * not be created, the output directory is the problem, not ffmpeg. */
        if (rc == EACCES || rc == EROFS || rc == ENOENT)
            snprintf(err, err_cap,
                     "could not start ffmpeg or create %s: %s", cc.ffmpeg_log, strerror(rc));
        else
            snprintf(err, err_cap, "could not start ffmpeg: %s", strerror(rc));
        if (cc.busy_fd >= 0) { close(cc.busy_fd); cc.busy_fd = -1; }
        return -1;
    }

    cc.child_pid = (int)pid;
    cc.spawn_wall = cc_now();
    cc.verify_at = cc.spawn_wall + CC_VERIFY_DELAY_S;
    cc.next_stat_at = cc.spawn_wall + CC_STAT_INTERVAL_S;
    cc.status.child_pid = (int)pid;
    cc.status.running = true;
    cc.status.starting = true;
    cc.status.start_offset_s = cc.spawn_wall - entry_t0;

    setpriority(PRIO_PROCESS, (id_t)pid, CC_CHILD_NICE);

    if (cc.inject == CC_INJECT_KILL)      kill(pid, SIGKILL);
    else if (cc.inject == CC_INJECT_HANG) kill(pid, SIGSTOP);
    return 0;
}

void gui_cc_record_poll(void)
{
    if (cc.child_pid <= 0) return;

    int wstatus = 0;
    pid_t r = waitpid(cc.child_pid, &wstatus, WNOHANG);
    if (r == cc.child_pid) {
        /* It died on its own. Whether that is a fault depends entirely on
         * whether we asked -- see gui_cc_record_finish() for the 255 rule. */
        bool expected = cc.stop_requested &&
                        WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 255;
        if (!expected) {
            char text[192];
            cc_classify_exit(text, sizeof(text));
            cc_set_error(text);
        }
        cc.child_pid = 0;
        cc.status.child_pid = 0;
        cc.status.running = false;
        cc.status.starting = false;
        cc.verify_at = 0.0;
        if (cc.busy_fd >= 0) { close(cc.busy_fd); cc.busy_fd = -1; }
        return;
    }

    double now = cc_now();
    if (cc.verify_at > 0.0 && now >= cc.verify_at) {
        /* Still alive past the window: the open succeeded and it is recording.
         * A tape with no captions still looks exactly like this. */
        cc.verify_at = 0.0;
        cc.status.starting = false;
    }
    if (now >= cc.next_stat_at) {
        cc.next_stat_at = now + CC_STAT_INTERVAL_S;
        struct stat sb;
        if (stat(cc.status.path, &sb) == 0) cc.status.output_bytes = (uint64_t)sb.st_size;
    }
}

void gui_cc_record_request_stop(void)
{
    if (cc.child_pid <= 0 || cc.stop_requested) return;
    cc.stop_requested = true;
    /* SIGCONT first, or a SIGSTOPped child queues the SIGINT and never acts. */
    kill(cc.child_pid, SIGCONT);
    kill(cc.child_pid, SIGINT);
}

/* Count data lines and decide whether anything was actually captioned.
 *
 * captions_seen is a plain size comparison because the SCC muxer writes NO
 * null padding -- measured, not assumed. The consequence is that a tape
 * carrying no captions leaves a header-only file, which is indistinguishable
 * from an ffmpeg whose v4l2vbi is too old to emit a full cc_data header byte.
 * Neither is an error, and this function must not pretend to tell them apart;
 * --cc-probe --live is what does that. */
static void cc_tally(void)
{
    struct stat sb;
    if (stat(cc.status.path, &sb) != 0) {
        cc_set_error("the caption sidecar was never created");
        return;
    }
    cc.status.output_bytes = (uint64_t)sb.st_size;
    cc.status.captions_seen = sb.st_size > CC_SCC_HEADER_BYTES;

    FILE *f = fopen(cc.status.path, "r");
    if (!f) return;
    char line[512];
    uint32_t lines = 0;
    while (fgets(line, sizeof(line), f))
        if (strchr(line, '\t')) lines++;
    fclose(f);
    cc.status.caption_lines = lines;
}

void gui_cc_record_finish(void)
{
    if (cc.child_pid <= 0) {
        if (cc.status.path[0] && !cc.status.error) cc_tally();
        return;
    }

    /* SIGCONT unconditionally: a SIGSTOPped child cannot act on anything else,
     * so without this the ladder always falls through to SIGKILL. */
    kill(cc.child_pid, SIGCONT);

    /* Deliberately NOT idempotent. A second SIGINT arriving during shutdown
     * makes ffmpeg abandon its trailer -- "Error submitting a packet to the
     * muxer: Immediate exit requested", "Error writing trailer" -- where a
     * single one is completely silent. Since the normal path always calls
     * request_stop() first, re-sending here would produce those errors on
     * every capture. */
    if (!cc.stop_requested) {
        cc.stop_requested = true;
        kill(cc.child_pid, SIGINT);
    }

    int wstatus = 0;
    bool reaped = false;
    double deadline = cc_now() + CC_REAP_LIMIT_S;
    while (cc_now() < deadline) {
        if (waitpid(cc.child_pid, &wstatus, WNOHANG) == cc.child_pid) { reaped = true; break; }
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (!reaped) {
        kill(cc.child_pid, SIGTERM);
        deadline = cc_now() + CC_TERM_LIMIT_S;
        while (cc_now() < deadline) {
            if (waitpid(cc.child_pid, &wstatus, WNOHANG) == cc.child_pid) { reaped = true; break; }
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (!reaped) {
        kill(cc.child_pid, SIGKILL);
        waitpid(cc.child_pid, &wstatus, 0);
    }

    /* ffmpeg exits 255 on its signal path. Without this the NORMAL stop of
     * EVERY capture would raise an error, and the operator would stop reading
     * the log -- which is the real damage. */
    bool clean = (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) ||
                 (cc.stop_requested && WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 255) ||
                 (cc.stop_requested && WIFSIGNALED(wstatus));
    if (!clean && !cc.status.error) {
        char text[192];
        cc_classify_exit(text, sizeof(text));
        cc_set_error(text);
    }

    cc.child_pid = 0;
    cc.status.child_pid = 0;
    cc.status.running = false;
    cc.status.starting = false;
    cc.verify_at = 0.0;
    if (cc.busy_fd >= 0) { close(cc.busy_fd); cc.busy_fd = -1; }

    if (!cc.status.error) cc_tally();
}

void gui_cc_record_shutdown(void)
{
    if (cc.child_pid > 0) {
        gui_cc_record_request_stop();
        gui_cc_record_finish();
    }
    if (cc.busy_fd >= 0) { close(cc.busy_fd); cc.busy_fd = -1; }
}

gui_cc_record_status_t gui_cc_record_get_status(void) { return cc.status; }
uint64_t gui_cc_record_output_bytes(void) { return cc.status.output_bytes; }
bool     gui_cc_record_is_running(void)   { return cc.child_pid > 0; }

/* ------------------------------------------------------------- record test */

int gui_cc_record_test_main(const char *device, const char *out_dir,
                            int seconds, const char *inject_spec)
{
    gui_cc_record_set_ffmpeg(getenv("MISRC_FFMPEG") ? getenv("MISRC_FFMPEG")
                                                    : "/usr/local/bin/ffmpeg");
    if (device && device[0]) gui_cc_record_set_device(device);

    cc_probe_state_t st = gui_cc_record_probe();
    /* A BUSY_DEVICE run is supposed to fail, so the probe refusing it is the
     * expected state, not a reason to bail out before we have tested it. */
    bool want_busy = inject_spec && strstr(inject_spec, "busy");
    if (st != CC_PROBE_OK && !want_busy) {
        printf("cc record test: probe refused: %s\n", gui_cc_record_probe_hint());
        return 1;
    }

    cc_inject_t inject = CC_INJECT_NONE;
    if (inject_spec && inject_spec[0]) {
        if (strstr(inject_spec, "kill"))      inject = CC_INJECT_KILL;
        else if (strstr(inject_spec, "hang")) inject = CC_INJECT_HANG;
        else if (strstr(inject_spec, "bad"))  inject = CC_INJECT_BAD_ARGS;
        else if (want_busy)                   inject = CC_INJECT_BUSY_DEVICE;
    }
    gui_cc_record_set_inject(inject);

    char out[600];
    snprintf(out, sizeof(out), "%s/cc_record_test.scc",
             (out_dir && out_dir[0]) ? out_dir : ".");

    char err[192];
    if (gui_cc_record_start(NULL, out, err, sizeof(err)) != 0) {
        printf("cc record test: start refused: %s\n", err);
        /* For the busy case that refusal IS the pass condition. */
        return want_busy ? 0 : 1;
    }
    printf("cc record test: pid=%d device=%s -> %s\n",
           gui_cc_record_get_status().child_pid, gui_cc_record_get_status().device, out);

    int secs = seconds > 0 ? seconds : 10;
    double t0 = cc_now();
    double next_report = t0 + 1.0;
    while (cc_now() - t0 < (double)secs) {
        gui_cc_record_poll();          /* a headless run has no frame loop */
        if (!gui_cc_record_is_running()) {
            printf("cc record test: child exited early\n");
            break;
        }
        if (cc_now() >= next_report) {
            next_report += 1.0;
            printf("  t=%2.0fs bytes=%llu\n", cc_now() - t0,
                   (unsigned long long)gui_cc_record_output_bytes());
        }
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    double stop_t0 = cc_now();
    gui_cc_record_request_stop();
    gui_cc_record_finish();
    double stop_s = cc_now() - stop_t0;

    gui_cc_record_status_t s = gui_cc_record_get_status();
    printf("cc record test: stop took %.2fs, bytes=%llu lines=%u captions_seen=%s\n",
           stop_s, (unsigned long long)s.output_bytes, s.caption_lines,
           s.captions_seen ? "yes" : "no");
    if (s.error) printf("cc record test: error: %s\n", s.err_text);

    int rc = 0;
    if (inject == CC_INJECT_NONE) {
        /* A tape with no captions is NOT a failure, so the assertion is about
         * the process, not the content. */
        if (s.error) { printf("FAIL: clean run reported an error\n"); rc = 1; }
        if (stop_s > CC_REAP_LIMIT_S) { printf("FAIL: stop exceeded the reap deadline\n"); rc = 1; }
    } else {
        /* Under injection the pass condition is that the fault was DETECTED
         * and the child still went away inside the deadline. */
        if (stop_s > CC_REAP_LIMIT_S + CC_TERM_LIMIT_S + 1.0) {
            printf("FAIL: reap exceeded the escalation deadline\n");
            rc = 1;
        }
        if (inject == CC_INJECT_BAD_ARGS && !s.error) {
            printf("FAIL: bad arguments were not detected\n");
            rc = 1;
        }
        /* The point of the busy case is that a node another reader has claimed
         * is REPORTED, not quietly recorded around. A run that succeeded means
         * the claim never took hold and the test proved nothing. */
        if (inject == CC_INJECT_BUSY_DEVICE && !s.error) {
            printf("FAIL: a busy device was not detected (the claim did not take)\n");
            rc = 1;
        }
    }
    printf("%s\n", rc ? "CC RECORD TEST FAILED" : "cc record test passed");
    return rc;
}

/* ---------------------------------------------------------- headless modes */

int gui_cc_record_argv_dump_main(const char *device, const char *out_dir)
{
    const char *dev = (device && device[0]) ? device : gui_cc_record_device();
    if (!dev || !dev[0]) dev = "/dev/vbi0";

    char out[600];
    snprintf(out, sizeof(out), "%s/captions.scc",
             (out_dir && out_dir[0]) ? out_dir : ".");

    char buf[2048];
    const char *argv[48];
    int n = cc_build_argv(dev, out, buf, sizeof(buf), argv, 48, false);
    if (n < 0) { fprintf(stderr, "could not build argv\n"); return 1; }

    for (int i = 0; i < n; i++) printf("%s\n", argv[i]);
    return 0;
}

/* Run the real command line for a few seconds, plus -- for diagnosis only --
 * a parallel SRT output. This is the ONLY thing that can tell "the tape has no
 * captions" apart from "this ffmpeg cannot write SCC": both leave a
 * header-only .scc, but only the broken-muxer case leaves SRT cues beside it. */
static int cc_live_selftest(void)
{
    const char *dev = gui_cc_record_device();
    if (!dev || !dev[0]) { printf("live: no device to test\n"); return 1; }

    char tmpl[] = "/tmp/misrc-cc-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }

    char scc[300], srt[300], cmd[1400];
    snprintf(scc, sizeof(scc), "%s/probe.scc", tmpl);
    snprintf(srt, sizeof(srt), "%s/probe.srt", tmpl);
    /* The wall-clock bound is not belt-and-braces: ffmpeg's -t counts STREAM
     * time, and this device advances stream time only as VBI buffers arrive.
     * With no signal on the input -- a stopped tape, an unplugged dongle --
     * none arrive, -t never fires and ffmpeg blocks indefinitely. Measured:
     * a `-t 3` run sat for over two minutes until signal returned, then
     * exited after its three seconds. So the diagnostic bounds itself. */
    snprintf(cmd, sizeof(cmd),
             "timeout -k 2 %d \"%s\" -hide_banner -nostdin -nostats -loglevel error -y "
             "-f v4l2vbi -fill_nulls 1 -i %s -t 3 "
             "-map 0:s:0 -c:s copy -f scc -flush_packets 1 %s "
             "-map 0:s:0 -c:s srt  -f srt -flush_packets 1 %s 2>&1",
             CC_LIVE_WALL_LIMIT_S, cc.ffmpeg, dev, scc, srt);

    printf("live: capturing 3s from %s (wall-clock limit %ds) ...\n",
           dev, CC_LIVE_WALL_LIMIT_S);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) printf("live: ffmpeg: %s", line);
        pclose(fp);
    }

    struct stat sb;
    long long scc_bytes = (stat(scc, &sb) == 0) ? (long long)sb.st_size : -1;
    long long srt_bytes = (stat(srt, &sb) == 0) ? (long long)sb.st_size : -1;
    printf("live: scc=%lld bytes  srt=%lld bytes\n", scc_bytes, srt_bytes);

    int rc = 0;
    if (scc_bytes > CC_SCC_HEADER_BYTES) {
        printf("live: OK - real caption data reached the .scc\n");
    } else if (srt_bytes > 0) {
        printf("live: BROKEN - the SRT carries cues but the SCC is header-only.\n");
        printf("live: that is the signature of a v4l2vbi older than the fix that\n");
        printf("live: made it emit a full 0xFC/0xFD cc_data header byte; sccenc\n");
        printf("live: compares the whole byte and drops every packet. Rebuild ffmpeg.\n");
        rc = 1;
    } else {
        printf("live: no captions in either output - most likely this tape carries\n");
        printf("live: none right now. Play a captioned tape and re-run to be sure.\n");
    }

    unlink(scc); unlink(srt); rmdir(tmpl);
    return rc;
}

int gui_cc_record_probe_main(bool live)
{
    cc_probe_state_t st = gui_cc_record_probe();

    printf("ffmpeg:      %s\n", cc.ffmpeg[0] ? cc.ffmpeg : "(none)");
    printf("v4l2vbi:     %s\n", cc.has_v4l2vbi ? "yes" : "NO");
    printf("scc muxer:   %s\n", cc.has_scc ? "yes" : "NO");
    printf("vbi device:  %s\n", cc.dev_path[0] ? cc.dev_path : "(none)");
    printf("preview dev: %s\n", cc.preview_dev[0] ? cc.preview_dev : "(unset)");
    printf("override:    %s\n", cc.override_dev[0] ? cc.override_dev : "(auto)");
    if (cc.dev_errno) printf("device err:  %s\n", strerror(cc.dev_errno));
    printf("state:       %d\n", (int)st);
    printf("hint:        %s\n", gui_cc_record_probe_hint());

    int rc = (st == CC_PROBE_OK) ? 0 : 1;
    if (live && st == CC_PROBE_OK) {
        int lrc = cc_live_selftest();
        if (lrc) rc = lrc;
    } else if (live) {
        printf("live: skipped - the static probe already failed\n");
    }
    return rc;
}

#else  /* !__linux__ */

cc_probe_state_t gui_cc_record_probe(void) { return CC_PROBE_UNSUPPORTED; }
bool gui_cc_record_available(void) { return false; }
const char *gui_cc_record_probe_hint(void)
{ return "closed captions require Linux and a raw VBI device"; }
const char *gui_cc_record_device(void) { return ""; }
void gui_cc_record_set_ffmpeg(const char *p) { (void)p; }
void gui_cc_record_set_preview_device(const char *p) { (void)p; }
void gui_cc_record_set_device(const char *p) { (void)p; }
void gui_cc_record_invalidate_probe(void) { }
int  gui_cc_record_start(const char *d, const char *o, char *err, size_t cap)
{ (void)d; (void)o; snprintf(err, cap, "closed-caption recording requires Linux"); return -1; }
void gui_cc_record_poll(void) { }
void gui_cc_record_request_stop(void) { }
void gui_cc_record_finish(void) { }
void gui_cc_record_shutdown(void) { }
void gui_cc_record_set_inject(cc_inject_t what) { (void)what; }
gui_cc_record_status_t gui_cc_record_get_status(void)
{ gui_cc_record_status_t s; memset(&s, 0, sizeof(s)); return s; }
uint64_t gui_cc_record_output_bytes(void) { return 0; }
bool gui_cc_record_is_running(void) { return false; }
int gui_cc_record_build_argv_test(const char *d, const char *o, char *b, size_t bc,
                                  const char *av[], size_t ac)
{ (void)d;(void)o;(void)b;(void)bc;(void)av;(void)ac; return -1; }
int gui_cc_record_probe_main(bool live)
{ (void)live; fprintf(stderr, "requires Linux\n"); return 2; }
int gui_cc_record_test_main(const char *d, const char *o, int s, const char *c)
{ (void)d;(void)o;(void)s;(void)c; fprintf(stderr, "requires Linux\n"); return 2; }
int gui_cc_record_argv_dump_main(const char *d, const char *o)
{ (void)d;(void)o; fprintf(stderr, "requires Linux\n"); return 2; }

#endif /* __linux__ */
