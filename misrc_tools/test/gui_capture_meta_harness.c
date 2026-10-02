/*
 * CPU-only checks for the capture metadata store (core/gui_capture_meta.c):
 * the descriptor table (order, offsets, caps, flags), the capture-log escape
 * (it must round-trip, so a notes field never splits a log line and can be
 * read back), the strict UTF-8 validator, and the linked/unlinked editing
 * rules the panel relies on; and the sidecar (output/gui_capture_sidecar.c):
 * golden JSON for a linked and an unlinked record, and an atomic write that
 * leaves no temp file. Run by meson (`meson test -C build-local`).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misrc_gui/core/gui_capture_meta.h"
#include "../misrc_gui/output/gui_capture_sidecar.h"

#if defined(_WIN32) || defined(_WIN64)
#include <process.h>
#define getpid_portable _getpid
#else
#include <unistd.h>
#define getpid_portable getpid
#endif

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
    expect(m->rf_requested_a == GUI_META_TRI_UNSET && m->rf_requested_b == GUI_META_TRI_UNSET &&
           !gui_capture_meta_rf_request(m, NULL, NULL), "init: no rf_channels request");

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

    /* The rf_channels request: none after init, both or neither, kept
     * across a (re)link, part of the snapshot. */
    bool ra = true, rb = true;
    expect(!gui_capture_meta_rf_request(m, &ra, &rb), "rf request: set_linked does not invent one");
    gen = m->generation;
    gui_capture_meta_set_rf_request(GUI_META_TRI_TRUE, GUI_META_TRI_FALSE);
    expect(gui_capture_meta_rf_request(m, &ra, &rb) && ra && !rb, "rf request: A on, B off kept");
    expect(m->generation != gen, "rf request bumps the generation");
    gui_capture_meta_set_linked(&src, "/tmp/s.json");
    expect(gui_capture_meta_rf_request(m, &ra, &rb) && ra && !rb, "rf request survives set_linked");
    gui_capture_meta_set_rf_request(GUI_META_TRI_TRUE, GUI_META_TRI_UNSET);
    expect(!gui_capture_meta_rf_request(m, &ra, &rb) && m->rf_requested_a == GUI_META_TRI_UNSET,
           "rf request: half a request clears both");
    gui_capture_meta_set_rf_request(GUI_META_TRI_FALSE, GUI_META_TRI_TRUE);

    gui_capture_meta_t snap;
    gui_capture_meta_snapshot(&snap);
    expect(memcmp(&snap, m, sizeof(snap)) == 0, "snapshot is a copy");
    expect(gui_capture_meta_rf_request(&snap, &ra, &rb) && !ra && rb, "snapshot carries the rf request");
    gui_capture_meta_init();
    expect(m->rf_requested_a == GUI_META_TRI_UNSET && m->rf_requested_b == GUI_META_TRI_UNSET,
           "init clears the rf request");
}

/* ---- sidecar -------------------------------------------------------------- */

/* gui_settings.c is not compiled here; the table's defaults need these. */
const char *gui_settings_get_desktop_path(void) { return "/tmp/misrc-harness-desktop"; }
uint32_t get_num_cores(void) { return 8; }

static void linked_fixture(gui_capture_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->linked = true;
    snprintf(m->asset_id, sizeof(m->asset_id), "asset_01");
    snprintf(m->client_id, sizeof(m->client_id), "client.7");
    snprintf(m->client_name, sizeof(m->client_name), "Kuhn Family");
    snprintf(m->display_name, sizeof(m->display_name), "Christmas 1994");
    snprintf(m->index_text, sizeof(m->index_text), "3");
    snprintf(m->label, sizeof(m->label), "Tape 3");
    snprintf(m->format, sizeof(m->format), "VHS");
    snprintf(m->tape_speed, sizeof(m->tape_speed), "SP");
    snprintf(m->video_system, sizeof(m->video_system), "NTSC");
    m->hifi_audio_equipped = GUI_META_TRI_FALSE;
    m->black_and_white = GUI_META_TRI_UNSET;
    snprintf(m->notes, sizeof(m->notes), "line 1\nsaid \"hi\"\tcaf\xc3\xa9 \\ end");
    snprintf(m->operator_name, sizeof(m->operator_name), "Reece");
    m->operator_source = GUI_META_OPERATOR_SESSION;
    snprintf(m->session_file, sizeof(m->session_file), "/tmp/s.json");
    /* The session asked for A+B; this seat recorded A only. */
    m->rf_requested_a = GUI_META_TRI_TRUE;
    m->rf_requested_b = GUI_META_TRI_TRUE;
}

static const char *const s_linked_golden =
    "{\n"
    "  \"schema\": \"misrc-gui.capture-meta/1\",\n"
    "  \"state\": \"complete\",\n"
    "  \"linked\": true,\n"
    "  \"asset\": {\n"
    "    \"client_name\": \"Kuhn Family\",\n"
    "    \"display_name\": \"Christmas 1994\",\n"
    "    \"index\": 3,\n"
    "    \"label\": \"Tape 3\",\n"
    "    \"format\": \"VHS\",\n"
    "    \"tape_speed\": \"SP\",\n"
    "    \"video_system\": \"NTSC\",\n"
    "    \"hifi_audio_equipped\": false,\n"
    "    \"black_and_white\": null,\n"
    "    \"notes\": \"line 1\\nsaid \\\"hi\\\"\\tcaf\xc3\xa9 \\\\ end\",\n"
    "    \"asset_id\": \"asset_01\",\n"
    "    \"client_id\": \"client.7\"\n"
    "  },\n"
    "  \"operator\": \"Reece\",\n"
    "  \"operator_source\": \"session\",\n"
    "  \"session_file\": \"/tmp/s.json\",\n"
    "  \"misrc_gui_version\": \"v1.2.3-gdh.9\",\n"
    "  \"computer_name\": \"wm\",\n"
    "  \"device\": {\"name\": \"[Simulated] Test Signal\", \"type\": \"simulated\"},\n"
    "  \"capture_format\": \"FLAC\",\n"
    "  \"started_at\": \"2026-10-02T15:30:28Z\",\n"
    "  \"ended_at\": \"2026-10-02T15:30:30Z\",\n"
    "  \"capture_seconds\": 2.004,\n"
    "  \"output_path\": \"/caps\",\n"
    "  \"base_name\": \"Kuhn_Tape_3\",\n"
    "  \"log_file\": \"Kuhn_Tape_3_2026.10.02_10.30.28_misrc_capture.log\",\n"
    "  \"rf_channels\": {\"requested\": {\"a\": true, \"b\": true}, \"recorded\": {\"a\": true, \"b\": false}},\n"
    "  \"files\": {\n"
    "    \"rf_a\": {\"name\": \"rfA_Kuhn_Tape_3_16-bit.flac\", \"bits\": 16, \"sample_rate_hz\": 40000000, "
    "\"samples\": 16777216, \"bytes\": 13281290},\n"
    "    \"rf_b\": null,\n"
    "    \"video\": null,\n"
    "    \"closed_captions\": \"Kuhn_Tape_3_captions.scc\",\n"
    "    \"audio\": {\"audio_4ch\": \"Kuhn_Tape_3_4ch.wav\", \"audio_2ch_12\": null, \"audio_2ch_34\": null, "
    "\"audio_1ch_1\": null, \"audio_1ch_2\": null, \"audio_1ch_3\": null, \"audio_1ch_4\": null}\n"
    "  },\n"
    "  \"result\": {\"drops\": 0, \"waits\": 5}\n"
    "}\n";

static const char *const s_unlinked_golden =
    "{\n"
    "  \"schema\": \"misrc-gui.capture-meta/1\",\n"
    "  \"state\": \"recording\",\n"
    "  \"linked\": false,\n"
    "  \"asset\": {\n"
    "    \"client_name\": \"\",\n"
    "    \"display_name\": \"\",\n"
    "    \"index\": null,\n"
    "    \"label\": \"\",\n"
    "    \"format\": \"\",\n"
    "    \"tape_speed\": \"\",\n"
    "    \"video_system\": \"\",\n"
    "    \"hifi_audio_equipped\": null,\n"
    "    \"black_and_white\": true,\n"
    "    \"notes\": \"\",\n"
    "    \"asset_id\": \"\",\n"
    "    \"client_id\": \"\"\n"
    "  },\n"
    "  \"operator\": \"op\",\n"
    "  \"operator_source\": \"os_login\",\n"
    "  \"session_file\": null,\n"
    "  \"misrc_gui_version\": \"dev\",\n"
    "  \"computer_name\": \"wm\",\n"
    "  \"device\": {\"name\": \"cxadc0\", \"type\": \"cxadc\"},\n"
    "  \"capture_format\": \"RAW\",\n"
    "  \"started_at\": \"2026-10-02T15:30:28Z\",\n"
    "  \"ended_at\": null,\n"
    "  \"capture_seconds\": null,\n"
    "  \"output_path\": \"/caps\",\n"
    "  \"base_name\": \"capture\",\n"
    "  \"log_file\": \"capture_2026.10.02_10.30.28_misrc_capture.log\",\n"
    "  \"rf_channels\": {\"requested\": null, \"recorded\": {\"a\": true, \"b\": true}},\n"
    "  \"files\": {\n"
    "    \"rf_a\": {\"name\": \"rfA_capture_8-bit.u8\", \"bits\": 8, \"sample_rate_hz\": 28636363, "
    "\"samples\": null, \"bytes\": null},\n"
    "    \"rf_b\": {\"name\": \"rfB_capture_16-bit.u16\", \"bits\": 16, \"sample_rate_hz\": 28636363, "
    "\"samples\": null, \"bytes\": null},\n"
    "    \"video\": \"capture_video.mkv\",\n"
    "    \"closed_captions\": null,\n"
    "    \"audio\": {\"audio_4ch\": null, \"audio_2ch_12\": null, \"audio_2ch_34\": null, "
    "\"audio_1ch_1\": null, \"audio_1ch_2\": null, \"audio_1ch_3\": null, \"audio_1ch_4\": null}\n"
    "  },\n"
    "  \"result\": null\n"
    "}\n";

static void report_diff(const char *what, const char *got, const char *want)
{
    size_t i = 0;
    while (got[i] && got[i] == want[i]) i++;
    fprintf(stderr, "FAIL: %s differs from the golden text at byte %zu:\n--- got\n%s\n--- want\n%s\n",
            what, i, got, want);
}

static void check_sidecar(void)
{
    gui_capture_meta_t m;
    linked_fixture(&m);
    gui_capture_sidecar_t s;
    memset(&s, 0, sizeof(s));
    s.complete = true;
    s.meta = &m;
    s.misrc_gui_version = "v1.2.3-gdh.9";
    s.computer_name = "wm";
    s.device_name = "[Simulated] Test Signal";
    s.device_type = "simulated";
    s.capture_format = "FLAC";
    s.started_at = "2026-10-02T15:30:28Z";
    s.ended_at = "2026-10-02T15:30:30Z";
    s.have_capture_seconds = true;
    s.capture_seconds = 2.0041;
    s.output_path = "/caps";
    s.base_name = "Kuhn_Tape_3";
    s.log_file = "Kuhn_Tape_3_2026.10.02_10.30.28_misrc_capture.log";
    s.rf_recorded_a = true;
    s.rf_recorded_b = false;
    s.rf_a =(gui_capture_sidecar_rf_t){ "rfA_Kuhn_Tape_3_16-bit.flac", 16, 40000000ULL,
                                         true, 16777216ULL, true, 13281290ULL };
    s.closed_captions = "Kuhn_Tape_3_captions.scc";
    s.audio[0] = "Kuhn_Tape_3_4ch.wav";
    s.have_result = true;
    s.drops = 0;
    s.waits = 5;

    size_t len = 0;
    char *text = gui_capture_sidecar_format_alloc(&s, &len);
    expect(text != NULL && len == strlen(text), "linked sidecar formats");
    if (text && strcmp(text, s_linked_golden) != 0) {
        report_diff("linked sidecar", text, s_linked_golden);
        failures++;
    }
    checks++;
    /* A short buffer truncates but still reports the whole length. */
    char small[16];
    expect(gui_capture_sidecar_format(&s, small, sizeof(small)) == len && strlen(small) == 15,
           "format truncates and reports the full length");

    /* Unlinked, mid-recording. */
    gui_capture_meta_t u;
    memset(&u, 0, sizeof(u));
    u.hifi_audio_equipped = GUI_META_TRI_UNSET;
    u.black_and_white = GUI_META_TRI_TRUE;
    u.rf_requested_a = GUI_META_TRI_UNSET;   /* no session request: "requested": null */
    u.rf_requested_b = GUI_META_TRI_UNSET;
    snprintf(u.operator_name, sizeof(u.operator_name), "op");
    u.operator_source = GUI_META_OPERATOR_OS_LOGIN;
    gui_capture_sidecar_t r;
    memset(&r, 0, sizeof(r));
    r.meta = &u;
    r.misrc_gui_version = "dev";
    r.computer_name = "wm";
    r.device_name = "cxadc0";
    r.device_type = "cxadc";
    r.capture_format = "RAW";
    r.started_at = "2026-10-02T15:30:28Z";
    r.output_path = "/caps";
    r.base_name = "capture";
    r.log_file = "capture_2026.10.02_10.30.28_misrc_capture.log";
    r.rf_recorded_a = true;
    r.rf_recorded_b = true;
    r.rf_a = (gui_capture_sidecar_rf_t){ "rfA_capture_8-bit.u8", 8, 28636363ULL, false, 0, false, 0 };
    r.rf_b = (gui_capture_sidecar_rf_t){ "rfB_capture_16-bit.u16", 16, 28636363ULL, false, 0, false, 0 };
    r.video = "capture_video.mkv";
    char *utext = gui_capture_sidecar_format_alloc(&r, NULL);
    expect(utext != NULL, "unlinked sidecar formats");
    if (utext && strcmp(utext, s_unlinked_golden) != 0) {
        report_diff("unlinked sidecar", utext, s_unlinked_golden);
        failures++;
    }
    checks++;

    /* Atomic write: the file holds the text and no temp file is left. */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = getenv("TEMP");
    if (!tmpdir || !tmpdir[0]) tmpdir = ".";
    char path[512], tmp_path[600];
    snprintf(path, sizeof(path), "%s/gui_capture_meta_harness_%d%s", tmpdir, (int)getpid_portable(),
             GUI_CAPTURE_SIDECAR_SUFFIX);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", path, (int)getpid_portable());
    char err[256];
    bool wrote = text && gui_capture_sidecar_write_atomic(path, text, len, err, sizeof(err));
    expect(wrote, "atomic write succeeds");
    /* A second write replaces the first in place. */
    wrote = utext && gui_capture_sidecar_write_atomic(path, utext, strlen(utext), err, sizeof(err));
    expect(wrote, "atomic write replaces an existing sidecar");
    FILE *f = fopen(path, "rb");
    char back[8192] = {0};
    size_t got = f ? fread(back, 1, sizeof(back) - 1, f) : 0;
    if (f) fclose(f);
    expect(utext && got == strlen(utext) && strcmp(back, utext) == 0, "the sidecar holds the last text");
    FILE *t = fopen(tmp_path, "rb");
    expect(t == NULL, "no .tmp file is left behind");
    if (t) fclose(t);
    remove(path);

    char bad[600];
    snprintf(bad, sizeof(bad), "%s/no_such_dir_%d/x%s", tmpdir, (int)getpid_portable(),
             GUI_CAPTURE_SIDECAR_SUFFIX);
    expect(!gui_capture_sidecar_write_atomic(bad, "{}", 2, err, sizeof(err)) && err[0],
           "a write into a missing directory fails with a reason");

    expect(strcmp(gui_capture_sidecar_basename("/a/b/c.flac"), "c.flac") == 0, "basename");
    expect(strcmp(gui_capture_sidecar_basename("c.flac"), "c.flac") == 0, "basename without a dir");

    free(text);
    free(utext);
}

int main(void)
{
    check_descriptors();
    check_escape();
    check_utf8();
    check_store();
    check_sidecar();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
