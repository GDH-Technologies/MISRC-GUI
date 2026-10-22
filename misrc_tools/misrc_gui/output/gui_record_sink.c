/*
 * MISRC GUI - RAW record output with hold-and-retry (see gui_record_sink.h)
 */

#include "gui_record_sink.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void gui_record_sink_init(gui_record_sink_t *s, FILE *fp) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->fp = fp;
}

void gui_record_sink_free(gui_record_sink_t *s) {
    if (!s) return;
    free(s->pend);
    s->pend = NULL;
    s->pend_len = s->pend_cap = 0;
}

static bool sink_hold(gui_record_sink_t *s, const uint8_t *bytes, size_t len) {
    if (len == 0) return true;
    if (s->pend_len + len > s->pend_cap) {
        size_t cap = s->pend_cap ? s->pend_cap : 65536;
        while (cap < s->pend_len + len) cap *= 2;
        uint8_t *p = (uint8_t *)realloc(s->pend, cap);
        if (!p) return false;
        s->pend = p;
        s->pend_cap = cap;
    }
    memcpy(s->pend + s->pend_len, bytes, len);
    s->pend_len += len;
    return true;
}

/* One fwrite attempt; returns how many bytes it took. */
static size_t sink_put(gui_record_sink_t *s, const uint8_t *bytes, size_t len) {
    if (len == 0) return 0;
    clearerr(s->fp);
    errno = 0;
    size_t n = fwrite(bytes, 1, len, s->fp);
    if (n < len) s->last_errno = errno ? errno : EIO;
    s->written += n;
    return n;
}

static void sink_fail(gui_record_sink_t *s, uint64_t now_ms) {
    s->failing = true;
    s->failures++;
    s->next_retry_ms = now_ms + GUI_RECORD_SINK_RETRY_MS;
}

bool gui_record_sink_retry(gui_record_sink_t *s, uint64_t now_ms, bool force) {
    if (!s) return false;
    if (s->pend_len == 0) {
        s->failing = false;
        return true;
    }
    if (!force && now_ms < s->next_retry_ms) return false;
    size_t n = sink_put(s, s->pend, s->pend_len);
    if (n > 0) {
        memmove(s->pend, s->pend + n, s->pend_len - n);
        s->pend_len -= n;
    }
    if (s->pend_len == 0) {
        s->failing = false;
        return true;
    }
    sink_fail(s, now_ms);
    return false;
}

bool gui_record_sink_write(gui_record_sink_t *s, const void *bytes, size_t len, uint64_t now_ms) {
    if (!s || !s->fp) return false;
    const uint8_t *b = (const uint8_t *)bytes;
    if (s->pend_len) {
        /* Older held bytes first; the caller should have retried already. */
        if (!gui_record_sink_retry(s, now_ms, true)) {
            if (!sink_hold(s, b, len)) s->last_errno = ENOMEM;
            return false;
        }
    }
    size_t n = sink_put(s, b, len);
    if (n == len) return true;
    if (!sink_hold(s, b + n, len - n)) s->last_errno = ENOMEM;
    sink_fail(s, now_ms);
    return false;
}
