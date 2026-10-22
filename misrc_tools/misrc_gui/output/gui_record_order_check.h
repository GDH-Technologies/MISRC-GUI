/*
 * MISRC GUI - record-order counter pattern and checker
 *
 * A self-describing RF test stream for proving the record path keeps every
 * sample, once, in capture order. The Simulated device emits it in place of
 * its CVBS/VHS pattern when asked (gui_simulated_set_pattern); the checker
 * reads a recorded FLAC or converted RAW .s16 file back and reports every
 * gap, reorder and corrupt group.
 *
 * Encoding: sample n carries 12 bits of group number g = n / 4. Samples
 * 4g, 4g+1 and 4g+2 hold bits 0-11, 12-23 and 24-35 of g; sample 4g+3 holds
 * their XOR with a per-channel key, so a group from the wrong channel or a
 * damaged group is rejected. Each value is stored as (piece - 2048), the
 * signed 12-bit range the capture path carries in int16. Groups never
 * repeat within 2^36 samples (28.6 minutes at 40 MSps), so a moved block is
 * always visible -- a pattern periodic in the block size would hide it.
 *
 * Raylib-free and GUI-free: the checker is shared by --record-order-selftest
 * and can be linked into a unit harness.
 */

#ifndef GUI_RECORD_ORDER_CHECK_H
#define GUI_RECORD_ORDER_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_RECORD_ORDER_KEY_A 0xA5Au
#define GUI_RECORD_ORDER_KEY_B 0x3C3u

/* The 12-bit signed sample at stream position n for the channel key. */
static inline int16_t gui_record_order_counter_sample(uint64_t n, uint16_t key)
{
    uint64_t g = n >> 2;
    uint32_t p0 = (uint32_t)(g & 0xFFFu);
    uint32_t p1 = (uint32_t)((g >> 12) & 0xFFFu);
    uint32_t p2 = (uint32_t)((g >> 24) & 0xFFFu);
    uint32_t piece;
    switch (n & 3u) {
        case 0:  piece = p0; break;
        case 1:  piece = p1; break;
        case 2:  piece = p2; break;
        default: piece = (p0 ^ p1 ^ p2 ^ key) & 0xFFFu; break;
    }
    return (int16_t)((int32_t)piece - 2048);
}

/* Pass as the checker key to accept either channel's key, locked to the one
 * the first group carries (a device in MISRC mode swaps A and B). */
#define GUI_RECORD_ORDER_KEY_ANY 0u

typedef struct {
    uint16_t key;            /* the channel key the stream carries */
    uint64_t samples;        /* samples read from the file */
    uint64_t groups;         /* whole 4-sample groups checked */
    uint64_t first_group;    /* g of the first valid group */
    uint64_t last_group;     /* g of the last valid group */
    uint64_t gaps;           /* forward jumps (groups missing) */
    uint64_t missing_groups; /* sum of forward jump sizes */
    uint64_t reorders;       /* backward jumps or repeats (a moved block) */
    uint64_t corrupt;        /* groups failing the key check */
    uint64_t first_bad_sample; /* stream position of the first problem, or UINT64_MAX */
    uint64_t tail_samples;   /* samples after the last whole group */
} gui_record_order_report_t;

/* Incremental checker, so a decoder callback can feed it a block at a time. */
typedef struct {
    uint16_t key;
    int shift;               /* right shift that maps a stored sample to 12 bits */
    int32_t pend[4];
    int pend_n;
    bool have_prev;
    uint64_t prev_group;
    gui_record_order_report_t r;
} gui_record_order_checker_t;

void gui_record_order_checker_init(gui_record_order_checker_t *c, uint16_t key, int shift);
void gui_record_order_checker_feed(gui_record_order_checker_t *c, const int32_t *samples, size_t n);
void gui_record_order_checker_finish(gui_record_order_checker_t *c, gui_record_order_report_t *out);

/* Whole-file checks. FLAC samples are decoded and shifted down by
 * (bits_per_sample - 12); RAW .s16 is the int16 12-bit value as stored.
 * Return false (with err filled) only when the file could not be read. */
bool gui_record_order_check_flac(const char *path, uint16_t key,
                                 gui_record_order_report_t *out, char *err, size_t err_size);
bool gui_record_order_check_s16(const char *path, uint16_t key,
                                gui_record_order_report_t *out, char *err, size_t err_size);

/* True when the report shows a gap-free, in-order, uncorrupted stream. */
bool gui_record_order_report_ok(const gui_record_order_report_t *r);

#endif /* GUI_RECORD_ORDER_CHECK_H */
