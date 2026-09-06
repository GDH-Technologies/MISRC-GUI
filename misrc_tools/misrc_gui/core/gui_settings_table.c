/*
 * MISRC GUI - Settings table
 *
 * One descriptor per persisted setting. The settings file writer and reader,
 * the JSON snapshot and the key/value setter all walk this table, so a
 * setting that exists in one of them exists in all of them. The table is in
 * the order the file has always been written, so a saved file is unchanged
 * apart from the keys the old writer emitted twice.
 *
 * This file must compile without raylib: the settings round-trip guard
 * builds it standalone (with gui_ui_scale.c and a stub for
 * gui_settings_get_desktop_path).
 */

#include "gui_settings.h"
#include "../ui/gui_ui_scale.h"
#ifdef ENABLE_DDD
#include "../../common/ddd_protocol.h"
#endif

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Defaults
 * ============================================================================ */
void gui_settings_init_defaults(gui_settings_t *settings) {
    if (!settings) return;

    // Zero out the structure
    memset(settings, 0, sizeof(gui_settings_t));

    // Basic settings
    settings->device_index = 0;
    settings->capture_a = true;
    settings->capture_b = true;

    // Set default save path to Desktop
    strncpy(settings->output_path, gui_settings_get_desktop_path(), MAX_FILENAME_LEN - 1);
    settings->output_path[MAX_FILENAME_LEN - 1] = '\0';

    // Auto naming defaults
    settings->auto_names_enabled = true;
    strcpy(settings->output_base_name, "capture");

    // Timestamp behavior
    settings->append_timestamp_on_capture_start = false;

    // Duration limits: removed from UI, force to 0 (unlimited)
    settings->capture_limit_seconds = 0;
    settings->record_limit_seconds = 0;

    // Default filenames (used when auto naming is disabled)
    strcpy(settings->output_filename_a, "rfA_capture.flac");
    strcpy(settings->output_filename_b, "rfB_capture.flac");
    strcpy(settings->aux_filename, "aux_data.bin");
    strcpy(settings->raw_filename, "raw_data.bin");
    strcpy(settings->audio_4ch_filename, "quad_4ch.wav");
    strcpy(settings->video_filename, "video.mkv");
    strcpy(settings->audio_2ch_12_filename, "stereo_ch1_ch2.wav");
    strcpy(settings->audio_2ch_34_filename, "stereo_ch3_ch4.wav");

    // RF bit depth defaults (per-channel)
    settings->rf_bits_a = 16;
    settings->rf_bits_b = 16;
    settings->cxadc_tenbit_mode_card[0] = false;
    settings->cxadc_tenbit_mode_card[1] = false;

    // Individual channel filenames
    for (int i = 0; i < 4; i++) {
        snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "audio_ch%d.wav", i + 1);
    }

    // Per-channel audio labels (optional, used for auto naming)
    for (int i = 0; i < 4; i++) {
        settings->audio_1ch_labels[i][0] = '\0';
    }
    // Optional tags for non-mono audio outputs
    for (int i = 0; i < 3; i++) {
        settings->audio_output_tags[i][0] = '\0';
    }
    settings->ingest_project[0] = '\0';
    settings->ingest_tape_id[0] = '\0';
    settings->ingest_tape_format[0] = '\0';
    settings->ingest_tape_size[0] = '\0';
    settings->ingest_tape_speed[0] = '\0';
    settings->ingest_tape_condition[0] = '\0';
    settings->ingest_operator[0] = '\0';
    settings->ingest_location[0] = '\0';
    settings->ingest_notes[0] = '\0';
    // Optional per-channel RF tags
    for (int i = 0; i < 2; i++) {
        settings->rf_channel_tags[i][0] = '\0';
    }

    // Capture control defaults
    settings->sample_count = 0; // Infinite
    strcpy(settings->capture_time, ""); // Empty = infinite
    settings->overwrite_files = false;

    // Processing options
    settings->pad_lower_bits = false;
    settings->show_peak_levels = true;
    settings->suppress_clip_a = false;
    settings->suppress_clip_b = false;
    settings->reduce_8bit_a = false;
    settings->reduce_8bit_b = false;

    // Resampling defaults
    settings->enable_resample_a = false;
    settings->enable_resample_b = false;
    settings->resample_rate_a = 4000.0f;  // 4 MHz
    settings->resample_rate_b = 4000.0f;  // 4 MHz
    settings->resample_quality_a = 3;     // High quality
    settings->resample_quality_b = 3;     // High quality
    settings->resample_gain_a = 0.0f;     // No gain
    settings->resample_gain_b = 0.0f;     // No gain
#ifdef ENABLE_DDD
    settings->ddd_decimation = DDD_DECIMATION_FULL_RATE;
#endif

    // FLAC defaults
    settings->use_flac = true;
    settings->flac_12bit = false;
    settings->flac_level = 4;             // Balanced compression
    settings->flac_verification = false;  // Faster
    settings->flac_threads = 0;           // Auto
    settings->flac_affinity_enabled = false;
    settings->flac_affinity_cpu_list[0] = '\0';

    // Audio output defaults
    settings->enable_audio_4ch = false;
    settings->video_record_enabled = false;
    settings->video_record_codec = 0;        /* H.264 */
    settings->video_output_tag[0] = '\0';
    settings->ffmpeg_path[0] = '\0';
    settings->preview_device_path[0] = '\0';
    settings->rtsp_stream_enabled = false;
    settings->rtsp_stream_lan = false;       /* loopback: going off-box is explicit */
    settings->rtsp_stream_port = 0;          /* 0 = the module's default */
    settings->rtsp_stream_encoder = 0;       /* auto */
    settings->rtsp_stream_bitrate_kbps = 0;  /* 0 = the module's default */
    settings->rtsp_stream_deinterlace = false;
    settings->rtsp_lan_acknowledged = false; /* the warning has not been seen yet */
    settings->rtsp_stream_password = false;  /* passwordless unless asked */
    settings->mediamtx_path[0] = '\0';
    settings->rtsp_audio_device[0] = '\0';
    settings->enable_audio_2ch_12 = false;
    settings->enable_audio_2ch_34 = false;
    for (int i = 0; i < 4; i++) {
        settings->enable_audio_1ch[i] = false;
    }

    // Audio monitoring defaults
    settings->audio_monitor_playback = false;
    settings->audio_monitor_ch34 = false;  // Default to CH1/2
    settings->misrc_mode = true;           // Default to MISRC mode (A/B swapped)
    settings->misrc_v15_v25_ab_swap = false;
    settings->stop_on_dropout = false;

    // Level autostop defaults (tape-end detection). Disabled by default.
    // Defaults mirror PR #11: 33% threshold, 5.0s sustain.
    settings->level_autostop_enabled = false;
    strcpy(settings->level_autostop_level_str, "33");
    strcpy(settings->level_autostop_duration_str, "5.0");

    // Display settings
    settings->ui_scale_auto = true;
    settings->show_grid = true;
    settings->time_scale = 1.0f;
    settings->amplitude_scale = 1.0f;
    settings->ui_scale_percent = GUI_UI_SCALE_DEFAULT_PERCENT;

    // V4L2/simple_capture device discovery is opt-in (disabled by default).
    settings->discover_simple_capture = false;
    // Core-pinning controls are hidden by default; users can enable from info page.
    settings->show_core_pinning_in_settings = false;
    // Max total buffer RAM budget (1-16 GB). Default 4 GB mirrors the older
    // code's ~4 GB target. Clamped on load; applied at buffer-manager init.
    settings->memory_budget_gb = 4;
    settings->update_last_check_unix_s = 0;
    settings->update_last_release_tag[0] = '\0';
    settings->update_available_cached = false;

    // RTL-SDR defaults (only relevant when an RTL-SDR device is selected)
    settings->rtlsdr_freq_hz = 100000000ULL;   // 100.0 MHz (FM broadcast band)
    settings->rtlsdr_gain_mode = 0;             // 0 = auto, 1 = manual
    settings->rtlsdr_gain_tenths_db = 0;        // manual gain (tenths dB); unused in auto mode
    settings->rtlsdr_sample_rate_hz = 2400000U;  // 2.4 MSPS (highest widely-stable)
    settings->rtlsdr_agc = true;                // RTL2832 AGC on
    settings->rtlsdr_offset_corr = false;       // RTL offset tuning correction off

    // Demod view defaults (device-agnostic; apply to the Demod panel)
    settings->demod_mode = 0;                   // 0=WFM,1=NFM,2=AM,3=USB,4=LSB
    settings->demod_bandwidth_hz = 0;           // 0 = use mode default bandwidth
    settings->demod_squelch = 0.0f;             // 0.0 = squelch open
    settings->demod_volume = 1.0f;              // 1.0 = unity gain
    settings->demod_output_pair = 0;            // 0 = CH1/2, 1 = CH3/4

    // Server/Client networking defaults (cxadc_vhs_server-style). Stock mode
    // is Local (no networking). Default port 8080 mirrors the reference server.
    settings->net_mode = 0;                     // 0=Local, 1=Server, 2=Client
    settings->net_server_port = 8080;
    snprintf(settings->net_server_port_str, sizeof(settings->net_server_port_str), "%u",
             (unsigned)settings->net_server_port);
    settings->net_client_host[0] = '\0';
    settings->net_client_port = 8080;
    snprintf(settings->net_client_port_str, sizeof(settings->net_client_port_str), "%u",
             (unsigned)settings->net_client_port);
}

/* ============================================================================
 * Auto-derived output names
 * ============================================================================ */
static uint8_t clamp_rf_bits_flac(uint8_t bits) {
    if (bits == 8 || bits == 12 || bits == 16) return bits;
    return 16;
}

static uint8_t rf_bits_for_raw(uint8_t requested) {
    // RAW supports 8/16 only; treat 12 as 16.
    return (requested == 8) ? 8 : 16;
}

static void format_msps_from_khz(char *dst, size_t dst_len, float khz) {
    if (!dst || dst_len == 0) return;
    uint32_t khz_u = (uint32_t)(khz + 0.5f);
    if ((khz_u % 1000U) == 0U) {
        snprintf(dst, dst_len, "%umsps", khz_u / 1000U);
    } else {
        snprintf(dst, dst_len, "%.1fmsps", (double)khz / 1000.0);
    }
}

static void sanitize_tag(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        char c = src[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == ' ' || c == '\t') {
            dst[j++] = '-';
        }
    }
    dst[j] = '\0';

    while (j > 0 && dst[j - 1] == '-') {
        dst[--j] = '\0';
    }
}

// Keep derived filenames in sync with current auto-naming settings.
// This refresh does NOT append capture-start timestamp (that is applied when recording starts).
void gui_settings_refresh_auto_names(gui_settings_t *settings) {
    if (!settings) return;
    if (!settings->auto_names_enabled) return;

    const char *base = settings->output_base_name[0] ? settings->output_base_name : "capture";

    if (settings->use_flac) {
        uint8_t bits_a = clamp_rf_bits_flac(settings->rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(settings->rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (settings->enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), settings->resample_rate_a);
        if (settings->enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), settings->resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), settings->rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), settings->rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.flac", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.flac", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.flac", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.flac", base, (unsigned)bits_b);
        }
    } else {
        uint8_t bits_a = rf_bits_for_raw(settings->rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(settings->rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (settings->enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), settings->resample_rate_a);
        if (settings->enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), settings->resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), settings->rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), settings->rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.raw", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(settings->output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.raw", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.raw", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(settings->output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.raw", base, (unsigned)bits_b);
        }
    }

    char audio_tag_4ch[40] = {0};
    char audio_tag_12[40] = {0};
    char audio_tag_34[40] = {0};
    sanitize_tag(audio_tag_4ch, sizeof(audio_tag_4ch), settings->audio_output_tags[0]);
    sanitize_tag(audio_tag_12, sizeof(audio_tag_12), settings->audio_output_tags[1]);
    sanitize_tag(audio_tag_34, sizeof(audio_tag_34), settings->audio_output_tags[2]);

    if (audio_tag_4ch[0]) {
        snprintf(settings->audio_4ch_filename, MAX_FILENAME_LEN, "%s_%s_quad_4ch.wav", base, audio_tag_4ch);
    } else {
        snprintf(settings->audio_4ch_filename, MAX_FILENAME_LEN, "%s_quad_4ch.wav", base);
    }

    /* Reference video. Deliberately no codec in the name: it is discoverable
     * from the file, and putting it here would make the two auto-name
     * functions diverge if the codec changed between a settings refresh and a
     * record start. Container is always .mkv. */
    char video_tag[40] = {0};
    sanitize_tag(video_tag, sizeof(video_tag), settings->video_output_tag);
    if (video_tag[0]) {
        snprintf(settings->video_filename, MAX_FILENAME_LEN, "%s_%s_video.mkv", base, video_tag);
    } else {
        snprintf(settings->video_filename, MAX_FILENAME_LEN, "%s_video.mkv", base);
    }
    if (audio_tag_12[0]) {
        snprintf(settings->audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch1_ch2.wav", base, audio_tag_12);
    } else {
        snprintf(settings->audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_stereo_ch1_ch2.wav", base);
    }
    if (audio_tag_34[0]) {
        snprintf(settings->audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch3_ch4.wav", base, audio_tag_34);
    } else {
        snprintf(settings->audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_stereo_ch3_ch4.wav", base);
    }

    for (int i = 0; i < 4; i++) {
        char tag[40];
        sanitize_tag(tag, sizeof(tag), settings->audio_1ch_labels[i]);
        if (tag[0]) {
            snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_%s_audio_ch%d.wav", base, tag, i + 1);
        } else {
            snprintf(settings->audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_audio_ch%d.wav", base, i + 1);
        }
    }
}

/* ============================================================================
 * Parse helpers
 * ============================================================================ */
static void set_err(char *err, size_t errcap, const char *msg) {
    if (err && errcap) snprintf(err, errcap, "%s", msg);
}

static void *field_ptr(gui_settings_t *s, const gui_setting_desc_t *d) {
    return (char *)s + d->offset;
}

static const void *field_cptr(const gui_settings_t *s, const gui_setting_desc_t *d) {
    return (const char *)s + d->offset;
}

static size_t field_size(const gui_setting_desc_t *d) {
    switch ((gui_setting_type_t)d->type) {
        case GS_BOOL:  return sizeof(bool);
        case GS_INT:   return sizeof(int);
        case GS_U8:    return sizeof(uint8_t);
        case GS_U16:   return sizeof(uint16_t);
        case GS_U32:   return sizeof(uint32_t);
        case GS_U64:   return sizeof(uint64_t);
        case GS_FLOAT: return sizeof(float);
        case GS_STR:   return d->cap;
    }
    return 0;
}

/* The file loader treats anything that is not "true" as false; the network
 * path insists on the two spellings. */
static bool parse_bool_text(const char *value, bool strict, bool *out, char *err, size_t errcap) {
    if (strcmp(value, "true") == 0) { *out = true; return true; }
    if (strcmp(value, "false") == 0) { *out = false; return true; }
    if (strict) { set_err(err, errcap, "expected true or false"); return false; }
    *out = false;
    return true;
}

/* atoi()/atol() semantics for the file (leading number, garbage tolerated);
 * a fully consumed decimal for the network. */
static bool parse_int_text(const char *value, bool strict, long long *out, char *err, size_t errcap) {
    errno = 0;
    char *end = NULL;
    long long v = strtoll(value, &end, 10);
    if (strict && (end == value || *end != '\0' || errno == ERANGE)) {
        set_err(err, errcap, "expected an integer");
        return false;
    }
    if (end == value) v = 0;
    *out = v;
    return true;
}

static bool parse_uint_text(const char *value, bool strict, unsigned long long *out, char *err, size_t errcap) {
    errno = 0;
    char *end = NULL;
    if (strict && value[0] == '-') {
        set_err(err, errcap, "expected an unsigned integer");
        return false;
    }
    unsigned long long v = strtoull(value, &end, 10);
    if (strict && (end == value || *end != '\0' || errno == ERANGE)) {
        set_err(err, errcap, "expected an unsigned integer");
        return false;
    }
    if (end == value) v = 0;
    *out = v;
    return true;
}

static bool parse_float_text(const char *value, bool strict, float *out, char *err, size_t errcap) {
    errno = 0;
    char *end = NULL;
    double v = strtod(value, &end);
    if (strict && (end == value || *end != '\0' || errno == ERANGE)) {
        set_err(err, errcap, "expected a number");
        return false;
    }
    if (end == value) v = 0.0;
    *out = (float)v;
    return true;
}

/* The file loader truncates silently; the network path refuses anything the
 * raw file dialect could not hold (a quote or a line break ends the value
 * early on the next load) and anything that would be cut. */
static bool store_string(char *dst, size_t cap, const char *value, bool strict, char *err, size_t errcap) {
    size_t len = strlen(value);
    if (strict) {
        if (len >= cap) { set_err(err, errcap, "value too long"); return false; }
        if (strchr(value, '"') || strchr(value, '\n') || strchr(value, '\r')) {
            set_err(err, errcap, "value may not contain a quote or a line break");
            return false;
        }
    }
    if (len >= cap) len = cap - 1;
    memcpy(dst, value, len);
    dst[len] = '\0';
    return true;
}

static bool apply_generic(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                          bool strict, char *err, size_t errcap) {
    void *p = field_ptr(s, d);
    switch ((gui_setting_type_t)d->type) {
        case GS_BOOL:
            return parse_bool_text(value, strict, (bool *)p, err, errcap);
        case GS_INT: {
            long long v;
            if (!parse_int_text(value, strict, &v, err, errcap)) return false;
            if (strict && (v < INT_MIN || v > INT_MAX)) { set_err(err, errcap, "out of range"); return false; }
            *(int *)p = (int)v;
            return true;
        }
        case GS_U8: {
            long long v;
            if (!parse_int_text(value, strict, &v, err, errcap)) return false;
            if (strict && (v < 0 || v > 255)) { set_err(err, errcap, "out of range"); return false; }
            *(uint8_t *)p = (uint8_t)v;
            return true;
        }
        case GS_U16: {
            long long v;
            if (!parse_int_text(value, strict, &v, err, errcap)) return false;
            if (strict && (v < 0 || v > 65535)) { set_err(err, errcap, "out of range"); return false; }
            *(uint16_t *)p = (uint16_t)v;
            return true;
        }
        case GS_U32: {
            unsigned long long v;
            if (!parse_uint_text(value, strict, &v, err, errcap)) return false;
            if (strict && v > UINT32_MAX) { set_err(err, errcap, "out of range"); return false; }
            *(uint32_t *)p = (uint32_t)v;
            return true;
        }
        case GS_U64: {
            unsigned long long v;
            if (!parse_uint_text(value, strict, &v, err, errcap)) return false;
            *(uint64_t *)p = (uint64_t)v;
            return true;
        }
        case GS_FLOAT:
            return parse_float_text(value, strict, (float *)p, err, errcap);
        case GS_STR:
            return store_string((char *)p, d->cap, value, strict, err, errcap);
    }
    set_err(err, errcap, "unknown setting type");
    return false;
}

/* ============================================================================
 * Per-key parse hooks: the clamps the file loader has always applied, so a
 * value set over the network gets the same treatment as one read from disk.
 * ============================================================================ */

// Backward-compat migration:
// Earlier builds could permanently prefix output_base_name with a timestamp like:
// - "yy.mm.dd_hh.mm.ss_<name>"
// - "yyyy.mm.dd_hh.mm.ss_<name>"
// That is no longer desired (timestamp is now appended at capture start only for derived filenames).
static void strip_timestamp_prefix_inplace(char *s) {
    if (!s) return;

    // yy.mm.dd_hh.mm.ss_
    const size_t len_yy = 18;
    // yyyy.mm.dd_hh.mm.ss_
    const size_t len_yyyy = 20;

    size_t n = strlen(s);
    if (n <= len_yy) return;

    bool match_yy = (n > len_yy) &&
        (s[2] == '.' && s[5] == '.' && s[8] == '_' && s[11] == '.' && s[14] == '.' && s[17] == '_');

    bool match_yyyy = (n > len_yyyy) &&
        (s[4] == '.' && s[7] == '.' && s[10] == '_' && s[13] == '.' && s[16] == '.' && s[19] == '_');

    if (match_yyyy) {
        memmove(s, s + len_yyyy, n - len_yyyy + 1);
    } else if (match_yy) {
        memmove(s, s + len_yy, n - len_yy + 1);
    }
}

static bool hook_base_name(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                           bool strict, char *err, size_t errcap) {
    if (!store_string(s->output_base_name, sizeof(s->output_base_name), value, strict, err, errcap)) return false;
    strip_timestamp_prefix_inplace(s->output_base_name);
    (void)d;
    return true;
}

/* Legacy single "cxadc_tenbit_mode" key: applies to both cards. The per-card
 * keys, which come later in the table, override it. */
static bool hook_cxadc_tenbit_alias(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                                    bool strict, char *err, size_t errcap) {
    bool mode;
    if (!parse_bool_text(value, strict, &mode, err, errcap)) return false;
    s->cxadc_tenbit_mode_card[0] = mode;
    s->cxadc_tenbit_mode_card[1] = mode;
    (void)d;
    return true;
}

/* The file loader stores whatever number it finds and lets the post-load
 * derivation repair anything that is not 8/12/16; the network path refuses. */
static bool hook_rf_bits(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                         bool strict, char *err, size_t errcap) {
    long long v;
    if (!parse_int_text(value, strict, &v, err, errcap)) return false;
    if (strict && v != 8 && v != 12 && v != 16) { set_err(err, errcap, "expected 8, 12 or 16"); return false; }
    *(uint8_t *)field_ptr(s, d) = (uint8_t)v;
    return true;
}

static bool hook_flac_level(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                            bool strict, char *err, size_t errcap) {
    long long v;
    if (!parse_int_text(value, strict, &v, err, errcap)) return false;
    if (strict && (v < 0 || v > 8)) { set_err(err, errcap, "expected 0..8"); return false; }
    s->flac_level = (int)v;
    (void)d;
    return true;
}

static bool hook_memory_budget(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                               bool strict, char *err, size_t errcap) {
    long long gb;
    if (!parse_int_text(value, strict, &gb, err, errcap)) return false;
    if (gb < 1) gb = 4;
    if (gb > 16) gb = 16;
    s->memory_budget_gb = (uint32_t)gb;
    (void)d;
    return true;
}

static bool hook_video_codec(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                             bool strict, char *err, size_t errcap) {
    long long c;
    if (!parse_int_text(value, strict, &c, err, errcap)) return false;
    /* Clamp: a hand-edited file must not select a codec that does not exist. */
    s->video_record_codec = (c == 1) ? 1 : 0;
    (void)d;
    return true;
}

static bool hook_rtsp_port(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                           bool strict, char *err, size_t errcap) {
    long long ll;
    if (!parse_int_text(value, strict, &ll, err, errcap)) return false;
    int p = (int)ll;
    /* Clamp to a usable, unprivileged port. A hand-edited 0 means "default",
     * and anything below 1024 would need root we do not have. */
    s->rtsp_stream_port = (p == 0 || (p >= 1024 && p <= 65535)) ? p : 0;
    (void)d;
    return true;
}

static bool hook_rtsp_encoder(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                              bool strict, char *err, size_t errcap) {
    long long ll;
    if (!parse_int_text(value, strict, &ll, err, errcap)) return false;
    int e = (int)ll;
    s->rtsp_stream_encoder = (e >= 0 && e <= 2) ? e : 0;
    (void)d;
    return true;
}

static bool hook_rtsp_bitrate(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                              bool strict, char *err, size_t errcap) {
    long long ll;
    if (!parse_int_text(value, strict, &ll, err, errcap)) return false;
    int b = (int)ll;
    /* 0 means "the module's default"; a silly value would otherwise produce
     * a stream nobody can watch. */
    s->rtsp_stream_bitrate_kbps = (b == 0 || (b >= 100 && b <= 100000)) ? b : 0;
    (void)d;
    return true;
}

static bool hook_ui_scale(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                          bool strict, char *err, size_t errcap) {
    (void)d; (void)strict; (void)err; (void)errcap;
    s->ui_scale_percent = gui_ui_scale_parse_percent(value);
    return true;
}

#ifdef ENABLE_DDD
static bool hook_ddd_decimation(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                                bool strict, char *err, size_t errcap) {
    long long ll;
    if (!parse_int_text(value, strict, &ll, err, errcap)) return false;
    uint8_t factor = (uint8_t)ll;
    if (ddd_decimation_is_supported(factor)) {
        s->ddd_decimation = factor;
    } else if (strict) {
        set_err(err, errcap, "unsupported decimation factor");
        return false;
    }
    (void)d;
    return true;
}
#endif

static bool hook_net_mode(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                          bool strict, char *err, size_t errcap) {
    long long ll;
    if (!parse_int_text(value, strict, &ll, err, errcap)) return false;
    int mode = (int)ll;
    if (mode < 0 || mode > 2) mode = 0;
    s->net_mode = mode;
    (void)d;
    return true;
}

static bool hook_port(const gui_setting_desc_t *d, gui_settings_t *s, const char *value,
                      bool strict, char *err, size_t errcap) {
    long long p;
    if (!parse_int_text(value, strict, &p, err, errcap)) return false;
    if (p < 1 || p > 65535) p = 8080;
    *(uint16_t *)field_ptr(s, d) = (uint16_t)p;
    return true;
}

/* ============================================================================
 * The table. Order = the order the file has always been written.
 * ============================================================================ */
#define GS_ENTRY(k, t, p, fl, member, capv, fn) \
    { k, (uint8_t)(t), (uint8_t)(p), (uint16_t)(fl), (uint32_t)offsetof(gui_settings_t, member), (uint32_t)(capv), fn }
#define GS_B(k, member, fl)        GS_ENTRY(k, GS_BOOL,  0, fl, member, 0, NULL)
#define GS_I(k, member, fl)        GS_ENTRY(k, GS_INT,   0, fl, member, 0, NULL)
#define GS_IH(k, member, fl, fn)   GS_ENTRY(k, GS_INT,   0, fl, member, 0, fn)
#define GS_U8(k, member, fl)       GS_ENTRY(k, GS_U8,    0, fl, member, 0, NULL)
#define GS_U8H(k, member, fl, fn)  GS_ENTRY(k, GS_U8,    0, fl, member, 0, fn)
#define GS_U16H(k, member, fl, fn) GS_ENTRY(k, GS_U16,   0, fl, member, 0, fn)
#define GS_U32(k, member, fl)      GS_ENTRY(k, GS_U32,   0, fl, member, 0, NULL)
#define GS_U32H(k, member, fl, fn) GS_ENTRY(k, GS_U32,   0, fl, member, 0, fn)
#define GS_U64(k, member, fl)      GS_ENTRY(k, GS_U64,   0, fl, member, 0, NULL)
#define GS_F(k, member, p, fl)     GS_ENTRY(k, GS_FLOAT, p, fl, member, 0, NULL)
#define GS_S(k, member, fl)        GS_ENTRY(k, GS_STR,   0, fl, member, sizeof(((gui_settings_t *)0)->member), NULL)
#define GS_SH(k, member, fl, fn)   GS_ENTRY(k, GS_STR,   0, fl, member, sizeof(((gui_settings_t *)0)->member), fn)
#define GS_BH(k, member, fl, fn)   GS_ENTRY(k, GS_BOOL,  0, fl, member, 0, fn)

#define GS_LOCAL GS_CLIENT_LOCAL
#define GS_NAME  GS_AUTO_NAME_INPUT

static const gui_setting_desc_t s_table[] = {
    GS_I  ("device_index",                     device_index,                GS_NO_REMOTE_SET),
    GS_S  ("output_path",                      output_path,                 0),
    GS_B  ("auto_names_enabled",               auto_names_enabled,          GS_NAME),
    GS_SH ("output_base_name",                 output_base_name,            GS_NAME, hook_base_name),
    GS_B  ("append_timestamp_on_capture_start", append_timestamp_on_capture_start, 0),
    GS_U8H("rf_bits_a",                        rf_bits_a,                   GS_NAME, hook_rf_bits),
    GS_U8H("rf_bits_b",                        rf_bits_b,                   GS_NAME, hook_rf_bits),
    /* Legacy single key, applied before the per-card keys so they win. */
    GS_BH ("cxadc_tenbit_mode",                cxadc_tenbit_mode_card[0],   GS_LOAD_ONLY, hook_cxadc_tenbit_alias),
    GS_B  ("cxadc_tenbit_mode_a",              cxadc_tenbit_mode_card[0],   0),
    GS_B  ("cxadc_tenbit_mode_b",              cxadc_tenbit_mode_card[1],   0),
    GS_S  ("rf_tag_a",                         rf_channel_tags[0],          GS_NAME),
    GS_S  ("rf_tag_b",                         rf_channel_tags[1],          GS_NAME),
    GS_S  ("output_filename_a",                output_filename_a,           0),
    GS_S  ("output_filename_b",                output_filename_b,           0),
    GS_B  ("capture_a",                        capture_a,                   0),
    GS_B  ("capture_b",                        capture_b,                   0),
    GS_U64("sample_count",                     sample_count,                GS_WRITE_ONLY),
    GS_S  ("capture_time",                     capture_time,                GS_WRITE_ONLY),
    GS_B  ("overwrite_files",                  overwrite_files,             0),
    GS_S  ("aux_filename",                     aux_filename,                GS_WRITE_ONLY),
    GS_S  ("raw_filename",                     raw_filename,                GS_WRITE_ONLY),

    // Audio filenames + enables (mirror CLI options)
    GS_S  ("audio_4ch_filename",               audio_4ch_filename,          0),
    GS_S  ("video_filename",                   video_filename,              0),
    GS_S  ("video_output_tag",                 video_output_tag,            GS_NAME),
    GS_S  ("ffmpeg_path",                      ffmpeg_path,                 0),
    GS_S  ("audio_2ch_12_filename",            audio_2ch_12_filename,       0),
    GS_S  ("audio_2ch_34_filename",            audio_2ch_34_filename,       0),
    GS_S  ("audio_1ch_1_filename",             audio_1ch_filenames[0],      0),
    GS_S  ("audio_1ch_2_filename",             audio_1ch_filenames[1],      0),
    GS_S  ("audio_1ch_3_filename",             audio_1ch_filenames[2],      0),
    GS_S  ("audio_1ch_4_filename",             audio_1ch_filenames[3],      0),
    GS_S  ("audio_1ch_1_label",                audio_1ch_labels[0],         GS_NAME),
    GS_S  ("audio_1ch_2_label",                audio_1ch_labels[1],         GS_NAME),
    GS_S  ("audio_1ch_3_label",                audio_1ch_labels[2],         GS_NAME),
    GS_S  ("audio_1ch_4_label",                audio_1ch_labels[3],         GS_NAME),
    GS_S  ("audio_tag_4ch",                    audio_output_tags[0],        GS_NAME),
    GS_S  ("audio_tag_2ch_12",                 audio_output_tags[1],        GS_NAME),
    GS_S  ("audio_tag_2ch_34",                 audio_output_tags[2],        GS_NAME),

    GS_B  ("enable_audio_4ch",                 enable_audio_4ch,            0),
    GS_B  ("video_record_enabled",             video_record_enabled,        0),
    GS_IH ("video_record_codec",               video_record_codec,          0, hook_video_codec),
    GS_S  ("preview_device_path",              preview_device_path,         0),
    GS_B  ("rtsp_stream_enabled",              rtsp_stream_enabled,         0),
    GS_B  ("rtsp_stream_lan",                  rtsp_stream_lan,             0),
    GS_IH ("rtsp_stream_port",                 rtsp_stream_port,            0, hook_rtsp_port),
    GS_IH ("rtsp_stream_encoder",              rtsp_stream_encoder,         0, hook_rtsp_encoder),
    GS_IH ("rtsp_stream_bitrate_kbps",         rtsp_stream_bitrate_kbps,    0, hook_rtsp_bitrate),
    GS_B  ("rtsp_stream_deinterlace",          rtsp_stream_deinterlace,     0),
    GS_B  ("rtsp_lan_acknowledged",            rtsp_lan_acknowledged,       0),
    GS_B  ("rtsp_stream_password",             rtsp_stream_password,        0),
    GS_S  ("mediamtx_path",                    mediamtx_path,               0),
    GS_S  ("rtsp_audio_device",                rtsp_audio_device,           0),
    GS_B  ("enable_audio_2ch_12",              enable_audio_2ch_12,         0),
    GS_B  ("enable_audio_2ch_34",              enable_audio_2ch_34,         0),
    GS_B  ("audio_monitor_playback",           audio_monitor_playback,      GS_LOCAL),
    GS_B  ("audio_monitor_ch34",               audio_monitor_ch34,          GS_LOCAL),
    GS_B  ("misrc_mode",                       misrc_mode,                  0),
    GS_B  ("misrc_v15_v25_ab_swap",            misrc_v15_v25_ab_swap,       0),
    GS_B  ("stop_on_dropout",                  stop_on_dropout,             0),
    GS_B  ("level_autostop_enabled",           level_autostop_enabled,      0),
    GS_S  ("level_autostop_level_str",         level_autostop_level_str,    0),
    GS_S  ("level_autostop_duration_str",      level_autostop_duration_str, 0),
    GS_S  ("ingest_project",                   ingest_project,              0),
    GS_S  ("ingest_tape_id",                   ingest_tape_id,              0),
    GS_S  ("ingest_tape_format",               ingest_tape_format,          0),
    GS_S  ("ingest_tape_size",                 ingest_tape_size,            0),
    GS_S  ("ingest_tape_speed",                ingest_tape_speed,           0),
    GS_S  ("ingest_tape_condition",            ingest_tape_condition,       0),
    GS_S  ("ingest_operator",                  ingest_operator,             0),
    GS_S  ("ingest_location",                  ingest_location,             0),
    GS_S  ("ingest_notes",                     ingest_notes,                0),
    GS_B  ("enable_audio_1ch_1",               enable_audio_1ch[0],         0),
    GS_B  ("enable_audio_1ch_2",               enable_audio_1ch[1],         0),
    GS_B  ("enable_audio_1ch_3",               enable_audio_1ch[2],         0),
    GS_B  ("enable_audio_1ch_4",               enable_audio_1ch[3],         0),

    GS_B  ("pad_lower_bits",                   pad_lower_bits,              GS_WRITE_ONLY),
    GS_B  ("show_peak_levels",                 show_peak_levels,            GS_WRITE_ONLY),
    GS_B  ("suppress_clip_a",                  suppress_clip_a,             GS_WRITE_ONLY),
    GS_B  ("suppress_clip_b",                  suppress_clip_b,             GS_WRITE_ONLY),
    GS_B  ("reduce_8bit_a",                    reduce_8bit_a,               0),
    GS_B  ("reduce_8bit_b",                    reduce_8bit_b,               0),
    GS_B  ("enable_resample_a",                enable_resample_a,           GS_NAME),
    GS_B  ("enable_resample_b",                enable_resample_b,           GS_NAME),
    GS_F  ("resample_rate_a",                  resample_rate_a,             1, GS_NAME),
    GS_F  ("resample_rate_b",                  resample_rate_b,             1, GS_NAME),
    GS_I  ("resample_quality_a",               resample_quality_a,          0),
    GS_I  ("resample_quality_b",               resample_quality_b,          0),
    GS_F  ("resample_gain_a",                  resample_gain_a,             1, 0),
    GS_F  ("resample_gain_b",                  resample_gain_b,             1, 0),
#ifdef ENABLE_DDD
    GS_U8H("ddd_decimation",                   ddd_decimation,              0, hook_ddd_decimation),
#endif
    GS_B  ("use_flac",                         use_flac,                    GS_NAME),
    GS_B  ("flac_12bit",                       flac_12bit,                  0),
    GS_IH ("flac_level",                       flac_level,                  0, hook_flac_level),
    GS_B  ("flac_verification",                flac_verification,           0),
    GS_I  ("flac_threads",                     flac_threads,                0),
    GS_B  ("flac_affinity_enabled",            flac_affinity_enabled,       0),
    GS_S  ("flac_affinity_cpu_list",           flac_affinity_cpu_list,      0),
    GS_B  ("show_grid",                        show_grid,                   GS_LOCAL),
    GS_F  ("time_scale",                       time_scale,                  2, GS_LOCAL),
    GS_F  ("amplitude_scale",                  amplitude_scale,             2, GS_LOCAL),
    GS_IH ("ui_scale_percent",                 ui_scale_percent,            GS_LOCAL, hook_ui_scale),
    GS_B  ("ui_scale_auto",                    ui_scale_auto,               GS_LOCAL),
    GS_B  ("discover_simple_capture",          discover_simple_capture,     0),
    GS_B  ("show_core_pinning_in_settings",    show_core_pinning_in_settings, GS_LOCAL),
    GS_U32H("memory_budget_gb",                memory_budget_gb,            GS_LOCAL, hook_memory_budget),
    GS_U64("update_last_check_unix_s",         update_last_check_unix_s,    GS_LOCAL),
    GS_S  ("update_last_release_tag",          update_last_release_tag,     GS_LOCAL),
    GS_B  ("update_available_cached",          update_available_cached,     GS_LOCAL),
    GS_U64("rtlsdr_freq_hz",                   rtlsdr_freq_hz,              0),
    GS_I  ("rtlsdr_gain_mode",                 rtlsdr_gain_mode,            0),
    GS_I  ("rtlsdr_gain_tenths_db",            rtlsdr_gain_tenths_db,       0),
    GS_U32("rtlsdr_sample_rate_hz",            rtlsdr_sample_rate_hz,       0),
    GS_B  ("rtlsdr_agc",                       rtlsdr_agc,                  0),
    GS_B  ("rtlsdr_offset_corr",               rtlsdr_offset_corr,          0),
    GS_I  ("demod_mode",                       demod_mode,                  GS_LOCAL),
    GS_I  ("demod_bandwidth_hz",               demod_bandwidth_hz,          GS_LOCAL),
    GS_F  ("demod_squelch",                    demod_squelch,               3, GS_LOCAL),
    GS_F  ("demod_volume",                     demod_volume,                3, GS_LOCAL),
    GS_I  ("demod_output_pair",                demod_output_pair,           GS_LOCAL),
    // Server/Client networking (cxadc_vhs_server-style peer mode).
    GS_IH ("net_mode",                         net_mode,                    GS_LOCAL, hook_net_mode),
    GS_U16H("net_server_port",                 net_server_port,             GS_LOCAL, hook_port),
    GS_S  ("net_server_port_str",              net_server_port_str,         GS_LOCAL),
    GS_S  ("net_client_host",                  net_client_host,             GS_LOCAL),
    GS_U16H("net_client_port",                 net_client_port,             GS_LOCAL, hook_port),
    GS_S  ("net_client_port_str",              net_client_port_str,         GS_LOCAL),
    GS_S  ("playback_file_a",                  playback_file_a,             GS_LOCAL),
    GS_S  ("playback_file_b",                  playback_file_b,             GS_LOCAL),
};

#define GS_TABLE_COUNT (sizeof(s_table) / sizeof(s_table[0]))

const gui_setting_desc_t *gui_settings_table(size_t *count) {
    if (count) *count = GS_TABLE_COUNT;
    return s_table;
}

const gui_setting_desc_t *gui_settings_find(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < GS_TABLE_COUNT; i++) {
        if (strcmp(s_table[i].key, key) == 0) return &s_table[i];
    }
    return NULL;
}

/* ============================================================================
 * Formatting
 * ============================================================================ */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;   /* length the full text needs (may exceed cap) */
} sink_t;

static void sink_put(sink_t *k, const char *text, size_t n) {
    if (k->buf && k->len < k->cap) {
        size_t room = k->cap - k->len - 1;
        size_t take = n < room ? n : room;
        memcpy(k->buf + k->len, text, take);
        k->buf[k->len + take] = '\0';
    }
    k->len += n;
}

static void sink_puts(sink_t *k, const char *text) {
    sink_put(k, text, strlen(text));
}

static void sink_printf(sink_t *k, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    sink_put(k, tmp, (size_t)n);
}

/* The bare value: numbers and booleans as they appear in the file, strings
 * without their quotes. */
static void format_bare(sink_t *k, const gui_settings_t *s, const gui_setting_desc_t *d) {
    const void *p = field_cptr(s, d);
    switch ((gui_setting_type_t)d->type) {
        case GS_BOOL:  sink_puts(k, *(const bool *)p ? "true" : "false"); break;
        case GS_INT:   sink_printf(k, "%d", *(const int *)p); break;
        case GS_U8:    sink_printf(k, "%u", (unsigned)*(const uint8_t *)p); break;
        case GS_U16:   sink_printf(k, "%u", (unsigned)*(const uint16_t *)p); break;
        case GS_U32:   sink_printf(k, "%u", (unsigned)*(const uint32_t *)p); break;
        case GS_U64:   sink_printf(k, "%llu", (unsigned long long)*(const uint64_t *)p); break;
        case GS_FLOAT: sink_printf(k, "%.*f", (int)d->prec, (double)*(const float *)p); break;
        case GS_STR:   sink_puts(k, (const char *)p); break;
    }
}

bool gui_settings_format_value(const gui_settings_t *s, const gui_setting_desc_t *d,
                               char *out, size_t cap) {
    if (!s || !d || !out || cap == 0) return false;
    sink_t k = { out, cap, 0 };
    out[0] = '\0';
    format_bare(&k, s, d);
    return k.len < cap;
}

size_t gui_settings_format_file(const gui_settings_t *s, char *buf, size_t cap) {
    sink_t k = { buf, cap, 0 };
    if (buf && cap) buf[0] = '\0';
    if (!s) return 0;

    sink_puts(&k, "{\n");
    bool first = true;
    for (size_t i = 0; i < GS_TABLE_COUNT; i++) {
        const gui_setting_desc_t *d = &s_table[i];
        if (d->flags & GS_LOAD_ONLY) continue;
        if (!first) sink_puts(&k, ",\n");
        first = false;
        sink_puts(&k, "  \"");
        sink_puts(&k, d->key);
        sink_puts(&k, "\": ");
        if (d->type == GS_STR) sink_puts(&k, "\"");
        format_bare(&k, s, d);
        if (d->type == GS_STR) sink_puts(&k, "\"");
    }
    sink_puts(&k, "\n}\n");
    return k.len;
}

static void sink_put_json_string(sink_t *k, const char *text) {
    sink_puts(k, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '"':  sink_puts(k, "\\\""); break;
            case '\\': sink_puts(k, "\\\\"); break;
            case '\n': sink_puts(k, "\\n"); break;
            case '\r': sink_puts(k, "\\r"); break;
            case '\t': sink_puts(k, "\\t"); break;
            default:
                if (*p < 0x20) {
                    sink_printf(k, "\\u%04x", (unsigned)*p);
                } else {
                    sink_put(k, (const char *)p, 1);
                }
                break;
        }
    }
    sink_puts(k, "\"");
}

size_t gui_settings_json_escape(const char *text, char *out, size_t cap) {
    sink_t k = { out, cap, 0 };
    if (out && cap) out[0] = '\0';
    if (!text) return 0;
    /* sink_put_json_string wraps in quotes; emit the same escapes bare. */
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
            case '"':  sink_puts(&k, "\\\""); break;
            case '\\': sink_puts(&k, "\\\\"); break;
            case '\n': sink_puts(&k, "\\n"); break;
            case '\r': sink_puts(&k, "\\r"); break;
            case '\t': sink_puts(&k, "\\t"); break;
            default:
                if (*p < 0x20) sink_printf(&k, "\\u%04x", (unsigned)*p);
                else sink_put(&k, (const char *)p, 1);
                break;
        }
    }
    return k.len;
}

size_t gui_settings_to_json(const gui_settings_t *s, unsigned exclude_flags,
                            char *buf, size_t cap) {
    sink_t k = { buf, cap, 0 };
    if (buf && cap) buf[0] = '\0';
    if (!s) return 0;

    sink_puts(&k, "{");
    bool first = true;
    for (size_t i = 0; i < GS_TABLE_COUNT; i++) {
        const gui_setting_desc_t *d = &s_table[i];
        if (d->flags & GS_LOAD_ONLY) continue;
        if (d->flags & exclude_flags) continue;
        if (!first) sink_puts(&k, ",");
        first = false;
        sink_puts(&k, "\"");
        sink_puts(&k, d->key);
        sink_puts(&k, "\":");
        if (d->type == GS_STR) {
            sink_put_json_string(&k, (const char *)field_cptr(s, d));
        } else {
            format_bare(&k, s, d);
        }
    }
    sink_puts(&k, "}");
    return k.len;
}

/* ============================================================================
 * Parsing
 * ============================================================================ */
static bool is_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool gui_settings_find_value(const char *content, const char *key,
                             char *out, size_t cap, bool json) {
    if (!content || !key || !out || cap == 0) return false;
    out[0] = '\0';

    char pat[80];
    int n = snprintf(pat, sizeof(pat), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(pat)) return false;

    /* First occurrence wins, as it always has; the key must not be the tail
     * of a longer key. */
    const char *pos = content;
    for (;;) {
        pos = strstr(pos, pat);
        if (!pos) return false;
        if (pos == content || !is_key_char(pos[-1])) break;
        pos++;
    }
    pos += (size_t)n;
    while (*pos == ' ' || *pos == '\t') pos++; // Skip whitespace

    if (*pos == '"') {
        // String value
        pos++;
        size_t j = 0;
        while (*pos && *pos != '"') {
            char c = *pos++;
            if (json && c == '\\' && *pos) {
                char e = *pos++;
                switch (e) {
                    case 'n': c = '\n'; break;
                    case 'r': c = '\r'; break;
                    case 't': c = '\t'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        int h[4];
                        bool ok = true;
                        for (int i = 0; i < 4; i++) {
                            h[i] = pos[i] ? hex_nibble(pos[i]) : -1;
                            if (h[i] < 0) ok = false;
                        }
                        if (!ok) return false;
                        unsigned cp = (unsigned)((h[0] << 12) | (h[1] << 8) | (h[2] << 4) | h[3]);
                        pos += 4;
                        c = (cp < 0x80) ? (char)cp : '?';
                        break;
                    }
                    default: c = e; break;   /* \" \\ \/ and anything else */
                }
            }
            if (j + 1 < cap) out[j++] = c;
        }
        if (*pos != '"') { out[0] = '\0'; return false; }   // unterminated
        out[j] = '\0';
        return true;
    }

    // Number or boolean
    const char *end = pos;
    while (*end && *end != ',' && *end != '\n' && *end != '}') end++;
    if (!*end) return false;
    size_t len = (size_t)(end - pos);
    if (len >= cap) len = cap - 1;
    memcpy(out, pos, len);
    out[len] = '\0';
    // Trim trailing whitespace
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r')) {
        out[--len] = '\0';
    }
    return true;
}

int gui_settings_apply_key(gui_settings_t *s, const char *key, const char *value,
                           bool strict, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!s || !key || !value) { set_err(err, errcap, "missing argument"); return -2; }
    const gui_setting_desc_t *d = gui_settings_find(key);
    if (!d) { set_err(err, errcap, "unknown key"); return -1; }
    bool ok = d->parse ? d->parse(d, s, value, strict, err, errcap)
                       : apply_generic(d, s, value, strict, err, errcap);
    return ok ? 0 : -2;
}

void gui_settings_post_load(gui_settings_t *settings) {
    if (!settings) return;

    // Duration limits: feature removed, ignore saved values and force to 0
    settings->capture_limit_seconds = 0;
    settings->record_limit_seconds = 0;

    // Re-sync port string mirrors if the numeric port was loaded but the
    // string mirror was absent/empty (older settings files).
    if (settings->net_server_port_str[0] == '\0') {
        snprintf(settings->net_server_port_str, sizeof(settings->net_server_port_str), "%u",
                 (unsigned)settings->net_server_port);
    }
    if (settings->net_client_port_str[0] == '\0') {
        snprintf(settings->net_client_port_str, sizeof(settings->net_client_port_str), "%u",
                 (unsigned)settings->net_client_port);
    }

    // Backward-compat migration:
    // - If rf_bits_* not present, derive from legacy flags.
    if (settings->rf_bits_a != 8 && settings->rf_bits_a != 12 && settings->rf_bits_a != 16) {
        settings->rf_bits_a = settings->reduce_8bit_a ? 8 : (settings->use_flac && settings->flac_12bit ? 12 : 16);
    }
    if (settings->rf_bits_b != 8 && settings->rf_bits_b != 12 && settings->rf_bits_b != 16) {
        settings->rf_bits_b = settings->reduce_8bit_b ? 8 : (settings->use_flac && settings->flac_12bit ? 12 : 16);
    }

    // Default auto naming to ON if missing.
    // (If the key is not present, defaults already set it true.)
    if (settings->output_base_name[0] == '\0') {
        strcpy(settings->output_base_name, "capture");
    }

    // Keep auto-derived names in sync on startup (RF prefixes are conditional on empty RF tags).
    gui_settings_refresh_auto_names(settings);
}

void gui_settings_parse_text(gui_settings_t *s, const char *content) {
    if (!s || !content) return;
    char value[512];
    for (size_t i = 0; i < GS_TABLE_COUNT; i++) {
        const gui_setting_desc_t *d = &s_table[i];
        if (d->flags & GS_WRITE_ONLY) continue;
        if (!gui_settings_find_value(content, d->key, value, sizeof(value), false)) continue;
        (void)gui_settings_apply_key(s, d->key, value, false, NULL, 0);
    }
    gui_settings_post_load(s);
}

/* ============================================================================
 * Field helpers
 * ============================================================================ */
bool gui_settings_field_equal(const gui_settings_t *a, const gui_settings_t *b,
                              const gui_setting_desc_t *d) {
    if (!a || !b || !d) return false;
    if (d->type == GS_STR) {
        return strncmp((const char *)field_cptr(a, d), (const char *)field_cptr(b, d), d->cap) == 0;
    }
    return memcmp(field_cptr(a, d), field_cptr(b, d), field_size(d)) == 0;
}

void gui_settings_copy_field(gui_settings_t *dst, const gui_settings_t *src,
                             const gui_setting_desc_t *d) {
    if (!dst || !src || !d || dst == src) return;
    memcpy(field_ptr(dst, d), field_cptr(src, d), field_size(d));
}

void gui_settings_copy_fields(gui_settings_t *dst, const gui_settings_t *src,
                              bool client_local) {
    if (!dst || !src || dst == src) return;
    for (size_t i = 0; i < GS_TABLE_COUNT; i++) {
        const gui_setting_desc_t *d = &s_table[i];
        if (d->flags & GS_LOAD_ONLY) continue;
        bool is_local = (d->flags & GS_CLIENT_LOCAL) != 0;
        if (is_local != client_local) continue;
        memcpy(field_ptr(dst, d), field_cptr(src, d), field_size(d));
    }
}

/* ============================================================================
 * Generation counter and save suspension
 * ============================================================================ */
static atomic_uint s_generation = 1;
static bool s_save_suspended = false;
static bool s_save_requested = false;

uint32_t gui_settings_generation(void) {
    return (uint32_t)atomic_load(&s_generation);
}

void gui_settings_bump_generation(void) {
    atomic_fetch_add(&s_generation, 1u);
}

bool gui_settings_suspend_save(bool suspend) {
    if (suspend) {
        s_save_suspended = true;
        s_save_requested = false;
        return false;
    }
    s_save_suspended = false;
    bool requested = s_save_requested;
    s_save_requested = false;
    return requested;
}

bool gui_settings_save_suspended(void) {
    return s_save_suspended;
}

void gui_settings_note_save_requested(void) {
    s_save_requested = true;
}
