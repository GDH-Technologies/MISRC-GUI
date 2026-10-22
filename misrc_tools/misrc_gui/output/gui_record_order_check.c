/*
 * MISRC GUI - record-order counter checker (see gui_record_order_check.h)
 */

#include "gui_record_order_check.h"

#include "../../common/file_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gui_record_order_checker_init(gui_record_order_checker_t *c, uint16_t key, int shift)
{
    memset(c, 0, sizeof(*c));
    c->key = key;
    c->r.key = key;
    c->shift = shift;
    c->r.first_bad_sample = UINT64_MAX;
}

static void gui_record_order_mark_bad(gui_record_order_checker_t *c, uint64_t at)
{
    if (c->r.first_bad_sample == UINT64_MAX) c->r.first_bad_sample = at;
}

static void gui_record_order_check_group(gui_record_order_checker_t *c)
{
    uint64_t at = c->r.samples - 4;
    uint32_t p[4];
    for (int i = 0; i < 4; i++) {
        p[i] = (uint32_t)(c->pend[i] + 2048) & 0xFFFu;
    }
    c->r.groups++;
    if (c->key == GUI_RECORD_ORDER_KEY_ANY) {
        uint32_t k = (p[0] ^ p[1] ^ p[2] ^ p[3]) & 0xFFFu;
        if (k == GUI_RECORD_ORDER_KEY_A || k == GUI_RECORD_ORDER_KEY_B) {
            c->key = (uint16_t)k;
            c->r.key = (uint16_t)k;
        }
    }
    if (c->key == GUI_RECORD_ORDER_KEY_ANY ||
        ((p[0] ^ p[1] ^ p[2] ^ c->key) & 0xFFFu) != p[3]) {
        c->r.corrupt++;
        gui_record_order_mark_bad(c, at);
        return;
    }
    uint64_t g = (uint64_t)p[0] | ((uint64_t)p[1] << 12) | ((uint64_t)p[2] << 24);
    if (!c->have_prev) {
        c->r.first_group = g;
    } else if (g != c->prev_group + 1) {
        if (g > c->prev_group + 1) {
            c->r.gaps++;
            c->r.missing_groups += g - c->prev_group - 1;
        } else {
            c->r.reorders++;
        }
        gui_record_order_mark_bad(c, at);
    }
    c->have_prev = true;
    c->prev_group = g;
    c->r.last_group = g;
}

void gui_record_order_checker_feed(gui_record_order_checker_t *c, const int32_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        c->pend[c->pend_n++] = samples[i] >> c->shift;
        c->r.samples++;
        if (c->pend_n == 4) {
            gui_record_order_check_group(c);
            c->pend_n = 0;
        }
    }
}

void gui_record_order_checker_finish(gui_record_order_checker_t *c, gui_record_order_report_t *out)
{
    c->r.tail_samples = (uint64_t)c->pend_n;
    if (out) *out = c->r;
}

bool gui_record_order_report_ok(const gui_record_order_report_t *r)
{
    return r && r->groups > 0 && r->gaps == 0 && r->reorders == 0 && r->corrupt == 0;
}

bool gui_record_order_check_s16(const char *path, uint16_t key,
                                gui_record_order_report_t *out, char *err, size_t err_size)
{
    FILE *f = file_open_read_utf8(path);
    if (!f) {
        if (err && err_size) snprintf(err, err_size, "cannot open %s", path);
        return false;
    }
    gui_record_order_checker_t c;
    gui_record_order_checker_init(&c, key, 0);
    enum { N = 1 << 16 };
    int16_t *in = (int16_t *)malloc(N * sizeof(int16_t));
    int32_t *wide = (int32_t *)malloc(N * sizeof(int32_t));
    if (!in || !wide) {
        free(in);
        free(wide);
        fclose(f);
        if (err && err_size) snprintf(err, err_size, "out of memory");
        return false;
    }
    size_t got;
    while ((got = fread(in, sizeof(int16_t), N, f)) > 0) {
        for (size_t i = 0; i < got; i++) wide[i] = in[i];
        gui_record_order_checker_feed(&c, wide, got);
    }
    free(in);
    free(wide);
    fclose(f);
    gui_record_order_checker_finish(&c, out);
    return true;
}

#if LIBFLAC_ENABLED == 1

#include "FLAC/stream_decoder.h"

typedef struct {
    gui_record_order_checker_t c;
    bool failed;
    bool shift_set;
} gui_record_order_flac_ctx_t;

static FLAC__StreamDecoderWriteStatus gui_record_order_flac_write(
    const FLAC__StreamDecoder *dec, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *user)
{
    (void)dec;
    gui_record_order_flac_ctx_t *x = (gui_record_order_flac_ctx_t *)user;
    if (!x->shift_set) {
        int bps = (int)frame->header.bits_per_sample;
        x->c.shift = bps > 12 ? bps - 12 : 0;
        x->shift_set = true;
    }
    gui_record_order_checker_feed(&x->c, (const int32_t *)buffer[0], frame->header.blocksize);
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void gui_record_order_flac_error(const FLAC__StreamDecoder *dec,
                                        FLAC__StreamDecoderErrorStatus status, void *user)
{
    (void)dec;
    (void)status;
    ((gui_record_order_flac_ctx_t *)user)->failed = true;
}

bool gui_record_order_check_flac(const char *path, uint16_t key,
                                 gui_record_order_report_t *out, char *err, size_t err_size)
{
    FILE *f = file_open_read_utf8(path);
    if (!f) {
        if (err && err_size) snprintf(err, err_size, "cannot open %s", path);
        return false;
    }
    FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
    if (!dec) {
        fclose(f);
        if (err && err_size) snprintf(err, err_size, "FLAC decoder alloc failed");
        return false;
    }
    gui_record_order_flac_ctx_t *x = (gui_record_order_flac_ctx_t *)calloc(1, sizeof(*x));
    if (!x) {
        FLAC__stream_decoder_delete(dec);
        fclose(f);
        if (err && err_size) snprintf(err, err_size, "out of memory");
        return false;
    }
    gui_record_order_checker_init(&x->c, key, 4);
    FLAC__stream_decoder_set_md5_checking(dec, false);
    /* init_FILE takes ownership of f and closes it in finish(). */
    if (FLAC__stream_decoder_init_FILE(dec, f, gui_record_order_flac_write, NULL,
                                       gui_record_order_flac_error, x) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(dec);
        fclose(f);
        free(x);
        if (err && err_size) snprintf(err, err_size, "FLAC decoder init failed for %s", path);
        return false;
    }
    bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec) && !x->failed;
    FLAC__stream_decoder_finish(dec);
    FLAC__stream_decoder_delete(dec);
    if (!ok && err && err_size) snprintf(err, err_size, "FLAC decode error in %s", path);
    gui_record_order_checker_finish(&x->c, out);
    free(x);
    return ok;
}

#else

bool gui_record_order_check_flac(const char *path, uint16_t key,
                                 gui_record_order_report_t *out, char *err, size_t err_size)
{
    (void)path;
    (void)key;
    if (out) memset(out, 0, sizeof(*out));
    if (err && err_size) snprintf(err, err_size, "built without libFLAC");
    return false;
}

#endif
