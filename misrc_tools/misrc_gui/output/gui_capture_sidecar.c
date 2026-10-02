/*
 * MISRC GUI - Capture metadata sidecar. See gui_capture_sidecar.h.
 *
 * Licensed under GNU GPL v3 or later
 */

#include "gui_capture_sidecar.h"
#include "../core/gui_settings.h"   /* gui_settings_json_escape */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <process.h>
#define sidecar_getpid _getpid
#else
#include <fcntl.h>
#include <unistd.h>
#define sidecar_getpid getpid
#endif

const char *const gui_capture_sidecar_audio_keys[GUI_CAPTURE_SIDECAR_AUDIO_COUNT] = {
    "audio_4ch", "audio_2ch_12", "audio_2ch_34",
    "audio_1ch_1", "audio_1ch_2", "audio_1ch_3", "audio_1ch_4",
};

/* ============================================================================
 * Formatting
 * ============================================================================ */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;   /* what the whole text needs */
} sk_t;

static void sk_put(sk_t *k, const char *text, size_t n) {
    if (k->buf && k->cap && k->len < k->cap) {
        size_t room = k->cap - k->len - 1;
        size_t take = n < room ? n : room;
        memcpy(k->buf + k->len, text, take);
        k->buf[k->len + take] = '\0';
    }
    k->len += n;
}

static void sk_puts(sk_t *k, const char *text) {
    sk_put(k, text, strlen(text));
}

static void sk_printf(sk_t *k, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) sk_put(k, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

/* A JSON string, quoted, or null for NULL. */
static void sk_str(sk_t *k, const char *s) {
    if (!s) { sk_puts(k, "null"); return; }
    size_t need = gui_settings_json_escape(s, NULL, 0);
    char stackbuf[512];
    char *esc = (need < sizeof(stackbuf)) ? stackbuf : malloc(need + 1);
    sk_puts(k, "\"");
    if (esc) {
        gui_settings_json_escape(s, esc, need + 1);
        sk_put(k, esc, need);
        if (esc != stackbuf) free(esc);
    } else {
        /* No memory for the escape: keep the length honest so the caller's
         * size probe and the real write still agree. */
        for (size_t i = 0; i < need; i++) sk_put(k, "?", 1);
    }
    sk_puts(k, "\"");
}

static void sk_key(sk_t *k, int indent, const char *key) {
    for (int i = 0; i < indent; i++) sk_puts(k, "  ");
    sk_puts(k, "\"");
    sk_puts(k, key);
    sk_puts(k, "\": ");
}

static void sk_u64_or_null(sk_t *k, bool have, uint64_t v) {
    if (have) sk_printf(k, "%llu", (unsigned long long)v);
    else sk_puts(k, "null");
}

static void sk_rf(sk_t *k, const gui_capture_sidecar_rf_t *rf) {
    if (!rf->name) { sk_puts(k, "null"); return; }
    sk_puts(k, "{\"name\": ");
    sk_str(k, rf->name);
    sk_printf(k, ", \"bits\": %u", rf->bits);
    sk_printf(k, ", \"sample_rate_hz\": %llu", (unsigned long long)rf->sample_rate_hz);
    sk_puts(k, ", \"samples\": ");
    sk_u64_or_null(k, rf->have_samples, rf->samples);
    sk_puts(k, ", \"bytes\": ");
    sk_u64_or_null(k, rf->have_bytes, rf->bytes);
    sk_puts(k, "}");
}

static void sk_asset(sk_t *k, const gui_capture_meta_t *m) {
    size_t n = 0;
    const gui_capture_meta_field_t *f = gui_capture_meta_fields(&n);
    bool first = true;
    sk_puts(k, "{\n");
    for (size_t i = 0; i < n; i++) {
        if (f[i].flags & GUI_META_F_TOP_LEVEL) continue;   /* operator: top level */
        if (!first) sk_puts(k, ",\n");
        first = false;
        sk_key(k, 2, f[i].key);
        if (f[i].type == GUI_META_TRI) {
            int8_t v = gui_capture_meta_tri(m, &f[i]);
            sk_puts(k, v == GUI_META_TRI_TRUE ? "true" : v == GUI_META_TRI_FALSE ? "false" : "null");
        } else if (f[i].type == GUI_META_INDEX) {
            uint64_t v;
            bool have = gui_capture_meta_index_value(m, &v);
            sk_u64_or_null(k, have, v);
        } else {
            sk_str(k, gui_capture_meta_str(m, &f[i]));
        }
    }
    sk_puts(k, "\n  }");
}

size_t gui_capture_sidecar_format(const gui_capture_sidecar_t *s, char *buf, size_t cap) {
    sk_t k = { buf, cap, 0 };
    if (buf && cap) buf[0] = '\0';
    if (!s || !s->meta) return 0;
    const gui_capture_meta_t *m = s->meta;

    sk_puts(&k, "{\n");
    sk_key(&k, 1, "schema");           sk_str(&k, GUI_CAPTURE_SIDECAR_SCHEMA); sk_puts(&k, ",\n");
    sk_key(&k, 1, "state");            sk_str(&k, s->complete ? "complete" : "recording"); sk_puts(&k, ",\n");
    sk_key(&k, 1, "linked");           sk_puts(&k, m->linked ? "true" : "false"); sk_puts(&k, ",\n");
    sk_key(&k, 1, "asset");            sk_asset(&k, m); sk_puts(&k, ",\n");
    sk_key(&k, 1, "operator");         sk_str(&k, m->operator_name); sk_puts(&k, ",\n");
    sk_key(&k, 1, "operator_source");
    sk_str(&k, gui_capture_meta_operator_source_name(m->operator_source)); sk_puts(&k, ",\n");
    sk_key(&k, 1, "session_file");     sk_str(&k, m->session_file[0] ? m->session_file : NULL); sk_puts(&k, ",\n");
    sk_key(&k, 1, "misrc_gui_version"); sk_str(&k, s->misrc_gui_version); sk_puts(&k, ",\n");
    sk_key(&k, 1, "computer_name");    sk_str(&k, s->computer_name); sk_puts(&k, ",\n");
    sk_key(&k, 1, "device");
    sk_puts(&k, "{\"name\": "); sk_str(&k, s->device_name);
    sk_puts(&k, ", \"type\": "); sk_str(&k, s->device_type); sk_puts(&k, "},\n");
    sk_key(&k, 1, "capture_format");   sk_str(&k, s->capture_format); sk_puts(&k, ",\n");
    sk_key(&k, 1, "started_at");       sk_str(&k, s->started_at); sk_puts(&k, ",\n");
    sk_key(&k, 1, "ended_at");         sk_str(&k, s->ended_at); sk_puts(&k, ",\n");
    sk_key(&k, 1, "capture_seconds");
    if (s->have_capture_seconds) sk_printf(&k, "%.3f", s->capture_seconds < 0.0 ? 0.0 : s->capture_seconds);
    else sk_puts(&k, "null");
    sk_puts(&k, ",\n");
    sk_key(&k, 1, "output_path");      sk_str(&k, s->output_path); sk_puts(&k, ",\n");
    sk_key(&k, 1, "base_name");        sk_str(&k, s->base_name); sk_puts(&k, ",\n");
    sk_key(&k, 1, "log_file");         sk_str(&k, s->log_file); sk_puts(&k, ",\n");
    sk_key(&k, 1, "rf_channels");
    {
        bool req_a = false, req_b = false;
        sk_puts(&k, "{\"requested\": ");
        if (gui_capture_meta_rf_request(m, &req_a, &req_b)) {
            sk_puts(&k, req_a ? "{\"a\": true" : "{\"a\": false");
            sk_puts(&k, req_b ? ", \"b\": true}" : ", \"b\": false}");
        } else {
            sk_puts(&k, "null");
        }
        sk_puts(&k, s->rf_recorded_a ? ", \"recorded\": {\"a\": true" : ", \"recorded\": {\"a\": false");
        sk_puts(&k, s->rf_recorded_b ? ", \"b\": true}},\n" : ", \"b\": false}},\n");
    }
    sk_key(&k, 1, "files");            sk_puts(&k, "{\n");
    sk_key(&k, 2, "rf_a");             sk_rf(&k, &s->rf_a); sk_puts(&k, ",\n");
    sk_key(&k, 2, "rf_b");             sk_rf(&k, &s->rf_b); sk_puts(&k, ",\n");
    sk_key(&k, 2, "video");            sk_str(&k, s->video); sk_puts(&k, ",\n");
    sk_key(&k, 2, "closed_captions");  sk_str(&k, s->closed_captions); sk_puts(&k, ",\n");
    sk_key(&k, 2, "audio");            sk_puts(&k, "{");
    for (int i = 0; i < GUI_CAPTURE_SIDECAR_AUDIO_COUNT; i++) {
        if (i) sk_puts(&k, ", ");
        sk_puts(&k, "\"");
        sk_puts(&k, gui_capture_sidecar_audio_keys[i]);
        sk_puts(&k, "\": ");
        sk_str(&k, s->audio[i]);
    }
    sk_puts(&k, "}\n  },\n");
    sk_key(&k, 1, "result");
    if (s->have_result) {
        sk_printf(&k, "{\"drops\": %llu", (unsigned long long)s->drops);
        sk_printf(&k, ", \"waits\": %llu}", (unsigned long long)s->waits);
    } else {
        sk_puts(&k, "null");
    }
    sk_puts(&k, "\n}\n");
    return k.len;
}

char *gui_capture_sidecar_format_alloc(const gui_capture_sidecar_t *s, size_t *len_out) {
    size_t need = gui_capture_sidecar_format(s, NULL, 0);
    char *buf = malloc(need + 1);
    if (!buf) return NULL;
    size_t got = gui_capture_sidecar_format(s, buf, need + 1);
    if (got != need) {   /* cannot happen: the same input formats the same */
        free(buf);
        return NULL;
    }
    if (len_out) *len_out = need;
    return buf;
}

/* ============================================================================
 * Atomic write
 * ============================================================================ */
static void set_err(char *err, size_t errcap, const char *what, const char *path) {
    if (err && errcap) {
        snprintf(err, errcap, "%s %s: %s", what, path, strerror(errno));
    }
}

bool gui_capture_sidecar_write_atomic(const char *path, const char *text, size_t len,
                                      char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!path || !path[0] || !text) {
        if (err && errcap) snprintf(err, errcap, "no sidecar path or text");
        return false;
    }
    size_t plen = strlen(path);
    char *tmp = malloc(plen + 32);
    if (!tmp) {
        if (err && errcap) snprintf(err, errcap, "out of memory");
        return false;
    }
    snprintf(tmp, plen + 32, "%s.tmp.%d", path, (int)sidecar_getpid());

    FILE *f = fopen(tmp, "wb");
    if (!f) {
        set_err(err, errcap, "cannot create", tmp);
        free(tmp);
        return false;
    }
    bool ok = fwrite(text, 1, len, f) == len;
    if (!ok) set_err(err, errcap, "cannot write", tmp);
    if (ok && fflush(f) != 0) { ok = false; set_err(err, errcap, "cannot flush", tmp); }
#if defined(_WIN32) || defined(_WIN64)
    if (ok && _commit(_fileno(f)) != 0) { ok = false; set_err(err, errcap, "cannot sync", tmp); }
#else
    if (ok && fsync(fileno(f)) != 0) { ok = false; set_err(err, errcap, "cannot sync", tmp); }
#endif
    if (fclose(f) != 0 && ok) { ok = false; set_err(err, errcap, "cannot close", tmp); }

    if (ok) {
#if defined(_WIN32) || defined(_WIN64)
        if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ok = false;
            if (err && errcap) {
                snprintf(err, errcap, "cannot rename %s to %s (Windows error %lu)",
                         tmp, path, (unsigned long)GetLastError());
            }
        }
#else
        if (rename(tmp, path) != 0) {
            ok = false;
            set_err(err, errcap, "cannot rename onto", path);
        } else {
            /* Best effort: make the rename itself durable. */
            char *dir = malloc(plen + 2);
            if (dir) {
                memcpy(dir, path, plen + 1);
                char *slash = strrchr(dir, '/');
                if (slash == dir) slash[1] = '\0';
                else if (slash) *slash = '\0';
                else snprintf(dir, plen + 2, ".");
                int dfd = open(dir, O_RDONLY);
                if (dfd >= 0) {
                    (void)fsync(dfd);
                    close(dfd);
                }
                free(dir);
            }
        }
#endif
    }
    if (!ok) remove(tmp);
    free(tmp);
    return ok;
}

const char *gui_capture_sidecar_basename(const char *path) {
    if (!path) return NULL;
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
#if defined(_WIN32) || defined(_WIN64)
        if (*p == '\\') name = p + 1;
#endif
    }
    return name;
}
