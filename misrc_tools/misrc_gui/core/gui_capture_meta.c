/*
 * MISRC GUI - Capture metadata. See gui_capture_meta.h.
 *
 * Licensed under GNU GPL v3 or later
 */

#include "gui_capture_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
/* %USERNAME% only: no Win32 headers in this raylib-free file. */
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define META_STR_FIELD(k, lbl, member, fl) \
    { k, lbl, GUI_META_STR, offsetof(gui_capture_meta_t, member), \
      sizeof(((gui_capture_meta_t *)0)->member), fl }

/* The one order. The guard suite pins it; the log block, the sidecar's asset
 * object, the panel's rows and the session parser all walk it. */
static const gui_capture_meta_field_t s_fields[] = {
    META_STR_FIELD("client_name",  "Client",       client_name,
                   GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE),
    META_STR_FIELD("display_name", "Title",        display_name,
                   GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE),
    { "index", "Index", GUI_META_INDEX, offsetof(gui_capture_meta_t, index_text),
      sizeof(((gui_capture_meta_t *)0)->index_text), GUI_META_F_UNLINKED_EDITABLE },
    META_STR_FIELD("label",        "Label",        label,
                   GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE),
    META_STR_FIELD("format",       "Format",       format,
                   GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE),
    META_STR_FIELD("tape_speed",   "Tape speed",   tape_speed,   GUI_META_F_UNLINKED_EDITABLE),
    META_STR_FIELD("video_system", "Video system", video_system, GUI_META_F_UNLINKED_EDITABLE),
    { "hifi_audio_equipped", "HiFi audio", GUI_META_TRI,
      offsetof(gui_capture_meta_t, hifi_audio_equipped), 0, GUI_META_F_UNLINKED_EDITABLE },
    { "black_and_white", "Black & white", GUI_META_TRI,
      offsetof(gui_capture_meta_t, black_and_white), 0, GUI_META_F_UNLINKED_EDITABLE },
    META_STR_FIELD("notes",        "Notes",        notes,
                   GUI_META_F_UNLINKED_EDITABLE | GUI_META_F_MULTILINE),
    META_STR_FIELD("asset_id",     "Asset ID",     asset_id,
                   GUI_META_F_ID | GUI_META_F_REQUIRED_LINKED),
    META_STR_FIELD("client_id",    "Client ID",    client_id,
                   GUI_META_F_ID | GUI_META_F_REQUIRED_LINKED),
    META_STR_FIELD("operator",     "Operator",     operator_name, GUI_META_F_TOP_LEVEL),
};

#define META_FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))

/* Main thread only. */
static gui_capture_meta_t s_meta;
static bool s_used_since_edit = false;

const gui_capture_meta_field_t *gui_capture_meta_fields(size_t *count) {
    if (count) *count = META_FIELD_COUNT;
    return s_fields;
}

const gui_capture_meta_field_t *gui_capture_meta_find_field(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < META_FIELD_COUNT; i++) {
        if (strcmp(s_fields[i].key, key) == 0) return &s_fields[i];
    }
    return NULL;
}

const char *gui_capture_meta_str(const gui_capture_meta_t *m, const gui_capture_meta_field_t *f) {
    if (!m || !f || f->type == GUI_META_TRI) return "";
    return (const char *)m + f->offset;
}

char *gui_capture_meta_str_mut(gui_capture_meta_t *m, const gui_capture_meta_field_t *f) {
    if (!m || !f || f->type == GUI_META_TRI) return NULL;
    return (char *)m + f->offset;
}

int8_t gui_capture_meta_tri(const gui_capture_meta_t *m, const gui_capture_meta_field_t *f) {
    if (!m || !f || f->type != GUI_META_TRI) return GUI_META_TRI_UNSET;
    int8_t v;
    memcpy(&v, (const char *)m + f->offset, sizeof(v));
    return (v == GUI_META_TRI_TRUE || v == GUI_META_TRI_FALSE) ? v : GUI_META_TRI_UNSET;
}

bool gui_capture_meta_index_value(const gui_capture_meta_t *m, uint64_t *out) {
    if (!m || !m->index_text[0]) return false;
    uint64_t v = 0;
    for (const char *p = m->index_text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        unsigned d = (unsigned)(*p - '0');
        if (v > (GUI_META_INDEX_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    if (out) *out = v;
    return true;
}

void gui_capture_meta_os_login(char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    dst[0] = '\0';
    const char *name = NULL;
#if defined(_WIN32) || defined(_WIN64)
    name = getenv("USERNAME");
#else
    struct passwd *pw = getpwuid(geteuid());
    if (pw && pw->pw_name && pw->pw_name[0]) name = pw->pw_name;
    if (!name || !name[0]) name = getenv("USER");
    if (!name || !name[0]) name = getenv("LOGNAME");
#endif
    if (!name || !name[0]) name = "unknown";
    snprintf(dst, cap, "%s", name);
}

void gui_capture_meta_init(void) {
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.hifi_audio_equipped = GUI_META_TRI_UNSET;
    s_meta.black_and_white = GUI_META_TRI_UNSET;
    gui_capture_meta_os_login(s_meta.operator_name, sizeof(s_meta.operator_name));
    s_meta.operator_source = GUI_META_OPERATOR_OS_LOGIN;
    static uint32_t s_init_generation = 0;
    s_meta.generation = ++s_init_generation;
    s_used_since_edit = false;
}

const gui_capture_meta_t *gui_capture_meta_get(void) {
    return &s_meta;
}

void gui_capture_meta_snapshot(gui_capture_meta_t *out) {
    if (out) *out = s_meta;
}

static void meta_set_operator(gui_capture_meta_t *m, const char *operator_name) {
    if (operator_name && operator_name[0]) {
        snprintf(m->operator_name, sizeof(m->operator_name), "%s", operator_name);
        m->operator_source = GUI_META_OPERATOR_SESSION;
    } else {
        gui_capture_meta_os_login(m->operator_name, sizeof(m->operator_name));
        m->operator_source = GUI_META_OPERATOR_OS_LOGIN;
    }
}

void gui_capture_meta_set_linked(const gui_capture_meta_t *src, const char *session_file) {
    if (!src) return;
    uint32_t gen = s_meta.generation;
    for (size_t i = 0; i < META_FIELD_COUNT; i++) {
        const gui_capture_meta_field_t *f = &s_fields[i];
        if (f->flags & GUI_META_F_TOP_LEVEL) continue;  /* operator, below */
        if (f->type == GUI_META_TRI) {
            memcpy((char *)&s_meta + f->offset, (const char *)src + f->offset, sizeof(int8_t));
        } else {
            snprintf((char *)&s_meta + f->offset, f->cap, "%s", (const char *)src + f->offset);
        }
    }
    meta_set_operator(&s_meta, src->operator_name);
    snprintf(s_meta.session_file, sizeof(s_meta.session_file), "%s", session_file ? session_file : "");
    s_meta.linked = true;
    s_meta.generation = gen + 1;
    s_used_since_edit = false;
}

void gui_capture_meta_set_operator(const char *operator_name) {
    meta_set_operator(&s_meta, operator_name);
    s_meta.generation++;
}

static bool slot_editable(size_t slot, const gui_capture_meta_field_t **out) {
    if (s_meta.linked || slot >= META_FIELD_COUNT) return false;
    const gui_capture_meta_field_t *f = &s_fields[slot];
    if (!(f->flags & GUI_META_F_UNLINKED_EDITABLE)) return false;
    *out = f;
    return true;
}

bool gui_capture_meta_text_buffer(size_t slot, char **dst, size_t *cap) {
    const gui_capture_meta_field_t *f = NULL;
    if (!slot_editable(slot, &f) || f->type == GUI_META_TRI) return false;
    if (dst) *dst = (char *)&s_meta + f->offset;
    if (cap) *cap = f->cap;
    return true;
}

bool gui_capture_meta_set_tri(size_t slot, int8_t value) {
    const gui_capture_meta_field_t *f = NULL;
    if (!slot_editable(slot, &f) || f->type != GUI_META_TRI) return false;
    if (value != GUI_META_TRI_TRUE && value != GUI_META_TRI_FALSE) value = GUI_META_TRI_UNSET;
    memcpy((char *)&s_meta + f->offset, &value, sizeof(value));
    gui_capture_meta_touch();
    return true;
}

void gui_capture_meta_touch(void) {
    s_meta.generation++;
    s_used_since_edit = false;
}

void gui_capture_meta_mark_used(void) {
    s_used_since_edit = true;
}

bool gui_capture_meta_used_since_edit(void) {
    return s_used_since_edit;
}

const char *gui_capture_meta_operator_source_name(gui_meta_operator_source_t src) {
    return (src == GUI_META_OPERATOR_SESSION) ? "session" : "os_login";
}

/* ============================================================================
 * Text helpers
 * ============================================================================ */

size_t gui_capture_meta_log_escape(const char *in, char *out, size_t cap) {
    size_t need = 0;   /* bytes the full escape takes */
    size_t len = 0;    /* bytes written to out */
    bool full = false;
    if (out && cap) out[0] = '\0';
    if (!in) return 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char piece[5];
        size_t n;
        switch (*p) {
            case '\\': piece[0] = '\\'; piece[1] = '\\'; n = 2; break;
            case '\n': piece[0] = '\\'; piece[1] = 'n';  n = 2; break;
            case '\r': piece[0] = '\\'; piece[1] = 'r';  n = 2; break;
            case '\t': piece[0] = '\\'; piece[1] = 't';  n = 2; break;
            default:
                if (*p < 0x20 || *p == 0x7F) {
                    static const char hex[] = "0123456789abcdef";
                    piece[0] = '\\'; piece[1] = 'x';
                    piece[2] = hex[*p >> 4]; piece[3] = hex[*p & 0xF];
                    n = 4;
                } else {
                    piece[0] = (char)*p;
                    n = 1;
                }
                break;
        }
        need += n;
        /* Whole pieces only, so a truncated line never ends in half an escape. */
        if (!full && out && cap && len + n < cap) {
            memcpy(out + len, piece, n);
            len += n;
        } else {
            full = true;
        }
    }
    if (out && cap) out[len] = '\0';
    return need;
}

bool gui_capture_meta_utf8_valid(const char *s, size_t len) {
    if (!s) return len == 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        if (c < 0x80) { i++; continue; }
        size_t n;
        unsigned cp, min;
        if (c >= 0xC2 && c <= 0xDF)      { n = 1; cp = c & 0x1F; min = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; min = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; min = 0x10000; }
        else return false;  /* stray continuation, C0/C1 overlong lead, F5+ */
        if (len - i <= n) return false;
        for (size_t k = 1; k <= n; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += n + 1;
    }
    return true;
}

size_t gui_capture_meta_format_log_value(const gui_capture_meta_t *m,
                                         const gui_capture_meta_field_t *f,
                                         char *out, size_t cap) {
    const char *literal = NULL;
    char num[24];
    if (!m || !f) {
        literal = "(unset)";
    } else if (f->type == GUI_META_TRI) {
        int8_t v = gui_capture_meta_tri(m, f);
        literal = (v == GUI_META_TRI_TRUE) ? "true" : (v == GUI_META_TRI_FALSE) ? "false" : "(unset)";
    } else if (f->type == GUI_META_INDEX) {
        uint64_t v;
        if (gui_capture_meta_index_value(m, &v)) {
            snprintf(num, sizeof(num), "%llu", (unsigned long long)v);
            literal = num;
        } else {
            literal = "(unset)";
        }
    } else {
        const char *s = gui_capture_meta_str(m, f);
        if (!s[0]) literal = "(empty)";
        else return gui_capture_meta_log_escape(s, out, cap);
    }
    if (out && cap) snprintf(out, cap, "%s", literal);
    return strlen(literal);
}
