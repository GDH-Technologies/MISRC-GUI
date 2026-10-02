/*
 * MISRC GUI - Session launch file (--session <path.json>)
 *
 * See gui_session.h for the file format and the persistence rule.
 *
 * The settings file is flat, so the settings loader's key finder
 * (gui_settings_find_value) cannot tell a top-level key from one inside
 * "asset", cannot list an object's keys and has no types. The small reader
 * below walks exactly this file's shape: one object of strings plus the
 * "asset" object of strings, one integer-or-null and two
 * boolean-or-null members. The asset's members are found through the
 * capture-metadata descriptor table (gui_capture_meta_fields), so a field
 * the table gains is a field this parser reads, with no list here to keep
 * in step. Values that land in a setting (output_path, output_base_name)
 * are stored through gui_settings_apply_key() in strict mode, the same
 * validation a network /set gets.
 *
 * Licensed under GNU GPL v3 or later
 */

#include "gui_session.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#define gui_session_getpid _getpid
#else
#include <unistd.h>
#define gui_session_getpid getpid
#endif

/* ============================================================================
 * Module state
 * ============================================================================ */
#define GUI_SESSION_MAX_OVERLAID 2   /* output_path, output_base_name */

static bool s_active = false;
static gui_settings_t *s_original = NULL;    /* the struct before the overlay */
static const char *s_overlaid[GUI_SESSION_MAX_OVERLAID];  /* table keys */
static size_t s_overlaid_count = 0;
static bool s_base_name_overlaid = false;

static void set_err(char *err, size_t errcap, const char *fmt, ...) {
    if (!err || errcap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errcap, fmt, ap);
    va_end(ap);
}

/* ============================================================================
 * Reader
 * ============================================================================ */
typedef struct {
    const char *p;
    const char *start;
    const char *end;
    char *err;
    size_t errcap;
} reader_t;

/* The first failure wins; it names the byte offset where reading stopped. */
static void rd_fail(reader_t *r, const char *what) {
    if (r->err && r->errcap && !r->err[0]) {
        snprintf(r->err, r->errcap, "%s (at byte %ld)", what, (long)(r->p - r->start));
    }
}

static void rd_failf(reader_t *r, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    rd_fail(r, msg);
}

static void rd_ws(reader_t *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

static bool rd_expect(reader_t *r, char c, const char *what) {
    rd_ws(r);
    if (r->p >= r->end || *r->p != c) {
        rd_fail(r, what);
        return false;
    }
    r->p++;
    return true;
}

static int hex4(const char *p, const char *end) {
    if (end - p < 4) return -1;
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int n;
        if (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else return -1;
        v = (v << 4) | n;
    }
    return v;
}

static void put_byte(char *out, size_t cap, size_t *len, unsigned char b, bool *too_long) {
    if (*len + 1 >= cap) { *too_long = true; return; }
    out[(*len)++] = (char)b;
}

static void put_utf8(char *out, size_t cap, size_t *len, unsigned cp, bool *too_long) {
    if (cp < 0x80) {
        put_byte(out, cap, len, (unsigned char)cp, too_long);
    } else if (cp < 0x800) {
        put_byte(out, cap, len, (unsigned char)(0xC0 | (cp >> 6)), too_long);
        put_byte(out, cap, len, (unsigned char)(0x80 | (cp & 0x3F)), too_long);
    } else if (cp < 0x10000) {
        put_byte(out, cap, len, (unsigned char)(0xE0 | (cp >> 12)), too_long);
        put_byte(out, cap, len, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)), too_long);
        put_byte(out, cap, len, (unsigned char)(0x80 | (cp & 0x3F)), too_long);
    } else {
        put_byte(out, cap, len, (unsigned char)(0xF0 | (cp >> 18)), too_long);
        put_byte(out, cap, len, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)), too_long);
        put_byte(out, cap, len, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)), too_long);
        put_byte(out, cap, len, (unsigned char)(0x80 | (cp & 0x3F)), too_long);
    }
}

/* Read a JSON string at r->p into out (decoded, NUL-terminated). Sets
 * *too_long when it did not fit. Raw control characters are not JSON and
 * are refused here; escaped ones decode and are judged per field. */
static bool rd_string(reader_t *r, char *out, size_t cap, bool *too_long) {
    size_t len = 0;
    *too_long = false;
    rd_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        rd_fail(r, "expected a string");
        return false;
    }
    r->p++;
    while (r->p < r->end) {
        unsigned char c = (unsigned char)*r->p++;
        if (c == '"') {
            out[len] = '\0';
            return true;
        }
        if (c < 0x20) {
            rd_fail(r, "raw control character inside a string");
            return false;
        }
        if (c != '\\') {
            put_byte(out, cap, &len, c, too_long);
            continue;
        }
        if (r->p >= r->end) break;
        char e = *r->p++;
        unsigned cp;
        switch (e) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u': {
                int h = hex4(r->p, r->end);
                if (h < 0) { rd_fail(r, "bad \\u escape"); return false; }
                r->p += 4;
                cp = (unsigned)h;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    int lo = (r->end - r->p >= 6 && r->p[0] == '\\' && r->p[1] == 'u')
                                 ? hex4(r->p + 2, r->end) : -1;
                    if (lo < 0xDC00 || lo > 0xDFFF) { rd_fail(r, "unpaired surrogate in \\u escape"); return false; }
                    r->p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    rd_fail(r, "unpaired surrogate in \\u escape");
                    return false;
                }
                if (cp == 0) { rd_fail(r, "\\u0000 inside a string"); return false; }
                break;
            }
            default:
                rd_fail(r, "bad escape inside a string");
                return false;
        }
        put_utf8(out, cap, &len, cp, too_long);
    }
    rd_fail(r, "unterminated string");
    return false;
}

/* Skip any JSON value (used for keys this schema does not know). */
static bool rd_skip_value(reader_t *r, int depth) {
    if (depth > 32) { rd_fail(r, "nested too deeply"); return false; }
    rd_ws(r);
    if (r->p >= r->end) { rd_fail(r, "expected a value"); return false; }
    char c = *r->p;
    if (c == '"') {
        char scratch[8];
        bool tl;
        return rd_string(r, scratch, sizeof(scratch), &tl);
    }
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        r->p++;
        rd_ws(r);
        if (r->p < r->end && *r->p == close) { r->p++; return true; }
        for (;;) {
            if (c == '{') {
                char scratch[8];
                bool tl;
                if (!rd_string(r, scratch, sizeof(scratch), &tl)) return false;
                if (!rd_expect(r, ':', "expected ':' after a key")) return false;
            }
            if (!rd_skip_value(r, depth + 1)) return false;
            rd_ws(r);
            if (r->p < r->end && *r->p == ',') { r->p++; continue; }
            if (r->p < r->end && *r->p == close) { r->p++; return true; }
            rd_fail(r, "expected ',' or the end of an object/array");
            return false;
        }
    }
    /* Number, true, false, null: a run of literal characters. */
    const char *start = r->p;
    while (r->p < r->end && ((*r->p >= '0' && *r->p <= '9') || (*r->p >= 'a' && *r->p <= 'z') ||
                             *r->p == '-' || *r->p == '+' || *r->p == '.' || *r->p == 'E')) {
        r->p++;
    }
    if (r->p == start) { rd_fail(r, "expected a value"); return false; }
    return true;
}

/* Iterate an object's members. Call with r->p at '{'. For each member the
 * callback gets the decoded key and must consume the value. */
typedef bool (*member_fn)(reader_t *r, const char *key, void *ctx);

static bool rd_object(reader_t *r, member_fn fn, void *ctx) {
    if (!rd_expect(r, '{', "expected an object")) return false;
    rd_ws(r);
    if (r->p < r->end && *r->p == '}') { r->p++; return true; }
    for (;;) {
        char key[128];
        bool too_long;
        if (!rd_string(r, key, sizeof(key), &too_long)) return false;
        if (too_long) { rd_fail(r, "a key is longer than 127 bytes"); return false; }
        if (!rd_expect(r, ':', "expected ':' after a key")) return false;
        if (!fn(r, key, ctx)) return false;
        rd_ws(r);
        if (r->p < r->end && *r->p == ',') { r->p++; continue; }
        if (r->p < r->end && *r->p == '}') { r->p++; return true; }
        rd_fail(r, "expected ',' or '}' in an object");
        return false;
    }
}

/* A bare word (true / false / null): the run of a-z at r->p. */
static size_t rd_word(reader_t *r, char *out, size_t cap) {
    rd_ws(r);
    size_t n = 0;
    while (r->p < r->end && *r->p >= 'a' && *r->p <= 'z') {
        if (n + 1 < cap) out[n] = *r->p;
        n++;
        r->p++;
    }
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}

/* ============================================================================
 * Field validation
 * ============================================================================ */
typedef struct {
    gui_session_data_t *data;
    bool has_schema;
    unsigned top_seen;        /* bit per known top-level key */
    uint32_t asset_seen;      /* bit per descriptor slot */
    char *scratch;            /* decoded string values (file-sized) */
    size_t scratch_cap;
} parse_ctx_t;

/* Validate a decoded string in place and copy it into out[cap]. `where`
 * names the field in every refusal. Multiline fields keep \n \r \t and
 * store CRLF as LF; every other control character is refused everywhere. */
static bool take_string(reader_t *r, const char *where, char *value,
                        char *out, size_t cap, bool multiline, bool id) {
    size_t len = strlen(value);
    if (!gui_capture_meta_utf8_valid(value, len)) {
        rd_failf(r, "%s is not valid UTF-8", where);
        return false;
    }
    if (multiline) {
        size_t w = 0;
        for (size_t i = 0; i < len; i++) {
            if (value[i] == '\r' && value[i + 1] == '\n') continue;   /* CRLF -> LF */
            value[w++] = value[i];
        }
        value[w] = '\0';
        len = w;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c >= 0x20 && c != 0x7F) continue;
        if (multiline && (c == '\n' || c == '\r' || c == '\t')) continue;
        rd_failf(r, multiline
                     ? "%s may not contain a control character other than a line break or tab"
                     : "%s may not contain a control character (line break, tab, ...)",
                 where);
        return false;
    }
    if (len >= cap) {
        rd_failf(r, "%s is longer than %u bytes", where, (unsigned)(cap - 1));
        return false;
    }
    if (id && len > 0) {
        for (size_t i = 0; i < len; i++) {
            char c = value[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '.' || c == '-';
            if (!ok) {
                rd_failf(r, "%s must be 1-%u characters of A-Z a-z 0-9 _ . -", where, (unsigned)(cap - 1));
                return false;
            }
        }
    }
    memcpy(out, value, len + 1);
    return true;
}

/* A string-typed member, decoded and validated into out[cap]. */
static bool rd_member_string(reader_t *r, parse_ctx_t *ctx, const char *where,
                             char *out, size_t cap, bool multiline, bool id) {
    rd_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        rd_failf(r, "%s must be a string", where);
        return false;
    }
    bool too_long;
    if (!rd_string(r, ctx->scratch, ctx->scratch_cap, &too_long)) return false;
    if (too_long) {
        rd_failf(r, "%s is longer than %u bytes", where, (unsigned)(cap - 1));
        return false;
    }
    return take_string(r, where, ctx->scratch, out, cap, multiline, id);
}

static bool on_asset_member(reader_t *r, const char *key, void *vctx) {
    parse_ctx_t *ctx = (parse_ctx_t *)vctx;
    size_t n = 0;
    const gui_capture_meta_field_t *fields = gui_capture_meta_fields(&n);
    size_t slot = n;
    for (size_t i = 0; i < n; i++) {
        if (!(fields[i].flags & GUI_META_F_TOP_LEVEL) && strcmp(fields[i].key, key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot == n) {
        fprintf(stderr, "[SESSION] ignoring unknown key asset.%s\n", key);
        return rd_skip_value(r, 1);
    }
    const gui_capture_meta_field_t *f = &fields[slot];
    char where[96];
    snprintf(where, sizeof(where), "asset.%s", f->key);
    if (ctx->asset_seen & (1u << slot)) {
        rd_failf(r, "%s appears twice", where);
        return false;
    }
    ctx->asset_seen |= 1u << slot;
    gui_capture_meta_t *m = &ctx->data->asset;

    if (f->type == GUI_META_STR) {
        return rd_member_string(r, ctx, where, gui_capture_meta_str_mut(m, f), f->cap,
                                (f->flags & GUI_META_F_MULTILINE) != 0,
                                (f->flags & GUI_META_F_ID) != 0);
    }
    if (f->type == GUI_META_TRI) {
        char word[8];
        size_t wn = rd_word(r, word, sizeof(word));
        int8_t v;
        if (wn == 4 && strcmp(word, "true") == 0) v = GUI_META_TRI_TRUE;
        else if (wn == 5 && strcmp(word, "false") == 0) v = GUI_META_TRI_FALSE;
        else if (wn == 4 && strcmp(word, "null") == 0) v = GUI_META_TRI_UNSET;
        else {
            rd_failf(r, "%s must be true, false or null", where);
            return false;
        }
        memcpy((char *)m + f->offset, &v, sizeof(v));
        return true;
    }
    /* GUI_META_INDEX: a JSON integer >= 0, or null. */
    char *out = gui_capture_meta_str_mut(m, f);
    rd_ws(r);
    if (r->p < r->end && *r->p == 'n') {
        char word[8];
        size_t wn = rd_word(r, word, sizeof(word));
        if (wn == 4 && strcmp(word, "null") == 0) {
            out[0] = '\0';
            return true;
        }
    } else if (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        const char *start = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
        size_t digits = (size_t)(r->p - start);
        bool fraction = r->p < r->end && (*r->p == '.' || *r->p == 'e' || *r->p == 'E');
        bool leading_zero = digits > 1 && start[0] == '0';
        if (!fraction && !leading_zero) {
            uint64_t v = 0;
            bool over = digits >= f->cap;
            for (size_t i = 0; !over && i < digits; i++) {
                unsigned d = (unsigned)(start[i] - '0');
                if (v > (GUI_META_INDEX_MAX - d) / 10) over = true;
                else v = v * 10 + d;
            }
            if (over) {
                rd_failf(r, "%s is larger than %llu", where, (unsigned long long)GUI_META_INDEX_MAX);
                return false;
            }
            snprintf(out, f->cap, "%llu", (unsigned long long)v);
            return true;
        }
    }
    rd_failf(r, "%s must be a non-negative integer or null", where);
    return false;
}

/* Known top-level keys, one bit each (a duplicate is refused). */
enum { TOP_SCHEMA = 1u << 0, TOP_OUTPUT_PATH = 1u << 1, TOP_BASE_NAME = 1u << 2,
       TOP_OPERATOR = 1u << 3, TOP_ASSET = 1u << 4 };

static bool top_once(reader_t *r, parse_ctx_t *ctx, unsigned bit, const char *key) {
    if (ctx->top_seen & bit) {
        rd_failf(r, "%s appears twice", key);
        return false;
    }
    ctx->top_seen |= bit;
    return true;
}

static bool on_top_member(reader_t *r, const char *key, void *vctx) {
    parse_ctx_t *ctx = (parse_ctx_t *)vctx;
    gui_session_data_t *d = ctx->data;
    if (strcmp(key, "schema") == 0) {
        if (!top_once(r, ctx, TOP_SCHEMA, key)) return false;
        char schema[96];
        if (!rd_member_string(r, ctx, "schema", schema, sizeof(schema), false, false)) return false;
        if (strcmp(schema, GUI_SESSION_SCHEMA) != 0) {
            rd_failf(r, "unsupported schema \"%s\" (this build reads \"%s\")", schema, GUI_SESSION_SCHEMA);
            return false;
        }
        ctx->has_schema = true;
        return true;
    }
    if (strcmp(key, "output_path") == 0) {
        if (!top_once(r, ctx, TOP_OUTPUT_PATH, key)) return false;
        if (!rd_member_string(r, ctx, "output_path", d->output_path, sizeof(d->output_path),
                              false, false)) return false;
        d->has_output_path = true;
        return true;
    }
    if (strcmp(key, "output_base_name") == 0) {
        if (!top_once(r, ctx, TOP_BASE_NAME, key)) return false;
        if (!rd_member_string(r, ctx, "output_base_name", d->output_base_name,
                              sizeof(d->output_base_name), false, false)) return false;
        d->has_output_base_name = true;
        return true;
    }
    if (strcmp(key, "operator") == 0) {
        if (!top_once(r, ctx, TOP_OPERATOR, key)) return false;
        const gui_capture_meta_field_t *f = gui_capture_meta_find_field("operator");
        if (!rd_member_string(r, ctx, "operator", d->operator_name, f ? f->cap : sizeof(d->operator_name),
                              false, false)) return false;
        d->has_operator = true;
        return true;
    }
    if (strcmp(key, "asset") == 0) {
        if (!top_once(r, ctx, TOP_ASSET, key)) return false;
        rd_ws(r);
        if (r->p >= r->end || *r->p != '{') { rd_fail(r, "asset must be an object"); return false; }
        if (!rd_object(r, on_asset_member, ctx)) return false;
        d->has_asset = true;
        return true;
    }
    /* Includes the retired "ingest" and "log_tags". */
    fprintf(stderr, "[SESSION] ignoring unknown key %s\n", key);
    return rd_skip_value(r, 1);
}

bool gui_session_parse_text(const char *text, gui_session_data_t *out,
                            char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!text || !out) {
        set_err(err, errcap, "no session text");
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->asset.hifi_audio_equipped = GUI_META_TRI_UNSET;
    out->asset.black_and_white = GUI_META_TRI_UNSET;
    size_t text_len = strlen(text);
    reader_t r = { text, text, text + text_len, err, errcap };
    /* A UTF-8 byte order mark is not JSON, but editors write one. */
    if (r.end - r.p >= 3 && (unsigned char)r.p[0] == 0xEF &&
        (unsigned char)r.p[1] == 0xBB && (unsigned char)r.p[2] == 0xBF) {
        r.p += 3;
    }
    /* Every decoded string fits in the file it came from. */
    parse_ctx_t ctx = { out, false, 0, 0, malloc(text_len + 1), text_len + 1 };
    if (!ctx.scratch) {
        set_err(err, errcap, "out of memory");
        return false;
    }
    bool ok = false;
    rd_ws(&r);
    if (r.p >= r.end || *r.p != '{') {
        rd_fail(&r, "not a JSON object");
    } else if (rd_object(&r, on_top_member, &ctx)) {
        rd_ws(&r);
        if (r.p != r.end) {
            rd_fail(&r, "trailing text after the JSON object");
        } else if (!ctx.has_schema) {
            set_err(err, errcap, "missing \"schema\" (expected \"%s\")", GUI_SESSION_SCHEMA);
        } else {
            ok = true;
        }
    }
    if (ok && out->has_asset) {
        /* An asset is all of its required fields or it is refused. */
        size_t n = 0;
        const gui_capture_meta_field_t *fields = gui_capture_meta_fields(&n);
        for (size_t i = 0; i < n; i++) {
            if (!(fields[i].flags & GUI_META_F_REQUIRED_LINKED)) continue;
            if (fields[i].type == GUI_META_STR && gui_capture_meta_str(&out->asset, &fields[i])[0]) continue;
            set_err(err, errcap, "asset.%s is required (non-empty) when an asset is given",
                    fields[i].key);
            ok = false;
            break;
        }
    }
    free(ctx.scratch);
    if (!ok) memset(out, 0, sizeof(*out));
    return ok;
}

/* ============================================================================
 * Overlay and persistence
 * ============================================================================ */

/* Every filename gui_settings_refresh_auto_names() derives from the base
 * name. A base-name overlay rewrites them in memory; the file must get the
 * operator's own back. */
static void copy_derived_names(gui_settings_t *dst, const gui_settings_t *src) {
    memcpy(dst->output_filename_a, src->output_filename_a, sizeof(dst->output_filename_a));
    memcpy(dst->output_filename_b, src->output_filename_b, sizeof(dst->output_filename_b));
    memcpy(dst->audio_4ch_filename, src->audio_4ch_filename, sizeof(dst->audio_4ch_filename));
    memcpy(dst->video_filename, src->video_filename, sizeof(dst->video_filename));
    memcpy(dst->usbref_cc_filename, src->usbref_cc_filename, sizeof(dst->usbref_cc_filename));
    memcpy(dst->audio_2ch_12_filename, src->audio_2ch_12_filename, sizeof(dst->audio_2ch_12_filename));
    memcpy(dst->audio_2ch_34_filename, src->audio_2ch_34_filename, sizeof(dst->audio_2ch_34_filename));
    memcpy(dst->audio_1ch_filenames, src->audio_1ch_filenames, sizeof(dst->audio_1ch_filenames));
}

/* The persist filter: put every overlaid field back to its pre-overlay
 * value. Overlaid fields are session-scoped -- an edit the operator makes to
 * one of them during the session lasts for the session and is not saved.
 * Every other field saves exactly as it is in memory. */
static void session_persist_filter(gui_settings_t *to_write) {
    if (!s_active || !s_original || !to_write) return;
    for (size_t i = 0; i < s_overlaid_count; i++) {
        const gui_setting_desc_t *d = gui_settings_find(s_overlaid[i]);
        if (d) gui_settings_copy_field(to_write, s_original, d);
    }
    if (s_base_name_overlaid) {
        to_write->auto_names_enabled = s_original->auto_names_enabled;
        if (to_write->auto_names_enabled) {
            /* Rederive from the restored base name, so a tag or format
             * changed during the session is reflected consistently. */
            gui_settings_refresh_auto_names(to_write);
        } else {
            /* Manual names: the operator's own, untouched. */
            copy_derived_names(to_write, s_original);
        }
    }
}

void gui_session_clear(void) {
    gui_settings_set_persist_filter(NULL);
    free(s_original);
    s_original = NULL;
    s_active = false;
    s_overlaid_count = 0;
    s_base_name_overlaid = false;
}

bool gui_session_apply(const gui_session_data_t *data, gui_settings_t *settings,
                       const char *session_path, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!data || !settings) {
        set_err(err, errcap, "nothing to apply");
        return false;
    }
    if (data->has_output_path && !data->output_path[0]) {
        set_err(err, errcap, "output_path is empty");
        return false;
    }
    if (data->has_output_base_name && !data->output_base_name[0]) {
        set_err(err, errcap, "output_base_name is empty");
        return false;
    }

    /* Build the overlaid struct on the side, so a refused value leaves the
     * live settings exactly as loaded. The capture metadata was validated
     * whole by the parser and is committed last, after nothing can fail. */
    gui_settings_t *next = malloc(sizeof(*next));
    gui_settings_t *orig = malloc(sizeof(*orig));
    if (!next || !orig) {
        free(next);
        free(orig);
        set_err(err, errcap, "out of memory");
        return false;
    }
    *next = *settings;
    *orig = *settings;

    const char *overlaid[GUI_SESSION_MAX_OVERLAID];
    size_t n_overlaid = 0;
    char why[128];
    const struct { bool present; const char *key; const char *value; } slots[GUI_SESSION_MAX_OVERLAID] = {
        { data->has_output_path,      "output_path",      data->output_path },
        { data->has_output_base_name, "output_base_name", data->output_base_name },
    };
    for (size_t i = 0; i < GUI_SESSION_MAX_OVERLAID; i++) {
        if (!slots[i].present) continue;
        const gui_setting_desc_t *d = gui_settings_find(slots[i].key);
        why[0] = '\0';
        if (!d || gui_settings_apply_key(next, slots[i].key, slots[i].value, true, why, sizeof(why)) != 0) {
            set_err(err, errcap, "%s: %s", slots[i].key, (d && why[0]) ? why : "refused");
            free(next);
            free(orig);
            return false;
        }
        overlaid[n_overlaid++] = d->key;
    }
    if (data->has_output_base_name) {
        /* A base name only names files while auto naming is on; with it off
         * the files keep the manual names and the session's name would reach
         * nothing but the log's file name. Session-scoped like the rest. */
        next->auto_names_enabled = true;
        gui_settings_refresh_auto_names(next);
    }

    /* Commit: settings overlay first, then the capture metadata. */
    gui_session_clear();
    *settings = *next;
    free(next);
    s_original = orig;
    memcpy(s_overlaid, overlaid, n_overlaid * sizeof(overlaid[0]));
    s_overlaid_count = n_overlaid;
    s_base_name_overlaid = data->has_output_base_name;
    s_active = true;
    gui_settings_set_persist_filter(session_persist_filter);

    const char *op = data->has_operator ? data->operator_name : "";
    if (data->has_asset) {
        gui_capture_meta_t linked = data->asset;
        snprintf(linked.operator_name, sizeof(linked.operator_name), "%s", op);
        gui_capture_meta_set_linked(&linked, session_path);
    } else {
        gui_capture_meta_set_unlinked_session(op, session_path);
    }
    return true;
}

bool gui_session_apply_file(const char *path, gui_settings_t *settings,
                            char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!path || !path[0]) {
        set_err(err, errcap, "--session needs a file path");
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, errcap, "cannot open %s", path);
        return false;
    }
    char *text = malloc(GUI_SESSION_MAX_FILE_BYTES + 1);
    if (!text) {
        fclose(f);
        set_err(err, errcap, "out of memory");
        return false;
    }
    size_t n = fread(text, 1, GUI_SESSION_MAX_FILE_BYTES + 1, f);
    bool read_error = ferror(f) != 0;
    fclose(f);
    if (read_error) {
        free(text);
        set_err(err, errcap, "cannot read %s", path);
        return false;
    }
    if (n > GUI_SESSION_MAX_FILE_BYTES) {
        free(text);
        set_err(err, errcap, "%s is larger than %d bytes", path, GUI_SESSION_MAX_FILE_BYTES);
        return false;
    }
    text[n] = '\0';
    if (strlen(text) != n) {
        free(text);
        set_err(err, errcap, "%s contains a NUL byte", path);
        return false;
    }

    gui_session_data_t *data = malloc(sizeof(*data));
    if (!data) {
        free(text);
        set_err(err, errcap, "out of memory");
        return false;
    }
    bool ok = gui_session_parse_text(text, data, err, errcap) &&
              gui_session_apply(data, settings, path, err, errcap);
    free(data);
    free(text);
    return ok;
}

bool gui_session_active(void) {
    return s_active;
}

/* ============================================================================
 * --session-selftest
 * ============================================================================ */
static int s_failures = 0;

#define ST_CHECK(cond, ...) do { \
        if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); s_failures++; } \
    } while (0)

static bool st_write(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    return (fclose(f) == 0) && ok;
}

static char *st_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(GUI_SETTINGS_MAX_FILE_BYTES + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, GUI_SETTINGS_MAX_FILE_BYTES, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* A rejected file must leave the settings, the capture metadata and the
 * module untouched. */
static void st_expect_rejected(const char *label, const char *path, const char *text,
                               gui_settings_t *settings) {
    if (text && !st_write(path, text)) {
        printf("FAIL: cannot write scratch session file %s\n", path);
        s_failures++;
        return;
    }
    gui_settings_t *before = malloc(sizeof(*before));
    gui_capture_meta_t *meta_before = malloc(sizeof(*meta_before));
    if (!before || !meta_before) { free(before); free(meta_before); s_failures++; return; }
    *before = *settings;
    gui_capture_meta_snapshot(meta_before);
    bool active_before = gui_session_active();
    char err[256];
    bool ok = gui_session_apply_file(path, settings, err, sizeof(err));
    ST_CHECK(!ok, "%s: accepted, must be rejected", label);
    ST_CHECK(err[0] != '\0', "%s: rejected without a reason", label);
    ST_CHECK(memcmp(before, settings, sizeof(*before)) == 0, "%s: a rejected file changed the settings", label);
    ST_CHECK(memcmp(meta_before, gui_capture_meta_get(), sizeof(*meta_before)) == 0,
             "%s: a rejected file changed the capture metadata", label);
    ST_CHECK(gui_session_active() == active_before, "%s: a rejected file changed the session state", label);
    if (!ok) printf("  rejected %-30s -> %s\n", label, err);
    free(before);
    free(meta_before);
}

/* A valid linked asset, with `extra` spliced in after the required fields
 * (it must start with ", " or be empty) and the given asset_id literal. */
static char *st_asset_session(const char *asset_id_json, const char *extra) {
    size_t cap = 1024 + strlen(extra);
    char *s = malloc(cap);
    if (!s) return NULL;
    snprintf(s, cap,
             "{\"schema\": \"misrc-gui.session/1\", \"asset\": {"
             "\"asset_id\": %s, \"client_id\": \"c1\", \"client_name\": \"Client\", "
             "\"display_name\": \"Title\", \"label\": \"Tape 1\", \"format\": \"VHS\"%s}}",
             asset_id_json, extra);
    return s;
}

int gui_session_selftest_main(void) {
    s_failures = 0;
    char dir[400];
#if defined(_WIN32) || defined(_WIN64)
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = ".";
    const char sep = '\\';
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    const char sep = '/';
#endif
    snprintf(dir, sizeof(dir), "%s", tmp);
    char cfg_path[512], ses_path[512], bad_path[512];
    int pid = (int)gui_session_getpid();
    snprintf(cfg_path, sizeof(cfg_path), "%s%cmisrc_session_selftest_%d_settings.json", dir, sep, pid);
    snprintf(ses_path, sizeof(ses_path), "%s%cmisrc_session_selftest_%d_session.json", dir, sep, pid);
    snprintf(bad_path, sizeof(bad_path), "%s%cmisrc_session_selftest_%d_bad.json", dir, sep, pid);
    remove(cfg_path);
    remove(ses_path);
    remove(bad_path);
    /* Never the live settings file, whatever else is on the command line. */
    gui_settings_set_override_path(cfg_path);
    gui_session_clear();
    gui_capture_meta_init();
    printf("settings file: %s (scratch)\n", cfg_path);
    printf("session file:  %s (scratch)\n", ses_path);

    gui_settings_t *live = calloc(1, sizeof(*live));
    gui_settings_t *disk = calloc(1, sizeof(*disk));
    if (!live || !disk) {
        free(live);
        free(disk);
        printf("SESSION SELFTEST FAILED: out of memory\n");
        return 1;
    }
    const gui_capture_meta_t *meta = gui_capture_meta_get();

    /* 1. The operator's own settings, unlike any session value. Manual file
     *    names (auto naming off) are the hard case for the base-name rule. */
    gui_settings_init_defaults(live);
    snprintf(live->output_path, sizeof(live->output_path), "/operator/captures");
    snprintf(live->output_base_name, sizeof(live->output_base_name), "OperatorBase");
    live->auto_names_enabled = false;
    snprintf(live->output_filename_a, sizeof(live->output_filename_a), "manual_a.flac");
    snprintf(live->output_filename_b, sizeof(live->output_filename_b), "manual_b.flac");
    snprintf(live->ffmpeg_path, sizeof(live->ffmpeg_path), "/opt/operator/ffmpeg");
    gui_settings_save(live);

    /* 2. Load them back the way main() does, then apply a linked session. */
    memset(live, 0, sizeof(*live));
    gui_settings_load(live);
    ST_CHECK(strcmp(live->output_path, "/operator/captures") == 0, "baseline did not round-trip (output_path=%s)", live->output_path);
    ST_CHECK(!live->auto_names_enabled, "baseline auto_names_enabled did not round-trip");

    const char *session_text =
        "\xEF\xBB\xBF{\n"
        "  \"schema\": \"misrc-gui.session/1\",\n"
        "  \"output_path\": \"/session/out\",\n"
        "  \"output_base_name\": \"Session_Tape\",\n"
        "  \"operator\": \"Session Operator\",\n"
        "  \"future_key\": {\"nested\": [1, 2.5, true, null, \"x\"]},\n"
        "  \"ingest\": {\"project\": \"retired\"},\n"
        "  \"log_tags\": {\"asset_id\": \"retired\"},\n"
        "  \"asset\": {\"asset_id\": \"asset_019abc\", \"client_id\": \"client.7\",\n"
        "            \"client_name\": \"Kuhn Family\", \"display_name\": \"Christmas 1994\",\n"
        "            \"index\": 3, \"label\": \"Tape 3 \\\"VHS-C\\\"\", \"format\": \"VHS\",\n"
        "            \"tape_speed\": \"SP\", \"video_system\": \"\",\n"
        "            \"hifi_audio_equipped\": true, \"black_and_white\": null,\n"
        "            \"notes\": \"caf\\u00e9 \\/ \\\\ \\ud83d\\udcfc\\r\\nline 2\\ttab\",\n"
        "            \"not_a_field\": 3}\n"
        "}\n";
    if (!st_write(ses_path, session_text)) {
        printf("FAIL: cannot write %s\n", ses_path);
        s_failures++;
    }
    char err[256];
    bool applied = gui_session_apply_file(ses_path, live, err, sizeof(err));
    ST_CHECK(applied, "valid linked session rejected: %s", err);
    ST_CHECK(gui_session_active(), "session not active after apply");

    /* 3. The overlay and the link are what is in memory. */
    ST_CHECK(strcmp(live->output_path, "/session/out") == 0, "output_path not overlaid (%s)", live->output_path);
    ST_CHECK(strcmp(live->output_base_name, "Session_Tape") == 0, "output_base_name not overlaid (%s)", live->output_base_name);
    ST_CHECK(live->auto_names_enabled, "auto naming not turned on for the session's base name");
    ST_CHECK(strstr(live->output_filename_a, "Session_Tape") != NULL, "derived name ignores the base name (%s)", live->output_filename_a);
    ST_CHECK(meta->linked, "the asset did not link the capture");
    ST_CHECK(strcmp(meta->asset_id, "asset_019abc") == 0, "asset_id not linked (%s)", meta->asset_id);
    ST_CHECK(strcmp(meta->client_id, "client.7") == 0, "client_id not linked");
    ST_CHECK(strcmp(meta->client_name, "Kuhn Family") == 0, "client_name not linked");
    ST_CHECK(strcmp(meta->display_name, "Christmas 1994") == 0, "display_name not linked");
    ST_CHECK(strcmp(meta->index_text, "3") == 0, "index not linked (%s)", meta->index_text);
    ST_CHECK(strcmp(meta->label, "Tape 3 \"VHS-C\"") == 0, "label with quotes not linked (%s)", meta->label);
    ST_CHECK(strcmp(meta->format, "VHS") == 0 && strcmp(meta->tape_speed, "SP") == 0, "format/tape_speed not linked");
    ST_CHECK(meta->video_system[0] == '\0', "an empty optional string is not empty");
    ST_CHECK(meta->hifi_audio_equipped == GUI_META_TRI_TRUE, "hifi true not linked");
    ST_CHECK(meta->black_and_white == GUI_META_TRI_UNSET, "black_and_white null is not unset");
    ST_CHECK(strcmp(meta->notes, "caf\xC3\xA9 / \\ \xF0\x9F\x93\xBC\nline 2\ttab") == 0,
             "notes escapes/CRLF decoded wrong (%s)", meta->notes);
    ST_CHECK(strcmp(meta->operator_name, "Session Operator") == 0 &&
             meta->operator_source == GUI_META_OPERATOR_SESSION, "session operator not taken");
    ST_CHECK(strcmp(meta->session_file, ses_path) == 0, "session_file not recorded (%s)", meta->session_file);
    printf("  linked: %s / %s (%s) index=%s operator=%s\n", meta->client_name, meta->display_name,
           meta->asset_id, meta->index_text, meta->operator_name);

    /* 4. The operator edits during the session: one overlaid field (session-
     *    scoped, must NOT persist) and one ordinary field (must persist). */
    snprintf(live->output_path, sizeof(live->output_path), "/edited/during/session");
    snprintf(live->ffmpeg_path, sizeof(live->ffmpeg_path), "/opt/edited/ffmpeg");
    gui_settings_save(live);
    ST_CHECK(strcmp(live->output_path, "/edited/during/session") == 0, "saving changed the overlay in memory");

    /* 5. The file holds the operator's values for every overlaid key, and
     *    nothing of the asset (capture metadata is never a setting). */
    gui_settings_load(disk);
    ST_CHECK(strcmp(disk->output_path, "/operator/captures") == 0, "output_path persisted the overlay (%s)", disk->output_path);
    ST_CHECK(strcmp(disk->output_base_name, "OperatorBase") == 0, "output_base_name persisted the overlay (%s)", disk->output_base_name);
    ST_CHECK(!disk->auto_names_enabled, "auto_names_enabled persisted the session's forced on");
    ST_CHECK(strcmp(disk->output_filename_a, "manual_a.flac") == 0, "manual RF A name lost (%s)", disk->output_filename_a);
    ST_CHECK(strcmp(disk->output_filename_b, "manual_b.flac") == 0, "manual RF B name lost (%s)", disk->output_filename_b);
    ST_CHECK(strcmp(disk->ffmpeg_path, "/opt/edited/ffmpeg") == 0, "an ordinary field's edit did not persist (%s)", disk->ffmpeg_path);
    char *raw = st_read(cfg_path);
    ST_CHECK(raw != NULL, "cannot read back %s", cfg_path);
    if (raw) {
        ST_CHECK(strstr(raw, "Session_Tape") == NULL, "the session base name leaked into the file");
        ST_CHECK(strstr(raw, "/session/out") == NULL, "the session output_path leaked into the file");
        ST_CHECK(strstr(raw, "Kuhn Family") == NULL && strstr(raw, "asset_019abc") == NULL,
                 "the asset leaked into the settings file");
        ST_CHECK(strstr(raw, "ingest_") == NULL, "a retired ingest_* key is still written");
        free(raw);
    }
    printf("  on disk: output_path=%s base=%s auto_names=%d rfA=%s ffmpeg=%s\n",
           disk->output_path, disk->output_base_name, (int)disk->auto_names_enabled,
           disk->output_filename_a, disk->ffmpeg_path);

    /* 6. With auto naming on in the operator's file, the restored derived
     *    names come from the operator's base name, not the session's. */
    gui_session_clear();
    gui_capture_meta_init();
    gui_settings_load(live);
    live->auto_names_enabled = true;
    gui_settings_refresh_auto_names(live);
    gui_settings_save(live);
    gui_settings_load(live);
    applied = gui_session_apply_file(ses_path, live, err, sizeof(err));
    ST_CHECK(applied, "valid session rejected on the second pass: %s", err);
    gui_settings_save(live);
    gui_settings_load(disk);
    ST_CHECK(disk->auto_names_enabled, "auto naming lost on the second pass");
    ST_CHECK(strstr(disk->output_filename_a, "OperatorBase") != NULL, "auto name not rederived from the operator's base (%s)", disk->output_filename_a);

    /* 7. A valid asset-less session: settings overlay, operator, no link. */
    gui_session_clear();
    gui_capture_meta_init();
    gui_settings_load(live);
    if (!st_write(ses_path, "{\"schema\": \"misrc-gui.session/1\", \"output_path\": \"/asset/less\", "
                            "\"operator\": \"Night Shift\"}")) {
        printf("FAIL: cannot write %s\n", ses_path);
        s_failures++;
    }
    applied = gui_session_apply_file(ses_path, live, err, sizeof(err));
    ST_CHECK(applied, "valid asset-less session rejected: %s", err);
    ST_CHECK(!meta->linked, "an asset-less session linked the capture");
    ST_CHECK(meta->asset_id[0] == '\0', "an asset-less session set an asset_id");
    ST_CHECK(strcmp(meta->operator_name, "Night Shift") == 0 && meta->operator_source == GUI_META_OPERATOR_SESSION,
             "asset-less session operator not taken");
    ST_CHECK(strcmp(meta->session_file, ses_path) == 0, "asset-less session_file not recorded");
    ST_CHECK(strcmp(live->output_path, "/asset/less") == 0, "asset-less output_path not overlaid");

    /* 8. Boundaries: notes of exactly 8191 bytes (CRLF stored as LF) and the
     *    largest index are accepted; one byte more is refused below. */
    {
        /* 8190 bytes + CRLF: 8192 as sent, 8191 once CRLF is stored as LF. */
        size_t notes_cap = GUI_META_NOTES_CAP - 1;
        size_t extra_size = notes_cap + 128;
        char *extra = malloc(extra_size);
        if (extra) {
            int at = snprintf(extra, extra_size, ", \"index\": 9007199254740991, \"notes\": \"");
            memset(extra + at, 'n', notes_cap - 1);
            /* The bound is what is left of the buffer: a fortified snprintf
             * (macOS by default) traps on a bound larger than that. */
            size_t used = (size_t)at + notes_cap - 1;
            snprintf(extra + used, extra_size - used, "\\r\\n\"");
            char *text = st_asset_session("\"a_cap\"", extra);
            gui_session_clear();
            gui_capture_meta_init();
            ST_CHECK(text && st_write(ses_path, text), "cannot write the boundary session");
            applied = gui_session_apply_file(ses_path, live, err, sizeof(err));
            ST_CHECK(applied, "notes at the cap / the largest index rejected: %s", err);
            ST_CHECK(strlen(meta->notes) == notes_cap && meta->notes[notes_cap - 1] == '\n',
                     "notes at the cap stored wrong (len %zu)", strlen(meta->notes));
            ST_CHECK(strcmp(meta->index_text, "9007199254740991") == 0, "largest index stored wrong (%s)", meta->index_text);
            free(text);
        } else {
            s_failures++;
        }
        free(extra);
    }

    /* 9. Every bad file is refused whole and changes nothing -- not the
     *    settings, not the capture metadata. Start from a linked state so a
     *    partial commit would show. */
    gui_session_clear();
    gui_capture_meta_init();
    gui_settings_load(live);
    if (!st_write(ses_path, session_text)) s_failures++;
    applied = gui_session_apply_file(ses_path, live, err, sizeof(err));
    ST_CHECK(applied && meta->linked, "could not re-link before the refusals: %s", err);
    struct { const char *label; const char *asset_id; const char *extra; } bad_assets[] = {
        { "asset_id with a space",       "\"asset 01\"", "" },
        { "asset_id over 63 bytes",      "\"a123456789012345678901234567890123456789012345678901234567890123\"", "" },
        { "empty required field",        "\"\"", "" },
        { "index -1",                    "\"a1\"", ", \"index\": -1" },
        { "index 2.5",                   "\"a1\"", ", \"index\": 2.5" },
        { "index as a string",           "\"a1\"", ", \"index\": \"3\"" },
        { "index with a leading zero",   "\"a1\"", ", \"index\": 03" },
        { "index over 2^53-1",           "\"a1\"", ", \"index\": 9007199254740992" },
        { "hifi as a string",            "\"a1\"", ", \"hifi_audio_equipped\": \"true\"" },
        { "b&w as a number",             "\"a1\"", ", \"black_and_white\": 1" },
        { "raw invalid UTF-8",           "\"a1\"", ", \"tape_speed\": \"S\xff\"" },
        { "overlong UTF-8",              "\"a1\"", ", \"tape_speed\": \"\xc0\xaf\"" },
        { "duplicate key in asset",      "\"a1\"", ", \"label\": \"again\"" },
        { "control char in video_system", "\"a1\"", ", \"video_system\": \"NT\\u0007SC\"" },
        { "line break in a non-notes",   "\"a1\"", ", \"tape_speed\": \"S\\nP\"" },
        { "control char in notes",       "\"a1\"", ", \"notes\": \"bell \\u0007\"" },
        { "tape_speed over 31 bytes",    "\"a1\"", ", \"tape_speed\": \"0123456789012345678901234567890123\"" },
        { "string as an object",         "\"a1\"", ", \"notes\": {\"a\": 1}" },
        { "asset_id as a number",        "7", "" },
    };
    for (size_t i = 0; i < sizeof(bad_assets) / sizeof(bad_assets[0]); i++) {
        char *text = st_asset_session(bad_assets[i].asset_id, bad_assets[i].extra);
        st_expect_rejected(bad_assets[i].label, bad_path, text, live);
        free(text);
    }
    {
        /* notes one byte over the cap */
        size_t over = GUI_META_NOTES_CAP;
        size_t extra_size = over + 32;
        char *extra = malloc(extra_size);
        if (extra) {
            int at = snprintf(extra, extra_size, ", \"notes\": \"");
            memset(extra + at, 'n', over);
            size_t used = (size_t)at + over;
            snprintf(extra + used, extra_size - used, "\"");
            char *text = st_asset_session("\"a1\"", extra);
            st_expect_rejected("notes over the cap", bad_path, text, live);
            free(text);
            free(extra);
        }
    }
    st_expect_rejected("control char in label", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"asset\": {\"asset_id\": \"a\", \"client_id\": \"c1\", "
                       "\"client_name\": \"C\", \"display_name\": \"T\", \"label\": \"Ta\\u001bpe\", "
                       "\"format\": \"VHS\"}}", live);
    st_expect_rejected("missing asset_id", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"asset\": {\"client_id\": \"c1\", "
                       "\"client_name\": \"C\", \"display_name\": \"T\", \"label\": \"L\", \"format\": \"VHS\"}}", live);
    st_expect_rejected("missing display_name", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"asset\": {\"asset_id\": \"a\", \"client_id\": \"c1\", "
                       "\"client_name\": \"C\", \"label\": \"L\", \"format\": \"VHS\"}}", live);
    st_expect_rejected("asset not an object", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"asset\": \"a1\"}", live);
    st_expect_rejected("operator with a tab", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"operator\": \"a\\tb\"}", live);
    st_expect_rejected("duplicate top-level key", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"operator\": \"a\", \"operator\": \"b\"}", live);
    st_expect_rejected("unknown schema", bad_path,
                       "{\"schema\": \"misrc-gui.session/2\", \"output_path\": \"/x\"}", live);
    st_expect_rejected("missing schema", bad_path, "{\"output_path\": \"/x\"}", live);
    st_expect_rejected("bad JSON", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_path\": \"/x\"", live);
    st_expect_rejected("trailing text", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\"} x", live);
    st_expect_rejected("non-string output_path", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_path\": 5}", live);
    st_expect_rejected("quote in a setting value", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_base_name\": \"12\\\" reel\"}", live);
    st_expect_rejected("empty output_base_name", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_base_name\": \"\"}", live);
    remove(bad_path);
    st_expect_rejected("unreadable file", bad_path, NULL, live);

    /* 10. Cleared: saves are unfiltered again. */
    gui_session_clear();
    ST_CHECK(!gui_session_active(), "clear left a session active");

    remove(cfg_path);
    remove(ses_path);
    remove(bad_path);
    gui_settings_set_override_path(NULL);
    free(live);
    free(disk);
    if (s_failures) {
        printf("SESSION SELFTEST FAILED (%d check(s))\n", s_failures);
        return 1;
    }
    printf("session selftest passed\n");
    return 0;
}
