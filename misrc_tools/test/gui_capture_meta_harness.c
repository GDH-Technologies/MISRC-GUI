/*
 * CPU-only checks for the capture metadata store (core/gui_capture_meta.c):
 * the descriptor table (order, offsets, caps, flags), the capture-log escape
 * (it must round-trip, so a notes field never splits a log line and can be
 * read back), the strict UTF-8 validator, and the linked/unlinked editing
 * rules the panel relies on. Run by meson (`meson test -C build-local`).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misrc_gui/core/gui_capture_meta.h"

static int checks;
static int failures;

static void expect(bool cond, const char *what)
{
    checks++;
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* The inverse of gui_capture_meta_log_escape, for the round trip. */
static bool unescape(const char *in, char *out, size_t cap)
{
    size_t len = 0;
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\') {
            p++;
            switch (*p) {
                case '\\': c = '\\'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'x': {
                    unsigned v = 0;
                    for (int k = 0; k < 2; k++) {
                        char h = *++p;
                        if (h >= '0' && h <= '9') v = v * 16 + (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v = v * 16 + (unsigned)(h - 'a' + 10);
                        else return false;
                    }
                    c = (unsigned char)v;
                    break;
                }
                default: return false;
            }
        }
        if (len + 1 >= cap) return false;
        out[len++] = (char)c;
    }
    out[len] = '\0';
    return true;
}

static void check_descriptors(void)
{
    static const char *const order[] = {
        "client_name", "display_name", "index", "label", "format", "tape_speed",
        "video_system", "hifi_audio_equipped", "black_and_white", "notes",
        "asset_id", "client_id", "operator",
    };
    size_t n = 0;
    const gui_capture_meta_field_t *f = gui_capture_meta_fields(&n);
    expect(n == sizeof(order) / sizeof(order[0]), "descriptor count is 13");
    for (size_t i = 0; i < n && i < sizeof(order) / sizeof(order[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "descriptor %zu is %s", i, order[i]);
        expect(strcmp(f[i].key, order[i]) == 0, what);
        expect(gui_capture_meta_find_field(order[i]) == &f[i], "find_field returns the row");
    }

    const gui_capture_meta_field_t *d;
#define EXPECT_STR(key, member, cap_bytes, fl) do { \
        d = gui_capture_meta_find_field(key); \
        expect(d && d->type == GUI_META_STR, key " is a string"); \
        expect(d && d->offset == offsetof(gui_capture_meta_t, member), key " offset"); \
        expect(d && d->cap == (cap_bytes), key " cap"); \
        expect(d && d->flags == (fl), key " flags"); \
    } while (0)
    EXPECT_STR("client_name",  client_name,   256, GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE);
    EXPECT_STR("display_name", display_name,  256, GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE);
    EXPECT_STR("label",        label,         256, GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE);
    EXPECT_STR("format",       format,         32, GUI_META_F_REQUIRED_LINKED | GUI_META_F_UNLINKED_EDITABLE);
    EXPECT_STR("tape_speed",   tape_speed,     32, GUI_META_F_UNLINKED_EDITABLE);
    EXPECT_STR("video_system", video_system,   32, GUI_META_F_UNLINKED_EDITABLE);
    EXPECT_STR("notes",        notes,        8192, GUI_META_F_UNLINKED_EDITABLE | GUI_META_F_MULTILINE);
    EXPECT_STR("asset_id",     asset_id,       64, GUI_META_F_ID | GUI_META_F_REQUIRED_LINKED);
    EXPECT_STR("client_id",    client_id,      64, GUI_META_F_ID | GUI_META_F_REQUIRED_LINKED);
    EXPECT_STR("operator",     operator_name, 128, GUI_META_F_TOP_LEVEL);
#undef EXPECT_STR
    d = gui_capture_meta_find_field("index");
    expect(d && d->type == GUI_META_INDEX && d->offset == offsetof(gui_capture_meta_t, index_text) &&
           d->cap == GUI_META_INDEX_CAP, "index is the INDEX row over index_text");
    d = gui_capture_meta_find_field("hifi_audio_equipped");
    expect(d && d->type == GUI_META_TRI && d->offset == offsetof(gui_capture_meta_t, hifi_audio_equipped),
           "hifi_audio_equipped is TRI");
    d = gui_capture_meta_find_field("black_and_white");
    expect(d && d->type == GUI_META_TRI && d->offset == offsetof(gui_capture_meta_t, black_and_white),
           "black_and_white is TRI");
    expect(gui_capture_meta_find_field("tape_id") == NULL, "no retired ingest key");
}

static void check_escape(void)
{
    const char *cases[] = {
        "plain",
        "",
        "line one\nline two\r\nthree\tTAB",
        "back\\slash \\n literal",
        "ctrl \x01\x1f del \x7f end",
        "caf\xc3\xa9 \"quoted\" \xf0\x9f\x93\xbc",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char esc[256], back[256];
        size_t need = gui_capture_meta_log_escape(cases[i], esc, sizeof(esc));
        expect(need == strlen(esc), "escape reports its length");
        expect(strchr(esc, '\n') == NULL && strchr(esc, '\r') == NULL && strchr(esc, '\t') == NULL,
               "escaped text is one line with no tabs");
        for (const unsigned char *p = (const unsigned char *)esc; *p; p++) {
            if (*p < 0x20 || *p == 0x7F) { expect(false, "no control byte survives the escape"); break; }
        }
        expect(unescape(esc, back, sizeof(back)) && strcmp(back, cases[i]) == 0, "escape round-trips");
    }
    char esc[64];
    gui_capture_meta_log_escape("a\nb\\c\x02", esc, sizeof(esc));
    expect(strcmp(esc, "a\\nb\\\\c\\x02") == 0, "escape spelling");
    /* Truncation never splits an escape. */
    char small[4];
    size_t need = gui_capture_meta_log_escape("ab\n", small, sizeof(small));
    expect(need == 4 && strcmp(small, "ab") == 0, "truncation keeps whole escapes");
}

static void check_utf8(void)
{
    struct { const char *s; bool ok; } cases[] = {
        { "ascii", true },
        { "caf\xc3\xa9", true },
        { "\xe2\x82\xac", true },              /* U+20AC */
        { "\xf0\x9f\x93\xbc", true },          /* U+1F4FC */
        { "\xf4\x8f\xbf\xbf", true },          /* U+10FFFF */
        { "\xc3", false },                     /* truncated */
        { "\xc0\xaf", false },                 /* overlong '/' */
        { "\xe0\x80\xaf", false },             /* overlong */
        { "\xed\xa0\x80", false },             /* surrogate D800 */
        { "\xf4\x90\x80\x80", false },         /* > U+10FFFF */
        { "\x80", false },                     /* stray continuation */
        { "\xff", false },
        { "ok\xe2\x82", false },               /* truncated at the end */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char what[64];
        snprintf(what, sizeof(what), "utf8 case %zu is %s", i, cases[i].ok ? "valid" : "invalid");
        expect(gui_capture_meta_utf8_valid(cases[i].s, strlen(cases[i].s)) == cases[i].ok, what);
    }
}

static void check_store(void)
{
    size_t n = 0;
    const gui_capture_meta_field_t *f = gui_capture_meta_fields(&n);
    gui_capture_meta_init();
    const gui_capture_meta_t *m = gui_capture_meta_get();
    expect(!m->linked, "init: unlinked");
    expect(m->operator_name[0] != '\0', "init: operator is the OS login");
    expect(m->operator_source == GUI_META_OPERATOR_OS_LOGIN, "init: operator_source os_login");
    expect(m->hifi_audio_equipped == GUI_META_TRI_UNSET && m->black_and_white == GUI_META_TRI_UNSET,
           "init: tri-states unset");

    char *buf = NULL;
    size_t cap = 0;
    expect(gui_capture_meta_text_buffer(0, &buf, &cap) && buf == m->client_name && cap == 256,
           "unlinked: client_name is editable");
    expect(gui_capture_meta_text_buffer(2, &buf, &cap) && buf == m->index_text, "unlinked: index is editable");
    expect(!gui_capture_meta_text_buffer(10, &buf, &cap), "asset_id is never editable");
    expect(!gui_capture_meta_text_buffer(12, &buf, &cap), "operator is never editable");
    expect(!gui_capture_meta_text_buffer(7, &buf, &cap), "a TRI slot has no text buffer");
    expect(!gui_capture_meta_text_buffer(n, &buf, &cap), "out-of-range slot refused");
    expect(gui_capture_meta_set_tri(7, GUI_META_TRI_TRUE) && m->hifi_audio_equipped == GUI_META_TRI_TRUE,
           "unlinked: HiFi set");
    expect(!gui_capture_meta_set_tri(0, GUI_META_TRI_TRUE), "set_tri on a string slot refused");

    gui_capture_meta_mark_used();
    expect(gui_capture_meta_used_since_edit(), "mark_used sets the toolbar mark");
    gui_capture_meta_touch();
    expect(!gui_capture_meta_used_since_edit(), "an edit clears the toolbar mark");

    uint64_t idx = 0;
    snprintf((char *)m->index_text, GUI_META_INDEX_CAP, "007");
    expect(gui_capture_meta_index_value(m, &idx) && idx == 7, "index text 007 is 7");
    snprintf((char *)m->index_text, GUI_META_INDEX_CAP, "%s", "");
    expect(!gui_capture_meta_index_value(m, &idx), "empty index is unset");

    char v[64];
    gui_capture_meta_format_log_value(m, &f[2], v, sizeof(v));
    expect(strcmp(v, "(unset)") == 0, "log value: unset index");
    gui_capture_meta_format_log_value(m, &f[0], v, sizeof(v));
    expect(strcmp(v, "(empty)") == 0, "log value: empty string");
    gui_capture_meta_format_log_value(m, &f[7], v, sizeof(v));
    expect(strcmp(v, "true") == 0, "log value: true");
    gui_capture_meta_format_log_value(m, &f[8], v, sizeof(v));
    expect(strcmp(v, "(unset)") == 0, "log value: unset tri");

    gui_capture_meta_t src;
    memset(&src, 0, sizeof(src));
    snprintf(src.asset_id, sizeof(src.asset_id), "asset_01");
    snprintf(src.client_id, sizeof(src.client_id), "client.7");
    snprintf(src.client_name, sizeof(src.client_name), "Kuhn Family");
    snprintf(src.display_name, sizeof(src.display_name), "Christmas 1994");
    snprintf(src.index_text, sizeof(src.index_text), "3");
    snprintf(src.label, sizeof(src.label), "Tape 3");
    snprintf(src.format, sizeof(src.format), "VHS");
    src.hifi_audio_equipped = GUI_META_TRI_FALSE;
    src.black_and_white = GUI_META_TRI_UNSET;
    uint32_t gen = m->generation;
    gui_capture_meta_set_linked(&src, "/tmp/s.json");
    expect(m->linked && strcmp(m->asset_id, "asset_01") == 0, "set_linked copies the asset");
    expect(m->hifi_audio_equipped == GUI_META_TRI_FALSE, "set_linked copies a tri");
    expect(m->operator_source == GUI_META_OPERATOR_OS_LOGIN && m->operator_name[0],
           "empty session operator keeps the OS login");
    expect(strcmp(m->session_file, "/tmp/s.json") == 0, "session_file kept");
    expect(m->generation != gen, "set_linked bumps the generation");
    expect(!gui_capture_meta_text_buffer(0, &buf, &cap), "linked: nothing is editable");
    expect(!gui_capture_meta_set_tri(7, GUI_META_TRI_TRUE), "linked: tri refused");

    snprintf(src.operator_name, sizeof(src.operator_name), "Reece");
    gui_capture_meta_set_linked(&src, "/tmp/s.json");
    expect(strcmp(m->operator_name, "Reece") == 0 && m->operator_source == GUI_META_OPERATOR_SESSION,
           "a session operator is source session");
    expect(strcmp(gui_capture_meta_operator_source_name(m->operator_source), "session") == 0,
           "operator source name");

    gui_capture_meta_t snap;
    gui_capture_meta_snapshot(&snap);
    expect(memcmp(&snap, m, sizeof(snap)) == 0, "snapshot is a copy");
}

int main(void)
{
    check_descriptors();
    check_escape();
    check_utf8();
    check_store();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
