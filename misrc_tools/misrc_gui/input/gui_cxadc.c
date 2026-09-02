#include "gui_cxadc.h"

#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../signal/gui_headswitch_lock.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>

#ifndef LIBASOUND_ENABLED
#define LIBASOUND_ENABLED 0
#endif
#if defined(_WIN32)
static int cxadc_win_card_path(int card_idx, char *path_out, size_t path_out_len)
{
    if (!path_out || path_out_len == 0) return -1;
    if (card_idx < 0 || card_idx >= 2) return -1;
    int n = snprintf(path_out, path_out_len, "\\\\.\\cxadc%d", card_idx);
    if (n <= 0 || (size_t)n >= path_out_len) return -1;
    return 0;
}

static void cxadc_win_trim(char *s)
{
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static int cxadc_win_run_ps_readline(const char *script, char *line_out, size_t line_out_len)
{
    if (!script || !line_out || line_out_len == 0) return -1;
    line_out[0] = '\0';

    char command[2048];
    int n = snprintf(command,
                     sizeof(command),
                     "powershell -NoProfile -ExecutionPolicy Bypass -Command \"%s\"",
                     script);
    if (n <= 0 || (size_t)n >= sizeof(command)) return -1;

    FILE *pipe = _popen(command, "r");
    if (!pipe) return -1;

    bool got_line = false;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        cxadc_win_trim(buf);
        if (buf[0] == '\0') continue;
        snprintf(line_out, line_out_len, "%s", buf);
        got_line = true;
        break;
    }

    int rc = _pclose(pipe);
    if (rc != 0 || !got_line) return -1;
    return 0;
}
static bool cxadc_win_ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int cxadc_win_parse_bool(const char *value, bool *out_bool)
{
    if (!value || !out_bool) return -1;
    if (cxadc_win_ieq(value, "true") || strcmp(value, "1") == 0) {
        *out_bool = true;
        return 0;
    }
    if (cxadc_win_ieq(value, "false") || strcmp(value, "0") == 0) {
        *out_bool = false;
        return 0;
    }
    return -1;
}

static int cxadc_win_get_center_offset(int card_idx, int *value_out)
{
    if (!value_out) return -1;
    char card_path[64];
    if (cxadc_win_card_path(card_idx, card_path, sizeof(card_path)) != 0) return -1;

    char script[1024];
    int n = snprintf(script,
                     sizeof(script),
                     "$ErrorActionPreference='Stop'; Import-Module CxadcWin -ErrorAction Stop; "
                     "(Get-CxadcWinConfig -Path '%s').CenterOffset",
                     card_path);
    if (n <= 0 || (size_t)n >= sizeof(script)) return -1;

    char line[128];
    if (cxadc_win_run_ps_readline(script, line, sizeof(line)) != 0) return -1;

    errno = 0;
    char *endptr = NULL;
    long parsed = strtol(line, &endptr, 10);
    if (errno != 0 || endptr == line) return -1;
    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0' || parsed < INT_MIN || parsed > INT_MAX) return -1;
    *value_out = (int)parsed;
    return 0;
}

static int cxadc_win_set_center_offset(int card_idx, int value)
{
    char card_path[64];
    if (cxadc_win_card_path(card_idx, card_path, sizeof(card_path)) != 0) return -1;

    char script[1400];
    int n = snprintf(script,
                     sizeof(script),
                     "$ErrorActionPreference='Stop'; Import-Module CxadcWin -ErrorAction Stop; "
                     "Set-CxadcWinConfig -Path '%s' -CenterOffset %d | Out-Null; "
                     "(Get-CxadcWinConfig -Path '%s').CenterOffset",
                     card_path, value, card_path);
    if (n <= 0 || (size_t)n >= sizeof(script)) return -1;

    char line[128];
    if (cxadc_win_run_ps_readline(script, line, sizeof(line)) != 0) return -1;

    errno = 0;
    char *endptr = NULL;
    long parsed = strtol(line, &endptr, 10);
    if (errno != 0 || endptr == line) return -1;
    while (*endptr && isspace((unsigned char)*endptr)) endptr++;
    if (*endptr != '\0') return -1;
    if ((int)parsed != value) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int cxadc_win_get_tenbit(int card_idx, bool *enabled_out)
{
    if (!enabled_out) return -1;
    char card_path[64];
    if (cxadc_win_card_path(card_idx, card_path, sizeof(card_path)) != 0) return -1;

    char script[1024];
    int n = snprintf(script,
                     sizeof(script),
                     "$ErrorActionPreference='Stop'; Import-Module CxadcWin -ErrorAction Stop; "
                     "(Get-CxadcWinConfig -Path '%s').EnableTenbit",
                     card_path);
    if (n <= 0 || (size_t)n >= sizeof(script)) return -1;

    char line[128];
    if (cxadc_win_run_ps_readline(script, line, sizeof(line)) != 0) return -1;
    return cxadc_win_parse_bool(line, enabled_out);
}

static int cxadc_win_set_tenbit(int card_idx, bool enabled)
{
    char card_path[64];
    if (cxadc_win_card_path(card_idx, card_path, sizeof(card_path)) != 0) return -1;
    const char *enabled_ps = enabled ? "$true" : "$false";

    char script[1400];
    int n = snprintf(script,
                     sizeof(script),
                     "$ErrorActionPreference='Stop'; Import-Module CxadcWin -ErrorAction Stop; "
                     "Set-CxadcWinConfig -Path '%s' -EnableTenbit %s | Out-Null; "
                     "(Get-CxadcWinConfig -Path '%s').EnableTenbit",
                     card_path, enabled_ps, card_path);
    if (n <= 0 || (size_t)n >= sizeof(script)) return -1;

    char line[128];
    if (cxadc_win_run_ps_readline(script, line, sizeof(line)) != 0) return -1;
    bool readback = false;
    if (cxadc_win_parse_bool(line, &readback) != 0) return -1;
    if (readback != enabled) {
        errno = EIO;
        return -1;
    }
    return 0;
}
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Shield raylib symbols from conflicting Win32 API declarations
// (same pattern as gui_capture.c): raylib.h is already included via gui_app.h.
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
// Define PKEY_Device_FriendlyName locally to avoid initguid conflicts.
static const PROPERTYKEY s_PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};
// PKEY_Device_InstanceId: returns device instance path like USB\VID_1209&PID_0001&MI_00\...
static const PROPERTYKEY s_PKEY_Device_InstanceId = {
    { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0xfc } }, 42
};
// MISRC Clockgen USB identifiers (Raspberry Pi Pico + Si5351 clockgen mod).
// Keep both PID values to support mixed firmware generations.
#define MISRC_CLOCKGEN_USB_VID_W L"vid_1209"
#define MISRC_CLOCKGEN_USB_PID_PRIMARY_W L"pid_0002"
#define MISRC_CLOCKGEN_USB_PID_ALT_W L"pid_0001"
#else
#include <unistd.h>
#include <fcntl.h>
#if LIBASOUND_ENABLED
#include <alsa/asoundlib.h>
#endif
#endif

extern volatile atomic_int do_exit;

#define CXADC_MAX_CARDS 2
#define CXADC_SAMPLE_RATE_8BIT_HZ 40000000U
#define CXADC_SAMPLE_RATE_TENBIT_HZ 20000000U
#define CXADC_READ_CHUNK_BYTES 65536
#define CXADC_AUDIO_SAMPLE_RATE_HZ 46875U
#define CXADC_AUDIO_CHANNEL_COUNT 3
#define CXADC_AUDIO_READ_FRAMES 1024
#define CXADC_AUDIO_PACKED_FRAME_BYTES 12

typedef enum {
    CXADC_AUDIO_FMT_NONE = 0,
    CXADC_AUDIO_FMT_S24_3LE,
    CXADC_AUDIO_FMT_S32_LE,
    CXADC_AUDIO_FMT_S16_LE,
    CXADC_AUDIO_FMT_FLOAT32
} cxadc_audio_format_t;

typedef struct {
    gui_app_t *app;
    atomic_bool running;
    thrd_t rf_thread;
    bool rf_thread_started;
    thrd_t audio_thread;
    bool audio_thread_started;
    int card_count;
    bool misrc_clockgen_mode;
    bool tenbit_mode[CXADC_MAX_CARDS];
    uint32_t card_sample_rate_hz[CXADC_MAX_CARDS];
    uint32_t rf_sample_rate_hz;
    // Audio format (shared across platforms)
    cxadc_audio_format_t audio_format;
    size_t audio_sample_bytes;
    uint32_t audio_sample_rate_hz;
    char audio_device_name[64];
    int audio_channels;
#if defined(_WIN32)
    HANDLE card_handles[CXADC_MAX_CARDS];
    // WASAPI clockgen audio capture
    IMMDevice *audio_endpoint;
    IAudioClient *audio_client;
    IAudioCaptureClient *audio_capture;
    bool audio_com_initialized;
#else
    int card_fds[CXADC_MAX_CARDS];
#if LIBASOUND_ENABLED
    snd_pcm_t *audio_pcm;
#endif
#endif
} cxadc_ctx_t;

static cxadc_ctx_t s_cxadc = {0};

#if !defined(_WIN32)
#define CXADC_SYSFS_CARD_MAX 255
#define CXADC_SYSFS_CENTER_OFFSET_MIN 0
#define CXADC_SYSFS_CENTER_OFFSET_MAX 255

static int cxadc_sysfs_build_param_path(int card_idx,
                                        const char *param_name,
                                        char *path_out,
                                        size_t path_out_len)
{
    if (!param_name || !param_name[0] || !path_out || path_out_len == 0) {
        return -1;
    }
    if (card_idx < 0 || card_idx > CXADC_SYSFS_CARD_MAX) {
        return -1;
    }
    int n = snprintf(path_out, path_out_len,
                     "/sys/class/cxadc/cxadc%d/device/parameters/%s",
                     card_idx, param_name);
    if (n <= 0 || (size_t)n >= path_out_len) {
        return -1;
    }
    return 0;
}

static int cxadc_sysfs_read_int_param(int card_idx,
                                      const char *param_name,
                                      int *value_out)
{
    if (!value_out) return -1;

    char path[160];
    if (cxadc_sysfs_build_param_path(card_idx, param_name, path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    errno = 0;
    char *endptr = NULL;
    long parsed = strtol(buf, &endptr, 10);
    if (errno != 0 || endptr == buf) {
        return -1;
    }
    while (*endptr && isspace((unsigned char)*endptr)) {
        endptr++;
    }
    if (*endptr != '\0') {
        return -1;
    }
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return -1;
    }

    *value_out = (int)parsed;
    return 0;
}

static int cxadc_sysfs_write_int_param(int card_idx,
                                       const char *param_name,
                                       int value)
{
    char path[160];
    if (cxadc_sysfs_build_param_path(card_idx, param_name, path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }

    int wrote = fprintf(f, "%d\n", value);
    int close_rc = fclose(f);
    if (wrote <= 0 || close_rc != 0) {
        return -1;
    }
    return 0;
}

/* The cxadc driver's _store() handlers return count unconditionally and never
 * range-check what they were given, so an out-of-range write is accepted
 * silently and can leave the register holding something other than what was
 * asked for. Read the value back so the UI never reports a setting the
 * hardware does not actually have. */
static int cxadc_sysfs_write_int_param_verified(int card_idx,
                                                const char *param_name,
                                                int value)
{
    if (cxadc_sysfs_write_int_param(card_idx, param_name, value) != 0) {
        return -1;
    }

    int readback = 0;
    if (cxadc_sysfs_read_int_param(card_idx, param_name, &readback) != 0) {
        return -1;
    }
    if (readback != value) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}
#endif

int gui_cxadc_get_center_offset(int card_idx, int *value_out)
{
    if (!value_out) return -1;
#if defined(_WIN32)
    return cxadc_win_get_center_offset(card_idx, value_out);
#else
    return cxadc_sysfs_read_int_param(card_idx, "center_offset", value_out);
#endif
}

int gui_cxadc_set_center_offset(int card_idx, int value)
{
#if defined(_WIN32)
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return cxadc_win_set_center_offset(card_idx, value);
#else
    if (value < CXADC_SYSFS_CENTER_OFFSET_MIN) value = CXADC_SYSFS_CENTER_OFFSET_MIN;
    if (value > CXADC_SYSFS_CENTER_OFFSET_MAX) value = CXADC_SYSFS_CENTER_OFFSET_MAX;
    return cxadc_sysfs_write_int_param_verified(card_idx, "center_offset", value);
#endif
}

int gui_cxadc_adjust_center_offset(int card_idx, int delta, int *new_value_out)
{
    int current = 0;
    if (gui_cxadc_get_center_offset(card_idx, &current) != 0) {
        return -1;
    }
    int next = current + delta;
    if (next < 0) next = 0;
    if (next > 255) next = 255;
    if (gui_cxadc_set_center_offset(card_idx, next) != 0) {
        return -1;
    }
    if (new_value_out) {
        *new_value_out = next;
    }
    return 0;
}

int gui_cxadc_get_level(int card_idx, int *value_out)
{
    if (!value_out) return -1;
#if defined(_WIN32)
    (void)card_idx;
    errno = ENOTSUP;
    return -1;
#else
    return cxadc_sysfs_read_int_param(card_idx, "level", value_out);
#endif
}

int gui_cxadc_set_level(int card_idx, int value)
{
#if defined(_WIN32)
    (void)card_idx;
    (void)value;
    errno = ENOTSUP;
    return -1;
#else
    if (value < CXADC_SYSFS_LEVEL_MIN) value = CXADC_SYSFS_LEVEL_MIN;
    if (value > CXADC_SYSFS_LEVEL_MAX) value = CXADC_SYSFS_LEVEL_MAX;
    return cxadc_sysfs_write_int_param_verified(card_idx, "level", value);
#endif
}

int gui_cxadc_adjust_level(int card_idx, int delta, int *new_value_out)
{
    int current = 0;
    if (gui_cxadc_get_level(card_idx, &current) != 0) {
        return -1;
    }
    int next = current + delta;
    if (next < CXADC_SYSFS_LEVEL_MIN) next = CXADC_SYSFS_LEVEL_MIN;
    if (next > CXADC_SYSFS_LEVEL_MAX) next = CXADC_SYSFS_LEVEL_MAX;
    if (gui_cxadc_set_level(card_idx, next) != 0) {
        return -1;
    }
    if (new_value_out) {
        *new_value_out = next;
    }
    return 0;
}

int gui_cxadc_get_tenbit(int card_idx, int *value_out)
{
    if (!value_out) return -1;
#if defined(_WIN32)
    bool enabled = false;
    if (cxadc_win_get_tenbit(card_idx, &enabled) != 0) return -1;
    *value_out = enabled ? 1 : 0;
    return 0;
#else
    int raw = 0;
    if (cxadc_sysfs_read_int_param(card_idx, "tenbit", &raw) != 0) {
        return -1;
    }
    *value_out = (raw != 0) ? 1 : 0;
    return 0;
#endif
}

int gui_cxadc_set_tenbit(int card_idx, bool enabled)
{
#if defined(_WIN32)
    return cxadc_win_set_tenbit(card_idx, enabled);
#else
    return cxadc_sysfs_write_int_param(card_idx, "tenbit", enabled ? 1 : 0);
#endif
}

static int cxadc_apply_tenbit_modes(int card_count, const bool enabled[CXADC_MAX_CARDS])
{
    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;
    for (int i = 0; i < card_count; i++) {
        bool mode = (enabled != NULL) ? enabled[i] : false;

        // Read the card's current tenbit setting first. The cxadc driver
        // defaults to 8-bit (tenbit=0), so when the user wants 8-bit (the
        // common case) and the card is already in 8-bit, skip the sysfs write
        // entirely. This restores the pre-v1.1.6 behaviour where a plain
        // 8-bit capture start needed no sysfs write permission at all: the
        // /sys/class/cxadc/cxadcN/device/parameters/* files are root:root
        // 0644 by default and need a one-time `chgrp video` (+ group memship)
        // to be writable by non-root users. Writing tenbit=0 on every start
        // regressed that by requiring write access for an 8-bit capture.
        int current = -1;
        bool have_current = (gui_cxadc_get_tenbit(i, &current) == 0);
        if (have_current && (((current != 0) ? true : false) == mode)) {
            continue;  // already in the requested mode; no sysfs write needed
        }

        if (gui_cxadc_set_tenbit(i, mode) != 0) {
#if !defined(_WIN32)
            int saved_errno = errno;
            fprintf(stderr, "[CXADC] set_tenbit(card %d, %s) failed: %s (errno %d)\n",
                    i, mode ? "10-bit" : "8-bit", strerror(saved_errno), saved_errno);
            // If the write was denied for permissions AND the requested mode
            // is 8-bit (the driver default), don't abort the whole capture:
            // the card is already in 8-bit (we either read it above or can't
            // read it, in which case read-only capture is still better than
            // refusing to start). Only abort when the user explicitly asked
            // for 10-bit and we can't set it (sample rate/format would be
            // wrong, so capture would be silently misconfigured).
            if ((saved_errno == EACCES || saved_errno == EPERM) && !mode) {
                fprintf(stderr, "[CXADC] 8-bit mode requested, sysfs write denied; "
                                "continuing with card default (no chgrp setup needed for 8-bit)\n");
                continue;
            }
#else
            fprintf(stderr, "[CXADC] set_tenbit(card %d, %s) failed\n",
                    i, mode ? "10-bit" : "8-bit");
#endif
            return -1;
        }
        int readback = -1;
        if (gui_cxadc_get_tenbit(i, &readback) != 0) {
#if !defined(_WIN32)
            fprintf(stderr, "[CXADC] tenbit readback failed on card %d: %s (errno %d)\n",
                    i, strerror(errno), errno);
#endif
            return -1;
        }
        if (((readback != 0) ? true : false) != mode) {
            fprintf(stderr, "[CXADC] tenbit readback mismatch on card %d: expected %s got %s\n",
                    i, mode ? "10-bit" : "8-bit", readback ? "10-bit" : "8-bit");
            errno = EIO;
            return -1;
        }
    }
    return 0;
}
static inline uint32_t cxadc_encode_raw_sample(int16_t sample_a, int16_t sample_b)
{
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;

    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    return ch_a | (ch_b << 20);
}
static inline int16_t cxadc_decode_sample_8bit(uint8_t sample)
{
    return (int16_t)(((int)sample - 128) << 4);
}

static inline int16_t cxadc_decode_sample_tenbit(uint8_t lo, uint8_t hi)
{
    uint16_t raw = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return (int16_t)(((int)raw - 32768) >> 4);
}

static void cxadc_reset_stats(gui_app_t *app, uint32_t rf_sample_rate_hz)
{
    if (!app) return;
    if (rf_sample_rate_hz == 0) {
        rf_sample_rate_hz = CXADC_SAMPLE_RATE_8BIT_HZ;
    }
    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);
    atomic_store(&app->sample_rate, rf_sample_rate_hz);
    atomic_store(&app->audio_sample_rate, CXADC_AUDIO_SAMPLE_RATE_HZ);
    atomic_store(&app->last_callback_time_ms, get_time_ms());
    atomic_store(&app->dropout_stop_requested, false);
    atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    for (int i = 0; i < 4; i++) {
        atomic_store(&app->audio_peak[i], 0);
    }

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;
}

#if defined(_WIN32)
static HANDLE cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "\\\\.\\cxadc%d", card_idx);
    return CreateFileA(path,
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       NULL);
}

static int cxadc_read_card(HANDLE handle, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    DWORD read_bytes = 0;
    BOOL ok = ReadFile(handle, buf, (DWORD)len, &read_bytes, NULL);
    if (!ok) {
        return -1;
    }
    return (int)read_bytes;
}

static void cxadc_close_handle(HANDLE *handle)
{
    if (!handle) return;
    if (*handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = INVALID_HANDLE_VALUE;
    }
}
#else
static int cxadc_open_card(int card_idx)
{
    char path[32];
    snprintf(path, sizeof(path), "/dev/cxadc%d", card_idx);
    return open(path, O_RDONLY);
}

static int cxadc_read_card(int fd, uint8_t *buf, size_t len)
{
    if (!buf || len == 0 || fd < 0) {
        return -1;
    }

    ssize_t r = read(fd, buf, len);
    if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            return 0;
        }
        return -1;
    }
    return (int)r;
}

static void cxadc_close_fd(int *fd)
{
    if (!fd) return;
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}
#endif

static void cxadc_close_cards(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        cxadc_close_handle(&ctx->card_handles[i]);
#else
        cxadc_close_fd(&ctx->card_fds[i]);
#endif
    }
}

static int cxadc_open_cards(cxadc_ctx_t *ctx, int card_count)
{
    if (!ctx) return -1;
    if (card_count < 1) card_count = 1;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    for (int i = 0; i < card_count; i++) {
#if defined(_WIN32)
        ctx->card_handles[i] = cxadc_open_card(i);
        if (ctx->card_handles[i] == INVALID_HANDLE_VALUE) {
            cxadc_close_cards(ctx);
            return -1;
        }
#else
        ctx->card_fds[i] = cxadc_open_card(i);
        if (ctx->card_fds[i] < 0) {
            cxadc_close_cards(ctx);
            return -1;
        }
#endif
    }
    return 0;
}

int gui_cxadc_detect_cards(void)
{
    int count = 0;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
#if defined(_WIN32)
        HANDLE h = cxadc_open_card(i);
        if (h != INVALID_HANDLE_VALUE) {
            count++;
            CloseHandle(h);
        }
#else
        char path[32];
        snprintf(path, sizeof(path), "/dev/cxadc%d", i);
        if (access(path, R_OK) == 0 || access(path, W_OK) == 0) {
            count++;
        }
#endif
    }
    return count;
}

static inline void cxadc_store_s24le(uint8_t *dst, int32_t sample)
{
    if (sample > 8388607) sample = 8388607;
    if (sample < -8388608) sample = -8388608;
    uint32_t raw = (uint32_t)(sample & 0x00FFFFFF);
    dst[0] = (uint8_t)(raw & 0xFF);
    dst[1] = (uint8_t)((raw >> 8) & 0xFF);
    dst[2] = (uint8_t)((raw >> 16) & 0xFF);
}

#if !defined(_WIN32) && LIBASOUND_ENABLED
typedef struct {
    snd_pcm_format_t alsa_format;
    cxadc_audio_format_t cxadc_format;
    size_t sample_bytes;
} cxadc_audio_format_desc_t;

static bool cxadc_str_contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) return false;
    size_t needle_len = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool cxadc_alsa_card_name_matches_cxadc_clockgen(const char *name, const char *longname)
{
    return cxadc_str_contains_nocase(name, "cxadc") ||
           cxadc_str_contains_nocase(name, "clockgen") ||
           cxadc_str_contains_nocase(longname, "cxadc") ||
           cxadc_str_contains_nocase(longname, "clockgen");
}

static bool cxadc_alsa_card_name_matches_misrc_clockgen(const char *name, const char *longname)
{
    bool name_has_misrc = cxadc_str_contains_nocase(name, "misrc") ||
                          cxadc_str_contains_nocase(longname, "misrc");
    bool name_has_clockgen = cxadc_str_contains_nocase(name, "clockgen") ||
                             cxadc_str_contains_nocase(longname, "clockgen");
    bool name_has_pcm1802 = cxadc_str_contains_nocase(name, "pcm1802") ||
                            cxadc_str_contains_nocase(longname, "pcm1802");
    return name_has_misrc && (name_has_clockgen || name_has_pcm1802);
}

static bool cxadc_alsa_card_name_matches_target(const cxadc_ctx_t *ctx,
                                                 const char *name,
                                                 const char *longname)
{
    if (ctx && ctx->misrc_clockgen_mode) {
        if (cxadc_alsa_card_name_matches_misrc_clockgen(name, longname)) {
            return true;
        }
        // Fallback so MISRC mode still works with legacy/unbranded card names.
        return cxadc_alsa_card_name_matches_cxadc_clockgen(name, longname);
    }
    return cxadc_alsa_card_name_matches_cxadc_clockgen(name, longname);
}

static int cxadc_configure_audio_pcm(snd_pcm_t *pcm,
                                     snd_pcm_format_t format,
                                     unsigned int *out_rate_hz)
{
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_hw_params_alloca(&params);

    int r;
    if ((r = snd_pcm_hw_params_any(pcm, params)) < 0) {
        fprintf(stderr, "[CXADC] ALSA hw_params_any failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_access(RW_INTERLEAVED) failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_hw_params_set_format(pcm, params, format)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_format(%s) failed: %s\n",
                snd_pcm_format_name(format), snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_hw_params_set_channels(pcm, params, CXADC_AUDIO_CHANNEL_COUNT)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_channels(%d) failed: %s "
                        "(clockgen may not expose %d channels via this device id)\n",
                CXADC_AUDIO_CHANNEL_COUNT, snd_strerror(r), CXADC_AUDIO_CHANNEL_COUNT);
        return -1;
    }

    unsigned int rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
    int dir = 0;
    if ((r = snd_pcm_hw_params_set_rate_near(pcm, params, &rate_hz, &dir)) < 0) {
        fprintf(stderr, "[CXADC] ALSA set_rate_near(%u) failed: %s\n",
                CXADC_AUDIO_SAMPLE_RATE_HZ, snd_strerror(r));
        return -1;
    }
    if (rate_hz < 46000 || rate_hz > 48000) {
        fprintf(stderr, "[CXADC] ALSA rate %u Hz out of range (need 46000-48000)\n", rate_hz);
        return -1;
    }

    snd_pcm_uframes_t period_frames = CXADC_AUDIO_READ_FRAMES;
    snd_pcm_uframes_t buffer_frames = CXADC_AUDIO_READ_FRAMES * 8;
    (void)snd_pcm_hw_params_set_period_size_near(pcm, params, &period_frames, &dir);
    (void)snd_pcm_hw_params_set_buffer_size_near(pcm, params, &buffer_frames);

    if ((r = snd_pcm_hw_params(pcm, params)) < 0) {
        fprintf(stderr, "[CXADC] ALSA hw_params apply failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_prepare(pcm)) < 0) {
        fprintf(stderr, "[CXADC] ALSA prepare failed: %s\n", snd_strerror(r));
        return -1;
    }
    if ((r = snd_pcm_nonblock(pcm, 1)) < 0) {
        fprintf(stderr, "[CXADC] ALSA nonblock failed: %s\n", snd_strerror(r));
        return -1;
    }

    if (out_rate_hz) {
        *out_rate_hz = rate_hz;
    }
    return 0;
}

static int cxadc_try_open_audio_device(cxadc_ctx_t *ctx, const char *device_name)
{
    if (!ctx || !device_name || !device_name[0]) return -1;

    snd_pcm_t *pcm = NULL;
    int open_rc = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_CAPTURE, 0);
    if (open_rc < 0) {
        fprintf(stderr, "[CXADC] ALSA snd_pcm_open('%s', CAPTURE) failed: %s\n",
                device_name, snd_strerror(open_rc));
        return -1;
    }

    static const cxadc_audio_format_desc_t formats[] = {
        { SND_PCM_FORMAT_S24_3LE, CXADC_AUDIO_FMT_S24_3LE, 3 },
        { SND_PCM_FORMAT_S32_LE,  CXADC_AUDIO_FMT_S32_LE,  4 },
        { SND_PCM_FORMAT_S16_LE,  CXADC_AUDIO_FMT_S16_LE,  2 },
    };

    for (size_t i = 0; i < (sizeof(formats) / sizeof(formats[0])); i++) {
        unsigned int configured_rate_hz = CXADC_AUDIO_SAMPLE_RATE_HZ;
        if (cxadc_configure_audio_pcm(pcm, formats[i].alsa_format, &configured_rate_hz) == 0) {
            ctx->audio_pcm = pcm;
            ctx->audio_format = formats[i].cxadc_format;
            ctx->audio_sample_bytes = formats[i].sample_bytes;
            ctx->audio_sample_rate_hz = configured_rate_hz;
            snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name), "%s", device_name);
            return 0;
        }
    }

    snd_pcm_close(pcm);
    return -1;
}

/* Resolve a card's stable ALSA id -- the udev ATTR{id} string, e.g.
 * "wm_cg_audio". Card *indices* move across reboots and replugs, so a device
 * string built from one points at whatever card inherited the number: it
 * breaks intermittently, which is the worst failure shape there is. The id
 * does not move. The control device is used deliberately here -- it stays
 * openable while the capture PCM is busy, so this works even when the card is
 * already held by another reader. */
static bool cxadc_alsa_card_id(int card, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0) return false;
    dst[0] = '\0';

    char ctl_name[16];
    snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);

    snd_ctl_t *ctl = NULL;
    if (snd_ctl_open(&ctl, ctl_name, 0) < 0) return false;

    snd_ctl_card_info_t *info = NULL;
    snd_ctl_card_info_alloca(&info);

    bool ok = false;
    if (snd_ctl_card_info(ctl, info) >= 0) {
        const char *id = snd_ctl_card_info_get_id(info);
        if (id && id[0]) {
            snprintf(dst, dst_len, "%s", id);
            ok = true;
        }
    }
    snd_ctl_close(ctl);
    return ok;
}

/* Try every device form for one card, stable id first, index forms only as a
 * last resort.
 *
 * Deliberately NOT tried here: a generic "dsnoop:CARD=<id>". Where a host has
 * already configured its own dsnoop instance (a different ipc_key), the
 * generic one opens a second hardware slave and EBUSYs against it, so it does
 * not solve the shared-card case it looks like it would solve. Configured
 * shared PCMs are probed by name in cxadc_open_audio_capture() instead. */
static int cxadc_try_open_card_devices(cxadc_ctx_t *ctx, int card)
{
    char dev[80];

    /* Stable id first. */
    static const char *const id_prefixes[] = { "usbstream", "hw", "plughw" };
    char card_id[32];
    if (cxadc_alsa_card_id(card, card_id, sizeof(card_id))) {
        for (size_t i = 0; i < sizeof(id_prefixes) / sizeof(id_prefixes[0]); i++) {
            snprintf(dev, sizeof(dev), "%s:CARD=%s", id_prefixes[i], card_id);
            if (cxadc_try_open_audio_device(ctx, dev) == 0) return 0;
        }
    }

    /* Index forms last: only reachable when the id could not be read. */
    static const char *const index_formats[] = { "usbstream:CARD=%d", "hw:%d", "plughw:%d" };
    for (size_t i = 0; i < sizeof(index_formats) / sizeof(index_formats[0]); i++) {
        snprintf(dev, sizeof(dev), index_formats[i], card);
        if (cxadc_try_open_audio_device(ctx, dev) == 0) return 0;
    }
    return -1;
}

static int cxadc_probe_alsa_cards_for_audio(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    int card = -1;
    if (snd_card_next(&card) < 0) return -1;

    bool matched_named_clockgen_card = false;
    while (card >= 0) {
        char *name = NULL;
        char *longname = NULL;
        (void)snd_card_get_name(card, &name);
        (void)snd_card_get_longname(card, &longname);

        bool likely_clockgen = cxadc_alsa_card_name_matches_target(ctx, name, longname);
        if (likely_clockgen) {
            matched_named_clockgen_card = true;
            if (cxadc_try_open_card_devices(ctx, card) == 0) {
                if (name) free(name);
                if (longname) free(longname);
                return 0;
            }
        }

        if (name) free(name);
        if (longname) free(longname);

        if (snd_card_next(&card) < 0) break;
    }

    // Fallback: if no obvious clockgen card name was found, probe all cards.
    if (matched_named_clockgen_card) return -1;

    card = -1;
    if (snd_card_next(&card) < 0) return -1;
    while (card >= 0) {
        if (cxadc_try_open_card_devices(ctx, card) == 0) {
            return 0;
        }
        if (snd_card_next(&card) < 0) break;
    }

    return -1;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;
    // Internal support note (ClockGen Lite feed, Linux):
    // If the aux/headswitch feed is missing, verify mixer state with:
    //   alsamixer -D usbstream:CARD=CXADCADCClockGe
    // and make sure "USB Stream Output" is enabled/unmuted.

    const char *env_device = ctx->misrc_clockgen_mode
        ? getenv("MISRC_CLOCKGEN_ALSA_DEVICE")
        : getenv("MISRC_CXADC_ALSA_DEVICE");
    if ((!env_device || !env_device[0]) && ctx->misrc_clockgen_mode) {
        // Backward-compatible override name.
        env_device = getenv("MISRC_CXADC_ALSA_DEVICE");
    }

    if (env_device && env_device[0]) {
        if (cxadc_try_open_audio_device(ctx, env_device) == 0) {
            return 0;
        }
    }

    /* Shared (dsnoop) PCMs are tried first. Where the host fans the clockgen
     * out to several readers via /etc/asound.conf (archival arecord + RTSP
     * preview), the hardware slave is exclusive: a direct hw:/plughw: open
     * returns EBUSY as soon as any other reader holds it, and the capture
     * silently degrades to RF-only. Joining the fan-out is the only way to
     * coexist. On hosts without that config these are simply unknown PCMs and
     * the probe falls through to the hardware names below.
     * Override with MISRC_CXADC_ALSA_DEVICE / MISRC_CLOCKGEN_ALSA_DEVICE. */
    static const char *shared_candidates[] = {
        "clockgen-dsnoop",
        NULL
    };
    for (size_t i = 0; shared_candidates[i]; i++) {
        if (cxadc_try_open_audio_device(ctx, shared_candidates[i]) == 0) {
            return 0;
        }
    }

    static const char *cxadc_candidates[] = {
        "usbstream:CARD=CXADCADCClockGe",
        "usbstream:CARD=CXADCADCClockGen",
        "usbstream:CARD=CXADCClockGen",
        "usbstream:CARD=CXADCClockGe",
        "hw:CARD=CXADCADCClockGe",
        "hw:CARD=CXADCADCClockGen",
        "hw:CARD=CXADCClockGen",
        "hw:CARD=CXADCClockGe",
        "plughw:CARD=CXADCADCClockGe",
        "plughw:CARD=CXADCADCClockGen",
        "plughw:CARD=CXADCClockGen",
        "plughw:CARD=CXADCClockGe",
        NULL
    };
    static const char *misrc_candidates[] = {
        "usbstream:CARD=MISRCClockgen",
        "usbstream:CARD=MISRCClockGe",
        "usbstream:CARD=MISRCClock",
        "hw:CARD=MISRCClockgen",
        "hw:CARD=MISRCClockGe",
        "hw:CARD=MISRCClock",
        "plughw:CARD=MISRCClockgen",
        "plughw:CARD=MISRCClockGe",
        "plughw:CARD=MISRCClock",
        "usbstream:CARD=PCM1802",
        "hw:CARD=PCM1802",
        "plughw:CARD=PCM1802",
        "usbstream:CARD=CXADCADCClockGe",
        "usbstream:CARD=CXADCADCClockGen",
        "hw:CARD=CXADCADCClockGe",
        "hw:CARD=CXADCADCClockGen",
        NULL
    };

    const char *const *candidates = ctx->misrc_clockgen_mode ? misrc_candidates : cxadc_candidates;
    for (size_t i = 0; candidates[i]; i++) {
        if (cxadc_try_open_audio_device(ctx, candidates[i]) == 0) {
            return 0;
        }
    }

    return cxadc_probe_alsa_cards_for_audio(ctx);
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_pcm) return;
    (void)snd_pcm_drop(ctx->audio_pcm);
    snd_pcm_close(ctx->audio_pcm);
    ctx->audio_pcm = NULL;
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_device_name[0] = '\0';
}
#elif defined(_WIN32)
// --- WASAPI clockgen audio capture (cxadc-win) ---

static const GUID s_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID s_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
// Explicit COM interface/class GUIDs for MinGW builds where the extern IID/CLSID
// symbols are declared by headers but not provided by import libs.
static const GUID s_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID s_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID s_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID s_IID_IAudioCaptureClient =
    { 0xC8ADBD64, 0xE71E, 0x48a0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

static bool cxadc_guid_equal(const GUID *a, const GUID *b)
{
    return a->Data1 == b->Data1 && a->Data2 == b->Data2 &&
           a->Data3 == b->Data3 && memcmp(a->Data4, b->Data4, 8) == 0;
}

static int cxadc_wasapi_parse_format(const WAVEFORMATEX *wfx, cxadc_ctx_t *ctx)
{
    if (!wfx || !ctx) return -1;

    WORD bits = wfx->wBitsPerSample;
    WORD valid_bits = bits;
    WORD format_tag = wfx->wFormatTag;
    bool is_float = false;

    if (format_tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        WAVEFORMATEXTENSIBLE *wfxe = (WAVEFORMATEXTENSIBLE *)wfx;
        valid_bits = wfxe->Samples.wValidBitsPerSample;
        if (cxadc_guid_equal(&wfxe->SubFormat, &s_KSDATAFORMAT_SUBTYPE_PCM)) {
            format_tag = WAVE_FORMAT_PCM;
        } else if (cxadc_guid_equal(&wfxe->SubFormat, &s_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            is_float = true;
        } else {
            return -1;
        }
    } else if (format_tag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (format_tag != WAVE_FORMAT_PCM) {
        return -1;
    }

    if (is_float) {
        if (bits != 32) return -1;
        ctx->audio_format = CXADC_AUDIO_FMT_FLOAT32;
        ctx->audio_sample_bytes = 4;
    } else if (valid_bits <= 16 && bits <= 16) {
        ctx->audio_format = CXADC_AUDIO_FMT_S16_LE;
        ctx->audio_sample_bytes = 2;
    } else if (valid_bits <= 24 && bits <= 24) {
        ctx->audio_format = CXADC_AUDIO_FMT_S24_3LE;
        ctx->audio_sample_bytes = 3;
    } else if (valid_bits <= 24 && bits <= 32) {
        ctx->audio_format = CXADC_AUDIO_FMT_S32_LE;
        ctx->audio_sample_bytes = 4;
    } else if (valid_bits <= 32 && bits <= 32) {
        ctx->audio_format = CXADC_AUDIO_FMT_S32_LE;
        ctx->audio_sample_bytes = 4;
    } else {
        return -1;
    }

    ctx->audio_channels = (int)wfx->nChannels;
    if (ctx->audio_channels < 1) ctx->audio_channels = 1;
    ctx->audio_sample_rate_hz = (uint32_t)wfx->nSamplesPerSec;
    return 0;
}

static bool cxadc_wasapi_wstr_contains_nocase(const wchar_t *haystack, const wchar_t *needle)
{
    if (!haystack || !needle || !needle[0]) return false;

    wchar_t haystack_lower[512];
    wcsncpy(haystack_lower, haystack, 511);
    haystack_lower[511] = L'\0';
    _wcslwr(haystack_lower);

    wchar_t needle_lower[128];
    wcsncpy(needle_lower, needle, 127);
    needle_lower[127] = L'\0';
    _wcslwr(needle_lower);

    return wcsstr(haystack_lower, needle_lower) != NULL;
}

static bool cxadc_wasapi_instance_matches_misrc_clockgen(const wchar_t *instance_id)
{
    if (!instance_id) return false;

    wchar_t id_lower[512];
    wcsncpy(id_lower, instance_id, 511);
    id_lower[511] = L'\0';
    _wcslwr(id_lower);
    if (!wcsstr(id_lower, MISRC_CLOCKGEN_USB_VID_W)) {
        return false;
    }
    return (wcsstr(id_lower, MISRC_CLOCKGEN_USB_PID_PRIMARY_W) != NULL) ||
           (wcsstr(id_lower, MISRC_CLOCKGEN_USB_PID_ALT_W) != NULL);
}

static bool cxadc_wasapi_device_matches_env_override(const char *env_name,
                                                     const wchar_t *friendly_name,
                                                     const wchar_t *instance_id)
{
    const char *env_device = getenv(env_name);
    if (env_device && env_device[0]) {
        wchar_t env_w[256];
        int len = MultiByteToWideChar(CP_UTF8, 0, env_device, -1, env_w, 256);
        if (len > 0) {
            if (friendly_name && _wcsicmp(friendly_name, env_w) == 0) return true;
            if (friendly_name && wcsstr(friendly_name, env_w) != NULL) return true;
            if (instance_id && wcsstr(instance_id, env_w) != NULL) return true;
        }
    }
    return false;
}

// Match a WASAPI capture endpoint against the selected clockgen variant.
// Checks: explicit env override, USB instance VID/PID, and friendly-name fallbacks.
static bool cxadc_wasapi_device_matches(const cxadc_ctx_t *ctx,
                                        const wchar_t *friendly_name,
                                        const wchar_t *instance_id)
{
    if (ctx && ctx->misrc_clockgen_mode) {
        if (cxadc_wasapi_device_matches_env_override("MISRC_CLOCKGEN_WASAPI_DEVICE",
                                                     friendly_name,
                                                     instance_id)) {
            return true;
        }
        if (cxadc_wasapi_device_matches_env_override("MISRC_CXADC_WASAPI_DEVICE",
                                                     friendly_name,
                                                     instance_id)) {
            return true;
        }
        if (cxadc_wasapi_instance_matches_misrc_clockgen(instance_id)) {
            return true;
        }
        if (friendly_name &&
            (cxadc_wasapi_wstr_contains_nocase(friendly_name, L"misrc clockgen") ||
             cxadc_wasapi_wstr_contains_nocase(friendly_name, L"pcm1802"))) {
            return true;
        }
        return false;
    }

    if (cxadc_wasapi_device_matches_env_override("MISRC_CXADC_WASAPI_DEVICE",
                                                 friendly_name,
                                                 instance_id)) {
        return true;
    }
    // Keep explicit MISRC mode and regular CXADC mode distinct by default.
    if (cxadc_wasapi_instance_matches_misrc_clockgen(instance_id)) {
        return false;
    }
    if (friendly_name) {
        const wchar_t *patterns[] = { L"CXADC", L"ClockGen", L"Clockgen", L"clockgen", NULL };
        for (int i = 0; patterns[i]; i++) {
            if (wcsstr(friendly_name, patterns[i]) != NULL) return true;
        }
    }
    return false;
}

// Clockgen ADC supports 48000 or 46875 Hz (configurable via CxadcClockGen module).
static bool cxadc_wasapi_rate_supported(uint32_t rate_hz)
{
    return rate_hz == 48000U || rate_hz == 46875U;
}

static int cxadc_open_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return -1;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return -1;
    }
    bool com_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator *pEnum = NULL;
    hr = CoCreateInstance(&s_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &s_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) {
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IMMDeviceCollection *pCollection = NULL;
    hr = pEnum->lpVtbl->EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    UINT count = 0;
    pCollection->lpVtbl->GetCount(pCollection, &count);

    IMMDevice *pDevice = NULL;
    bool found = false;
    for (UINT i = 0; i < count && !found; i++) {
        hr = pCollection->lpVtbl->Item(pCollection, i, &pDevice);
        if (FAILED(hr)) continue;

        IPropertyStore *pProps = NULL;
        hr = pDevice->lpVtbl->OpenPropertyStore(pDevice, STGM_READ, &pProps);
        if (FAILED(hr)) {
            pDevice->lpVtbl->Release(pDevice);
            pDevice = NULL;
            continue;
        }

        wchar_t *friendly_name_w = NULL;
        PROPVARIANT pv_name;
        memset(&pv_name, 0, sizeof(pv_name));
        hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_FriendlyName, &pv_name);
        if (SUCCEEDED(hr) && pv_name.vt == VT_LPWSTR && pv_name.pwszVal) {
            friendly_name_w = pv_name.pwszVal;
        }

        wchar_t *instance_id_w = NULL;
        PROPVARIANT pv_id;
        memset(&pv_id, 0, sizeof(pv_id));
        hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_InstanceId, &pv_id);
        if (SUCCEEDED(hr) && pv_id.vt == VT_LPWSTR && pv_id.pwszVal) {
            instance_id_w = pv_id.pwszVal;
        }

        if (cxadc_wasapi_device_matches(ctx, friendly_name_w, instance_id_w)) {
            found = true;
            if (ctx->misrc_clockgen_mode) {
                snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name),
                         "%s", "WASAPI:MISRC Clockgen");
            } else {
                const wchar_t *label = friendly_name_w ? friendly_name_w : instance_id_w;
                snprintf(ctx->audio_device_name, sizeof(ctx->audio_device_name),
                         "WASAPI:%ls", label ? label : L"clockgen");
            }
        }

        if (pv_name.vt == VT_LPWSTR && pv_name.pwszVal) CoTaskMemFree(pv_name.pwszVal);
        if (pv_id.vt == VT_LPWSTR && pv_id.pwszVal) CoTaskMemFree(pv_id.pwszVal);
        pProps->lpVtbl->Release(pProps);
        if (!found) {
            pDevice->lpVtbl->Release(pDevice);
            pDevice = NULL;
        }
    }
    pCollection->lpVtbl->Release(pCollection);

    if (!found || !pDevice) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IAudioClient *pClient = NULL;
    hr = pDevice->lpVtbl->Activate(pDevice, &s_IID_IAudioClient, CLSCTX_ALL,
                                   NULL, (void **)&pClient);
    if (FAILED(hr)) {
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    WAVEFORMATEX *pwfx = NULL;
    hr = pClient->lpVtbl->GetMixFormat(pClient, &pwfx);
    if (FAILED(hr) || !pwfx) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    if (cxadc_wasapi_parse_format(pwfx, ctx) != 0) {
        CoTaskMemFree(pwfx);
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    // Clockgen ADC supports 48000 or 46875 Hz. Warn (but continue) if the
    // device reports a different rate — the user may need to configure the
    // clockgen ADC rate via the CxadcClockGen PowerShell module first.
    if (!cxadc_wasapi_rate_supported(ctx->audio_sample_rate_hz)) {
        fprintf(stderr, "[CXADC] WASAPI: device rate %u Hz is not 48000/46875; "
                "configure clockgen ADC rate via CxadcClockGen module\n",
                ctx->audio_sample_rate_hz);
    }

    REFERENCE_TIME buffer_duration = 500000; // 50ms in 100ns units
    hr = pClient->lpVtbl->Initialize(pClient, AUDCLNT_SHAREMODE_SHARED, 0,
                                     buffer_duration, 0, pwfx, NULL);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    IAudioCaptureClient *pCapture = NULL;
    hr = pClient->lpVtbl->GetService(pClient, &s_IID_IAudioCaptureClient,
                                     (void **)&pCapture);
    if (FAILED(hr)) {
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    hr = pClient->lpVtbl->Start(pClient);
    if (FAILED(hr)) {
        pCapture->lpVtbl->Release(pCapture);
        pClient->lpVtbl->Release(pClient);
        pDevice->lpVtbl->Release(pDevice);
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return -1;
    }

    ctx->audio_endpoint = pDevice;
    ctx->audio_client = pClient;
    ctx->audio_capture = pCapture;
    ctx->audio_com_initialized = com_initialized;
    pEnum->lpVtbl->Release(pEnum);
    return 0;
}

static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx || !ctx->audio_client) return;
    ctx->audio_client->lpVtbl->Stop(ctx->audio_client);
}

static void cxadc_close_audio_capture(cxadc_ctx_t *ctx)
{
    if (!ctx) return;
    if (ctx->audio_client) {
        ctx->audio_client->lpVtbl->Stop(ctx->audio_client);
    }
    if (ctx->audio_capture) {
        ctx->audio_capture->lpVtbl->Release(ctx->audio_capture);
        ctx->audio_capture = NULL;
    }
    if (ctx->audio_client) {
        ctx->audio_client->lpVtbl->Release(ctx->audio_client);
        ctx->audio_client = NULL;
    }
    if (ctx->audio_endpoint) {
        ctx->audio_endpoint->lpVtbl->Release(ctx->audio_endpoint);
        ctx->audio_endpoint = NULL;
    }
    ctx->audio_format = CXADC_AUDIO_FMT_NONE;
    ctx->audio_sample_bytes = 0;
    ctx->audio_sample_rate_hz = 0;
    ctx->audio_channels = 0;
    ctx->audio_device_name[0] = '\0';
    if (ctx->audio_com_initialized) {
        CoUninitialize();
        ctx->audio_com_initialized = false;
    }
}
#else
static int cxadc_open_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; return -1; }
static void cxadc_abort_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
static void cxadc_close_audio_capture(cxadc_ctx_t *ctx) { (void)ctx; }
#endif

static int cxadc_audio_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;
    gui_app_t *app = ctx->app;

#if defined(_WIN32)
    if (!ctx->audio_capture || !ctx->audio_client ||
        ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);
    int dev_channels = ctx->audio_channels;
    if (dev_channels < 1) dev_channels = 1;
    size_t input_frame_bytes = (size_t)dev_channels * ctx->audio_sample_bytes;

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        UINT32 packet_frames = 0;
        HRESULT hr = ctx->audio_capture->lpVtbl->GetNextPacketSize(ctx->audio_capture, &packet_frames);
        if (FAILED(hr)) {
            gui_app_set_status(app, "CXADC WASAPI audio read error");
            break;
        }
        if (packet_frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        while (packet_frames > 0) {
            BYTE *data = NULL;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            hr = ctx->audio_capture->lpVtbl->GetBuffer(ctx->audio_capture, &data,
                                                       &frames_available, &flags,
                                                       NULL, NULL);
            if (FAILED(hr)) {
                gui_app_set_status(app, "CXADC WASAPI GetBuffer error");
                goto cxadc_wasapi_done;
            }

            if (data && frames_available > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                size_t output_bytes = (size_t)frames_available * CXADC_AUDIO_PACKED_FRAME_BYTES;
                uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO,
                                                             output_bytes, NULL);
                if (out) {
                    for (UINT32 i = 0; i < frames_available; i++) {
                        const uint8_t *src_frame = data + ((size_t)i * input_frame_bytes);
                        uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
                        for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                            uint8_t *dst = dst_frame + ((size_t)ch * 3);
                            if (ch < dev_channels) {
                                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                                    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                                    int32_t s32 = (int32_t)((uint32_t)src[0] |
                                                            ((uint32_t)src[1] << 8) |
                                                            ((uint32_t)src[2] << 16) |
                                                            ((uint32_t)src[3] << 24));
                                    cxadc_store_s24le(dst, s32 >> 8);
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                                    int16_t s16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                                    cxadc_store_s24le(dst, ((int32_t)s16) << 8);
                                } else if (ctx->audio_format == CXADC_AUDIO_FMT_FLOAT32) {
                                    float f;
                                    memcpy(&f, src, 4);
                                    int32_t s32 = (int32_t)(f * 8388607.0f);
                                    cxadc_store_s24le(dst, s32);
                                } else {
                                    dst[0] = dst[1] = dst[2] = 0;
                                }
                            } else {
                                dst[0] = dst[1] = dst[2] = 0;
                            }
                        }
                        // CH4: the clockgen device is 3-channel (CH1/CH2 + headswitch
                        // CH3). There is no CH4 input — always leave it zero. Do
                        // NOT mirror CH3 into CH4 (the clockgen has no CH3/4
                        // pair control path on any platform).
                        dst_frame[9] = 0;
                        dst_frame[10] = 0;
                        dst_frame[11] = 0;
                    }
                    gui_headswitch_lock_ingest_s24le_interleaved(out, output_bytes, ctx->audio_sample_rate_hz);
                    bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
                    bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
                    atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
                    atomic_store(&app->last_callback_time_ms, get_time_ms());
                } else {
                    atomic_fetch_add(&app->rb_drop_count, 1);
                }
            }

            ctx->audio_capture->lpVtbl->ReleaseBuffer(ctx->audio_capture, frames_available);
            hr = ctx->audio_capture->lpVtbl->GetNextPacketSize(ctx->audio_capture, &packet_frames);
            if (FAILED(hr)) {
                packet_frames = 0;
                break;
            }
        }
    }
cxadc_wasapi_done:
    return 0;
#elif !LIBASOUND_ENABLED
    (void)app;
    return 0;
#else
    if (!ctx->audio_pcm || ctx->audio_format == CXADC_AUDIO_FMT_NONE || ctx->audio_sample_bytes == 0) {
        return 0;
    }

    thrd_set_priority(THRD_PRIORITY_ABOVE);

    size_t input_frame_bytes = (size_t)CXADC_AUDIO_CHANNEL_COUNT * ctx->audio_sample_bytes;
    size_t input_buffer_bytes = (size_t)CXADC_AUDIO_READ_FRAMES * input_frame_bytes;
    uint8_t *input_buf = (uint8_t *)malloc(input_buffer_bytes);
    if (!input_buf) {
        gui_app_set_status(app, "CXADC: failed to allocate ALSA audio buffer");
        return -1;
    }

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
        snd_pcm_sframes_t frames = snd_pcm_readi(ctx->audio_pcm, input_buf, CXADC_AUDIO_READ_FRAMES);
        if (frames == -EAGAIN || frames == 0) {
            thrd_sleep_ms(1);
            continue;
        }
        if (frames == -EPIPE) {
            fprintf(stderr, "[CXADC] ALSA underrun (EPIPE) on %s, preparing\n",
                    ctx->audio_device_name);
            (void)snd_pcm_prepare(ctx->audio_pcm);
            continue;
        }
        if (frames < 0) {
            fprintf(stderr, "[CXADC] ALSA readi error on %s: %s (errno %d)\n",
                    ctx->audio_device_name, snd_strerror((int)frames), (int)frames);
            int rec = snd_pcm_recover(ctx->audio_pcm, (int)frames, 1);
            if (rec < 0) {
                fprintf(stderr, "[CXADC] ALSA recover failed: %s\n", snd_strerror(rec));
            }
            if (rec >= 0) {
                continue;
            }
            fprintf(stderr, "[CXADC] ALSA audio thread exiting after unrecoverable read error\n");
            gui_app_set_status(app, "CXADC ALSA audio read error");
            break;
        }

        size_t output_bytes = (size_t)frames * CXADC_AUDIO_PACKED_FRAME_BYTES;
        uint8_t *out = (uint8_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes, NULL);
        if (!out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            const uint8_t *src_frame = input_buf + ((size_t)i * input_frame_bytes);
            uint8_t *dst_frame = out + ((size_t)i * CXADC_AUDIO_PACKED_FRAME_BYTES);
            for (int ch = 0; ch < CXADC_AUDIO_CHANNEL_COUNT; ch++) {
                const uint8_t *src = src_frame + ((size_t)ch * ctx->audio_sample_bytes);
                uint8_t *dst = dst_frame + ((size_t)ch * 3);
                if (ctx->audio_format == CXADC_AUDIO_FMT_S24_3LE) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S32_LE) {
                    int32_t sample32 = (int32_t)((uint32_t)src[0] |
                                                 ((uint32_t)src[1] << 8) |
                                                 ((uint32_t)src[2] << 16) |
                                                 ((uint32_t)src[3] << 24));
                    cxadc_store_s24le(dst, sample32 >> 8);
                } else if (ctx->audio_format == CXADC_AUDIO_FMT_S16_LE) {
                    int16_t sample16 = (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
                    cxadc_store_s24le(dst, ((int32_t)sample16) << 8);
                } else {
                    dst[0] = dst[1] = dst[2] = 0;
                }
            }
            dst_frame[9] = 0;
            dst_frame[10] = 0;
            dst_frame[11] = 0;
        }
        gui_headswitch_lock_ingest_s24le_interleaved(out, output_bytes, ctx->audio_sample_rate_hz);

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_AUDIO, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_AUDIO);
        atomic_store(&app->audio_sample_rate, ctx->audio_sample_rate_hz);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
    }

    free(input_buf);
    return 0;
#endif
}

static int cxadc_capture_thread(void *ctx_ptr)
{
    cxadc_ctx_t *ctx = (cxadc_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->app) return -1;

    gui_app_t *app = ctx->app;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *card_buf_a = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    uint8_t *card_buf_b = (uint8_t *)malloc(CXADC_READ_CHUNK_BYTES);
    if (!card_buf_a || !card_buf_b) {
        free(card_buf_a);
        free(card_buf_b);
        gui_app_set_status(app, "CXADC: failed to allocate capture buffers");
        return -1;
    }
    const int input_bytes_per_sample_a = ctx->tenbit_mode[0] ? 2 : 1;
    const int input_bytes_per_sample_b = ctx->tenbit_mode[1] ? 2 : 1;

    atomic_store(&app->stream_synced, true);
    atomic_store(&app->sample_rate, ctx->rf_sample_rate_hz);

    while (atomic_load(&ctx->running) && app->is_capturing && !atomic_load(&do_exit)) {
#if defined(_WIN32)
        int read_a = cxadc_read_card(ctx->card_handles[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#else
        int read_a = cxadc_read_card(ctx->card_fds[0], card_buf_a, CXADC_READ_CHUNK_BYTES);
#endif
        if (read_a < 0) {
            gui_app_set_status(app, "CXADC read error on card 0");
            break;
        }
        if (read_a == 0) {
            thrd_sleep_ms(1);
            continue;
        }

        int output_samples = read_a / input_bytes_per_sample_a;
        if (output_samples <= 0) {
            thrd_sleep_ms(1);
            continue;
        }
        if (ctx->card_count > 1) {
#if defined(_WIN32)
            int read_b = cxadc_read_card(ctx->card_handles[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#else
            int read_b = cxadc_read_card(ctx->card_fds[1], card_buf_b, CXADC_READ_CHUNK_BYTES);
#endif
            if (read_b < 0) {
                gui_app_set_status(app, "CXADC read error on card 1");
                break;
            }
            if (read_b == 0) {
                thrd_sleep_ms(1);
                continue;
            }
            int output_samples_b = read_b / input_bytes_per_sample_b;
            if (output_samples_b < output_samples) {
                output_samples = output_samples_b;
            }
        }

        if (output_samples <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

        size_t output_bytes = (size_t)output_samples * sizeof(uint32_t);
        uint32_t *raw_out = (uint32_t *)bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF, output_bytes, NULL);
        if (!raw_out) {
            atomic_fetch_add(&app->rb_drop_count, 1);
            continue;
        }

        for (int i = 0; i < output_samples; i++) {
            int16_t sample_a = 0;
            if (ctx->tenbit_mode[0]) {
                int idx = i * 2;
                sample_a = cxadc_decode_sample_tenbit(card_buf_a[idx], card_buf_a[idx + 1]);
            } else {
                sample_a = cxadc_decode_sample_8bit(card_buf_a[i]);
            }
            int16_t sample_b = 0;
            if (ctx->card_count > 1) {
                if (ctx->tenbit_mode[1]) {
                    int idx = i * 2;
                    sample_b = cxadc_decode_sample_tenbit(card_buf_b[idx], card_buf_b[idx + 1]);
                } else {
                    sample_b = cxadc_decode_sample_8bit(card_buf_b[i]);
                }
            }
            raw_out[i] = cxadc_encode_raw_sample(sample_a, sample_b);
        }

        bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, output_bytes);
        bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);

        atomic_store(&app->last_callback_time_ms, get_time_ms());
        atomic_fetch_add(&app->frame_count, 1);
    }

    free(card_buf_a);
    free(card_buf_b);
    return 0;
}

bool gui_cxadc_detect_misrc_clockgen_audio(void)
{
#if defined(_WIN32)
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    bool com_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator *pEnum = NULL;
    hr = CoCreateInstance(&s_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &s_IID_IMMDeviceEnumerator, (void **)&pEnum);
    if (FAILED(hr)) { if (com_initialized) CoUninitialize(); return false; }

    IMMDeviceCollection *pCollection = NULL;
    hr = pEnum->lpVtbl->EnumAudioEndpoints(pEnum, eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) {
        pEnum->lpVtbl->Release(pEnum);
        if (com_initialized) CoUninitialize();
        return false;
    }

    UINT count = 0;
    pCollection->lpVtbl->GetCount(pCollection, &count);
    bool found = false;
    for (UINT i = 0; i < count && !found; i++) {
        IMMDevice *pDevice = NULL;
        hr = pCollection->lpVtbl->Item(pCollection, i, &pDevice);
        if (FAILED(hr)) continue;
        IPropertyStore *pProps = NULL;
        hr = pDevice->lpVtbl->OpenPropertyStore(pDevice, STGM_READ, &pProps);
        if (SUCCEEDED(hr)) {
            wchar_t *friendly_name_w = NULL;
            PROPVARIANT pv_name;
            memset(&pv_name, 0, sizeof(pv_name));
            hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_FriendlyName, &pv_name);
            if (SUCCEEDED(hr) && pv_name.vt == VT_LPWSTR && pv_name.pwszVal) {
                friendly_name_w = pv_name.pwszVal;
            }
            wchar_t *instance_id_w = NULL;
            PROPVARIANT pv_id;
            memset(&pv_id, 0, sizeof(pv_id));
            hr = pProps->lpVtbl->GetValue(pProps, &s_PKEY_Device_InstanceId, &pv_id);
            if (SUCCEEDED(hr) && pv_id.vt == VT_LPWSTR && pv_id.pwszVal) {
                instance_id_w = pv_id.pwszVal;
            }
            // Reuse the MISRC matchers (USB instance VID/PID + friendly name).
            if (cxadc_wasapi_instance_matches_misrc_clockgen(instance_id_w) ||
                (friendly_name_w &&
                 (cxadc_wasapi_wstr_contains_nocase(friendly_name_w, L"misrc clockgen") ||
                  cxadc_wasapi_wstr_contains_nocase(friendly_name_w, L"pcm1802")))) {
                found = true;
            }
            if (pv_name.vt == VT_LPWSTR && pv_name.pwszVal) CoTaskMemFree(pv_name.pwszVal);
            if (pv_id.vt == VT_LPWSTR && pv_id.pwszVal) CoTaskMemFree(pv_id.pwszVal);
            pProps->lpVtbl->Release(pProps);
        }
        pDevice->lpVtbl->Release(pDevice);
    }
    pCollection->lpVtbl->Release(pCollection);
    pEnum->lpVtbl->Release(pEnum);
    if (com_initialized) CoUninitialize();
    return found;
#elif !defined(_WIN32) && LIBASOUND_ENABLED
    int card = -1;
    if (snd_card_next(&card) < 0) return false;
    while (card >= 0) {
        char *name = NULL;
        char *longname = NULL;
        (void)snd_card_get_name(card, &name);
        (void)snd_card_get_longname(card, &longname);
        bool match = cxadc_alsa_card_name_matches_misrc_clockgen(name, longname);
        if (name) free(name);
        if (longname) free(longname);
        if (match) return true;
        if (snd_card_next(&card) < 0) break;
    }
    return false;
#else
    return false;
#endif
}

bool gui_cxadc_audio_capture_active(void)
{
    /* audio_device_name is set only on a successful open and cleared on close
     * and at start, on both the ALSA and WASAPI paths. */
    return s_cxadc.audio_device_name[0] != '\0';
}

int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode)
{
    if (!app) return -1;
    if (atomic_load(&s_cxadc.running)) return 0;

    // card_count == 0 is valid for the audio-only MISRC Clockgen path (no
    // cxadcN RF card nodes; the clockgen USB-audio feed is the sole source).
    // Clamp negatives to 0; cap at CXADC_MAX_CARDS.
    if (card_count < 0) card_count = 0;
    if (card_count > CXADC_MAX_CARDS) card_count = CXADC_MAX_CARDS;

    memset(&s_cxadc, 0, sizeof(s_cxadc));
    s_cxadc.app = app;
    s_cxadc.card_count = card_count;
    s_cxadc.misrc_clockgen_mode = misrc_clockgen_mode;
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        bool mode = app->settings.cxadc_tenbit_mode_card[i];
        if (i >= card_count) {
            mode = false;
        }
        s_cxadc.tenbit_mode[i] = mode;
        s_cxadc.card_sample_rate_hz[i] = mode
            ? CXADC_SAMPLE_RATE_TENBIT_HZ
            : CXADC_SAMPLE_RATE_8BIT_HZ;
    }
    s_cxadc.rf_sample_rate_hz = s_cxadc.card_sample_rate_hz[0];
#if defined(_WIN32)
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_handles[i] = INVALID_HANDLE_VALUE;
    }
    s_cxadc.audio_endpoint = NULL;
    s_cxadc.audio_client = NULL;
    s_cxadc.audio_capture = NULL;
    s_cxadc.audio_com_initialized = false;
#else
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_fds[i] = -1;
    }
#if LIBASOUND_ENABLED
    s_cxadc.audio_pcm = NULL;
#endif
#endif
    // Shared audio format fields (zeroed by memset, set explicitly for clarity)
    s_cxadc.audio_format = CXADC_AUDIO_FMT_NONE;
    s_cxadc.audio_sample_bytes = 0;
    s_cxadc.audio_sample_rate_hz = 0;
    s_cxadc.audio_channels = 0;
    s_cxadc.audio_device_name[0] = '\0';

    gui_headswitch_lock_reset();
    // Skip CXADC card programming/open when there are no RF cards (audio-only
    // MISRC Clockgen). tenbit/center-offset sysfs writes would fail and abort.
    if (card_count > 0) {
        if (cxadc_apply_tenbit_modes(card_count, s_cxadc.tenbit_mode) != 0) {
#if !defined(_WIN32)
            int saved_errno = errno;
            fprintf(stderr, "[CXADC] failed to apply tenbit mode on %d card(s): %s (errno %d)\n",
                    card_count, strerror(saved_errno), saved_errno);
            if (saved_errno == EACCES || saved_errno == EPERM) {
                fprintf(stderr, "[CXADC] sysfs write denied. For 10-bit mode you need one-time setup:\n"
                                "          sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*\n"
                                "          sudo chmod g+w   /sys/class/cxadc/cxadc*/device/parameters/*\n"
                                "          sudo usermod -aG video $USER   (then log out/in)\n");
                gui_app_set_status(app, "CXADC permission denied: run sudo chgrp video /sys/class/cxadc/cxadc*/device/parameters/*");
            } else {
                gui_app_set_status(app, "CXADC: failed to apply tenbit mode");
            }
#else
            fprintf(stderr, "[CXADC] failed to apply tenbit mode on %d card(s)\n", card_count);
            gui_app_set_status(app, "CXADC: failed to apply tenbit mode");
#endif
            return -1;
        }

        if (cxadc_open_cards(&s_cxadc, card_count) != 0) {
#if !defined(_WIN32)
            fprintf(stderr, "[CXADC] failed to open /dev/cxadc0..%d: %s (errno %d)\n",
                    card_count - 1, strerror(errno), errno);
            fprintf(stderr, "[CXADC] check the cxadc driver is loaded and /dev/cxadcN exists (ls -l /dev/cxadc*)\n");
#else
            fprintf(stderr, "[CXADC] failed to open card device(s)\\n");
#endif
            gui_app_set_status(app, "CXADC: failed to open card device(s)");
            return -1;
        }
    }

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[CXADC] failed to initialize RF ringbuffer\n");
        gui_app_set_status(app, "CXADC: failed to initialize RF buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        fprintf(stderr, "[CXADC] failed to initialize audio ringbuffer\n");
        gui_app_set_status(app, "CXADC: failed to initialize audio buffer");
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    cxadc_reset_stats(app, s_cxadc.rf_sample_rate_hz);

    bool audio_capture_available = false;
    if (cxadc_open_audio_capture(&s_cxadc) == 0) {
        audio_capture_available = true;
        if (s_cxadc.audio_sample_rate_hz > 0) {
            atomic_store(&app->audio_sample_rate, s_cxadc.audio_sample_rate_hz);
        }
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] audio capture device: %s (%u Hz, %d ch)\n",
                s_cxadc.audio_device_name,
                s_cxadc.audio_sample_rate_hz,
                s_cxadc.audio_channels);
#endif
    } else {
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] %s audio device not available; continuing RF-only\n",
                s_cxadc.misrc_clockgen_mode ? "MISRC clockgen" : "clockgen");
#if !defined(_WIN32) && LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] support note: for ClockGen Lite feed, check alsamixer card "
                        "\"CXADC+ADC-ClockGen\" (e.g. -D usbstream:CARD=CXADCADCClockGe) "
                        "and ensure \"USB Stream Output\" is enabled\n");
#endif
#endif
    }

    app->is_capturing = true;
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[CXADC] failed to start extraction thread\n");
        gui_app_set_status(app, "CXADC: failed to start extraction thread");
        app->is_capturing = false;
        cxadc_close_audio_capture(&s_cxadc);
        cxadc_close_cards(&s_cxadc);
        return -1;
    }

    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[CXADC] Failed to start display thread (non-fatal)\n");
        }
    }

    (void)gui_audio_start(app, &app->buffers);

    atomic_store(&s_cxadc.running, true);
    // The RF card-reading thread only applies when there are cxadcN cards.
    // In audio-only MISRC Clockgen mode (card_count == 0) the clockgen USB-audio
    // feed is the sole source, so skip the RF thread and run audio-only.
    if (card_count > 0) {
        if (thrd_create_with_priority(&s_cxadc.rf_thread,
                                      cxadc_capture_thread,
                                      &s_cxadc,
                                      THRD_PRIORITY_CRITICAL) != thrd_success) {
            fprintf(stderr, "[CXADC] failed to create RF capture thread\n");
            gui_app_set_status(app, "CXADC: failed to start RF capture thread");
            atomic_store(&s_cxadc.running, false);
            gui_audio_stop(app);
            if (app->display_thread) {
                gui_display_thread_stop(app->display_thread);
            }
            gui_extract_stop();
            app->is_capturing = false;
            cxadc_close_audio_capture(&s_cxadc);
            cxadc_close_cards(&s_cxadc);
            return -1;
        }
        s_cxadc.rf_thread_started = true;
    }

    if (audio_capture_available) {
        if (thrd_create_with_priority(&s_cxadc.audio_thread,
                                      cxadc_audio_capture_thread,
                                      &s_cxadc,
                                      THRD_PRIORITY_ABOVE) != thrd_success) {
            fprintf(stderr, "[CXADC] Failed to start audio capture thread; continuing RF-only\n");
            cxadc_close_audio_capture(&s_cxadc);
        } else {
            s_cxadc.audio_thread_started = true;
        }
    }

    // Status is mode-aware: MISRC Clockgen is reported regardless of card_count
    // (audio-only rigs have 0 cards); CXADC Clockgen only applies with >1 card.
    if (s_cxadc.misrc_clockgen_mode) {
        gui_app_set_status(app, "MISRC Clockgen capture running");
    } else if (card_count > 1) {
        gui_app_set_status(app, "CXADC Clockgen capture running");
    } else {
        gui_app_set_status(app, "CXADC capture running");
    }
    return 0;
}

void gui_cxadc_stop(gui_app_t *app)
{
    if (!atomic_load(&s_cxadc.running) && !s_cxadc.rf_thread_started && !s_cxadc.audio_thread_started) {
        return;
    }

    if (!app) {
        app = s_cxadc.app;
    }
    // MISRC Clockgen is a clockgen variant even with 0 RF cards (audio-only).
    bool was_clockgen_mode = (s_cxadc.card_count > 1) || s_cxadc.misrc_clockgen_mode;
    bool was_misrc_clockgen_mode = s_cxadc.misrc_clockgen_mode;

    if (app) {
        app->is_capturing = false;
    }
    atomic_store(&s_cxadc.running, false);
    cxadc_abort_audio_capture(&s_cxadc);

    if (s_cxadc.rf_thread_started) {
        thrd_join(s_cxadc.rf_thread, NULL);
        s_cxadc.rf_thread_started = false;
    }
    if (s_cxadc.audio_thread_started) {
        thrd_join(s_cxadc.audio_thread, NULL);
        s_cxadc.audio_thread_started = false;
    }

    if (app && app->display_thread) {
        gui_display_thread_stop(app->display_thread);
    }
    gui_audio_stop(app);
    gui_extract_stop();
    cxadc_close_audio_capture(&s_cxadc);

    cxadc_close_cards(&s_cxadc);
    gui_headswitch_lock_reset();

    if (app) {
        atomic_store(&app->stream_synced, false);
        atomic_store(&app->dropout_stop_requested, false);
        atomic_store(&app->dropout_stop_reason, GUI_DROPOUT_NONE);
        if (was_clockgen_mode) {
            gui_app_set_status(app, was_misrc_clockgen_mode
                ? "MISRC Clockgen capture stopped"
                : "CXADC Clockgen capture stopped");
        } else {
            gui_app_set_status(app, "CXADC capture stopped");
        }
    }

    memset(&s_cxadc, 0, sizeof(s_cxadc));
}

bool gui_cxadc_is_running(void)
{
    return atomic_load(&s_cxadc.running);
}

int gui_cxadc_start_clockgen_audio(gui_app_t *app, bool misrc_clockgen_mode)
{
    // Audio-only subset of gui_cxadc_start: opens the clockgen USB-audio capture
    // device (WASAPI/ALSA) and starts the cxadc audio thread feeding
    // BUF_CAPTURE_AUDIO + headswitch ingest. Does NOT open CXADC RF cards, start
    // the RF thread, or own extraction/display/audio-monitor threads (the caller's
    // RF path owns those). Used by MISRC Clockgen, which runs the hsdaoh raw-
    // parser RF feed (A+B) in parallel and shares one set of extraction/display/
    // audio-monitor threads.
    if (!app) return -1;
    if (atomic_load(&s_cxadc.running)) return 0;

    memset(&s_cxadc, 0, sizeof(s_cxadc));
    s_cxadc.app = app;
    s_cxadc.card_count = 0;  // audio-only; no CXADC RF cards
    s_cxadc.misrc_clockgen_mode = misrc_clockgen_mode;
#if defined(_WIN32)
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_handles[i] = INVALID_HANDLE_VALUE;
    }
    s_cxadc.audio_endpoint = NULL;
    s_cxadc.audio_client = NULL;
    s_cxadc.audio_capture = NULL;
    s_cxadc.audio_com_initialized = false;
#else
    for (int i = 0; i < CXADC_MAX_CARDS; i++) {
        s_cxadc.card_fds[i] = -1;
    }
#if LIBASOUND_ENABLED
    s_cxadc.audio_pcm = NULL;
#endif
#endif
    s_cxadc.audio_format = CXADC_AUDIO_FMT_NONE;
    s_cxadc.audio_sample_bytes = 0;
    s_cxadc.audio_sample_rate_hz = 0;
    s_cxadc.audio_channels = 0;
    s_cxadc.audio_device_name[0] = '\0';

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_AUDIO) != 0) {
        gui_app_set_status(app, "MISRC Clockgen: failed to initialize audio buffer");
        return -1;
    }

    gui_headswitch_lock_reset();

    if (cxadc_open_audio_capture(&s_cxadc) != 0) {
#if defined(_WIN32) || LIBASOUND_ENABLED
        fprintf(stderr, "[CXADC] %s audio device not available; continuing RF-only\n",
                s_cxadc.misrc_clockgen_mode ? "MISRC clockgen" : "clockgen");
#endif
        return -1;
    }

    if (s_cxadc.audio_sample_rate_hz > 0) {
        atomic_store(&app->audio_sample_rate, s_cxadc.audio_sample_rate_hz);
    }
#if defined(_WIN32) || LIBASOUND_ENABLED
    fprintf(stderr, "[CXADC] audio capture device: %s (%u Hz, %d ch)\n",
            s_cxadc.audio_device_name,
            s_cxadc.audio_sample_rate_hz,
            s_cxadc.audio_channels);
#endif

    atomic_store(&s_cxadc.running, true);
    if (thrd_create_with_priority(&s_cxadc.audio_thread,
                                  cxadc_audio_capture_thread,
                                  &s_cxadc,
                                  THRD_PRIORITY_ABOVE) != thrd_success) {
        fprintf(stderr, "[CXADC] Failed to start clockgen audio thread; continuing RF-only\n");
        cxadc_close_audio_capture(&s_cxadc);
        atomic_store(&s_cxadc.running, false);
        return -1;
    }
    s_cxadc.audio_thread_started = true;
    return 0;
}

void gui_cxadc_stop_clockgen_audio(void)
{
    // Audio-only subset of gui_cxadc_stop: stops/joins the cxadc audio thread
    // and closes the audio device. Does NOT stop extraction/display/audio-
    // monitor/cards (owned by the caller's RF path).
    if (!atomic_load(&s_cxadc.running) && !s_cxadc.audio_thread_started) {
        return;
    }
    atomic_store(&s_cxadc.running, false);
    cxadc_abort_audio_capture(&s_cxadc);
    if (s_cxadc.audio_thread_started) {
        thrd_join(s_cxadc.audio_thread, NULL);
        s_cxadc.audio_thread_started = false;
    }
    cxadc_close_audio_capture(&s_cxadc);
    gui_headswitch_lock_reset();
    memset(&s_cxadc, 0, sizeof(s_cxadc));
}
