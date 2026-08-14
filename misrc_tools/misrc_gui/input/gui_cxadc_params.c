/*
 * CXADC runtime parameter access (sysfs). See gui_cxadc_params.h for the
 * driver behaviour this is written around.
 */

#include "gui_cxadc_params.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#define CXP_SYSFS_FMT "/sys/class/cxadc/cxadc%d/device/parameters/%s"

/* Polling cadences, seconds. Idle is slow enough to be free; the popover-open
 * rate only needs to beat human reaction time. The capturing counter poll
 * exists because overrun_count is the one value that moves on its own. */
#define CXP_POLL_IDLE_S       2.0
#define CXP_POLL_FOCUSED_S    0.5
#define CXP_POLL_COUNTER_S    0.5
#define CXP_MAX_FAILURES      3

typedef struct {
    const char *name;
    bool        writable;
    bool        live;      /* re-applied by the driver on every read() */
    int         lo, hi;
} cxp_attr_meta_t;

/* Ranges are what the hardware fields actually accept:
 *  - level      bits 16..20 of MO_AGC_GAIN_ADJ4      -> 0..31
 *  - sixdb      bit 23 of the same register          -> 0..1
 *  - center_off low 8 bits of MO_AGC_SYNC_TIP3       -> 0..255
 *  - vmux       masked with 3 by the driver          -> 0..3
 *  - tenbit     bit 5 of MO_CAPTURE_CTRL             -> 0..1
 * The driver itself only enforces the level clamp, and only at open()/read().
 * sixdb and center_offset are shifted into their registers unchecked, so these
 * bounds are the only thing preventing a write from corrupting a neighbouring
 * field. tenxfsc and crystal are left wide: tenxfsc is overloaded (0/1/2, then
 * 11..99 as MHz, then >=100 as exact Hz) and crystal is just what the user
 * tells the driver is on the board. Neither is writable from this UI. */
static const cxp_attr_meta_t s_meta[CXP_ATTR_COUNT] = {
    [CXP_ATTR_LEVEL]         = { "level",         true,  true,  0, 31 },
    [CXP_ATTR_SIXDB]         = { "sixdb",         true,  true,  0, 1 },
    [CXP_ATTR_CENTER_OFFSET] = { "center_offset", true,  true,  0, 255 },
    [CXP_ATTR_VMUX]          = { "vmux",          false, false, 0, 3 },
    [CXP_ATTR_TENBIT]        = { "tenbit",        false, false, 0, 1 },
    [CXP_ATTR_TENXFSC]       = { "tenxfsc",       false, false, 0, 200000000 },
    [CXP_ATTR_CRYSTAL]       = { "crystal",       false, false, 0, 200000000 },
    [CXP_ATTR_OVERRUN_COUNT] = { "overrun_count", false, false, 0, 0 },
};

static cxp_card_t s_cards[CXP_MAX_CARDS];
static bool       s_initialised = false;

const char *gui_cxadc_params_attr_name(cxp_attr_t attr)
{
    if (attr < 0 || attr >= CXP_ATTR_COUNT) return "?";
    return s_meta[attr].name;
}

bool gui_cxadc_params_attr_writable(cxp_attr_t attr)
{
    if (attr < 0 || attr >= CXP_ATTR_COUNT) return false;
    return s_meta[attr].writable;
}

bool gui_cxadc_params_attr_is_live(cxp_attr_t attr)
{
    if (attr < 0 || attr >= CXP_ATTR_COUNT) return false;
    return s_meta[attr].live;
}

void gui_cxadc_params_attr_range(cxp_attr_t attr, int *lo, int *hi)
{
    if (attr < 0 || attr >= CXP_ATTR_COUNT) {
        if (lo) *lo = 0;
        if (hi) *hi = 0;
        return;
    }
    if (lo) *lo = s_meta[attr].lo;
    if (hi) *hi = s_meta[attr].hi;
}

double gui_cxadc_params_effective_rate_hz(const cxp_card_t *card)
{
    if (!card || !card->valid[CXP_ATTR_TENXFSC] || !card->valid[CXP_ATTR_CRYSTAL]) {
        return 0.0;
    }
    /* Same rules as the cxadc-status tool. tenxfsc is overloaded: 0 is the
     * crystal itself, 1 is 1.25x, anything under 100 is a MHz figure, and
     * anything else is an exact Hz figure. 16-bit mode halves the rate. */
    int tenxfsc = card->value[CXP_ATTR_TENXFSC];
    double crystal = (double)card->value[CXP_ATTR_CRYSTAL];
    double base;

    if (tenxfsc == 0)        base = crystal;
    else if (tenxfsc == 1)   base = crystal * 1.25;
    else if (tenxfsc < 100)  base = (double)tenxfsc * 1000000.0;
    else                     base = (double)tenxfsc;

    if (card->valid[CXP_ATTR_TENBIT] && card->value[CXP_ATTR_TENBIT] == 1) {
        base /= 2.0;
    }
    return base;
}

#if !defined(__linux__)

/* cxadc is a Linux kernel driver; there is nothing to talk to elsewhere. */

cxp_status_t gui_cxadc_params_read_all(int card, cxp_card_t *out)
{
    (void)card;
    if (out) memset(out, 0, sizeof(*out));
    return CXP_ERR_UNSUPPORTED;
}

cxp_status_t gui_cxadc_params_read_one(int card, cxp_attr_t attr, int *out_value)
{
    (void)card; (void)attr; (void)out_value;
    return CXP_ERR_UNSUPPORTED;
}

cxp_status_t gui_cxadc_params_write_one(int card, cxp_attr_t attr, int value,
                                        int *out_readback)
{
    (void)card; (void)attr; (void)value; (void)out_readback;
    return CXP_ERR_UNSUPPORTED;
}

cxp_status_t gui_cxadc_params_probe_writable(int card, bool *out_writable)
{
    (void)card;
    if (out_writable) *out_writable = false;
    return CXP_ERR_UNSUPPORTED;
}

void gui_cxadc_params_tick(bool enabled, double now_s, bool want_live,
                           int focus_card, bool capturing)
{
    (void)enabled; (void)now_s; (void)want_live; (void)focus_card; (void)capturing;
}

#else /* __linux__ */

static cxp_status_t cxp_errno_to_status(int err)
{
    switch (err) {
        case ENOENT: return CXP_ERR_NO_ATTR;
        case EACCES:
        case EPERM:  return CXP_ERR_PERM;
        default:     return CXP_ERR_IO;
    }
}

static bool cxp_card_dir_exists(int card)
{
    char path[128];
    snprintf(path, sizeof(path), CXP_SYSFS_FMT, card, s_meta[CXP_ATTR_LEVEL].name);
    return access(path, F_OK) == 0;
}

static cxp_status_t cxp_read_attr(int card, cxp_attr_t attr, int *out_value)
{
    char path[128];
    char buf[64];

    snprintf(path, sizeof(path), CXP_SYSFS_FMT, card, s_meta[attr].name);

    /* O_NONBLOCK so a card being torn down cannot wedge the render thread in
     * the driver core; O_CLOEXEC so a popout/child process never inherits it. */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return cxp_errno_to_status(errno);
    }

    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    int read_errno = errno;
    close(fd);

    if (n < 0)  return cxp_errno_to_status(read_errno);
    if (n == 0) return CXP_ERR_IO;

    buf[n] = '\0';

    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf || errno != 0) {
        return CXP_ERR_IO;
    }
    if (out_value) *out_value = (int)v;
    return CXP_OK;
}

static cxp_status_t cxp_write_attr(int card, cxp_attr_t attr, int value)
{
    char path[128];
    char buf[32];

    snprintf(path, sizeof(path), CXP_SYSFS_FMT, card, s_meta[attr].name);

    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return cxp_errno_to_status(errno);
    }

    int len = snprintf(buf, sizeof(buf), "%d\n", value);
    ssize_t n = write(fd, buf, (size_t)len);
    int write_errno = errno;
    close(fd);

    if (n != (ssize_t)len) {
        return cxp_errno_to_status(write_errno);
    }
    return CXP_OK;
}

cxp_status_t gui_cxadc_params_read_one(int card, cxp_attr_t attr, int *out_value)
{
    if (card < 0 || card >= CXP_MAX_CARDS) return CXP_ERR_NO_CARD;
    if (attr < 0 || attr >= CXP_ATTR_COUNT) return CXP_ERR_NO_ATTR;
    return cxp_read_attr(card, attr, out_value);
}

cxp_status_t gui_cxadc_params_read_all(int card, cxp_card_t *out)
{
    if (!out) return CXP_ERR_IO;
    if (card < 0 || card >= CXP_MAX_CARDS) {
        memset(out, 0, sizeof(*out));
        return CXP_ERR_NO_CARD;
    }

    if (!cxp_card_dir_exists(card)) {
        bool had = out->present;
        memset(out, 0, sizeof(*out));
        out->last_error = CXP_ERR_NO_CARD;
        snprintf(out->last_error_text, sizeof(out->last_error_text),
                 "cxadc%d is not present", card);
        (void)had;
        return CXP_ERR_NO_CARD;
    }

    out->present = true;
    cxp_status_t worst = CXP_OK;
    cxp_attr_t worst_attr = CXP_ATTR_LEVEL;

    for (int a = 0; a < CXP_ATTR_COUNT; a++) {
        int v = 0;
        cxp_status_t st = cxp_read_attr(card, (cxp_attr_t)a, &v);
        if (st == CXP_OK) {
            if (a == CXP_ATTR_OVERRUN_COUNT && out->valid[a]) {
                out->overrun_delta = v - out->value[a];
            }
            out->value[a] = v;
            out->valid[a] = true;
        } else {
            out->valid[a] = false;
            if (worst == CXP_OK) {
                worst = st;
                worst_attr = (cxp_attr_t)a;
            }
        }
    }

    out->last_error = worst;
    out->last_error_attr = worst_attr;
    if (worst == CXP_OK) {
        out->last_error_text[0] = '\0';
    } else if (worst == CXP_ERR_NO_ATTR) {
        snprintf(out->last_error_text, sizeof(out->last_error_text),
                 "cxadc driver does not expose '%s'", s_meta[worst_attr].name);
    } else if (worst == CXP_ERR_PERM) {
        snprintf(out->last_error_text, sizeof(out->last_error_text),
                 "No access to cxadc%d parameters - add your user to the 'video' group",
                 card);
    } else {
        snprintf(out->last_error_text, sizeof(out->last_error_text),
                 "cxadc%d: could not read '%s'", card, s_meta[worst_attr].name);
    }

    return worst;
}

cxp_status_t gui_cxadc_params_probe_writable(int card, bool *out_writable)
{
    if (out_writable) *out_writable = false;
    if (card < 0 || card >= CXP_MAX_CARDS) return CXP_ERR_NO_CARD;

    char path[128];
    snprintf(path, sizeof(path), CXP_SYSFS_FMT, card, s_meta[CXP_ATTR_LEVEL].name);

    /* Open and close without writing. The mode bits cannot be used for this:
     * the udev rule chmods the whole parameters directory g+w, which leaves
     * the genuinely read-only overrun_count reading as -r--rw-r--. */
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return cxp_errno_to_status(errno);
    }
    close(fd);
    if (out_writable) *out_writable = true;
    return CXP_OK;
}

cxp_status_t gui_cxadc_params_write_one(int card, cxp_attr_t attr, int value,
                                        int *out_readback)
{
    if (card < 0 || card >= CXP_MAX_CARDS) return CXP_ERR_NO_CARD;
    if (attr < 0 || attr >= CXP_ATTR_COUNT) return CXP_ERR_NO_ATTR;
    if (!s_meta[attr].writable) return CXP_ERR_IO;

    /* Clamp before writing. The driver does not do this: every _store()
     * handler discards kstrtoint()'s result, and sixdb/center_offset are never
     * range-checked at all before being shifted into their registers. */
    if (value < s_meta[attr].lo) value = s_meta[attr].lo;
    if (value > s_meta[attr].hi) value = s_meta[attr].hi;

    cxp_status_t st = cxp_write_attr(card, attr, value);
    if (st != CXP_OK) {
        return st;
    }

    /* Always read back. A successful write() proves nothing -- the store
     * handler returns count even when the value failed to parse. */
    int readback = 0;
    cxp_status_t rst = cxp_read_attr(card, attr, &readback);
    if (rst != CXP_OK) {
        return rst;
    }
    if (out_readback) *out_readback = readback;

    /* Invalidate the cache timestamp so the next tick re-reads everything:
     * one write can move other values (a tenxfsc in MHz is rewritten to Hz),
     * and level is re-clamped by the driver once a capture starts. */
    if (card < CXP_MAX_CARDS) {
        s_cards[card].last_full_read_s = 0.0;
    }

    return (readback == value) ? CXP_OK : CXP_ERR_REJECTED;
}

void gui_cxadc_params_tick(bool enabled, double now_s, bool want_live,
                           int focus_card, bool capturing)
{
    if (!s_initialised) {
        memset(s_cards, 0, sizeof(s_cards));
        s_initialised = true;
    }

    /* Cost nothing when the selected device is not a CXADC card. */
    if (!enabled) {
        return;
    }

    for (int card = 0; card < CXP_MAX_CARDS; card++) {
        cxp_card_t *c = &s_cards[card];

        /* Stop hammering a card that keeps failing; a re-open of the popover
         * calls invalidate() and gives it another chance. */
        if (c->consecutive_failures >= CXP_MAX_FAILURES) {
            continue;
        }

        bool focused = want_live && (card == focus_card);
        double full_interval = focused ? CXP_POLL_FOCUSED_S : CXP_POLL_IDLE_S;

        if (now_s - c->last_full_read_s >= full_interval) {
            cxp_status_t st = gui_cxadc_params_read_all(card, c);
            c->last_full_read_s = now_s;
            c->last_counter_read_s = now_s;

            if (st == CXP_ERR_NO_CARD || st == CXP_ERR_IO) {
                c->consecutive_failures++;
            } else {
                c->consecutive_failures = 0;
            }

            if (c->present && !c->write_probed) {
                bool w = false;
                cxp_status_t pst = gui_cxadc_params_probe_writable(card, &w);
                c->write_probed = true;
                c->writable = (pst == CXP_OK) && w;
                if (!c->writable && c->last_error == CXP_OK) {
                    c->last_error = CXP_ERR_PERM;
                    c->last_error_attr = CXP_ATTR_LEVEL;
                    snprintf(c->last_error_text, sizeof(c->last_error_text),
                             "No write access to cxadc%d parameters - add your user "
                             "to the 'video' group and log out and back in", card);
                }
            }
            continue;
        }

        /* overrun_count is the only value that moves without anyone writing
         * it, so poll it on its own while a capture is running. */
        if (capturing && c->present &&
            now_s - c->last_counter_read_s >= CXP_POLL_COUNTER_S) {
            int v = 0;
            if (cxp_read_attr(card, CXP_ATTR_OVERRUN_COUNT, &v) == CXP_OK) {
                if (c->valid[CXP_ATTR_OVERRUN_COUNT]) {
                    c->overrun_delta = v - c->value[CXP_ATTR_OVERRUN_COUNT];
                }
                c->value[CXP_ATTR_OVERRUN_COUNT] = v;
                c->valid[CXP_ATTR_OVERRUN_COUNT] = true;
            }
            c->last_counter_read_s = now_s;
        }
    }
}

#endif /* __linux__ */

const cxp_card_t *gui_cxadc_params_get(int card)
{
    static const cxp_card_t empty = { 0 };
    if (card < 0 || card >= CXP_MAX_CARDS) return &empty;
    return &s_cards[card];
}

void gui_cxadc_params_invalidate(void)
{
    memset(s_cards, 0, sizeof(s_cards));
    s_initialised = true;
}
