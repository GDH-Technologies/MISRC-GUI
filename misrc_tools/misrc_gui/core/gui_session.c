/*
 * MISRC GUI - Session launch file (--session <path.json>)
 *
 * See gui_session.h for the file format and the persistence rule.
 *
 * The settings file is flat, so the settings loader's key finder
 * (gui_settings_find_value) cannot tell a top-level key from one inside
 * "ingest" or "log_tags", and cannot list an object's keys at all. The small
 * reader below walks exactly the shape this file has: one object of strings
 * and two nested objects of strings. Every overlaid value is then stored
 * through gui_settings_apply_key() in strict mode, the same validation a
 * network /set gets, so a session can never put a value in memory that the
 * settings table would refuse.
 *
 * Licensed under GNU GPL v3 or later
 */

#include "gui_session.h"

#include <stdarg.h>
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

/* "ingest" member -> settings table key. Order is the log block's order. */
static const struct {
    const char *member;
    const char *setting;
} s_ingest_map[GUI_SESSION_INGEST_COUNT] = {
    { "project",        "ingest_project" },
    { "tape_id",        "ingest_tape_id" },
    { "tape_format",    "ingest_tape_format" },
    { "tape_size",      "ingest_tape_size" },
    { "tape_speed",     "ingest_tape_speed" },
    { "tape_condition", "ingest_tape_condition" },
    { "operator",       "ingest_operator" },
    { "location",       "ingest_location" },
    { "notes",          "ingest_notes" },
};

/* ============================================================================
 * Module state
 * ============================================================================ */
#define GUI_SESSION_MAX_OVERLAID (2 + GUI_SESSION_INGEST_COUNT)

static bool s_active = false;
static gui_settings_t *s_original = NULL;    /* the struct before the overlay */
static const char *s_overlaid[GUI_SESSION_MAX_OVERLAID];  /* table keys */
static size_t s_overlaid_count = 0;
static bool s_base_name_overlaid = false;
static size_t s_tag_count = 0;
static gui_session_tag_t s_tags[GUI_SESSION_MAX_TAGS];

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

static bool put_byte(char *out, size_t cap, size_t *len, unsigned char b, bool *too_long) {
    if (*len + 1 >= cap) { *too_long = true; return true; }
    out[(*len)++] = (char)b;
    return true;
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
 * *too_long when it did not fit; *has_ctrl when the DECODED text holds a
 * control character (a line break would split the capture log's line). */
static bool rd_string(reader_t *r, char *out, size_t cap, bool *too_long, bool *has_ctrl) {
    size_t len = 0;
    *too_long = false;
    *has_ctrl = false;
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
            if (c == 0x7F) *has_ctrl = true;
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
        if (cp < 0x20 || cp == 0x7F) *has_ctrl = true;
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
        bool tl, hc;
        return rd_string(r, scratch, sizeof(scratch), &tl, &hc);
    }
    if (c == '{' || c == '[') {
        char close = (c == '{') ? '}' : ']';
        r->p++;
        rd_ws(r);
        if (r->p < r->end && *r->p == close) { r->p++; return true; }
        for (;;) {
            if (c == '{') {
                char scratch[8];
                bool tl, hc;
                if (!rd_string(r, scratch, sizeof(scratch), &tl, &hc)) return false;
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
        bool too_long, has_ctrl;
        if (!rd_string(r, key, sizeof(key), &too_long, &has_ctrl)) return false;
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

/* A string-typed member, into out. where names it in errors. */
static bool rd_member_string(reader_t *r, const char *where, char *out, size_t cap) {
    rd_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        char msg[192];
        snprintf(msg, sizeof(msg), "%s must be a string", where);
        rd_fail(r, msg);
        return false;
    }
    bool too_long, has_ctrl;
    if (!rd_string(r, out, cap, &too_long, &has_ctrl)) return false;
    char msg[192];
    if (too_long) {
        snprintf(msg, sizeof(msg), "%s is longer than %u bytes", where, (unsigned)(cap - 1));
        rd_fail(r, msg);
        return false;
    }
    if (has_ctrl) {
        snprintf(msg, sizeof(msg), "%s may not contain a control character (line break, tab, ...)", where);
        rd_fail(r, msg);
        return false;
    }
    return true;
}

typedef struct {
    gui_session_data_t *data;
    bool has_schema;
} parse_ctx_t;

static bool on_ingest_member(reader_t *r, const char *key, void *vctx) {
    parse_ctx_t *ctx = (parse_ctx_t *)vctx;
    for (size_t i = 0; i < GUI_SESSION_INGEST_COUNT; i++) {
        if (strcmp(key, s_ingest_map[i].member) != 0) continue;
        char where[160];
        snprintf(where, sizeof(where), "ingest.%s", key);
        if (!rd_member_string(r, where, ctx->data->ingest[i], sizeof(ctx->data->ingest[i]))) return false;
        ctx->data->has_ingest[i] = true;
        return true;
    }
    fprintf(stderr, "[SESSION] ignoring unknown key ingest.%s\n", key);
    return rd_skip_value(r, 1);
}

static bool tag_key_ok(const char *k) {
    size_t n = strlen(k);
    if (n == 0 || n >= GUI_SESSION_TAG_KEY_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = k[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static bool on_tag_member(reader_t *r, const char *key, void *vctx) {
    parse_ctx_t *ctx = (parse_ctx_t *)vctx;
    gui_session_data_t *d = ctx->data;
    char msg[224];
    if (!tag_key_ok(key)) {
        snprintf(msg, sizeof(msg),
                 "log_tags key \"%.80s\" must be 1-%d characters of A-Z a-z 0-9 _ . -",
                 key, GUI_SESSION_TAG_KEY_MAX - 1);
        rd_fail(r, msg);
        return false;
    }
    for (size_t i = 0; i < d->tag_count; i++) {
        if (strcmp(d->tags[i].key, key) == 0) {
            snprintf(msg, sizeof(msg), "log_tags key \"%s\" appears twice", key);
            rd_fail(r, msg);
            return false;
        }
    }
    if (d->tag_count >= GUI_SESSION_MAX_TAGS) {
        snprintf(msg, sizeof(msg), "more than %d log_tags", GUI_SESSION_MAX_TAGS);
        rd_fail(r, msg);
        return false;
    }
    gui_session_tag_t *t = &d->tags[d->tag_count];
    snprintf(msg, sizeof(msg), "log_tags.%s", key);
    if (!rd_member_string(r, msg, t->value, sizeof(t->value))) return false;
    snprintf(t->key, sizeof(t->key), "%s", key);
    d->tag_count++;
    return true;
}

static bool on_top_member(reader_t *r, const char *key, void *vctx) {
    parse_ctx_t *ctx = (parse_ctx_t *)vctx;
    gui_session_data_t *d = ctx->data;
    if (strcmp(key, "schema") == 0) {
        char schema[96];
        if (!rd_member_string(r, "schema", schema, sizeof(schema))) return false;
        if (strcmp(schema, GUI_SESSION_SCHEMA) != 0) {
            char msg[192];
            snprintf(msg, sizeof(msg), "unsupported schema \"%s\" (this build reads \"%s\")",
                     schema, GUI_SESSION_SCHEMA);
            rd_fail(r, msg);
            return false;
        }
        ctx->has_schema = true;
        return true;
    }
    if (strcmp(key, "output_path") == 0) {
        if (!rd_member_string(r, "output_path", d->output_path, sizeof(d->output_path))) return false;
        d->has_output_path = true;
        return true;
    }
    if (strcmp(key, "output_base_name") == 0) {
        if (!rd_member_string(r, "output_base_name", d->output_base_name, sizeof(d->output_base_name))) return false;
        d->has_output_base_name = true;
        return true;
    }
    if (strcmp(key, "ingest") == 0) {
        rd_ws(r);
        if (r->p >= r->end || *r->p != '{') { rd_fail(r, "ingest must be an object"); return false; }
        return rd_object(r, on_ingest_member, ctx);
    }
    if (strcmp(key, "log_tags") == 0) {
        rd_ws(r);
        if (r->p >= r->end || *r->p != '{') { rd_fail(r, "log_tags must be an object"); return false; }
        return rd_object(r, on_tag_member, ctx);
    }
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
    reader_t r = { text, text, text + strlen(text), err, errcap };
    /* A UTF-8 byte order mark is not JSON, but editors write one. */
    if (r.end - r.p >= 3 && (unsigned char)r.p[0] == 0xEF &&
        (unsigned char)r.p[1] == 0xBB && (unsigned char)r.p[2] == 0xBF) {
        r.p += 3;
    }
    parse_ctx_t ctx = { out, false };
    rd_ws(&r);
    if (r.p >= r.end || *r.p != '{') {
        rd_fail(&r, "not a JSON object");
        memset(out, 0, sizeof(*out));
        return false;
    }
    if (!rd_object(&r, on_top_member, &ctx)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    rd_ws(&r);
    if (r.p != r.end) {
        rd_fail(&r, "trailing text after the JSON object");
        memset(out, 0, sizeof(*out));
        return false;
    }
    if (!ctx.has_schema) {
        set_err(err, errcap, "missing \"schema\" (expected \"%s\")", GUI_SESSION_SCHEMA);
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
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
    s_tag_count = 0;
    memset(s_tags, 0, sizeof(s_tags));
}

bool gui_session_apply(const gui_session_data_t *data, gui_settings_t *settings,
                       char *err, size_t errcap) {
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
     * live settings exactly as loaded. */
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

    /* Slot 0 output_path, 1 output_base_name, 2.. the ingest map. */
    for (size_t i = 0; i < GUI_SESSION_MAX_OVERLAID; i++) {
        bool present;
        const char *setting, *value, *name;
        if (i == 0) {
            present = data->has_output_path;
            setting = name = "output_path";
            value = data->output_path;
        } else if (i == 1) {
            present = data->has_output_base_name;
            setting = name = "output_base_name";
            value = data->output_base_name;
        } else {
            present = data->has_ingest[i - 2];
            setting = s_ingest_map[i - 2].setting;
            name = s_ingest_map[i - 2].member;
            value = data->ingest[i - 2];
        }
        if (!present) continue;
        const gui_setting_desc_t *d = gui_settings_find(setting);
        if (!d || gui_settings_apply_key(next, setting, value, true, why, sizeof(why)) != 0) {
            set_err(err, errcap, "%s%s: %s", (i >= 2) ? "ingest." : "", name,
                    (d && why[0]) ? why : "refused");
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

    /* Commit. */
    gui_session_clear();
    *settings = *next;
    free(next);
    s_original = orig;
    memcpy(s_overlaid, overlaid, n_overlaid * sizeof(overlaid[0]));
    s_overlaid_count = n_overlaid;
    s_base_name_overlaid = data->has_output_base_name;
    s_tag_count = data->tag_count;
    memcpy(s_tags, data->tags, data->tag_count * sizeof(data->tags[0]));
    s_active = true;
    gui_settings_set_persist_filter(session_persist_filter);
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
              gui_session_apply(data, settings, err, errcap);
    free(data);
    free(text);
    return ok;
}

bool gui_session_active(void) {
    return s_active;
}

size_t gui_session_tag_count(void) {
    return s_active ? s_tag_count : 0;
}

const gui_session_tag_t *gui_session_tag_at(size_t index) {
    if (!s_active || index >= s_tag_count) return NULL;
    return &s_tags[index];
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

/* A rejected file must leave the settings and the module untouched. */
static void st_expect_rejected(const char *label, const char *path, const char *text,
                               gui_settings_t *settings) {
    if (text && !st_write(path, text)) {
        printf("FAIL: cannot write scratch session file %s\n", path);
        s_failures++;
        return;
    }
    gui_settings_t *before = malloc(sizeof(*before));
    if (!before) { s_failures++; return; }
    *before = *settings;
    char err[256];
    bool ok = gui_session_apply_file(path, settings, err, sizeof(err));
    ST_CHECK(!ok, "%s: accepted, must be rejected", label);
    ST_CHECK(err[0] != '\0', "%s: rejected without a reason", label);
    ST_CHECK(memcmp(before, settings, sizeof(*before)) == 0, "%s: a rejected file changed the settings", label);
    ST_CHECK(!gui_session_active(), "%s: a rejected file activated a session", label);
    if (!ok) printf("  rejected %-28s -> %s\n", label, err);
    free(before);
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

    /* 1. The operator's own settings, unlike any session value. Manual file
     *    names (auto naming off) are the hard case for the base-name rule. */
    gui_settings_init_defaults(live);
    snprintf(live->output_path, sizeof(live->output_path), "/operator/captures");
    snprintf(live->output_base_name, sizeof(live->output_base_name), "OperatorBase");
    live->auto_names_enabled = false;
    snprintf(live->output_filename_a, sizeof(live->output_filename_a), "manual_a.flac");
    snprintf(live->output_filename_b, sizeof(live->output_filename_b), "manual_b.flac");
    snprintf(live->ingest_project, sizeof(live->ingest_project), "Operator Project");
    snprintf(live->ingest_operator, sizeof(live->ingest_operator), "Operator Name");
    snprintf(live->ingest_notes, sizeof(live->ingest_notes), "operator notes");
    live->ingest_tape_id[0] = '\0';
    live->ingest_location[0] = '\0';
    gui_settings_save(live);

    /* 2. Load them back the way main() does, then apply a session. */
    memset(live, 0, sizeof(*live));
    gui_settings_load(live);
    ST_CHECK(strcmp(live->output_path, "/operator/captures") == 0, "baseline did not round-trip (output_path=%s)", live->output_path);
    ST_CHECK(!live->auto_names_enabled, "baseline auto_names_enabled did not round-trip");

    const char *session_text =
        "\xEF\xBB\xBF{\n"
        "  \"schema\": \"misrc-gui.session/1\",\n"
        "  \"output_path\": \"/session/out\",\n"
        "  \"output_base_name\": \"Session_Tape\",\n"
        "  \"future_key\": {\"nested\": [1, 2.5, true, null, \"x\"]},\n"
        "  \"ingest\": {\"project\": \"Session Project\", \"tape_id\": \"T-001\",\n"
        "             \"tape_format\": \"VHS\", \"tape_speed\": \"SP\",\n"
        "             \"notes\": \"caf\\u00e9 \\/ \\\\ \\ud83d\\udcfc\", \"not_a_field\": 3},\n"
        "  \"log_tags\": {\"asset_id\": \"a_019\", \"output_path\": \"tag, not a setting\",\n"
        "               \"hifi.equipped\": \"false\"}\n"
        "}\n";
    if (!st_write(ses_path, session_text)) {
        printf("FAIL: cannot write %s\n", ses_path);
        s_failures++;
    }
    char err[256];
    bool applied = gui_session_apply_file(ses_path, live, err, sizeof(err));
    ST_CHECK(applied, "valid session rejected: %s", err);
    ST_CHECK(gui_session_active(), "session not active after apply");

    /* 3. The overlay is what is in memory. */
    ST_CHECK(strcmp(live->output_path, "/session/out") == 0, "output_path not overlaid (%s)", live->output_path);
    ST_CHECK(strcmp(live->output_base_name, "Session_Tape") == 0, "output_base_name not overlaid (%s)", live->output_base_name);
    ST_CHECK(live->auto_names_enabled, "auto naming not turned on for the session's base name");
    ST_CHECK(strstr(live->output_filename_a, "Session_Tape") != NULL, "derived name ignores the base name (%s)", live->output_filename_a);
    ST_CHECK(strcmp(live->ingest_project, "Session Project") == 0, "ingest.project not overlaid (%s)", live->ingest_project);
    ST_CHECK(strcmp(live->ingest_tape_id, "T-001") == 0, "ingest.tape_id not overlaid");
    ST_CHECK(strcmp(live->ingest_tape_format, "VHS") == 0, "ingest.tape_format not overlaid");
    ST_CHECK(strcmp(live->ingest_tape_speed, "SP") == 0, "ingest.tape_speed not overlaid");
    ST_CHECK(strcmp(live->ingest_notes, "caf\xC3\xA9 / \\ \xF0\x9F\x93\xBC") == 0, "ingest.notes escapes decoded wrong (%s)", live->ingest_notes);
    ST_CHECK(strcmp(live->ingest_operator, "Operator Name") == 0, "an absent ingest key was overlaid");
    ST_CHECK(gui_session_tag_count() == 3, "expected 3 log tags, got %u", (unsigned)gui_session_tag_count());
    {
        const gui_session_tag_t *t0 = gui_session_tag_at(0);
        const gui_session_tag_t *t1 = gui_session_tag_at(1);
        const gui_session_tag_t *t2 = gui_session_tag_at(2);
        ST_CHECK(t0 && strcmp(t0->key, "asset_id") == 0 && strcmp(t0->value, "a_019") == 0, "log tag 0 wrong");
        ST_CHECK(t1 && strcmp(t1->key, "output_path") == 0, "log tag 1 out of order");
        ST_CHECK(t2 && strcmp(t2->key, "hifi.equipped") == 0 && strcmp(t2->value, "false") == 0, "log tag 2 wrong");
    }
    printf("  overlay: output_path=%s base=%s rfA=%s tags=%u\n",
           live->output_path, live->output_base_name, live->output_filename_a,
           (unsigned)gui_session_tag_count());

    /* 4. The operator edits during the session: one overlaid field (session-
     *    scoped, must NOT persist) and one ordinary field (must persist). */
    snprintf(live->ingest_notes, sizeof(live->ingest_notes), "edited during the session");
    snprintf(live->ingest_location, sizeof(live->ingest_location), "Bench 2");
    gui_settings_save(live);
    ST_CHECK(strcmp(live->output_path, "/session/out") == 0, "saving changed the overlay in memory");

    /* 5. The file holds the operator's values for every overlaid key. */
    gui_settings_load(disk);
    ST_CHECK(strcmp(disk->output_path, "/operator/captures") == 0, "output_path persisted the overlay (%s)", disk->output_path);
    ST_CHECK(strcmp(disk->output_base_name, "OperatorBase") == 0, "output_base_name persisted the overlay (%s)", disk->output_base_name);
    ST_CHECK(!disk->auto_names_enabled, "auto_names_enabled persisted the session's forced on");
    ST_CHECK(strcmp(disk->output_filename_a, "manual_a.flac") == 0, "manual RF A name lost (%s)", disk->output_filename_a);
    ST_CHECK(strcmp(disk->output_filename_b, "manual_b.flac") == 0, "manual RF B name lost (%s)", disk->output_filename_b);
    ST_CHECK(strcmp(disk->ingest_project, "Operator Project") == 0, "ingest_project persisted the overlay (%s)", disk->ingest_project);
    ST_CHECK(disk->ingest_tape_id[0] == '\0', "ingest_tape_id persisted the overlay (%s)", disk->ingest_tape_id);
    ST_CHECK(disk->ingest_tape_format[0] == '\0', "ingest_tape_format persisted the overlay (%s)", disk->ingest_tape_format);
    ST_CHECK(strcmp(disk->ingest_notes, "operator notes") == 0, "an overlaid field's in-session edit persisted (%s)", disk->ingest_notes);
    ST_CHECK(strcmp(disk->ingest_location, "Bench 2") == 0, "an ordinary field's edit did not persist (%s)", disk->ingest_location);
    char *raw = st_read(cfg_path);
    ST_CHECK(raw != NULL, "cannot read back %s", cfg_path);
    if (raw) {
        ST_CHECK(strstr(raw, "Session_Tape") == NULL, "the session base name leaked into the file");
        ST_CHECK(strstr(raw, "/session/out") == NULL, "the session output_path leaked into the file");
        ST_CHECK(strstr(raw, "Session Project") == NULL, "the session ingest value leaked into the file");
        free(raw);
    }
    printf("  on disk: output_path=%s base=%s auto_names=%d rfA=%s notes=%s location=%s\n",
           disk->output_path, disk->output_base_name, (int)disk->auto_names_enabled,
           disk->output_filename_a, disk->ingest_notes, disk->ingest_location);

    /* 6. With auto naming on in the operator's file, the restored derived
     *    names come from the operator's base name, not the session's. */
    gui_session_clear();
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
    gui_session_clear();

    /* 7. Every bad file is refused whole, and changes nothing. */
    gui_settings_load(live);
    st_expect_rejected("unknown schema", bad_path,
                       "{\"schema\": \"misrc-gui.session/2\", \"output_path\": \"/x\"}", live);
    st_expect_rejected("missing schema", bad_path, "{\"output_path\": \"/x\"}", live);
    st_expect_rejected("bad JSON", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_path\": \"/x\"", live);
    st_expect_rejected("trailing text", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\"} x", live);
    st_expect_rejected("non-string value", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_path\": 5}", live);
    st_expect_rejected("line break in a value", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"ingest\": {\"notes\": \"a\\nb\"}}", live);
    st_expect_rejected("quote in a setting value", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"ingest\": {\"notes\": \"12\\\" reel\"}}", live);
    st_expect_rejected("over-long ingest value", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"ingest\": {\"project\": "
                       "\"0123456789012345678901234567890123456789012345678901234567890123"
                       "4567890123456789012345678901234567890123456789012345678901234567890\"}}", live);
    st_expect_rejected("bad log tag key", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"log_tags\": {\"a b\": \"x\"}}", live);
    st_expect_rejected("duplicate log tag", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"log_tags\": {\"a\": \"x\", \"a\": \"y\"}}", live);
    st_expect_rejected("empty output_base_name", bad_path,
                       "{\"schema\": \"misrc-gui.session/1\", \"output_base_name\": \"\"}", live);
    remove(bad_path);
    st_expect_rejected("unreadable file", bad_path, NULL, live);

    /* 8. No session: saves are unfiltered again. */
    ST_CHECK(!gui_session_active() && gui_session_tag_count() == 0, "clear left a session active");

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
