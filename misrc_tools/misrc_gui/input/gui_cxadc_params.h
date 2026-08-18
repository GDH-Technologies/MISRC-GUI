/*
 * CXADC runtime parameter access (sysfs).
 *
 * The cxadc driver exposes its tunables as a per-device attribute group at
 *
 *     /sys/class/cxadc/cxadc<N>/device/parameters/<attr>
 *
 * Note the extra "parameters/" component, and note that these are NOT module
 * parameters -- /sys/module/cxadc/parameters/ does not exist, and an
 * "options cxadc ..." line in modprobe.d is silently useless.
 *
 * Three properties of the driver dictate this API's shape:
 *
 *  1. Only level, sixdb and center_offset take effect during a capture. The
 *     driver rewrites those two registers on every read() of the character
 *     device. vmux, tenbit, tenxfsc and crystal are applied in open() only, so
 *     writing them mid-capture changes nothing in the stream while making the
 *     reported configuration a lie.
 *
 *  2. Writes never fail on bad input. Every _store() handler calls kstrtoint()
 *     and returns count unconditionally, discarding the parse result. Worse,
 *     sixdb and center_offset are never range-checked anywhere: they are
 *     shifted straight into register writes, so sixdb=2 sets an adjacent
 *     control bit and center_offset=256 bleeds into the neighbouring field.
 *     Clamping here, before the write, is therefore a correctness requirement
 *     rather than a nicety. Read-back verification catches only parse failures
 *     and permission problems -- never range.
 *
 *  3. The driver rewrites values in place behind the application's back:
 *     level is clamped to 0..31 at open() and on every read(), vmux is masked
 *     with 3, and a tenxfsc between 11 and 99 is converted from MHz to Hz. A
 *     displayed value can therefore change without the user touching anything,
 *     which is why this module is a cache with an expiry and never a shadow
 *     copy of what we last wrote.
 *
 * sysfs is the source of truth. Nothing here is persisted or re-applied at
 * startup: udev writes vmux and crystal on every card add, and cxlvlcavdd
 * rewrites level in a loop during CAV captures. Silently overriding either
 * would be discovered only after a tape was already captured wrong.
 *
 * This module deliberately has no raylib, no Clay and no gui_app_t dependency.
 */

#ifndef GUI_CXADC_PARAMS_H
#define GUI_CXADC_PARAMS_H

#include <stdbool.h>

#define CXP_MAX_CARDS 2

typedef enum {
    CXP_OK = 0,
    CXP_ERR_NO_CARD,      /* /sys/class/cxadc/cxadcN is absent */
    CXP_ERR_NO_ATTR,      /* parameters/<attr> is absent (older driver) */
    CXP_ERR_PERM,         /* EACCES/EPERM -- user is probably not in group video */
    CXP_ERR_IO,           /* any other open/read/write failure */
    CXP_ERR_REJECTED,     /* write reported success but the read-back disagreed */
    CXP_ERR_UNSUPPORTED   /* not a Linux build */
} cxp_status_t;

typedef enum {
    /* Live-tunable: the driver re-applies these on every read(). */
    CXP_ATTR_LEVEL = 0,
    CXP_ATTR_SIXDB,
    CXP_ATTR_CENTER_OFFSET,
    /* Applied at open() only -- writing these mid-capture does nothing. */
    CXP_ATTR_VMUX,
    CXP_ATTR_TENBIT,
    CXP_ATTR_TENXFSC,
    CXP_ATTR_CRYSTAL,
    /* Read-only counter, reset by the driver on open(). */
    CXP_ATTR_OVERRUN_COUNT,
    CXP_ATTR_COUNT
} cxp_attr_t;

typedef struct {
    bool         present;                 /* the card's parameters dir exists */
    bool         write_probed;            /* writable has been determined */
    bool         writable;                /* an O_WRONLY open succeeded */

    int          value[CXP_ATTR_COUNT];
    bool         valid[CXP_ATTR_COUNT];   /* false if that attr could not be read */

    int          overrun_delta;           /* since the previous counter read */

    cxp_status_t last_error;
    cxp_attr_t   last_error_attr;
    char         last_error_text[128];    /* ready to display; empty when OK */

    double       last_full_read_s;
    double       last_counter_read_s;
    int          consecutive_failures;
} cxp_card_t;

/* ---- Attribute metadata ------------------------------------------------- */

const char *gui_cxadc_params_attr_name(cxp_attr_t attr);
bool        gui_cxadc_params_attr_writable(cxp_attr_t attr);
/* Inclusive clamp range. Meaningless for read-only attributes. */
void        gui_cxadc_params_attr_range(cxp_attr_t attr, int *lo, int *hi);
/* True for the three the driver re-applies on every read(). */
bool        gui_cxadc_params_attr_is_live(cxp_attr_t attr);

/* ---- Direct access ------------------------------------------------------ */

cxp_status_t gui_cxadc_params_read_all(int card, cxp_card_t *out);
cxp_status_t gui_cxadc_params_read_one(int card, cxp_attr_t attr, int *out_value);

/* Clamps to the attribute's range, writes, then re-reads and reports what the
 * card actually holds. out_readback is always filled on CXP_OK and on
 * CXP_ERR_REJECTED. Returns CXP_ERR_REJECTED when the read-back disagrees with
 * the (clamped) value written. */
cxp_status_t gui_cxadc_params_write_one(int card, cxp_attr_t attr, int value,
                                        int *out_readback);

/* Opens level O_WRONLY and closes it immediately without writing. Do not infer
 * writability from st_mode: the udev rule chmods the whole directory g+w,
 * which leaves the read-only overrun_count as -r--rw-r--. */
cxp_status_t gui_cxadc_params_probe_writable(int card, bool *out_writable);

/* ---- Cache facade (what the UI uses) ------------------------------------ */

/* Call once per frame. now_s is a monotonic seconds clock (GetTime()).
 * want_live selects the faster cadence used while a popover is open;
 * focus_card is the card that cadence applies to (-1 for none).
 * capturing shortens the counter poll. Does no I/O at all when enabled is
 * false, so a non-CXADC device costs nothing. */
void gui_cxadc_params_tick(bool enabled, double now_s, bool want_live,
                           int focus_card, bool capturing);

/* Last known state. Never NULL for a valid card index. */
const cxp_card_t *gui_cxadc_params_get(int card);

/* Forget everything; forces a fresh read on the next tick. Call when the
 * device list changes. */
void gui_cxadc_params_invalidate(void);

/* Effective sample rate in Hz derived from tenxfsc/crystal/tenbit, following
 * the same rules as the cxadc-status tool. Returns 0 if unknown. */
double gui_cxadc_params_effective_rate_hz(const cxp_card_t *card);

#endif /* GUI_CXADC_PARAMS_H */
