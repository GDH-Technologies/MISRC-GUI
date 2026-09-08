/*
 * Settings round-trip harness.
 *
 * Compiles misrc_gui/core/gui_settings_table.c (the descriptor table, the
 * file writer/reader, the JSON snapshot and the key/value setter) standalone
 * and drives it with a settings file in the format the hand-written writer
 * produced before the table existed. Asserts:
 *
 *   1. load -> save -> load is a fixed point: the second struct equals the
 *      first field for field and the second text equals the first byte for
 *      byte;
 *   2. the saved text has exactly the fixture's keys, in the fixture's order,
 *      with every key written once (the old writer emitted nine keys twice)
 *      and unknown keys dropped; values match except where the loader has
 *      always transformed them (the base-name timestamp strip, the derived
 *      output names, the port-string mirror, the write-only keys);
 *   3. every entry survives format_value -> apply_key, and the JSON snapshot
 *      survives find_value -> apply_key including quotes and backslashes;
 *   4. the load-time clamps and migrations still apply;
 *   5. write-only keys never change the struct, the legacy tenbit alias still
 *      loads, CRLF files load, an unterminated string is ignored;
 *   6. strict (network) parsing refuses what the raw file dialect cannot hold
 *      or what the UI could never produce;
 *   7. the generation counter and save suspension behave.
 *
 * Usage: gui_settings_roundtrip_harness <fixture.json>
 */
#include "gui_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gui_settings.c is not compiled here; the defaults need a desktop path. */
const char *gui_settings_get_desktop_path(void) {
    return "/tmp/misrc-harness-desktop";
}

static int s_fails = 0;
static int s_passes = 0;

static void check(bool ok, const char *what) {
    if (ok) {
        s_passes++;
        printf("PASS: %s\n", what);
    } else {
        s_fails++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void load_text(gui_settings_t *s, const char *text) {
    gui_settings_init_defaults(s);
    gui_settings_parse_text(s, text);
}

/* Field-for-field comparison through the table; names the first mismatch. */
static bool structs_equal(const gui_settings_t *a, const gui_settings_t *b, const char *label) {
    size_t n = 0;
    const gui_setting_desc_t *t = gui_settings_table(&n);
    for (size_t i = 0; i < n; i++) {
        if (t[i].flags & GS_LOAD_ONLY) continue;
        if (!gui_settings_field_equal(a, b, &t[i])) {
            char va[512], vb[512];
            gui_settings_format_value(a, &t[i], va, sizeof(va));
            gui_settings_format_value(b, &t[i], vb, sizeof(vb));
            fprintf(stderr, "  %s: %s differs: '%s' vs '%s'\n", label, t[i].key, va, vb);
            return false;
        }
    }
    return true;
}

/* Split settings-file text into (key, bare value) pairs in order. */
typedef struct { char key[64]; char value[512]; } kv_t;

static size_t split_pairs(const char *text, kv_t *out, size_t cap) {
    size_t count = 0;
    const char *p = text;
    while ((p = strchr(p, '"')) != NULL && count < cap) {
        const char *ke = strchr(p + 1, '"');
        if (!ke || ke[1] != ':') { p = ke ? ke + 1 : p + 1; continue; }
        size_t kl = (size_t)(ke - p - 1);
        if (kl >= sizeof(out[count].key)) { p = ke + 1; continue; }
        memcpy(out[count].key, p + 1, kl);
        out[count].key[kl] = '\0';
        const char *v = ke + 2;
        while (*v == ' ' || *v == '\t') v++;
        const char *ve;
        if (*v == '"') {
            v++;
            ve = strchr(v, '"');
            if (!ve) break;
            p = ve + 1;
        } else {
            ve = v;
            while (*ve && *ve != ',' && *ve != '\n' && *ve != '}') ve++;
            p = ve;
        }
        size_t vl = (size_t)(ve - v);
        while (vl > 0 && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r')) vl--;
        if (vl >= sizeof(out[count].value)) vl = sizeof(out[count].value) - 1;
        memcpy(out[count].value, v, vl);
        out[count].value[vl] = '\0';
        count++;
    }
    return count;
}

static bool in_list(const char *key, const char *const *list) {
    for (size_t i = 0; list[i]; i++) if (strcmp(key, list[i]) == 0) return true;
    return false;
}

/* Keys whose saved value legitimately differs from the fixture: the loader
 * has always transformed them on the way in. */
static const char *const s_transformed[] = {
    "output_base_name",        /* timestamp prefix stripped */
    "output_filename_a", "output_filename_b", "audio_4ch_filename", "video_filename",
    "audio_2ch_12_filename", "audio_2ch_34_filename",
    "audio_1ch_1_filename", "audio_1ch_2_filename", "audio_1ch_3_filename", "audio_1ch_4_filename",
    "net_server_port_str",     /* re-synced from the numeric port when empty */
    NULL,
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixture.json>\n", argv[0]);
        return 2;
    }
    char *fixture = read_file(argv[1]);
    if (!fixture) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    size_t table_n = 0;
    const gui_setting_desc_t *table = gui_settings_table(&table_n);

    /* --- table sanity ------------------------------------------------- */
    bool keys_unique = true;
    for (size_t i = 0; i < table_n && keys_unique; i++)
        for (size_t j = i + 1; j < table_n; j++)
            if (strcmp(table[i].key, table[j].key) == 0) { keys_unique = false; break; }
    check(keys_unique, "table keys are unique");
    bool find_ok = true;
    for (size_t i = 0; i < table_n; i++) if (gui_settings_find(table[i].key) != &table[i]) find_ok = false;
    check(find_ok, "gui_settings_find resolves every table key to its entry");
    check(gui_settings_find("no_such_key") == NULL, "gui_settings_find rejects an unknown key");

    /* --- 1. load -> save -> load ---------------------------------------- */
    gui_settings_t a, b;
    load_text(&a, fixture);
    static char text1[GUI_SETTINGS_MAX_FILE_BYTES];
    static char text2[GUI_SETTINGS_MAX_FILE_BYTES];
    size_t n1 = gui_settings_format_file(&a, text1, sizeof(text1));
    check(n1 > 0 && n1 < sizeof(text1), "formatted file fits the size cap");
    load_text(&b, text1);
    size_t n2 = gui_settings_format_file(&b, text2, sizeof(text2));
    check(structs_equal(&a, &b, "reload"), "load -> save -> load yields the same struct");
    check(n1 == n2 && strcmp(text1, text2) == 0, "save -> load -> save yields the same text");
    check(strncmp(text1, "{\n  \"device_index\": ", 20) == 0 && text1[n1 - 1] == '\n' && text1[n1 - 2] == '}',
          "file text keeps the historic layout ({, two-space keys, closing })");

    /* --- 2. keys, order, duplicates, values --------------------------- */
    static kv_t fx[256], sv[256];
    size_t nfx = split_pairs(fixture, fx, 256);
    size_t nsv = split_pairs(text1, sv, 256);

    /* expected = fixture minus second occurrences minus unknown keys */
    static kv_t expect[256];
    size_t nexp = 0;
    for (size_t i = 0; i < nfx; i++) {
        bool seen = false;
        for (size_t j = 0; j < nexp; j++) if (strcmp(expect[j].key, fx[i].key) == 0) { seen = true; break; }
        if (seen) continue;
        const gui_setting_desc_t *d = gui_settings_find(fx[i].key);
        if (!d || (d->flags & GS_LOAD_ONLY)) continue;
        expect[nexp++] = fx[i];
    }
    /* The fixture is an OLD settings file, and the build under test may know
     * keys it does not carry. So the requirement is not equality -- that would
     * make every new setting fail this check -- but that the fixture's keys
     * come first, in order, and anything extra is genuinely new and appended.
     *
     * That is exactly the table's own rule: its row order IS the file-write
     * order, so a row inserted mid-table shifts every key after it. When that
     * happens the positional value comparison below reports a mismatch on some
     * unrelated key, which looks nothing like the cause. New rows go on the end. */
    bool same_keys = (nsv >= nexp);
    if (!same_keys)
        fprintf(stderr, "  saved %zu keys, fewer than the %zu the fixture expects\n", nsv, nexp);
    for (size_t i = 0; same_keys && i < nexp; i++) {
        if (strcmp(expect[i].key, sv[i].key) != 0) {
            fprintf(stderr, "  key order differs at %zu: expected %s, saved %s\n", i, expect[i].key, sv[i].key);
            fprintf(stderr, "  (a new table row inserted mid-table rather than appended does this)\n");
            same_keys = false;
        }
    }
    for (size_t i = nexp; same_keys && i < nsv; i++) {
        for (size_t j = 0; j < nfx; j++) {
            if (strcmp(fx[j].key, sv[i].key) == 0) {
                fprintf(stderr, "  key %s is in the fixture but was written after the new keys\n", sv[i].key);
                same_keys = false;
                break;
            }
        }
    }
    check(same_keys, "the fixture's keys are written first, in order, once each, unknown keys dropped, new keys appended");
    bool no_dups = true;
    for (size_t i = 0; i < nsv && no_dups; i++)
        for (size_t j = i + 1; j < nsv; j++)
            if (strcmp(sv[i].key, sv[j].key) == 0) { fprintf(stderr, "  duplicate key %s\n", sv[i].key); no_dups = false; break; }
    check(no_dups, "no key is written twice");
    bool values_kept = true;
    for (size_t i = 0; i < nexp && i < nsv; i++) {
        const gui_setting_desc_t *d = gui_settings_find(expect[i].key);
        if (!d || (d->flags & GS_WRITE_ONLY) || in_list(expect[i].key, s_transformed)) continue;
        if (strcmp(expect[i].value, sv[i].value) != 0) {
            fprintf(stderr, "  value of %s changed: fixture '%s', saved '%s'\n", expect[i].key, expect[i].value, sv[i].value);
            values_kept = false;
        }
    }
    check(values_kept, "every untransformed value survives load -> save verbatim");
    check(strstr(text1, "\"bogus_future_key\"") == NULL, "an unknown key is not re-emitted");

    /* spot checks on the transformed ones */
    check(strcmp(a.output_base_name, "ROUNDTRIP") == 0, "timestamp prefix stripped from output_base_name");
    check(strcmp(a.net_server_port_str, "8090") == 0 && a.net_server_port == 8090,
          "empty net_server_port_str re-synced from net_server_port");
    check(strcmp(a.rf_channel_tags[0], "videoRF") == 0 && strcmp(a.audio_1ch_labels[2], "audio_1ch_3_label_v") == 0
              && a.cxadc_tenbit_mode_card[1] == true && a.enable_audio_1ch[3] == true && a.capture_b == false
              && strcmp(a.level_autostop_level_str, "45") == 0 && strcmp(a.net_client_port_str, "8091") == 0,
          "renamed and array keys land in the right elements");
    check(strcmp(a.output_path, "C:\\Captures\\Tape 01") == 0, "a Windows path with backslashes loads verbatim");
    check(a.ui_scale_percent == 150 && a.memory_budget_gb == 9 && a.flac_level == 6 && a.rf_bits_a == 8 && a.rf_bits_b == 12,
          "numeric fixture values load");
    check(strncmp(a.output_filename_a, "ROUNDTRIP_videoRF_8-bit", 23) == 0, "auto-derived names refreshed after load");
    check(a.capture_limit_seconds == 0 && a.record_limit_seconds == 0, "duration limits forced to 0");

    /* --- 3. format_value -> apply_key, JSON round trip ---------------- */
    bool fv_ok = true;
    for (size_t i = 0; i < table_n; i++) {
        const gui_setting_desc_t *d = &table[i];
        if (d->flags & GS_LOAD_ONLY) continue;
        gui_settings_t c;
        gui_settings_init_defaults(&c);
        char v[512], err[128];
        if (!gui_settings_format_value(&a, d, v, sizeof(v))) { fprintf(stderr, "  format_value failed for %s\n", d->key); fv_ok = false; continue; }
        if (gui_settings_apply_key(&c, d->key, v, true, err, sizeof(err)) != 0) {
            fprintf(stderr, "  strict apply_key refused its own formatted value for %s: '%s' (%s)\n", d->key, v, err);
            fv_ok = false;
            continue;
        }
        if (!gui_settings_field_equal(&a, &c, d)) { fprintf(stderr, "  %s did not round-trip through format_value/apply_key\n", d->key); fv_ok = false; }
    }
    check(fv_ok, "every entry round-trips through format_value -> strict apply_key");

    gui_settings_t j1 = a;
    strcpy(j1.ingest_notes, "say \"hi\" \\ back\ttab \xc3\xa9");
    strcpy(j1.mediamtx_path, "C:\\Tools\\mediamtx.exe");
    static char json[GUI_SETTINGS_MAX_FILE_BYTES];
    size_t nj = gui_settings_to_json(&j1, 0, json, sizeof(json));
    check(nj > 0 && nj < sizeof(json) && json[0] == '{' && json[nj - 1] == '}', "JSON snapshot fits and is an object");
    check(strstr(json, "\"ingest_notes\":\"say \\\"hi\\\" \\\\ back\\ttab \xc3\xa9\"") != NULL, "JSON escapes quotes, backslashes and tabs; UTF-8 passes through");
    gui_settings_t j2;
    gui_settings_init_defaults(&j2);
    bool json_ok = true;
    for (size_t i = 0; i < table_n; i++) {
        const gui_setting_desc_t *d = &table[i];
        if (d->flags & GS_LOAD_ONLY) continue;
        char v[512];
        if (!gui_settings_find_value(json, d->key, v, sizeof(v), true)) { fprintf(stderr, "  %s missing from JSON\n", d->key); json_ok = false; continue; }
        if (gui_settings_apply_key(&j2, d->key, v, false, NULL, 0) != 0) { fprintf(stderr, "  %s from JSON refused\n", d->key); json_ok = false; }
    }
    check(json_ok && structs_equal(&j1, &j2, "json"), "JSON snapshot -> find_value(json) -> apply_key restores every field");
    size_t nj_srv = gui_settings_to_json(&j1, GS_CLIENT_LOCAL | GS_WRITE_ONLY, json, sizeof(json));
    check(nj_srv > 0 && strstr(json, "\"net_mode\"") == NULL && strstr(json, "\"ui_scale_percent\"") == NULL
              && strstr(json, "\"aux_filename\"") == NULL && strstr(json, "\"flac_level\"") != NULL,
          "exclude_flags drops client-local and write-only keys from the snapshot");
    char tiny[16];
    size_t need = gui_settings_to_json(&j1, 0, tiny, sizeof(tiny));
    check(need == nj && strlen(tiny) == sizeof(tiny) - 1, "to_json reports the needed length and never overruns a small buffer");

    /* --- 4. clamps and migrations ------------------------------------- */
    gui_settings_t c;
    load_text(&c, "{\n  \"memory_budget_gb\": 0\n}\n");   check(c.memory_budget_gb == 4, "memory_budget_gb 0 -> 4");
    load_text(&c, "{\n  \"memory_budget_gb\": 99\n}\n");  check(c.memory_budget_gb == 16, "memory_budget_gb 99 -> 16");
    load_text(&c, "{\n  \"video_record_codec\": 99\n}\n"); check(c.video_record_codec == 0, "video_record_codec 99 -> 0");
    load_text(&c, "{\n  \"rtsp_stream_port\": 80\n}\n");  check(c.rtsp_stream_port == 0, "rtsp_stream_port 80 -> 0");
    load_text(&c, "{\n  \"rtsp_stream_encoder\": 7\n}\n"); check(c.rtsp_stream_encoder == 0, "rtsp_stream_encoder 7 -> 0");
    load_text(&c, "{\n  \"rtsp_stream_bitrate_kbps\": 5\n}\n"); check(c.rtsp_stream_bitrate_kbps == 0, "rtsp_stream_bitrate_kbps 5 -> 0");
    load_text(&c, "{\n  \"ui_scale_percent\": junk\n}\n"); check(c.ui_scale_percent == 100, "ui_scale_percent junk -> default");
    load_text(&c, "{\n  \"net_mode\": 7\n}\n");            check(c.net_mode == 0, "net_mode 7 -> 0");
    load_text(&c, "{\n  \"net_server_port\": 70000\n}\n"); check(c.net_server_port == 8080 && strcmp(c.net_server_port_str, "8080") == 0, "net_server_port 70000 -> 8080 and mirror");
    /* The mirror is only re-synced when the file left it EMPTY; a file that
     * carries the number alone keeps the default string, as it always has. */
    load_text(&c, "{\n  \"net_client_port\": 9001\n}\n");  check(c.net_client_port == 9001 && strcmp(c.net_client_port_str, "8080") == 0, "net_client_port without its mirror keeps the default mirror (historic)");
    load_text(&c, "{\n  \"net_client_port\": 9001,\n  \"net_client_port_str\": \"\"\n}\n");  check(strcmp(c.net_client_port_str, "9001") == 0, "an empty net_client_port_str is re-synced from the number");
    load_text(&c, "{\n  \"rf_bits_a\": 10\n}\n");          check(c.rf_bits_a == 16, "rf_bits_a 10 -> derived 16");
    load_text(&c, "{\n  \"rf_bits_a\": 10,\n  \"reduce_8bit_a\": true\n}\n"); check(c.rf_bits_a == 8, "rf_bits_a 10 + reduce_8bit_a -> 8");
    load_text(&c, "{\n  \"rf_bits_b\": 10,\n  \"flac_12bit\": true\n}\n"); check(c.rf_bits_b == 12, "rf_bits_b 10 + flac_12bit -> 12");
    load_text(&c, "{\n  \"output_base_name\": \"2024.01.15_10.30.00_LONG\"\n}\n"); check(strcmp(c.output_base_name, "LONG") == 0, "yyyy timestamp prefix stripped");
    load_text(&c, "{\n  \"output_base_name\": \"\"\n}\n"); check(strcmp(c.output_base_name, "capture") == 0, "empty base name -> capture");
    load_text(&c, "{\n  \"flac_level\": 6\n}\n");
    gui_settings_t defaults;
    gui_settings_init_defaults(&defaults);
    gui_settings_post_load(&defaults);
    defaults.flac_level = 6;
    check(structs_equal(&c, &defaults, "single-key"), "a single-key file leaves every other field at its default");

    /* --- 5. write-only, alias, CRLF, unterminated --------------------- */
    load_text(&c, "{\n  \"show_peak_levels\": false,\n  \"aux_filename\": \"zzz\",\n  \"sample_count\": 5\n}\n");
    check(c.show_peak_levels == true && strcmp(c.aux_filename, "aux_data.bin") == 0 && c.sample_count == 0,
          "write-only keys never change the struct");
    load_text(&c, "{\n  \"cxadc_tenbit_mode\": true\n}\n");
    check(c.cxadc_tenbit_mode_card[0] && c.cxadc_tenbit_mode_card[1], "legacy cxadc_tenbit_mode sets both cards");
    load_text(&c, "{\n  \"cxadc_tenbit_mode\": true,\n  \"cxadc_tenbit_mode_b\": false\n}\n");
    check(c.cxadc_tenbit_mode_card[0] && !c.cxadc_tenbit_mode_card[1], "per-card tenbit keys override the alias");
    check(strstr(text1, "\"cxadc_tenbit_mode\":") == NULL, "the legacy alias is never written");

    size_t flen = strlen(fixture);
    char *crlf = malloc(flen * 2 + 1);
    size_t w = 0;
    for (size_t i = 0; i < flen; i++) { if (fixture[i] == '\n') crlf[w++] = '\r'; crlf[w++] = fixture[i]; }
    crlf[w] = '\0';
    gui_settings_t cr;
    load_text(&cr, crlf);
    check(structs_equal(&a, &cr, "crlf"), "a CRLF-edited file loads the same values");
    free(crlf);

    load_text(&c, "{\n  \"output_path\": \"abc\n}\n");
    check(strcmp(c.output_path, gui_settings_get_desktop_path()) == 0, "an unterminated string is ignored");
    load_text(&c, "{\n  \"flac_level\": 3,\n  \"flac_level\": 5\n}\n");
    check(c.flac_level == 3, "the first occurrence of a key wins");

    /* --- 6. strict parsing ------------------------------------------- */
    gui_settings_init_defaults(&c);
    char err[128];
    check(gui_settings_apply_key(&c, "no_such_key", "1", true, err, sizeof(err)) == -1, "strict: unknown key -> -1");
    check(gui_settings_apply_key(&c, "flac_level", "9", true, err, sizeof(err)) == -2 && err[0], "strict: flac_level 9 refused with a reason");
    check(gui_settings_apply_key(&c, "flac_level", "6", true, err, sizeof(err)) == 0 && c.flac_level == 6, "strict: flac_level 6 accepted");
    check(gui_settings_apply_key(&c, "rf_bits_a", "10", true, err, sizeof(err)) == -2, "strict: rf_bits_a 10 refused");
    check(gui_settings_apply_key(&c, "rf_bits_a", "12", true, err, sizeof(err)) == 0 && c.rf_bits_a == 12, "strict: rf_bits_a 12 accepted");
    check(gui_settings_apply_key(&c, "capture_a", "yes", true, err, sizeof(err)) == -2, "strict: bool 'yes' refused");
    check(gui_settings_apply_key(&c, "capture_a", "yes", false, err, sizeof(err)) == 0 && c.capture_a == false, "loose: bool 'yes' reads as false (as it always has)");
    check(gui_settings_apply_key(&c, "flac_threads", "12abc", true, err, sizeof(err)) == -2, "strict: '12abc' refused");
    check(gui_settings_apply_key(&c, "flac_threads", "12abc", false, err, sizeof(err)) == 0 && c.flac_threads == 12, "loose: '12abc' reads 12 (atoi semantics)");
    check(gui_settings_apply_key(&c, "ingest_notes", "a \"quoted\" note", true, err, sizeof(err)) == -2, "strict: a quote in a string refused");
    check(gui_settings_apply_key(&c, "ingest_notes", "line\nbreak", true, err, sizeof(err)) == -2, "strict: a line break refused");
    char longv[400];
    memset(longv, 'x', sizeof(longv) - 1);
    longv[sizeof(longv) - 1] = '\0';
    check(gui_settings_apply_key(&c, "rf_tag_a", longv, true, err, sizeof(err)) == -2, "strict: over-cap string refused");
    check(gui_settings_apply_key(&c, "rf_tag_a", longv, false, err, sizeof(err)) == 0 && strlen(c.rf_channel_tags[0]) == 31, "loose: over-cap string truncated to cap-1");
    check(gui_settings_apply_key(&c, "output_path", "/mnt/x9 pro/captures", true, err, sizeof(err)) == 0
              && strcmp(c.output_path, "/mnt/x9 pro/captures") == 0, "strict: a path with a space accepted verbatim");
    check(gui_settings_apply_key(&c, "resample_rate_a", "2500.5", true, err, sizeof(err)) == 0 && c.resample_rate_a > 2500.4f && c.resample_rate_a < 2500.6f, "strict: float accepted");
    check(gui_settings_apply_key(&c, "memory_budget_gb", "-3", true, err, sizeof(err)) == 0 && c.memory_budget_gb == 4, "strict: memory_budget_gb -3 clamps to 4");
    check(gui_settings_apply_key(&c, "rtlsdr_freq_hz", "-1", true, err, sizeof(err)) == -2, "strict: negative unsigned refused");

    /* --- 7. generation and save suspension ---------------------------- */
    uint32_t g0 = gui_settings_generation();
    gui_settings_bump_generation();
    check(gui_settings_generation() == g0 + 1 && g0 >= 1, "generation counter starts at 1 and bumps");
    check(!gui_settings_save_suspended(), "saves are not suspended by default");
    check(gui_settings_suspend_save(true) == false && gui_settings_save_suspended(), "suspend_save(true) suspends");
    gui_settings_note_save_requested();
    check(gui_settings_suspend_save(false) == true && !gui_settings_save_suspended(), "un-suspending reports the requested save once");
    check(gui_settings_suspend_save(false) == false, "a second un-suspend reports nothing");

    /* --- 8. copy_fields ---------------------------------------------- */
    gui_settings_t own, peer;
    gui_settings_init_defaults(&own);
    gui_settings_init_defaults(&peer);
    own.ui_scale_percent = 175; own.net_mode = 2; own.flac_level = 1; strcpy(own.output_path, "/own");
    peer.ui_scale_percent = 80;  peer.net_mode = 1; peer.flac_level = 8; strcpy(peer.output_path, "/peer");
    gui_settings_t view = peer;
    gui_settings_copy_fields(&view, &own, true);
    check(view.ui_scale_percent == 175 && view.net_mode == 2 && view.flac_level == 8 && strcmp(view.output_path, "/peer") == 0,
          "copy_fields(client_local=true) copies only the client-local fields");
    gui_settings_copy_fields(&view, &own, false);
    check(view.flac_level == 1 && strcmp(view.output_path, "/own") == 0 && view.ui_scale_percent == 175,
          "copy_fields(client_local=false) copies only the server-owned fields");

    free(fixture);
    printf("%d passed, %d failed\n", s_passes, s_fails);
    return s_fails ? 1 : 0;
}
