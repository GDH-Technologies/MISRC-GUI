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
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#define CC_PROBE_TTL_S   2.0
#define CC_MAX_VBI_NODES 8

/* Wall-clock ceiling for the --cc-probe --live self-test. See cc_live_selftest()
 * for why a stream-time -t cannot bound it on its own. */
#define CC_LIVE_WALL_LIMIT_S 12

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

    /* --- injected --- */
    char   override_dev[64];   /* settings cc_vbi_device; "" = auto */
    char   preview_dev[64];    /* settings preview_device_path; "" = no correlation */

    /* --- derived --- */
    cc_probe_state_t state;
    char   hint[240];

    /* --- child, so the probe can decline to fight its own recording --- */
    int    child_pid;
} cc;

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

/* Open as the availability test, then close immediately. Returns 0 on
 * success, or the errno that explains the refusal. */
static int cc_try_open(const char *node)
{
    int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno ? errno : ENOENT;

    struct v4l2_capability capbuf;
    memset(&capbuf, 0, sizeof(capbuf));
    int rc = 0;
    if (ioctl(fd, VIDIOC_QUERYCAP, &capbuf) < 0) {
        rc = ENOTTY;
    } else if (!(capbuf.device_caps & V4L2_CAP_VBI_CAPTURE)) {
        /* device_caps, NOT capabilities: the VBI node's device-wide
         * capabilities field also advertises VIDEO_CAPTURE for the sibling
         * video node, so testing the wrong field accepts anything. A
         * sliced-only node is also not what the demuxer wants. */
        rc = ENODEV;
    }
    close(fd);
    return rc;
}

static void cc_resolve_device(void)
{
    double now = cc_now();
    if (cc.dev_checked_at > 0.0 && (now - cc.dev_checked_at) < CC_PROBE_TTL_S) return;
    cc.dev_checked_at = now;
    cc.dev_path[0] = '\0';
    cc.dev_errno = ENOENT;

    /* An explicit setting is the only candidate: if the operator named a node,
     * silently using a different one would be worse than failing. */
    if (cc.override_dev[0]) {
        cc.dev_errno = cc_try_open(cc.override_dev);
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

        int err = cc_try_open(node);
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

    /* Correlation found nothing. Do NOT silently fall back to a VBI node on
     * some other device: with the preview on a dongle that has no VBI sibling,
     * "no VBI node" is the truthful answer and grabbing an unrelated one would
     * caption the wrong source. */
    if (have_parent) {
        cc.dev_errno = first_ok[0] ? ENODEV : first_err;
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

/* --------------------------------------------------- lifecycle (step 3) */

int gui_cc_record_start(const char *device, const char *out_path,
                        char *err, size_t err_cap)
{
    (void)device; (void)out_path;
    snprintf(err, err_cap, "caption recording is not wired up yet");
    return -1;
}

void gui_cc_record_poll(void) { }
void gui_cc_record_request_stop(void) { }
void gui_cc_record_finish(void) { }
void gui_cc_record_shutdown(void) { }
void gui_cc_record_set_inject(cc_inject_t what) { (void)what; }

gui_cc_record_status_t gui_cc_record_get_status(void)
{
    gui_cc_record_status_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.device, sizeof(s.device), "%s", cc.dev_path);
    return s;
}

uint64_t gui_cc_record_output_bytes(void) { return 0; }
bool     gui_cc_record_is_running(void)   { return cc.child_pid > 0; }

int gui_cc_record_test_main(const char *device, const char *out_dir,
                            int seconds, const char *inject_spec)
{
    (void)device; (void)out_dir; (void)seconds; (void)inject_spec;
    fprintf(stderr, "caption recording is not wired up yet\n");
    return 2;
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
