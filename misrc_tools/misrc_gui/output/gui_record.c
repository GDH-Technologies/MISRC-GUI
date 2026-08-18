/*
 * MISRC - hsdaoh-rp2350 GUI - Recording Module
 *
 * Handles file recording with optional FLAC compression.
 * Uses writer threads to write extracted samples to files.
 * The extraction thread (in gui_extract.c) writes to record ringbuffers
 * when recording is enabled.
 */

#include "gui_record.h"
#include "gui_video_record.h"
#include "../input/gui_preview_v4l2.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../ui/gui_popup.h"
#include "gui_audio.h"
#include "../input/gui_capture.h"

#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/flac_writer.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include "version.h"

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define Rectangle Win32_Rectangle
#define CloseWindow Win32_CloseWindow
#define ShowCursor Win32_ShowCursor
#include <windows.h>
#undef ShowCursor
#undef CloseWindow
#undef Rectangle
#include <io.h>
#define access _access
#define F_OK 0
#endif

#ifndef MIRSC_TOOLS_VERSION
#define MIRSC_TOOLS_VERSION "dev"
#endif

#include <sys/types.h>
#include <sys/stat.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#endif

#if LIBFLAC_ENABLED == 1
#include "FLAC/metadata.h"
#endif

// Buffer sizes
#define BUFFER_READ_SIZE 65536
// Use larger CLI-style writer chunks for record-path throughput stability.
// CLI writer path uses 65536*32 byte blocks; GUI records int16 samples, so this
// maps to 65536*32 bytes == 1048576 samples per channel per writer read.
#define GUI_RECORD_WRITER_BLOCK_BYTES ((size_t)65536 * 32)
#define GUI_RECORD_WRITER_BLOCK_SAMPLES (GUI_RECORD_WRITER_BLOCK_BYTES / sizeof(int16_t))

// Format file size into human-readable string
static void format_file_size_u64(uint64_t size, char *buf, size_t buf_size) {
    if (size >= 1073741824ULL) {  // >= 1 GB
        snprintf(buf, buf_size, "%.2f GB", (double)size / 1073741824.0);
    } else if (size >= 1048576ULL) {  // >= 1 MB
        snprintf(buf, buf_size, "%.2f MB", (double)size / 1048576.0);
    } else if (size >= 1024ULL) {  // >= 1 KB
        snprintf(buf, buf_size, "%.2f KB", (double)size / 1024.0);
    } else {
        snprintf(buf, buf_size, "%llu bytes", (unsigned long long)size);
    }
}

static void format_duration_hhmmss(double seconds, char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    if (seconds < 0.0) seconds = 0.0;
    uint64_t total_seconds = (uint64_t)seconds;
    uint64_t hh = total_seconds / 3600ULL;
    uint64_t mm = (total_seconds / 60ULL) % 60ULL;
    uint64_t ss = total_seconds % 60ULL;
    snprintf(dst, dst_len, "%02" PRIu64 ".%02" PRIu64 ".%02" PRIu64, hh, mm, ss);
}

// Format data sizes for capture logs using clear MB/GB units
static void format_log_data_size_u64(uint64_t size, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;

    double mb = (double)size / 1048576.0;
    double gb = (double)size / 1073741824.0;

    if (gb >= 1.0) {
        snprintf(buf, buf_size, "%.3f GB (%" PRIu64 " bytes)", gb, size);
    } else {
        snprintf(buf, buf_size, "%.2f MB (%" PRIu64 " bytes)", mb, size);
    }
}

static void gui_record_build_system_timestamp(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    time_t t = time(NULL);
    if (t == (time_t)-1) return;
    struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
    if (localtime_s(&tmv, &t) != 0) return;
#else
    if (!localtime_r(&t, &tmv)) return;
#endif
    snprintf(dst, dst_len, "%04d.%02d.%02d_%02d.%02d.%02d",
             (tmv.tm_year + 1900),
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
}

static bool gui_record_extract_timestamp_token(const char *path, char *dst, size_t dst_len) {
    if (!path || !path[0] || !dst || dst_len == 0) return false;
    dst[0] = '\0';

    const char *name = path;
    const char *slash = strrchr(path, '/');
#if defined(_WIN32) || defined(_WIN64)
    const char *bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash)) {
        slash = bslash;
    }
#endif
    if (slash && slash[1]) {
        name = slash + 1;
    }

    size_t len = strlen(name);
    for (size_t i = 0; i + 19 <= len; i++) {
        const char *p = name + i;
        bool match =
            isdigit((unsigned char)p[0]) &&
            isdigit((unsigned char)p[1]) &&
            isdigit((unsigned char)p[2]) &&
            isdigit((unsigned char)p[3]) &&
            p[4] == '.' &&
            isdigit((unsigned char)p[5]) &&
            isdigit((unsigned char)p[6]) &&
            p[7] == '.' &&
            isdigit((unsigned char)p[8]) &&
            isdigit((unsigned char)p[9]) &&
            p[10] == '_' &&
            isdigit((unsigned char)p[11]) &&
            isdigit((unsigned char)p[12]) &&
            p[13] == '.' &&
            isdigit((unsigned char)p[14]) &&
            isdigit((unsigned char)p[15]) &&
            p[16] == '.' &&
            isdigit((unsigned char)p[17]) &&
            isdigit((unsigned char)p[18]);
        if (match) {
            snprintf(dst, dst_len, "%.*s", 19, p);
            return true;
        }
    }

    return false;
}

static void gui_record_build_iso8601_timestamp(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    time_t t = time(NULL);
    if (t == (time_t)-1) return;
    struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
    if (localtime_s(&tmv, &t) != 0) return;
#else
    if (!localtime_r(&t, &tmv)) return;
#endif
    snprintf(dst, dst_len, "%04d-%02d-%02dT%02d:%02d:%02d",
             (tmv.tm_year + 1900),
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
}

static void gui_record_get_host_name(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    const char *host = getenv("COMPUTERNAME");
    if (host && host[0]) {
        snprintf(dst, dst_len, "%s", host);
    }
#else
    if (gethostname(dst, dst_len - 1) == 0) {
        dst[dst_len - 1] = '\0';
    }
#endif
    if (!dst[0]) {
        snprintf(dst, dst_len, "%s", "unknown");
    }
}

static void gui_record_get_user_name(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    const char *user = getenv("USERNAME");
#else
    const char *user = getenv("USER");
    if (!user || !user[0]) {
        user = getenv("LOGNAME");
    }
#endif
    snprintf(dst, dst_len, "%s", (user && user[0]) ? user : "unknown");
}

static void gui_record_get_os_string(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(_WIN32) || defined(_WIN64)
    snprintf(dst, dst_len, "%s", "Windows");
#else
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(dst, dst_len, "%s %s (%s)", uts.sysname, uts.release, uts.machine);
    } else {
        snprintf(dst, dst_len, "%s", "unknown");
    }
#endif
}
static bool gui_record_get_free_space_bytes(const char *path, uint64_t *free_bytes_out);

bool gui_record_get_output_free_space_bytes(const gui_app_t *app, uint64_t *free_bytes_out) {
    if (!app || !free_bytes_out || !app->settings.output_path[0]) {
        return false;
    }
    return gui_record_get_free_space_bytes(app->settings.output_path, free_bytes_out);
}


static uint32_t gui_record_get_cpu_core_count(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char *cores_env = getenv("NUMBER_OF_PROCESSORS");
    if (cores_env && cores_env[0]) {
        int cores = atoi(cores_env);
        if (cores > 0) return (uint32_t)cores;
    }
    return 0;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    return (cores > 0) ? (uint32_t)cores : 0;
#endif
}

// Resolve the per-channel FLAC encoder thread count.
//
// user_threads == 0  -> auto: pick a real parallel count with a 4-core target
//   base so a single level-8 encoder stream can keep up with 40 MSPS on modern
//   desktop CPUs. We use half the online cores (leaving headroom for capture /
//   extract / display / audio / GUI), clamped to [4, 8]. On an 18-core box this
//   resolves to 8 threads per channel; on an 8-core box it resolves to 4.
// user_threads > 0   -> honor the user's explicit override verbatim.
//
// Returns 0 only when the core count cannot be determined, in which case the
// caller falls back to libFLAC's own default.
static uint32_t gui_record_resolve_flac_threads(int32_t user_threads) {
    if (user_threads > 0) {
        return (uint32_t)user_threads;
    }
    uint32_t cores = gui_record_get_cpu_core_count();
    if (cores == 0) {
        return 0;  // unknown -> let libFLAC pick its default
    }
    uint32_t half = cores / 2;
    if (half < 4) half = 4;    // 4-core target base
    if (half > 8) half = 8;    // cap: diminishing returns + leave headroom
    return half;
}

static void gui_record_get_cpu_model(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
#if defined(__linux__)
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (!cpuinfo) {
        snprintf(dst, dst_len, "%s", "unknown");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), cpuinfo)) {
        if (strncmp(line, "model name", 10) == 0) {
            char *sep = strchr(line, ':');
            if (sep) {
                sep++;
                while (*sep == ' ' || *sep == '\t') sep++;
                size_t len = strlen(sep);
                while (len > 0 && (sep[len - 1] == '\n' || sep[len - 1] == '\r')) {
                    sep[--len] = '\0';
                }
                snprintf(dst, dst_len, "%s", sep);
                break;
            }
        }
    }
    fclose(cpuinfo);
#endif
    if (!dst[0]) {
        snprintf(dst, dst_len, "%s", "unknown");
    }
}

static const char *gui_record_device_type_name(const gui_app_t *app) {
    if (!app || app->selected_device < 0 || app->selected_device >= app->device_count) {
        return "unknown";
    }
    device_type_t type = app->devices[app->selected_device].type;
    switch (type) {
        case DEVICE_TYPE_HSDAOH:
            return "hsdaoh";
        case DEVICE_TYPE_SIMPLE_CAPTURE:
            return "simple_capture";
        case DEVICE_TYPE_CXADC:
            return "cxadc";
        case DEVICE_TYPE_SIMULATED:
            return "simulated";
        case DEVICE_TYPE_PLAYBACK:
            return "playback";
#ifdef ENABLE_FX3
        case DEVICE_TYPE_FX3:
            return "fx3";
#endif
#ifdef ENABLE_DDD
        case DEVICE_TYPE_DDD:
            return "ddd";
#endif
        default:
            return "unknown";
    }
}

static void gui_record_build_log_timestamp(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    time_t t = time(NULL);
    if (t == (time_t)-1) return;
    struct tm tmv;
#if defined(_WIN32) || defined(_WIN64)
    if (localtime_s(&tmv, &t) != 0) return;
#else
    if (!localtime_r(&t, &tmv)) return;
#endif
    snprintf(dst, dst_len, "%04d-%02d-%02d %02d:%02d:%02d",
             (tmv.tm_year + 1900),
             tmv.tm_mon + 1,
             tmv.tm_mday,
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec);
}

// do_exit is declared in gui_capture.h (defined in misrc_gui.c)

// Finalize thread (one finalize in flight at most)
static thrd_t s_finalize_thread;
static atomic_bool s_finalize_thread_running = ATOMIC_VAR_INIT(false);
static char s_record_path_video[600];
static char s_video_start_msg[480];
static atomic_bool s_record_stop_finalizing = ATOMIC_VAR_INIT(false);
static atomic_bool s_record_stop_finalize_done = ATOMIC_VAR_INIT(false);
static atomic_flag s_capture_log_lock = ATOMIC_FLAG_INIT;

// File writer context
typedef struct writer_ctx writer_ctx_t;
typedef struct gui_record_session gui_record_session_t;

struct writer_ctx {
    buffer_manager_t *bufmgr;  // Buffer manager pointer
    buffer_id_t buf_id;        // BUF_RECORD_A or BUF_RECORD_B
    FILE *file;
    int channel;  // 0 = A, 1 = B

    // RF bit depth requested for output
    // - FLAC: 8/12/16
    // - RAW:  8/16
    uint8_t rf_bits;

    // For RAW writer: bytes per sample (1=8-bit, 2=16-bit)
    size_t raw_bytes_per_sample;

    // RF resampling (rate in kHz; 0 or disabled = passthrough)
    bool enable_resample;
    float resample_rate_khz;
    int resample_quality;      // 0-4
    float resample_gain_db;

#if LIBSOXR_ENABLED
    void *soxr;                // soxr_t (NULL if not initialized)
    float soxr_rate_khz;       // configured output rate (kHz)
#endif

#if LIBFLAC_ENABLED == 1
    flac_writer_t *writer;
    atomic_uint_fast64_t *compressed_bytes;
    uint8_t flac_bits_per_sample;
#endif
    gui_app_t *app;                 // For error reporting
    gui_record_session_t *ses;      // Owning recording session
};

/* All state one recording needs through finalize. Heap-allocated at record
 * start, handed to the finalize thread at stop, freed once finalize is
 * reaped -- so a new recording can start while the previous one finalizes. */
struct gui_record_session {
    gui_app_t *app;

    /* True while this session is actively recording; cleared at stop so the
     * session's writer threads switch to drain mode. */
    atomic_bool recording;

    /* Latched at start -- finalize must never read live settings. */
    bool use_flac;
    bool capture_a, capture_b;
    bool video_started;

    FILE *file_a, *file_b;
    char path_a[512], path_b[512];
#if LIBFLAC_ENABLED == 1
    flac_writer_t *flac_a, *flac_b;
    uint32_t sample_rate_a, sample_rate_b;
#endif

    thrd_t writer_thread_a, writer_thread_b;
    bool writer_threads_running;
    writer_ctx_t ctx_a, ctx_b;

    FILE *log_file;
    char log_path[512];

    double recording_start_time;
    double stop_request_time;
    uint32_t start_rec_a_waits, start_rec_a_drops;
    uint32_t start_rec_b_waits, start_rec_b_drops;
};

static gui_record_session_t *s_active = NULL;      /* recording in progress */
static gui_record_session_t *s_finalizing = NULL;  /* handed to finalize thread */

static void gui_record_log_lock(void) {
    while (atomic_flag_test_and_set(&s_capture_log_lock)) {
        thrd_sleep_ms(1);
    }
}

static void gui_record_log_unlock(void) {
    atomic_flag_clear(&s_capture_log_lock);
}

static void gui_record_close_session_log_ses(gui_record_session_t *ses) {
    if (!ses) return;
    gui_record_log_lock();
    if (ses->log_file) {
        fclose(ses->log_file);
        ses->log_file = NULL;
    }
    ses->log_path[0] = '\0';
    gui_record_log_unlock();
}

static void gui_record_close_session_log(void) {
    gui_record_close_session_log_ses(s_active);
}

static void gui_record_trim_trailing_newline(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

/* Caller holds the log lock. Writes to the active session's log (only used
 * while opening that log, so s_active is set). */
static void gui_record_log_write_line_locked(const char *level, const char *message) {
    if (!s_active || !s_active->log_file || !message || !message[0]) return;
    char ts[32];
    gui_record_build_log_timestamp(ts, sizeof(ts));
    fprintf(s_active->log_file, "[%s] [%s] %s\n", ts, (level && level[0]) ? level : "INFO", message);
    fflush(s_active->log_file);
}

static void gui_record_log_write_line_ses(gui_record_session_t *ses,
                                          const char *level, const char *message) {
    if (!ses || !message || !message[0]) return;
    gui_record_log_lock();
    if (ses->log_file) {
        char ts[32];
        gui_record_build_log_timestamp(ts, sizeof(ts));
        fprintf(ses->log_file, "[%s] [%s] %s\n", ts,
                (level && level[0]) ? level : "INFO", message);
        fflush(ses->log_file);
    }
    gui_record_log_unlock();
}

static void gui_record_log_write_line(const char *level, const char *message) {
    gui_record_log_write_line_ses(s_active, level, message);
}

static void gui_record_log_writef_ses(gui_record_session_t *ses,
                                      const char *level, const char *format, ...) {
    if (!ses || !format || !format[0]) return;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    gui_record_trim_trailing_newline(message);
    if (!message[0]) return;
    gui_record_log_write_line_ses(ses, level, message);
}

static void gui_record_log_writef(const char *level, const char *format, ...) {
    if (!format || !format[0]) return;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    gui_record_trim_trailing_newline(message);
    if (!message[0]) return;
    gui_record_log_write_line(level, message);
}

// Overwrite confirmation pending state
static bool s_overwrite_pending = false;
static gui_app_t *s_pending_app = NULL;

static bool gui_record_collect_finalize_if_done(void) {
    if (!atomic_exchange(&s_record_stop_finalize_done, false)) {
        return false;
    }
    if (atomic_load(&s_finalize_thread_running)) {
        thrd_join(s_finalize_thread, NULL);
        atomic_store(&s_finalize_thread_running, false);
    }
    if (s_finalizing) {
        free(s_finalizing);
        s_finalizing = NULL;
    }
    return true;
}

#define GUI_RECORD_SPILL_CHANNELS 2
#define GUI_RECORD_SPILL_LOG_STEP_BYTES ((uint64_t)256 * 1024 * 1024)
#define GUI_RECORD_DISK_GUARD_CHECK_INTERVAL_MS 1000ULL
#define GUI_RECORD_DISK_GUARD_MIN_HEADROOM_BYTES ((uint64_t)2 * 1000 * 1000 * 1000)
#define GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS 180.0

static atomic_bool s_disk_guard_tripped = ATOMIC_VAR_INIT(false);
static atomic_uint_fast64_t s_disk_guard_last_check_ms = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_last_free_bytes = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_last_output_bytes = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_output_rate_bps = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t s_disk_guard_last_required_bytes = ATOMIC_VAR_INIT(0);

typedef struct {
    FILE *fp;
    char path[512];
    atomic_uint_fast64_t write_offset;
    atomic_uint_fast64_t read_offset;
    atomic_uint_fast64_t last_backlog_log_mark;
    atomic_bool forced_mode;
    atomic_bool opened;
    atomic_flag io_lock;
} gui_record_spill_channel_t;

static gui_record_spill_channel_t s_record_spill[GUI_RECORD_SPILL_CHANNELS];

// Per-channel output-file write error flags. Set by the writer threads when
// the FLAC encoder or RAW fwrite fails (e.g. file locked by another app for
// viewing). When set, the writer spills raw data to the temp file to protect
// the capture, and the UI flashes the finalizing icon at capture stop.
static atomic_bool s_record_write_error[GUI_RECORD_SPILL_CHANNELS] = {
    ATOMIC_VAR_INIT(false), ATOMIC_VAR_INIT(false)
};

static const char *gui_record_spill_channel_name(int channel) {
    return (channel == 0) ? "A" : "B";
}

static void gui_record_spill_lock(gui_record_spill_channel_t *spill) {
    while (atomic_flag_test_and_set(&spill->io_lock)) {
        thrd_sleep_ms(0);
    }
}

static void gui_record_spill_unlock(gui_record_spill_channel_t *spill) {
    atomic_flag_clear(&spill->io_lock);
}

static bool gui_record_spill_valid_channel(int channel) {
    return (channel >= 0 && channel < GUI_RECORD_SPILL_CHANNELS);
}

static void gui_record_reset_disk_guard_state(void) {
    atomic_store(&s_disk_guard_tripped, false);
    atomic_store(&s_disk_guard_last_check_ms, 0);
    atomic_store(&s_disk_guard_last_free_bytes, 0);
    atomic_store(&s_disk_guard_last_output_bytes, 0);
    atomic_store(&s_disk_guard_output_rate_bps, 0);
    atomic_store(&s_disk_guard_last_required_bytes, 0);
}

static bool gui_record_get_free_space_bytes(const char *path, uint64_t *free_bytes_out) {
    if (!path || !path[0] || !free_bytes_out) {
        return false;
    }

#if defined(_WIN32) || defined(_WIN64)
    ULARGE_INTEGER free_bytes_available;
    if (!GetDiskFreeSpaceExA(path, &free_bytes_available, NULL, NULL)) {
        return false;
    }
    *free_bytes_out = (uint64_t)free_bytes_available.QuadPart;
    return true;
#else
    struct statvfs fs_stats;
    if (statvfs(path, &fs_stats) != 0) {
        return false;
    }
    *free_bytes_out = (uint64_t)fs_stats.f_bavail * (uint64_t)fs_stats.f_frsize;
    return true;
#endif
}

#if defined(_WIN32) || defined(_WIN64)
#define GUI_RECORD_FSEEK(stream, offset, whence) _fseeki64((stream), (__int64)(offset), (whence))
#else
#define GUI_RECORD_FSEEK(stream, offset, whence) fseeko((stream), (off_t)(offset), (whence))
#endif

#if !defined(_WIN32) && !defined(_WIN64)
static bool gui_record_spill_open_channel_in_dir(const char *base_dir,
                                                 int channel,
                                                 FILE **out_fp,
                                                 char *out_path,
                                                 size_t out_path_size) {
    if (!base_dir || !base_dir[0] || !out_fp || !out_path || out_path_size == 0) {
        return false;
    }

    char template_path[512];
    snprintf(template_path, sizeof(template_path), "%s/.misrc_record_spill_%s_XXXXXX",
             base_dir, gui_record_spill_channel_name(channel));

    int fd = mkstemp(template_path);
    if (fd < 0) {
        return false;
    }

    FILE *fp = fdopen(fd, "w+b");
    if (!fp) {
        close(fd);
        unlink(template_path);
        return false;
    }

    (void)setvbuf(fp, NULL, _IOFBF, 1024 * 1024);
    *out_fp = fp;
    snprintf(out_path, out_path_size, "%s", template_path);
    return true;
}
#endif

static bool gui_record_spill_open_channel(gui_app_t *app, int channel, char *error_msg, size_t error_msg_size) {
    if (!gui_record_spill_valid_channel(channel)) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (atomic_load(&spill->opened)) {
        return true;
    }

    FILE *fp = NULL;
    char path_buf[512] = {0};
    const char *output_dir = NULL;

#if defined(_WIN32) || defined(_WIN64)
    fp = tmpfile();
    if (fp) {
        snprintf(path_buf, sizeof(path_buf), "%s", "(tmpfile)");
    }
    if (fp) {
        (void)setvbuf(fp, NULL, _IOFBF, 1024 * 1024);
    }
#else
    output_dir = (app && app->settings.output_path[0]) ? app->settings.output_path : NULL;
    const char *tmp_dir = getenv("TMPDIR");
    if (!tmp_dir || !tmp_dir[0]) {
        tmp_dir = "/tmp";
    }
    int last_err = 0;
    // Keep spill files on the capture target filesystem so free-space
    // accounting tracks the real recording destination.
    if (output_dir && output_dir[0]) {
        if (!gui_record_spill_open_channel_in_dir(output_dir, channel, &fp, path_buf, sizeof(path_buf))) {
            last_err = errno;
        }
    } else if (!gui_record_spill_open_channel_in_dir(tmp_dir, channel, &fp, path_buf, sizeof(path_buf))) {
        last_err = errno;
    }
    if (!fp && last_err != 0) {
        errno = last_err;
    }
#endif

    if (!fp) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size,
                     "Failed to open spill temp file for channel %s in capture path %s: %s",
                     gui_record_spill_channel_name(channel),
                     (output_dir && output_dir[0]) ? output_dir : "(unset)",
                     strerror(errno));
        }
        return false;
    }

    spill->fp = fp;
    snprintf(spill->path, sizeof(spill->path), "%s", path_buf);
    atomic_store(&spill->write_offset, 0);
    atomic_store(&spill->read_offset, 0);
    atomic_store(&spill->last_backlog_log_mark, 0);
    atomic_store(&spill->opened, true);

    return true;
}

static void gui_record_spill_close_channel(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    FILE *fp = spill->fp;
    spill->fp = NULL;
    if (fp) {
        fclose(fp);
    }
#if !defined(_WIN32) && !defined(_WIN64)
    if (spill->path[0] != '\0' && strcmp(spill->path, "(tmpfile)") != 0) {
        unlink(spill->path);
    }
#endif
    spill->path[0] = '\0';
    atomic_store(&spill->write_offset, 0);
    atomic_store(&spill->read_offset, 0);
    atomic_store(&spill->last_backlog_log_mark, 0);
    atomic_store(&spill->forced_mode, false);
    atomic_store(&spill->opened, false);
}

static void gui_record_spill_reset_all(void) {
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        atomic_flag_clear(&s_record_spill[i].io_lock);
        gui_record_spill_close_channel(i);
    }
}

static uint64_t gui_record_spill_backlog_bytes_locked(int channel) {
    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    uint64_t write_off = atomic_load(&spill->write_offset);
    uint64_t read_off = atomic_load(&spill->read_offset);
    return (write_off >= read_off) ? (write_off - read_off) : 0;
}

static uint64_t gui_record_spill_backlog_total_bytes(void) {
    uint64_t total = 0;
    for (int channel = 0; channel < GUI_RECORD_SPILL_CHANNELS; channel++) {
        total += gui_record_spill_backlog_bytes_locked(channel);
    }
    return total;
}

static uint64_t gui_record_get_raw_total_bytes(const gui_app_t *app) {
    if (!app) return 0;
    return atomic_load(&app->recording_raw_a) + atomic_load(&app->recording_raw_b);
}

static uint64_t gui_record_get_encoded_total_bytes(const gui_app_t *app) {
    if (!app) return 0;
    return atomic_load(&app->recording_compressed_a) + atomic_load(&app->recording_compressed_b);
}

/* NOTE: the reference video is included here; the audio WAVs still are not,
 * which is a pre-existing under-estimate this narrows without closing. The
 * video can be the largest single output under FFV1, so leaving it out would
 * make the runway estimate badly optimistic. */
static uint64_t gui_record_get_effective_output_total_bytes(const gui_app_t *app,
                                                            uint64_t raw_total,
                                                            uint64_t encoded_total) {
    if (!app) return 0;
    if (app->settings.use_flac) {
        return ((encoded_total > 0) ? encoded_total : raw_total)
             + gui_video_record_output_bytes();
    }
    return raw_total + gui_video_record_output_bytes();
}

static uint64_t gui_record_estimate_output_bytes_from_raw_backlog(const gui_app_t *app,
                                                                  uint64_t backlog_raw_bytes,
                                                                  uint64_t raw_total,
                                                                  uint64_t encoded_total) {
    if (!app || backlog_raw_bytes == 0) return 0;
    if (!app->settings.use_flac) return backlog_raw_bytes;

    double encoded_per_raw = 1.0;
    if (raw_total > 0 && encoded_total > 0) {
        encoded_per_raw = (double)encoded_total / (double)raw_total;
        if (encoded_per_raw < 0.10) encoded_per_raw = 0.10;
        if (encoded_per_raw > 1.00) encoded_per_raw = 1.00;
    }

    double estimate = (double)backlog_raw_bytes * encoded_per_raw;
    if (estimate >= (double)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)estimate;
}
static void gui_record_spill_recycle_if_drained(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (!atomic_load(&spill->opened) || !spill->fp) {
        return;
    }

    uint64_t write_off = atomic_load(&spill->write_offset);
    uint64_t read_off = atomic_load(&spill->read_offset);
    if (write_off != read_off) {
        return;
    }

    gui_record_spill_lock(spill);
    write_off = atomic_load(&spill->write_offset);
    read_off = atomic_load(&spill->read_offset);
    if (write_off == read_off && spill->fp) {
        bool truncated_ok = false;
        (void)GUI_RECORD_FSEEK(spill->fp, 0, SEEK_SET);
#if defined(_WIN32) || defined(_WIN64)
        int fd = _fileno(spill->fp);
        if (fd >= 0) {
            if (fflush(spill->fp) == 0 && _chsize_s(fd, 0) == 0) {
                truncated_ok = true;
            }
        }
#else
        int fd = fileno(spill->fp);
        if (fd >= 0) {
            if (fflush(spill->fp) == 0 && ftruncate(fd, 0) == 0) {
                truncated_ok = true;
            }
        }
#endif
        if (!truncated_ok) {
            clearerr(spill->fp);
        }
        (void)GUI_RECORD_FSEEK(spill->fp, 0, SEEK_SET);
        atomic_store(&spill->write_offset, 0);
        atomic_store(&spill->read_offset, 0);
        atomic_store(&spill->last_backlog_log_mark, 0);
        atomic_store(&spill->forced_mode, false);
    }
    gui_record_spill_unlock(spill);
}

static bool gui_record_spill_read_block(int channel, int16_t *dst, size_t bytes) {
    if (!gui_record_spill_valid_channel(channel) || !dst || bytes == 0) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    if (!atomic_load(&spill->opened) || !spill->fp) {
        return false;
    }

    uint64_t write_off = atomic_load(&spill->write_offset);
    uint64_t read_off = atomic_load(&spill->read_offset);
    if (write_off < read_off || (write_off - read_off) < bytes) {
        return false;
    }

    bool ok = false;
    gui_record_spill_lock(spill);
    if (GUI_RECORD_FSEEK(spill->fp, read_off, SEEK_SET) == 0) {
        size_t nread = fread((void *)dst, 1, bytes, spill->fp);
        if (nread == bytes) {
            ok = true;
        } else {
            clearerr(spill->fp);
        }
    }
    gui_record_spill_unlock(spill);

    if (ok) {
        atomic_store(&spill->read_offset, read_off + bytes);
        gui_record_spill_recycle_if_drained(channel);
    }

    return ok;
}

bool gui_record_spill_is_forced(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return false;
    }
    return atomic_load(&s_record_spill[channel].forced_mode);
}

void gui_record_spill_clear_forced(int channel) {
    if (!gui_record_spill_valid_channel(channel)) {
        return;
    }
    // Clear the sticky flag so the extraction thread resumes direct
    // ringbuffer writes. The spill temp file itself is recycled by the
    // existing gui_record_spill_recycle_if_drained() path once the
    // writer thread drains it.
    atomic_store(&s_record_spill[channel].forced_mode, false);
}

bool gui_record_spill_enqueue(gui_app_t *app, int channel, const int16_t *samples, size_t bytes,
                              uint32_t frame_index, char *error_msg, size_t error_msg_size) {
    if (!gui_record_spill_valid_channel(channel) || !samples || bytes == 0) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Invalid spill enqueue request");
        }
        return false;
    }

    if (!gui_record_spill_open_channel(app, channel, error_msg, error_msg_size)) {
        return false;
    }

    gui_record_spill_channel_t *spill = &s_record_spill[channel];
    bool first_force = !atomic_exchange(&spill->forced_mode, true);
    if (first_force && app) {
        char msg[640];
        snprintf(msg, sizeof(msg),
                 "Record buffer backpressure on channel %s at frame=%u. Enabling temp spill file: %s",
                 gui_record_spill_channel_name(channel), frame_index,
                 spill->path[0] ? spill->path : "(temp)");
        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
    }

    uint64_t write_off = atomic_load(&spill->write_offset);
    bool ok = false;
    gui_record_spill_lock(spill);
    if (GUI_RECORD_FSEEK(spill->fp, write_off, SEEK_SET) == 0) {
        size_t nwritten = fwrite(samples, 1, bytes, spill->fp);
        if (nwritten == bytes) {
            ok = true;
        } else {
            clearerr(spill->fp);
        }
    }
    gui_record_spill_unlock(spill);

    if (!ok) {
        if (error_msg && error_msg_size > 0) {
            snprintf(error_msg, error_msg_size, "Failed writing spill data for channel %s: %s",
                     gui_record_spill_channel_name(channel), strerror(errno));
        }
        return false;
    }

    atomic_store(&spill->write_offset, write_off + bytes);
    uint64_t backlog = gui_record_spill_backlog_bytes_locked(channel);
    uint64_t mark = backlog / GUI_RECORD_SPILL_LOG_STEP_BYTES;
    uint64_t last_mark = atomic_load(&spill->last_backlog_log_mark);
    if (mark > last_mark) {
        atomic_store(&spill->last_backlog_log_mark, mark);
        if (app) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Spill backlog channel %s: %.2f MB",
                     gui_record_spill_channel_name(channel),
                     (double)backlog / (1024.0 * 1024.0));
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
    }

    return true;
}

/* writer_ctx_t is defined with gui_record_session_t near the top of the file. */

#if LIBSOXR_ENABLED
#include <soxr.h>

static soxr_quality_spec_t soxr_quality_from_setting(int q) {
    // Map GUI setting 0-4 to soxr quality presets
    // 0=QQ, 1=LQ, 2=MQ, 3=HQ, 4=VHQ
    switch (q) {
        case 0: return soxr_quality_spec(SOXR_QQ, 0);
        case 1: return soxr_quality_spec(SOXR_LQ, 0);
        case 2: return soxr_quality_spec(SOXR_MQ, 0);
        case 3: return soxr_quality_spec(SOXR_HQ, 0);
        case 4: return soxr_quality_spec(SOXR_VHQ, 0);
        default: return soxr_quality_spec(SOXR_HQ, 0);
    }
}

static soxr_t ensure_soxr(writer_ctx_t *wctx, float out_rate_khz) {
    if (!wctx) return NULL;

    // Only support downsampling (or passthrough) like CLI
    if (out_rate_khz <= 0.0f || out_rate_khz >= 40000.0f) {
        return NULL;
    }

    // Reuse existing if already configured
    if (wctx->soxr && fabsf(wctx->soxr_rate_khz - out_rate_khz) < 1e-3f) {
        return (soxr_t)wctx->soxr;
    }

    if (wctx->soxr) {
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
    }

    soxr_error_t err = NULL;
    soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);

    // Only apply user gain (no implicit scaling)
    io_spec.scale = pow(10.0, (double)wctx->resample_gain_db / 20.0);

    soxr_quality_spec_t qual_spec = soxr_quality_from_setting(wctx->resample_quality);

    soxr_t s = soxr_create(40000.0, (double)out_rate_khz, 1, &err, &io_spec, &qual_spec, NULL);
    if (!s || err) {
        if (s) soxr_delete(s);
        return NULL;
    }

    wctx->soxr = (void *)s;
    wctx->soxr_rate_khz = out_rate_khz;
    return s;
}
#endif

#if LIBFLAC_ENABLED == 1
// Error callback for GUI FLAC writer
static void gui_flac_error_callback(void *user_data, flac_writer_error_t error, const char *message) {
    (void)error;
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->app) {
        gui_app_set_status(wctx->app, message);
        gui_record_log_capture_event(wctx->app, "ERROR", message, GUI_ERROR_CLASS_SYSTEM, 1);
    }
    fprintf(stderr, "FLAC ERROR: %s\n", message);
}

// Bytes written callback for compression ratio tracking
static void gui_flac_bytes_callback(void *user_data, size_t bytes_written) {
    writer_ctx_t *wctx = (writer_ctx_t *)user_data;
    if (wctx && wctx->compressed_bytes) {
        atomic_fetch_add(wctx->compressed_bytes, bytes_written);
    }
}
static inline int8_t gui_record_sample_12bit_to_i8(int16_t sample) {
    // BUF_RECORD samples are signed 12-bit values carried in int16_t.
    // Downscale to signed 8-bit with nearest rounding.
    int32_t v = sample;
    if (v >= 0) {
        v = (v + 8) >> 4;
    } else {
        v = -(((-v) + 8) >> 4);
    }
    if (v > 127) v = 127;
    if (v < -128) v = -128;
    return (int8_t)v;
}

static void convert_i16_to_flac_i32(int32_t *dst, const int16_t *src, size_t n, uint8_t bits) {
    if (!dst || !src || n == 0) return;

    if (bits == 8) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = (int32_t)gui_record_sample_12bit_to_i8(src[i]);
        }
        return;
    }

    if (bits == 12) {
        for (size_t i = 0; i < n; i++) {
            dst[i] = (int32_t)src[i];
        }
        return;
    }

    // 16-bit output: expand 12-bit capture samples to 16-bit range
    for (size_t i = 0; i < n; i++) {
        dst[i] = (int32_t)src[i] << 4;
    }
}

static bool gui_record_get_next_block(writer_ctx_t *wctx, size_t block_bytes, int timeout_ms,
                                      int16_t *spill_block, const int16_t **out_samples,
                                      bool *from_ringbuffer) {
    if (!wctx || !spill_block || !out_samples || !from_ringbuffer) {
        return false;
    }

    void *buf = bufmgr_read_begin(wctx->bufmgr, wctx->buf_id, block_bytes, timeout_ms);
    if (buf) {
        *out_samples = (const int16_t *)buf;
        *from_ringbuffer = true;
        return true;
    }

    if (gui_record_spill_read_block(wctx->channel, spill_block, block_bytes)) {
        *out_samples = spill_block;
        *from_ringbuffer = false;
        return true;
    }

    return false;
}

// FLAC file writer thread
static int flac_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;
    size_t len = GUI_RECORD_WRITER_BLOCK_BYTES;
    size_t raw_bytes_per_block = GUI_RECORD_WRITER_BLOCK_BYTES;
    bool flac_encoder_error_logged = false;

    // Boost thread priority to avoid backpressure when window is minimized
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    // Scratch buffers
    int16_t *tmp_i16 = NULL;
    int32_t *tmp_i32 = NULL;
    size_t tmp_cap = 0;

#if LIBSOXR_ENABLED
    // Max output samples per block (downsampling, so <= input, but keep some slack)
    size_t max_out = GUI_RECORD_WRITER_BLOCK_SAMPLES;
    tmp_i16 = (int16_t *)aligned_alloc(32, max_out * sizeof(int16_t));
    tmp_i32 = (int32_t *)aligned_alloc(32, max_out * sizeof(int32_t));
    tmp_cap = max_out;
#else
    // No soxr: only need int32 conversion buffer
    tmp_i32 = (int32_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * sizeof(int32_t));
    tmp_cap = GUI_RECORD_WRITER_BLOCK_SAMPLES;
#endif

    if (!tmp_i32) {
        fprintf(stderr, "[FLAC] Failed to allocate conversion buffers\n");
        if (wctx && wctx->app) {
            gui_record_log_capture_event(wctx->app, "ERROR", "FLAC writer failed to allocate conversion buffers",
                                         GUI_ERROR_CLASS_SYSTEM, 1);
        }
        return 0;
    }
    int16_t *spill_i16 = (int16_t *)aligned_alloc(32, len);
    if (!spill_i16) {
#if LIBSOXR_ENABLED
        if (tmp_i16) aligned_free(tmp_i16);
#endif
        aligned_free(tmp_i32);
        fprintf(stderr, "[FLAC] Failed to allocate spill read buffer\n");
        if (wctx && wctx->app) {
            gui_record_log_capture_event(wctx->app, "ERROR", "FLAC writer failed to allocate spill buffer",
                                         GUI_ERROR_CLASS_SYSTEM, 1);
        }
        return 0;
    }

    fprintf(stderr, "[FLAC] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    if (wctx->writer) {
        flac_writer_error_t aff_err = flac_writer_apply_thread_affinity(wctx->writer);
        if (aff_err != FLAC_WRITER_OK) {
            fprintf(stderr, "[FLAC] Writer thread %c affinity warning: %s\n",
                    wctx->channel == 0 ? 'A' : 'B',
                    flac_writer_get_error_string(wctx->writer));
        }
    }

    while (1) {
        const int16_t *in = NULL;
        bool from_ringbuffer = false;
        bool encode_failed = false;
        int timeout_ms = (atomic_load(&do_exit) || !atomic_load(&wctx->ses->recording)) ? 0 : 10;

        if (!gui_record_get_next_block(wctx, len, timeout_ms, spill_i16, &in, &from_ringbuffer)) {
            if (timeout_ms == 0) {
                break;
            }
            continue;
        }
        size_t out_n = GUI_RECORD_WRITER_BLOCK_SAMPLES;
        bool encoded_block = false;

#if LIBSOXR_ENABLED
        if (wctx->enable_resample &&
            wctx->resample_rate_khz > 0.0f &&
            wctx->resample_rate_khz < 40000.0f) {
            soxr_t s = ensure_soxr(wctx, wctx->resample_rate_khz);
            if (s) {
                size_t in_done = 0, out_done = 0;
                soxr_error_t err = soxr_process(s, in, GUI_RECORD_WRITER_BLOCK_SAMPLES, &in_done,
                                               tmp_i16, tmp_cap, &out_done);
                if (!err && out_done > 0) {
                    out_n = out_done;
                    convert_i16_to_flac_i32(tmp_i32, tmp_i16, out_n, wctx->flac_bits_per_sample);
                    int result = flac_writer_process(wctx->writer, tmp_i32, (uint32_t)out_n);
                    if (result < 0 && !flac_encoder_error_logged) {
                        char msg[128];
                        snprintf(msg, sizeof(msg), "FLAC encoder error on channel %c", wctx->channel == 0 ? 'A' : 'B');
                        fprintf(stderr, "%s\n", msg);
                        if (wctx && wctx->app) {
                            gui_record_log_capture_event(wctx->app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
                        }
                        flac_encoder_error_logged = true;
                    }
                    if (result < 0) {
                        encode_failed = true;
                    } else {
                        encoded_block = true;
                    }
                }
            }
        }
#endif
        if (!encode_failed && !encoded_block) {
            convert_i16_to_flac_i32(tmp_i32, in, GUI_RECORD_WRITER_BLOCK_SAMPLES, wctx->flac_bits_per_sample);
            int result = flac_writer_process(wctx->writer, tmp_i32, GUI_RECORD_WRITER_BLOCK_SAMPLES);
            if (result < 0 && !flac_encoder_error_logged) {
                char msg[128];
                snprintf(msg, sizeof(msg), "FLAC encoder error on channel %c", wctx->channel == 0 ? 'A' : 'B');
                fprintf(stderr, "%s\n", msg);
                if (wctx && wctx->app) {
                    gui_record_log_capture_event(wctx->app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
                }
                flac_encoder_error_logged = true;
            }
            if (result < 0) {
                encode_failed = true;
            }
        }

        // On encode failure (output file write callback failed, e.g. file
        // locked by another app for viewing): do NOT abort the encoder or
        // stop capture. Spill the raw block to the temp file to preserve the
        // data and protect the capture path from backpressure, set the
        // write-error flag so the UI flashes the finalize icon, and continue.
        // The encoder stays alive; if the file becomes writable again, the
        // next flac_writer_process call's write callback will succeed and
        // encoding resumes (recovery detected below).
        if (encode_failed) {
            atomic_store(&s_record_write_error[wctx->channel], true);
            if (from_ringbuffer && wctx->app) {
                // Spill the raw block before marking it consumed (the
                // ringbuffer memory is still valid here). Blocks that came
                // from the spill are not re-spilled (already preserved).
                char spill_err[256] = {0};
                if (!gui_record_spill_enqueue(wctx->app, wctx->channel,
                                              in, len, 0, spill_err, sizeof(spill_err))) {
                    if (spill_err[0]) {
                        gui_record_log_capture_event(wctx->app, "ERROR", spill_err,
                                                     GUI_ERROR_CLASS_SYSTEM, 1);
                    }
                }
            }
            // Mark ringbuffer blocks as consumed; spill blocks are consumed by file read offset.
            if (from_ringbuffer) {
                bufmgr_read_end(wctx->bufmgr, wctx->buf_id, len);
            }
            continue;
        }

        // Encode succeeded. If we were previously in a write-error state,
        // clear the flag and log recovery so the UI stops flashing.
        if (atomic_load(&s_record_write_error[wctx->channel])) {
            atomic_store(&s_record_write_error[wctx->channel], false);
            flac_encoder_error_logged = false;
            fprintf(stderr, "[FLAC] Channel %c write recovered\n",
                    wctx->channel == 0 ? 'A' : 'B');
            if (wctx->app) {
                gui_record_log_capture_event(wctx->app, "INFO",
                    "FLAC output file write recovered",
                    GUI_ERROR_CLASS_NONE, 0);
            }
        }

        // Mark ringbuffer blocks as consumed; spill blocks are consumed by file read offset.
        if (from_ringbuffer) {
            bufmgr_read_end(wctx->bufmgr, wctx->buf_id, len);
        }

        if (wctx->app) {
            atomic_fetch_add(&wctx->app->recording_bytes, len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&wctx->app->recording_raw_a, raw_bytes_per_block);
            } else {
                atomic_fetch_add(&wctx->app->recording_raw_b, raw_bytes_per_block);
            }
        }
    }

#if LIBSOXR_ENABLED
    if (wctx->soxr) {
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
    }
#endif

    if (tmp_i16) aligned_free(tmp_i16);
    if (tmp_i32) aligned_free(tmp_i32);

    if (spill_i16) aligned_free(spill_i16);
    fprintf(stderr, "[FLAC] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}
#endif

static void convert_i16_to_raw_bytes(uint8_t *dst, const int16_t *src, size_t n, uint8_t bits) {
    if (!dst || !src || n == 0) return;

    if (bits == 8) {
        int8_t *d = (int8_t *)dst;
        for (size_t i = 0; i < n; i++) {
            d[i] = gui_record_sample_12bit_to_i8(src[i]);
        }
        return;
    }

    // 16-bit raw: write int16 as-is
    memcpy(dst, src, n * sizeof(int16_t));
}

// RAW file writer thread
static int raw_writer_thread(void *ctx) {
    writer_ctx_t *wctx = (writer_ctx_t *)ctx;

    // Input is always int16 blocks from BUF_RECORD
    size_t in_len = GUI_RECORD_WRITER_BLOCK_BYTES;

    // Output bytes per sample (1=8-bit, 2=16-bit)
    size_t bps = (wctx->raw_bytes_per_sample == 1) ? 1 : 2;

    // Boost thread priority to avoid backpressure when window is minimized
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

#if LIBSOXR_ENABLED
    int16_t *tmp_i16 = (int16_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * sizeof(int16_t));
    if (!tmp_i16) {
        fprintf(stderr, "[RAW] Failed to allocate resample buffer\n");
        return 0;
    }
#endif
    uint8_t *tmp_out = (uint8_t *)aligned_alloc(32, GUI_RECORD_WRITER_BLOCK_SAMPLES * bps);
    if (!tmp_out) {
        fprintf(stderr, "[RAW] Failed to allocate output buffer\n");
#if LIBSOXR_ENABLED
        aligned_free(tmp_i16);
#endif
        return 0;
    }
    int16_t *spill_i16 = (int16_t *)aligned_alloc(32, in_len);
    if (!spill_i16) {
        fprintf(stderr, "[RAW] Failed to allocate spill read buffer\n");
#if LIBSOXR_ENABLED
        aligned_free(tmp_i16);
#endif
        aligned_free(tmp_out);
        return 0;
    }

    fprintf(stderr, "[RAW] Writer thread %c started\n", wctx->channel == 0 ? 'A' : 'B');

    bool write_error = false;
    double last_retry_time = 0.0;
    uint64_t raw_write_err_count = 0;

    while (1) {
        const int16_t *in = NULL;
        bool from_ringbuffer = false;
        int timeout_ms = (atomic_load(&do_exit) || !atomic_load(&wctx->ses->recording)) ? 0 : 10;

        // When in write-error state (output file locked by another app),
        // drain the ringbuffer to the spill temp file to protect the capture
        // from backpressure. Don't read from the spill (leave it for when the
        // output recovers). Periodically retry the output file; if the retry
        // write succeeds, resume normal operation (which will drain the spill
        // via gui_record_get_next_block).
        if (write_error) {
            void *rb_buf = bufmgr_read_begin(wctx->bufmgr, wctx->buf_id, in_len, timeout_ms);
            if (!rb_buf) {
                if (timeout_ms == 0) {
                    break;
                }
                continue;
            }

            // Throttle retry attempts to every ~2 seconds.
            double now = GetTime();
            bool should_retry = (last_retry_time == 0.0) || (now - last_retry_time >= 2.0);
            if (should_retry) {
                last_retry_time = now;
                clearerr(wctx->file);
                // Seek to end so we append after any previously-written data.
                GUI_RECORD_FSEEK(wctx->file, 0, SEEK_END);
                size_t retry_out_n = GUI_RECORD_WRITER_BLOCK_SAMPLES;
                convert_i16_to_raw_bytes(tmp_out, (const int16_t *)rb_buf, retry_out_n, wctx->rf_bits);
                size_t retry_bytes = retry_out_n * bps;
                size_t written = fwrite(tmp_out, 1, retry_bytes, wctx->file);
                if (written == retry_bytes && !ferror(wctx->file)) {
                    // Write recovered — resume normal operation.
                    write_error = false;
                    atomic_store(&s_record_write_error[wctx->channel], false);
                    fprintf(stderr, "[RAW] Channel %c write recovered\n",
                            wctx->channel == 0 ? 'A' : 'B');
                    if (wctx->app) {
                        gui_record_log_capture_event(wctx->app, "INFO",
                            "RAW output file write recovered",
                            GUI_ERROR_CLASS_NONE, 0);
                    }
                    bufmgr_read_end(wctx->bufmgr, wctx->buf_id, in_len);
                    if (wctx->app) {
                        atomic_fetch_add(&wctx->app->recording_bytes, in_len);
                        if (wctx->channel == 0) {
                            atomic_fetch_add(&wctx->app->recording_raw_a, in_len);
                        } else {
                            atomic_fetch_add(&wctx->app->recording_raw_b, in_len);
                        }
                    }
                    continue;
                }
                clearerr(wctx->file);
            }

            // Spill the block to the temp file (preserve the data).
            char spill_err[256] = {0};
            if (!gui_record_spill_enqueue(wctx->app, wctx->channel,
                                          (const int16_t *)rb_buf, in_len,
                                          0, spill_err, sizeof(spill_err))) {
                if (spill_err[0] && wctx->app) {
                    gui_record_log_capture_event(wctx->app, "ERROR", spill_err,
                                                 GUI_ERROR_CLASS_SYSTEM, 1);
                }
            }
            bufmgr_read_end(wctx->bufmgr, wctx->buf_id, in_len);
            continue;
        }

        if (!gui_record_get_next_block(wctx, in_len, timeout_ms, spill_i16, &in, &from_ringbuffer)) {
            if (timeout_ms == 0) {
                break;
            }
            continue;
        }
        size_t out_n = GUI_RECORD_WRITER_BLOCK_SAMPLES;
        bool wrote_block = false;
        bool write_ok = false;
        size_t write_bytes = 0;

#if LIBSOXR_ENABLED
        if (wctx->enable_resample &&
            wctx->resample_rate_khz > 0.0f &&
            wctx->resample_rate_khz < 40000.0f) {
            soxr_t s = ensure_soxr(wctx, wctx->resample_rate_khz);
            if (s) {
                size_t in_done = 0, out_done = 0;
                soxr_error_t err = soxr_process(s, in, GUI_RECORD_WRITER_BLOCK_SAMPLES, &in_done,
                                               tmp_i16, GUI_RECORD_WRITER_BLOCK_SAMPLES, &out_done);
                if (!err && out_done > 0) {
                    out_n = out_done;
                    convert_i16_to_raw_bytes(tmp_out, tmp_i16, out_n, wctx->rf_bits);
                    write_bytes = out_n * bps;
                    wrote_block = true;
                }
            }
        }
#endif
        if (!wrote_block) {
            convert_i16_to_raw_bytes(tmp_out, in, out_n, wctx->rf_bits);
            write_bytes = out_n * bps;
        }

        if (write_bytes > 0) {
            size_t written = fwrite(tmp_out, 1, write_bytes, wctx->file);
            write_ok = (written == write_bytes && !ferror(wctx->file));
        }

        if (from_ringbuffer) {
            bufmgr_read_end(wctx->bufmgr, wctx->buf_id, in_len);
        }

        if (!write_ok) {
            // Output file write failed (e.g. locked by another app for
            // viewing). Enter write-error state, spill the block to the
            // temp file to preserve the data, and keep the capture running.
            write_error = true;
            atomic_store(&s_record_write_error[wctx->channel], true);
            clearerr(wctx->file);
            raw_write_err_count++;
            if (raw_write_err_count <= 5 || (raw_write_err_count % 1000) == 0) {
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "RAW write error on channel %c (#%llu): output file may be locked",
                         wctx->channel == 0 ? 'A' : 'B',
                         (unsigned long long)raw_write_err_count);
                fprintf(stderr, "[RAW] %s\n", msg);
                if (wctx->app) {
                    gui_record_log_capture_event(wctx->app, "ERROR", msg,
                                                 GUI_ERROR_CLASS_SYSTEM, 1);
                }
            }
            if (from_ringbuffer && wctx->app) {
                char spill_err[256] = {0};
                gui_record_spill_enqueue(wctx->app, wctx->channel, in, in_len,
                                         0, spill_err, sizeof(spill_err));
            }
            continue;
        }

        if (wctx->app) {
            // Approximate byte accounting: count input bytes consumed
            atomic_fetch_add(&wctx->app->recording_bytes, in_len);
            if (wctx->channel == 0) {
                atomic_fetch_add(&wctx->app->recording_raw_a, in_len);
            } else {
                atomic_fetch_add(&wctx->app->recording_raw_b, in_len);
            }
        }
    }

#if LIBSOXR_ENABLED
    if (wctx->soxr) {
        soxr_delete((soxr_t)wctx->soxr);
        wctx->soxr = NULL;
        wctx->soxr_rate_khz = 0.0f;
    }
    aligned_free(tmp_i16);
#endif
    aligned_free(spill_i16);
    aligned_free(tmp_out);

    fprintf(stderr, "[RAW] Writer thread %c exiting\n", wctx->channel == 0 ? 'A' : 'B');
    return 0;
}

// Initialize recording subsystem
void gui_record_init(void) {
    atomic_store(&s_record_stop_finalizing, false);
    atomic_store(&s_record_stop_finalize_done, false);
    gui_record_reset_disk_guard_state();
    gui_record_spill_reset_all();
}

// Cleanup recording subsystem
void gui_record_cleanup(void) {
    while (atomic_load(&s_record_stop_finalizing)) {
        thrd_sleep_ms(10);
    }
    if (atomic_load(&s_finalize_thread_running)) {
        thrd_join(s_finalize_thread, NULL);
        atomic_store(&s_finalize_thread_running, false);
    }
    if (s_finalizing) {
        free(s_finalizing);
        s_finalizing = NULL;
    }
    gui_record_reset_disk_guard_state();
    gui_record_spill_reset_all();
    gui_record_close_session_log();
}

// Check if recording is active
bool gui_record_is_active(void) {
    return s_active != NULL && s_active->app->is_recording;
}

// Check if any recording channel has a persistent output-file write error.
bool gui_record_has_write_error(void) {
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        if (atomic_load(&s_record_write_error[i])) {
            return true;
        }
    }
    return false;
}

// Check if waiting for popup confirmation
bool gui_record_is_pending(void) {
    return s_overwrite_pending;
}
static bool gui_record_level_is_error(const char *level) {
    if (!level) return false;
    return (strcmp(level, "ERROR") == 0 || strcmp(level, "CRITICAL") == 0);
}

void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count) {
    if (!app || !message || !message[0]) {
        return;
    }

    char clean[1024];
    snprintf(clean, sizeof(clean), "%s", message);
    gui_record_trim_trailing_newline(clean);
    if (!clean[0]) {
        return;
    }

    if (gui_record_level_is_error(level)) {
        uint32_t increment = (error_count > 0) ? error_count : 1;
        if (error_class == GUI_ERROR_CLASS_PARSER) {
            gui_app_count_parser_errors(app, increment);
        } else if (error_class == GUI_ERROR_CLASS_SYSTEM) {
            gui_app_count_system_errors(app, increment);
        }
    }
    if (s_active && app == s_active->app) {
        gui_record_log_write_line(level, clean);
    }
}

bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame_index,
                                       char *status_msg, size_t status_msg_size) {
    if (status_msg && status_msg_size > 0) {
        status_msg[0] = '\0';
    }

    if (!app || !app->is_recording || !app->settings.output_path[0]) {
        return false;
    }

    if (atomic_load(&s_disk_guard_tripped)) {
        return true;
    }

    uint64_t now_ms = get_time_ms();
    uint64_t last_check_ms = atomic_load(&s_disk_guard_last_check_ms);
    if (last_check_ms != 0 && now_ms >= last_check_ms &&
        (now_ms - last_check_ms) < GUI_RECORD_DISK_GUARD_CHECK_INTERVAL_MS) {
        return false;
    }
    atomic_store(&s_disk_guard_last_check_ms, now_ms);

    uint64_t free_bytes = 0;
    if (!gui_record_get_free_space_bytes(app->settings.output_path, &free_bytes)) {
        return false;
    }
    atomic_store(&s_disk_guard_last_free_bytes, free_bytes);
    uint64_t raw_total = gui_record_get_raw_total_bytes(app);
    uint64_t encoded_total = gui_record_get_encoded_total_bytes(app);
    uint64_t output_total = gui_record_get_effective_output_total_bytes(app, raw_total, encoded_total);
    uint64_t previous_output_total = atomic_load(&s_disk_guard_last_output_bytes);
    uint64_t output_rate_bps = atomic_load(&s_disk_guard_output_rate_bps);

    if (last_check_ms != 0 && now_ms > last_check_ms && output_total >= previous_output_total) {
        uint64_t elapsed_ms = now_ms - last_check_ms;
        if (elapsed_ms > 0) {
            uint64_t delta_bytes = output_total - previous_output_total;
            uint64_t instant_bps = (delta_bytes * 1000ULL) / elapsed_ms;
            if (output_rate_bps == 0) {
                output_rate_bps = instant_bps;
            } else {
                output_rate_bps = (output_rate_bps * 3ULL + instant_bps) / 4ULL;
            }
            atomic_store(&s_disk_guard_output_rate_bps, output_rate_bps);
        }
    }
    atomic_store(&s_disk_guard_last_output_bytes, output_total);

    uint64_t spill_backlog_raw = gui_record_spill_backlog_total_bytes();
    uint64_t spill_backlog_output = gui_record_estimate_output_bytes_from_raw_backlog(
        app, spill_backlog_raw, raw_total, encoded_total);

    uint64_t projected_growth = 0;
    if (output_rate_bps > 0) {
        double projected = (double)output_rate_bps * GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS;
        projected_growth = (projected >= (double)UINT64_MAX) ? UINT64_MAX : (uint64_t)projected;
    }

    uint64_t required_headroom = GUI_RECORD_DISK_GUARD_MIN_HEADROOM_BYTES;
    if (projected_growth > required_headroom) {
        required_headroom = projected_growth;
    }
    if (spill_backlog_output > (UINT64_MAX - required_headroom)) {
        required_headroom = UINT64_MAX;
    } else {
        required_headroom += spill_backlog_output;
    }
    atomic_store(&s_disk_guard_last_required_bytes, required_headroom);

    if (free_bytes >= required_headroom) {
        return false;
    }

    char free_buf[32];
    char required_buf[32];
    char backlog_buf[32];
    char msg[640];
    double rate_mb_s = (double)output_rate_bps / (1024.0 * 1024.0);
    format_file_size_u64(free_bytes, free_buf, sizeof(free_buf));
    format_file_size_u64(required_headroom, required_buf, sizeof(required_buf));
    format_file_size_u64(spill_backlog_output, backlog_buf, sizeof(backlog_buf));
    snprintf(msg, sizeof(msg),
             "Disk-space guard triggered at extract_frame=%u: free=%s required=%s (lookahead=%.0fs, output_rate=%.2f MB/s, spill_backlog=%s) on output path %s; requesting safe capture stop.",
             frame_index, free_buf, required_buf, GUI_RECORD_DISK_GUARD_LOOKAHEAD_SECONDS,
             rate_mb_s, backlog_buf, app->settings.output_path);
    if (status_msg && status_msg_size > 0) {
        snprintf(status_msg, status_msg_size, "%s", msg);
    }

    if (!atomic_exchange(&s_disk_guard_tripped, true)) {
        gui_record_log_capture_event(app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
    }
    return true;
}

// Forward declaration of actual recording start (after confirmation)
static int gui_record_start_confirmed(gui_app_t *app);

static uint8_t clamp_rf_bits_flac(uint8_t bits) {
    if (bits == 8 || bits == 12 || bits == 16) return bits;
    return 16;
}

#if LIBFLAC_ENABLED == 1
static void gui_record_embed_flac_duration_metadata(gui_app_t *app,
                                                    const char *path,
                                                    const char *channel_label,
                                                    uint64_t total_samples,
                                                    uint32_t sample_rate_hz)
{
    if (!path || !path[0] || total_samples == 0 || sample_rate_hz == 0) return;
    uint64_t rf_sample_rate_hz = (uint64_t)sample_rate_hz * 1000ULL; // RF FLAC stores kHz in STREAMINFO.sample_rate
    double duration_seconds = (double)total_samples / (double)rf_sample_rate_hz;
    if (duration_seconds < 0.0) duration_seconds = 0.0;
    uint64_t length_ms = (uint64_t)llround(duration_seconds * 1000.0);

    char duration_seconds_str[64];
    char length_ms_str[32];
    char total_samples_str[32];
    char sample_rate_str[32];
    char sample_rate_khz_str[32];
    snprintf(duration_seconds_str, sizeof(duration_seconds_str), "%.6f", duration_seconds);
    snprintf(length_ms_str, sizeof(length_ms_str), "%" PRIu64, length_ms);
    snprintf(total_samples_str, sizeof(total_samples_str), "%" PRIu64, total_samples);
    snprintf(sample_rate_str, sizeof(sample_rate_str), "%" PRIu64, rf_sample_rate_hz);
    snprintf(sample_rate_khz_str, sizeof(sample_rate_khz_str), "%u", sample_rate_hz);

    flac_writer_tag_t tags[5] = {
        { "DURATION_SECONDS",   duration_seconds_str },
        { "LENGTH",             length_ms_str },
        { "RF_TOTAL_SAMPLES",   total_samples_str },
        { "RF_SAMPLE_RATE",     sample_rate_str },
        { "RF_SAMPLE_RATE_KHZ", sample_rate_khz_str },
    };
    bool rewrote = false;
    if (!flac_writer_embed_tags(path, tags, 5, &rewrote)) {
        if (app) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed writing FLAC duration metadata for %s (%s)",
                     channel_label ? channel_label : "RF", path);
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
        return;
    }
    if (rewrote && app) {
        /* Legacy file without reserved padding: libFLAC rewrote the whole
         * file, which takes minutes on full-tape captures. */
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "FLAC duration metadata for %s required a full-file rewrite "
                 "(no padding block; capture predates padded layout): %s",
                 channel_label ? channel_label : "RF", path);
        gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
    }
}

// STREAMINFO.total_samples must hold the true Hz-domain sample count even though
// sample_rate is stored in kHz: readers like libsndfile trust it and stop reading
// there, so a scaled-down count silently truncates decodes. libFLAC's encoder
// finish writes the exact count already; this only patches the >2^36 overflow
// case (total_samples -> 0 = unknown). Header-derived durations come out 1000x
// long in naive players; the honest duration lives in the Vorbis tags above.
static void gui_record_finalize_flac_streaminfo(gui_app_t *app,
                                                const char *path,
                                                const char *channel_label,
                                                uint64_t total_samples)
{
    if (!path || !path[0] || total_samples == 0) return;

    if (!flac_writer_finalize_streaminfo(path, total_samples)) {
        if (app) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Failed to finalize FLAC STREAMINFO for %s (%s)",
                     channel_label ? channel_label : "RF", path);
            gui_record_log_capture_event(app, "WARN", msg, GUI_ERROR_CLASS_NONE, 0);
        }
    }
}
#endif

static uint8_t rf_bits_for_raw(uint8_t requested) {
    // RAW supports 8/16 only; treat 12 as 16.
    return (requested == 8) ? 8 : 16;
}

static void format_msps_from_khz(char *dst, size_t dst_len, float khz) {
    if (!dst || dst_len == 0) return;
    double msps = (double)khz / 1000.0;
    // Render without trailing .0 when possible.
    if (fabs(msps - (double)((int)msps)) < 1e-6) {
        snprintf(dst, dst_len, "%dmsps", (int)msps);
    } else {
        snprintf(dst, dst_len, "%.1fmsps", msps);
    }
}

static void sanitize_tag(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src || !src[0]) return;

    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_len; i++) {
        char c = src[i];
        // allow [A-Za-z0-9._-], map spaces to '-'
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dst[j++] = c;
        } else if (c == ' ' || c == '\t') {
            dst[j++] = '-';
        } else {
            // skip other chars (slashes etc.)
        }
    }
    dst[j] = '\0';

    // Trim trailing '-'
    while (j > 0 && dst[j - 1] == '-') {
        dst[--j] = '\0';
    }
}

static bool gui_record_cvbs_preview_enabled_locked(const channel_panel_config_t *config) {
    if (!config) return false;
    return (config->left_view == PANEL_VIEW_CVBS) ||
           (config->split && config->right_view == PANEL_VIEW_CVBS);
}

static void gui_record_open_session_log(gui_app_t *app, const char *path_a, const char *path_b) {
    if (!app) return;

    gui_record_close_session_log();

    const char *base_src = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";
    char base_name[128];
    sanitize_tag(base_name, sizeof(base_name), base_src);
    if (!base_name[0]) {
        snprintf(base_name, sizeof(base_name), "%s", "capture");
    }

    char date_tag[32];
    date_tag[0] = '\0';
    bool have_date_tag = false;
    if (app->settings.capture_a && path_a && path_a[0]) {
        have_date_tag = gui_record_extract_timestamp_token(path_a, date_tag, sizeof(date_tag));
    }
    if (!have_date_tag && app->settings.capture_b && path_b && path_b[0]) {
        have_date_tag = gui_record_extract_timestamp_token(path_b, date_tag, sizeof(date_tag));
    }
    if (!have_date_tag && app->capture_timestamp[0]) {
        snprintf(date_tag, sizeof(date_tag), "%s", app->capture_timestamp);
    }
    if (!date_tag[0]) {
        gui_record_build_system_timestamp(date_tag, sizeof(date_tag));
    }
    if (!date_tag[0]) {
        snprintf(date_tag, sizeof(date_tag), "%s", "session");
    }

    snprintf(s_active->log_path, sizeof(s_active->log_path), "%s/%s_%s_misrc_capture.log",
             app->settings.output_path, base_name, date_tag);

    bool cvbs_preview_a = false;
    bool cvbs_preview_b = false;
    while (atomic_flag_test_and_set(&app->panel_config_lock)) {}
    cvbs_preview_a = gui_record_cvbs_preview_enabled_locked(&app->panel_config_a);
    cvbs_preview_b = gui_record_cvbs_preview_enabled_locked(&app->panel_config_b);
    atomic_flag_clear(&app->panel_config_lock);

    gui_record_log_lock();
    s_active->log_file = fopen(s_active->log_path, "w");
    if (!s_active->log_file) {
        s_active->log_path[0] = '\0';
        gui_record_log_unlock();
        return;
    }

    char msg[1024];
    char iso_ts[32];
    char host_name[128];
    char user_name[128];
    char os_name[256];
    char cpu_model[256];
    gui_record_build_iso8601_timestamp(iso_ts, sizeof(iso_ts));
    gui_record_get_host_name(host_name, sizeof(host_name));
    gui_record_get_user_name(user_name, sizeof(user_name));
    gui_record_get_os_string(os_name, sizeof(os_name));
    gui_record_get_cpu_model(cpu_model, sizeof(cpu_model));
    uint32_t cpu_cores = gui_record_get_cpu_core_count();

    const char *device_name = "unknown";
    if (app->selected_device >= 0 && app->selected_device < app->device_count) {
        device_name = app->devices[app->selected_device].name;
    }

    snprintf(msg, sizeof(msg), "MISRC capture log started (%s)", app->settings.use_flac ? "FLAC" : "RAW");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "computer_name: %s", host_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "computer_model_name: %s", cpu_model);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "computer_cores: %u", (unsigned)cpu_cores);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "user_name: %s", user_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "operating_system_VERSION: %s", os_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "misrc_tools_version: %s", MIRSC_TOOLS_VERSION);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "datetime_start: %s", iso_ts[0] ? iso_ts : "unknown");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_log_path: %s", s_active->log_path);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_base_name: %s", base_src);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "output_path: %s", app->settings.output_path);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_device_name: %s", device_name);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_device_type: %s", gui_record_device_type_name(app));
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "capture_format: %s", app->settings.use_flac ? "FLAC" : "RAW");
    gui_record_log_write_line_locked("INFO", msg);

    snprintf(msg, sizeof(msg), "Capture channels: A=%s B=%s", app->settings.capture_a ? "on" : "off", app->settings.capture_b ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "CVBS preview state: A=%s B=%s",
             cvbs_preview_a ? "on" : "off",
             cvbs_preview_b ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    uint8_t bits_a = app->settings.use_flac ? clamp_rf_bits_flac(app->settings.rf_bits_a) : rf_bits_for_raw(app->settings.rf_bits_a);
    uint8_t bits_b = app->settings.use_flac ? clamp_rf_bits_flac(app->settings.rf_bits_b) : rf_bits_for_raw(app->settings.rf_bits_b);
    snprintf(msg, sizeof(msg), "RF settings: bitsA=%u bitsB=%u resampleA=%s(%.1f kHz) resampleB=%s(%.1f kHz)",
             (unsigned)bits_a, (unsigned)bits_b,
             app->settings.enable_resample_a ? "on" : "off", app->settings.resample_rate_a,
             app->settings.enable_resample_b ? "on" : "off", app->settings.resample_rate_b);
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Capture limits: capture_limit_seconds=%u record_limit_seconds=%u (%s)",
             (unsigned)app->settings.capture_limit_seconds,
             (unsigned)app->settings.record_limit_seconds,
             app->settings.record_limit_seconds > 0 ? "record_timer_armed" : "record_timer_disarmed");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Audio monitor: playback=%s monitor_ch34=%s misrc_mode=%s",
             app->settings.audio_monitor_playback ? "on" : "off",
             app->settings.audio_monitor_ch34 ? "on" : "off",
             app->user_capture_mode_misrc ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "MISRC V1.5/V2.5 A/B swap override: %s",
             app->settings.misrc_v15_v25_ab_swap ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Dropout handling: stop_on_dropout=%s",
             app->settings.stop_on_dropout ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata project: %s",
             app->settings.ingest_project[0] ? app->settings.ingest_project : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_id: %s",
             app->settings.ingest_tape_id[0] ? app->settings.ingest_tape_id : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_format: %s",
             app->settings.ingest_tape_format[0] ? app->settings.ingest_tape_format : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_size: %s",
             app->settings.ingest_tape_size[0] ? app->settings.ingest_tape_size : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_speed: %s",
             app->settings.ingest_tape_speed[0] ? app->settings.ingest_tape_speed : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata tape_condition: %s",
             app->settings.ingest_tape_condition[0] ? app->settings.ingest_tape_condition : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata operator: %s",
             app->settings.ingest_operator[0] ? app->settings.ingest_operator : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata location: %s",
             app->settings.ingest_location[0] ? app->settings.ingest_location : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);
    snprintf(msg, sizeof(msg), "Ingest metadata notes: %s",
             app->settings.ingest_notes[0] ? app->settings.ingest_notes : "(empty)");
    gui_record_log_write_line_locked("INFO", msg);

    if (app->settings.use_flac) {
        snprintf(msg, sizeof(msg), "FLAC settings: level=%d verify=%s threads=%d",
                 app->settings.flac_level,
                 app->settings.flac_verification ? "on" : "off",
                 app->settings.flac_threads);
        gui_record_log_write_line_locked("INFO", msg);
        snprintf(msg, sizeof(msg), "FLAC affinity: enabled=%s cpu_list=%s support=%s",
                 app->settings.flac_affinity_enabled ? "on" : "off",
                 app->settings.flac_affinity_cpu_list[0] ? app->settings.flac_affinity_cpu_list : "(none)",
                 flac_writer_affinity_supported() ? "linux" : "unsupported");
        gui_record_log_write_line_locked("INFO", msg);
    }

    if (app->settings.capture_a && path_a && path_a[0]) {
        snprintf(msg, sizeof(msg), "FILE_PATH_A: %s", path_a);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (app->settings.capture_b && path_b && path_b[0]) {
        snprintf(msg, sizeof(msg), "FILE_PATH_B: %s", path_b);
        gui_record_log_write_line_locked("INFO", msg);
    }

    snprintf(msg, sizeof(msg), "Audio outputs: 4ch=%s 2ch12=%s 2ch34=%s",
             app->settings.enable_audio_4ch ? "on" : "off",
             app->settings.enable_audio_2ch_12 ? "on" : "off",
             app->settings.enable_audio_2ch_34 ? "on" : "off");
    gui_record_log_write_line_locked("INFO", msg);

    /* Built from settings alone. This function runs before the video spawn in
     * the FLAC path and after it in the RAW path, so it must not read live
     * video-module state or the two logs would disagree. */
    snprintf(msg, sizeof(msg), "Reference video: enabled=%s codec=%s",
             app->settings.video_record_enabled ? "on" : "off",
             app->settings.video_record_codec == 1 ? "FFV1" : "H.264");
    gui_record_log_write_line_locked("INFO", msg);

    if (app->settings.video_record_enabled && app->settings.video_filename[0]) {
        snprintf(msg, sizeof(msg), "VIDEO_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.video_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }

    if (app->settings.enable_audio_4ch && app->settings.audio_4ch_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_4CH_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_4ch_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (app->settings.enable_audio_2ch_12 && app->settings.audio_2ch_12_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_2CH_12_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_2ch_12_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    if (app->settings.enable_audio_2ch_34 && app->settings.audio_2ch_34_filename[0]) {
        snprintf(msg, sizeof(msg), "AUDIO_2CH_34_FILE_PATH: %s/%s",
                 app->settings.output_path, app->settings.audio_2ch_34_filename);
        gui_record_log_write_line_locked("INFO", msg);
    }
    for (int i = 0; i < 4; i++) {
        if (app->settings.enable_audio_1ch[i] && app->settings.audio_1ch_filenames[i][0]) {
            snprintf(msg, sizeof(msg), "AUDIO_1CH_%d_FILE_PATH: %s/%s",
                     i + 1, app->settings.output_path, app->settings.audio_1ch_filenames[i]);
            gui_record_log_write_line_locked("INFO", msg);
        }
    }

    gui_record_log_unlock();
}

static void gui_record_apply_auto_names(gui_app_t *app) {
    if (!app) return;
    if (!app->settings.auto_names_enabled) return;

    const char *base = app->settings.output_base_name[0] ? app->settings.output_base_name : "capture";

    // Optionally append system timestamp sampled at record-start.
    // This does not mutate output_base_name.
    char base_with_ts[256];
    if (app->settings.append_timestamp_on_capture_start) {
        char timestamp_now[32];
        gui_record_build_system_timestamp(timestamp_now, sizeof(timestamp_now));
        if (timestamp_now[0]) {
            snprintf(base_with_ts, sizeof(base_with_ts), "%s_%s", base, timestamp_now);
            base = base_with_ts;
        }
    }

    // RF filenames
    if (app->settings.use_flac) {
        uint8_t bits_a = clamp_rf_bits_flac(app->settings.rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(app->settings.rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (app->settings.enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), app->settings.resample_rate_a);
        if (app->settings.enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), app->settings.resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), app->settings.rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), app->settings.rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.flac", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.flac", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.flac", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.flac", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.flac", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.flac", base, (unsigned)bits_b);
        }
    } else {
        // RAW: 8/16 only
        uint8_t bits_a = rf_bits_for_raw(app->settings.rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(app->settings.rf_bits_b);
        char rate_tag_a[32] = {0};
        char rate_tag_b[32] = {0};
        char rf_tag_a[40] = {0};
        char rf_tag_b[40] = {0};
        if (app->settings.enable_resample_a) format_msps_from_khz(rate_tag_a, sizeof(rate_tag_a), app->settings.resample_rate_a);
        if (app->settings.enable_resample_b) format_msps_from_khz(rate_tag_b, sizeof(rate_tag_b), app->settings.resample_rate_b);
        sanitize_tag(rf_tag_a, sizeof(rf_tag_a), app->settings.rf_channel_tags[0]);
        sanitize_tag(rf_tag_b, sizeof(rf_tag_b), app->settings.rf_channel_tags[1]);

        if (rf_tag_a[0] && rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_a, (unsigned)bits_a, rate_tag_a);
        } else if (rf_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_a, (unsigned)bits_a);
        } else if (rate_tag_a[0]) {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit_%s.raw", base, (unsigned)bits_a, rate_tag_a);
        } else {
            snprintf(app->settings.output_filename_a, MAX_FILENAME_LEN, "rfA_%s_%u-bit.raw", base, (unsigned)bits_a);
        }
        if (rf_tag_b[0] && rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit_%s.raw", base, rf_tag_b, (unsigned)bits_b, rate_tag_b);
        } else if (rf_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "%s_%s_%u-bit.raw", base, rf_tag_b, (unsigned)bits_b);
        } else if (rate_tag_b[0]) {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit_%s.raw", base, (unsigned)bits_b, rate_tag_b);
        } else {
            snprintf(app->settings.output_filename_b, MAX_FILENAME_LEN, "rfB_%s_%u-bit.raw", base, (unsigned)bits_b);
        }
    }

    // Audio filenames (WAV)
    char audio_tag_4ch[40] = {0};
    char audio_tag_12[40] = {0};
    char audio_tag_34[40] = {0};
    sanitize_tag(audio_tag_4ch, sizeof(audio_tag_4ch), app->settings.audio_output_tags[0]);
    sanitize_tag(audio_tag_12, sizeof(audio_tag_12), app->settings.audio_output_tags[1]);
    sanitize_tag(audio_tag_34, sizeof(audio_tag_34), app->settings.audio_output_tags[2]);

    if (audio_tag_4ch[0]) {
        snprintf(app->settings.audio_4ch_filename, MAX_FILENAME_LEN, "%s_%s_quad_4ch.wav", base, audio_tag_4ch);
    } else {
        snprintf(app->settings.audio_4ch_filename, MAX_FILENAME_LEN, "%s_quad_4ch.wav", base);
    }

    /* Reference video -- must stay in lockstep with the same block in
     * gui_settings_refresh_auto_names(). The only intended difference between
     * the two functions is that base already carries the record-start
     * timestamp here. */
    char video_tag[40] = {0};
    sanitize_tag(video_tag, sizeof(video_tag), app->settings.video_output_tag);
    if (video_tag[0]) {
        snprintf(app->settings.video_filename, MAX_FILENAME_LEN, "%s_%s_video.mkv", base, video_tag);
    } else {
        snprintf(app->settings.video_filename, MAX_FILENAME_LEN, "%s_video.mkv", base);
    }
    if (audio_tag_12[0]) {
        snprintf(app->settings.audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch1_ch2.wav", base, audio_tag_12);
    } else {
        snprintf(app->settings.audio_2ch_12_filename, MAX_FILENAME_LEN, "%s_stereo_ch1_ch2.wav", base);
    }
    if (audio_tag_34[0]) {
        snprintf(app->settings.audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_%s_stereo_ch3_ch4.wav", base, audio_tag_34);
    } else {
        snprintf(app->settings.audio_2ch_34_filename, MAX_FILENAME_LEN, "%s_stereo_ch3_ch4.wav", base);
    }

    for (int i = 0; i < 4; i++) {
        char tag[40];
        sanitize_tag(tag, sizeof(tag), app->settings.audio_1ch_labels[i]);
        if (tag[0]) {
            snprintf(app->settings.audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_%s_audio_ch%d.wav", base, tag, i + 1);
        } else {
            snprintf(app->settings.audio_1ch_filenames[i], MAX_FILENAME_LEN, "%s_audio_ch%d.wav", base, i + 1);
        }
    }
}

// Start recording - checks for file existence first
/* Prints what both auto-name functions produce from one settings blob. They
 * are separate implementations that must agree, and the only sanctioned
 * difference is the record-start timestamp -- so with timestamping off they
 * must match exactly, and with it on they must differ only by that segment. */
/* Round-trips the reference-video settings through the on-disk file. Point
 * XDG_CONFIG_HOME at a scratch directory before running, or this rewrites the
 * real settings file. */
int gui_record_video_settings_test_main(void)
{
    gui_settings_t a;
    memset(&a, 0, sizeof(a));
    gui_settings_load(&a);

    /* Values chosen to be different from every default, so a field that is
     * silently not persisted shows up as a mismatch rather than a coincidence. */
    a.video_record_enabled = true;
    a.video_record_codec = 1;                       /* FFV1 */
    snprintf(a.video_output_tag, sizeof(a.video_output_tag), "refcam");
    snprintf(a.ffmpeg_path, sizeof(a.ffmpeg_path), "/opt/custom/ffmpeg");
    gui_settings_save(&a);

    gui_settings_t b;
    memset(&b, 0, sizeof(b));
    gui_settings_load(&b);

    int rc = 0;
    printf("round-trip:\n");
    printf("  video_record_enabled : %d -> %d\n", a.video_record_enabled, b.video_record_enabled);
    printf("  video_record_codec   : %d -> %d\n", a.video_record_codec, b.video_record_codec);
    printf("  video_output_tag     : %s -> %s\n", a.video_output_tag, b.video_output_tag);
    printf("  ffmpeg_path          : %s -> %s\n", a.ffmpeg_path, b.ffmpeg_path);
    printf("  video_filename       : %s\n", b.video_filename);

    if (a.video_record_enabled != b.video_record_enabled) { printf("FAIL: enabled\n"); rc = 1; }
    if (a.video_record_codec != b.video_record_codec)     { printf("FAIL: codec\n"); rc = 1; }
    if (strcmp(a.video_output_tag, b.video_output_tag))   { printf("FAIL: tag\n"); rc = 1; }
    if (strcmp(a.ffmpeg_path, b.ffmpeg_path))             { printf("FAIL: ffmpeg_path\n"); rc = 1; }
    /* The load path re-runs the namer, so the tag must have reached the name. */
    if (b.video_filename[0] && !strstr(b.video_filename, "refcam")) {
        printf("FAIL: tag did not reach the generated filename\n"); rc = 1;
    }

    /* A hand-edited file must not be able to select a codec that does not
     * exist -- the loader clamps rather than trusting the number. */
    gui_settings_t c = b;
    c.video_record_codec = 99;
    gui_settings_save(&c);
    gui_settings_t d;
    memset(&d, 0, sizeof(d));
    gui_settings_load(&d);
    printf("  out-of-range codec 99 -> %d (must be 0 or 1)\n", d.video_record_codec);
    if (d.video_record_codec != 0 && d.video_record_codec != 1) {
        printf("FAIL: codec not clamped\n"); rc = 1;
    }

    printf("%s\n", rc ? "SETTINGS TEST FAILED" : "settings test passed");
    return rc;
}

/* Spawn the reference-video encoder. Called from both the FLAC and RAW start
 * paths at the same relative position: after the RF writer threads are latched
 * and before the producer is enabled.
 *
 * That placement is what keeps the unwind cost at one site -- every existing
 * failure path in gui_record_start_confirmed sits above it, so none can be
 * reached with the video already started. Moving this earlier would mean
 * adding video teardown to eight other unwind edits. */
static void gui_record_start_video_if_enabled(gui_app_t *app)
{
    if (!app->settings.video_record_enabled) return;

    preview_status_t pv = gui_preview_get_status();
    uint32_t pitch = gui_preview_negotiated_pitch();
    if (pitch == 0) pitch = pv.width * 2;

    snprintf(s_record_path_video, sizeof(s_record_path_video), "%s/%s",
             app->settings.output_path, app->settings.video_filename);

    /* Hold the stream for the whole recording, so closing the preview panel
     * stops the picture without stopping the file. */
    gui_preview_hold_acquire();

    char err[256] = {0};
    if (gui_video_record_start(s_record_path_video,
                               app->settings.video_record_codec == 1 ? VIDEO_CODEC_FFV1
                                                                     : VIDEO_CODEC_H264,
                               pv.width, pv.height, pitch,
                               pv.fps_num, pv.fps_den, err, sizeof(err)) != 0) {
        gui_preview_hold_release();
        /* Deliberately not fatal here. The RF writers are already running and
         * the producer is about to be enabled; losing the reference video is
         * not a reason to lose the capture. Preflight is where a missing
         * encoder refuses -- by this point everything it checked has passed. */
        char msg[320];
        snprintf(msg, sizeof(msg), "Reference video failed to start: %s", err);
        gui_record_log_capture_event(app, "ERROR", msg, GUI_ERROR_CLASS_SYSTEM, 1);
        gui_app_set_status(app, msg);
        s_record_path_video[0] = '\0';
        return;
    }

    s_active->video_started = true;
    /* Stashed rather than logged here. The FLAC path opens the session log
     * before this runs and the RAW path opens it after, so logging directly
     * would silently drop the line in the RAW branch. */
    gui_app_set_status(app, "Reference video recording started");
    snprintf(s_video_start_msg, sizeof(s_video_start_msg),
             "Reference video started: %s (%s, %ux%u @ %.2f fps)",
             s_record_path_video,
             app->settings.video_record_codec == 1 ? "FFV1" : "H.264",
             pv.width, pv.height,
             pv.fps_den ? (double)pv.fps_num / pv.fps_den : 0.0);
}

/* Emit whatever the video spawn stashed. Called from a point in each branch
 * where the session log is known to be open. */
static void gui_record_flush_video_start_log(gui_app_t *app)
{
    if (!s_video_start_msg[0]) return;
    /* Only consume the message once there is a file to write it to.
     * gui_record_log_capture_event silently drops the line when the session log
     * is not open yet, which is exactly the RAW path's situation at the spawn
     * site -- clearing it there lost the line entirely. */
    if (!s_active || !s_active->log_file) return;
    gui_record_log_capture_event(app, "INFO", s_video_start_msg, GUI_ERROR_CLASS_NONE, 0);
    s_video_start_msg[0] = '\0';
}

/* Headless record: captures from the simulated device straight to files, with
 * no window and no clicking. Exists for one comparison in particular -- the RF
 * outputs from a run with the reference video ON must be byte-identical to a
 * run with it OFF. That is the proof that the video path cannot perturb the
 * capture it sits beside. */
int gui_record_auto_record_main(const char *out_dir, int seconds, bool with_video,
                                bool use_flac)
{
    static gui_app_t app;
    memset(&app, 0, sizeof(app));
    gui_settings_init_defaults(&app.settings);

    snprintf(app.settings.output_path, sizeof(app.settings.output_path), "%s", out_dir);
    snprintf(app.settings.output_base_name, sizeof(app.settings.output_base_name), "auto");
    app.settings.auto_names_enabled = true;
    /* Deterministic filenames, so the two runs are directly comparable. */
    app.settings.append_timestamp_on_capture_start = false;
    app.settings.use_flac = use_flac;
    app.settings.capture_a = true;
    app.settings.capture_b = true;
    app.settings.video_record_enabled = with_video;
    app.settings.enable_audio_4ch = false;
    app.settings.enable_audio_2ch_12 = false;
    app.settings.enable_audio_2ch_34 = false;
    gui_settings_refresh_auto_names(&app.settings);

    gui_app_init(&app);
    /* main() does this for the real app; without it the preview singleton is
     * all zeros and reports UNSUPPORTED. */
    gui_preview_init(NULL);
    if (with_video) gui_preview_refresh_devices();
    gui_app_enumerate_devices(&app);

    int sim = -1;
    for (int i = 0; i < app.device_count; i++) {
        if (app.devices[i].type == DEVICE_TYPE_SIMULATED) { sim = i; break; }
    }
    if (sim < 0) { fprintf(stderr, "no simulated device\n"); return 2; }
    app.selected_device = sim;

    /* Returns 0 on success, not a bool. */
    int cap_rc = gui_app_start_capture(&app);
    if (cap_rc != 0) {
        fprintf(stderr, "start_capture failed (%d): %s\n", cap_rc, app.status_message);
        return 2;
    }
    struct timespec settle = { 1, 0 };
    nanosleep(&settle, NULL);

    int rs = gui_record_start(&app);
    if (rs != RECORD_OK) {
        fprintf(stderr, "record_start returned %d: %s\n", rs, app.status_message);
        gui_app_stop_capture(&app);
        return 2;
    }
    printf("recording %ds (video=%s, %s) -> %s\n", seconds, with_video ? "on" : "off",
           use_flac ? "flac" : "raw", out_dir);

    for (int i = 0; i < seconds; i++) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
    }

    gui_app_stop_recording(&app);
    /* Finalize runs on its own thread; wait it out rather than racing it. */
    for (int i = 0; i < 300 && gui_record_is_finalizing(); i++) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    gui_app_stop_capture(&app);

    if (with_video) {
        gui_video_record_status_t vs = gui_video_record_get_status();
        printf("video: submitted=%llu written=%llu dropped=%llu bytes=%llu%s%s\n",
               (unsigned long long)vs.frames_submitted,
               (unsigned long long)vs.frames_written,
               (unsigned long long)vs.frames_dropped,
               (unsigned long long)vs.output_bytes,
               vs.error ? " err=" : "", vs.error ? vs.err_text : "");
    }
    printf("rfA=%s rfB=%s\n", app.settings.output_filename_a, app.settings.output_filename_b);
    return 0;
}

int gui_record_name_test_main(void)
{
    static gui_app_t app;
    memset(&app, 0, sizeof(app));
    gui_settings_init_defaults(&app.settings);

    snprintf(app.settings.output_base_name, sizeof(app.settings.output_base_name), "tapetest");
    snprintf(app.settings.video_output_tag, sizeof(app.settings.video_output_tag), "ref cam");
    snprintf(app.settings.rf_channel_tags[0], sizeof(app.settings.rf_channel_tags[0]), "luma");
    snprintf(app.settings.audio_output_tags[0], sizeof(app.settings.audio_output_tags[0]), "quad");
    app.settings.auto_names_enabled = true;
    app.settings.use_flac = true;
    app.settings.video_record_enabled = true;

    int rc = 0;

    /* Pass 1: timestamping off, so the two must be character-identical. */
    app.settings.append_timestamp_on_capture_start = false;
    gui_settings_refresh_auto_names(&app.settings);
    char s_video[MAX_FILENAME_LEN], s_a[MAX_FILENAME_LEN], s_4ch[MAX_FILENAME_LEN];
    snprintf(s_video, sizeof(s_video), "%s", app.settings.video_filename);
    snprintf(s_a, sizeof(s_a), "%s", app.settings.output_filename_a);
    snprintf(s_4ch, sizeof(s_4ch), "%s", app.settings.audio_4ch_filename);

    gui_record_apply_auto_names(&app);
    printf("no timestamp:\n");
    printf("  settings namer video : %s\n", s_video);
    printf("  record   namer video : %s\n", app.settings.video_filename);
    printf("  settings namer rfA   : %s\n", s_a);
    printf("  record   namer rfA   : %s\n", app.settings.output_filename_a);
    printf("  settings namer 4ch   : %s\n", s_4ch);
    printf("  record   namer 4ch   : %s\n", app.settings.audio_4ch_filename);

    if (strcmp(s_video, app.settings.video_filename) != 0) {
        printf("FAIL: video names diverge with timestamping off\n"); rc = 1;
    }
    if (strcmp(s_a, app.settings.output_filename_a) != 0) {
        printf("FAIL: rfA names diverge with timestamping off\n"); rc = 1;
    }
    if (strcmp(s_4ch, app.settings.audio_4ch_filename) != 0) {
        printf("FAIL: 4ch names diverge with timestamping off\n"); rc = 1;
    }

    /* Pass 2: timestamping on -- the record namer must differ, and only by
     * inserting the timestamp after the base name. */
    app.settings.append_timestamp_on_capture_start = true;
    gui_record_apply_auto_names(&app);
    printf("with timestamp:\n  record namer video : %s\n", app.settings.video_filename);

    if (strcmp(s_video, app.settings.video_filename) == 0) {
        printf("FAIL: timestamping had no effect on the video name\n"); rc = 1;
    } else if (strncmp(app.settings.video_filename, "tapetest_", 9) != 0 ||
               strstr(app.settings.video_filename, "_ref-cam_video.mkv") == NULL) {
        printf("FAIL: timestamped video name is not base + timestamp + tag + suffix\n"); rc = 1;
    }

    /* The tag contained a space; sanitize_tag must map it to '-' rather than
     * leave whitespace in a filename. */
    if (strstr(s_video, "ref-cam") == NULL) {
        printf("FAIL: tag was not sanitised (%s)\n", s_video); rc = 1;
    }

    printf("%s\n", rc ? "NAME TEST FAILED" : "name test passed");
    return rc;
}

int gui_record_start(gui_app_t *app) {

    (void)gui_record_collect_finalize_if_done();

    if (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)) {
        gui_app_set_status(app, "Finalizing previous recording...");
        return RECORD_ERROR;
    }

    if (!app->is_capturing) {
        gui_app_set_status(app, "Start capture first");
        return RECORD_ERROR;
    }

    if (app->is_recording) {
        return RECORD_OK;
    }

    // If already pending confirmation, don't show another popup
    if (s_overwrite_pending) {
        return RECORD_PENDING;
    }

    // Apply auto naming (must happen before overwrite checks)
    gui_record_apply_auto_names(app);

    // Build full output paths (output_path + filenames)
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/%s", app->settings.output_path, app->settings.output_filename_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", app->settings.output_path, app->settings.output_filename_b);

    // Check if output files already exist
    struct stat stat_a, stat_b, stat_v;
    bool file_a_exists = app->settings.capture_a && (stat(path_a, &stat_a) == 0);
    bool file_b_exists = app->settings.capture_b && (stat(path_b, &stat_b) == 0);
    /* Without this the RF files prompt while the reference video is silently
     * clobbered. */
    char path_video[600];
    snprintf(path_video, sizeof(path_video), "%s/%s",
             app->settings.output_path, app->settings.video_filename);
    bool file_v_exists = app->settings.video_record_enabled && (stat(path_video, &stat_v) == 0);

    if (file_a_exists || file_b_exists || file_v_exists) {
        // Build detailed message with file info
        char message[512];
        char size_buf[32];
        int offset = 0;

        offset += snprintf(message + offset, sizeof(message) - offset,
            "The following files will be overwritten:\n\n");

        if (file_a_exists) {
            format_file_size_u64((uint64_t)stat_a.st_size, size_buf, sizeof(size_buf));
            offset += snprintf(message + offset, sizeof(message) - offset,
                "CH A: %s (%s)\n", path_a, size_buf);
        }

        if (file_b_exists) {
            format_file_size_u64((uint64_t)stat_b.st_size, size_buf, sizeof(size_buf));
            offset += snprintf(message + offset, sizeof(message) - offset,
                "CH B: %s (%s)\n", path_b, size_buf);
        }

        // Show confirmation popup with detailed info
        gui_popup_confirm("Overwrite Files?", message, "Overwrite", "Cancel", app);
        s_overwrite_pending = true;
        s_pending_app = app;
        return RECORD_PENDING;
    }

    // No files exist, start recording directly
    return gui_record_start_confirmed(app);
}

// Check popup result and continue recording if confirmed
void gui_record_check_popup(gui_app_t *app) {
    if (gui_record_collect_finalize_if_done()) {
        gui_app_set_status(app, "Recording stopped");
    }

    if (!s_overwrite_pending) {
        return;
    }

    popup_result_t result = gui_popup_get_result();

    if (result == POPUP_RESULT_NONE) {
        // Popup still open, wait
        return;
    }

    // Popup closed, clear pending state
    s_overwrite_pending = false;

    if (result == POPUP_RESULT_YES) {
        // User confirmed, start recording
        gui_record_start_confirmed(app);
    } else {
        // User cancelled
        gui_app_set_status(app, "Recording cancelled");
    }

    s_pending_app = NULL;
}

// Internal: Start recording after confirmation
static int gui_record_start_confirmed(gui_app_t *app) {
    gui_record_reset_disk_guard_state();

    // Build full output paths (output_path + filenames)
    char path_a[512];
    char path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/%s", app->settings.output_path, app->settings.output_filename_a);
    snprintf(path_b, sizeof(path_b), "%s/%s", app->settings.output_path, app->settings.output_filename_b);
    // Check if using simulated device (doesn't use extraction thread)
    bool is_simulated = false;
    if (app->device_count > 0 && app->selected_device < app->device_count) {
        is_simulated = (app->devices[app->selected_device].type == DEVICE_TYPE_SIMULATED);
    }

    // Verify extraction thread is running (or simulated capture)
    if (!gui_extract_is_running() && !is_simulated) {
        gui_app_set_status(app, "Extraction not running");
        return RECORD_ERROR;
    }

    // For simulated capture, ensure record buffers are initialized
    if (is_simulated) {
        gui_extract_init_record_rbs(app);
    }
    // Ensure record buffers are initialized in buffer manager for enabled channels
    if ((app->settings.capture_a && bufmgr_ensure_init(&app->buffers, BUF_RECORD_A) < 0) ||
        (app->settings.capture_b && bufmgr_ensure_init(&app->buffers, BUF_RECORD_B) < 0)) {
        gui_app_set_status(app, "Record buffers not initialized");
        return RECORD_ERROR;
    }

    // Recording uses a hard user-set mode latched at record start.
    bool prev_runtime_mode = app->capture_mode_runtime_misrc;
    app->capture_mode_runtime_misrc = app->user_capture_mode_misrc;
    TraceLog(LOG_INFO,
             "MODE TRACE: source=gui_record_start_confirmed latch_runtime old=%s new=%s user=%s",
             prev_runtime_mode ? "MISRC" : "HSDAOH",
             app->capture_mode_runtime_misrc ? "MISRC" : "HSDAOH",
             app->user_capture_mode_misrc ? "MISRC" : "HSDAOH");

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac && app->settings.flac_affinity_enabled) {
        if (!flac_writer_affinity_supported()) {
            gui_app_set_status(app, "FLAC affinity is only supported on Linux");
            return RECORD_ERROR;
        }
        char aff_err[256] = {0};
        if (!flac_writer_validate_affinity_cpu_list(app->settings.flac_affinity_cpu_list, aff_err, sizeof(aff_err))) {
            char status_msg[320];
            snprintf(status_msg, sizeof(status_msg), "Invalid FLAC affinity CPU list: %s",
                     aff_err[0] ? aff_err : "parse failure");
            gui_app_set_status(app, status_msg);
            return RECORD_ERROR;
        }
    }
#endif

    /* Reference video preflight. Deliberately here: before any output file is
     * opened and before the session is created, so a refusal leaves nothing to
     * unwind. */
    s_record_path_video[0] = '\0';
    s_video_start_msg[0] = '\0';
    if (app->settings.video_record_enabled) {
        gui_video_record_set_ffmpeg_path(app->settings.ffmpeg_path);
        if (!gui_video_record_probe()) {
            gui_app_set_status(app, "Reference video is on but ffmpeg was not found. "
                                    "Set ffmpeg_path in Settings, or turn Reference video off.");
            return RECORD_ERROR;
        }

        preview_status_t pv = gui_preview_get_status();
        if (pv.state == PREVIEW_STATE_UNSUPPORTED) {
            gui_app_set_status(app, "Reference video requires Linux/V4L2 and is not available "
                                    "in this build. Turn Reference video off to record.");
            return RECORD_ERROR;
        }
        if (pv.state == PREVIEW_STATE_DISCONNECTED || pv.state == PREVIEW_STATE_NO_DEVICE) {
            /* Connect on demand. The stream is normally owned by a panel, and
             * requiring one to be open would block an RF capture for a reason
             * that has nothing to do with the RF. */
            if (gui_preview_connect() != 0) {
                pv = gui_preview_get_status();
                char msg[320];
                snprintf(msg, sizeof(msg),
                         "Reference video is on but the preview device could not be opened: %s. "
                         "Turn Reference video off to record without it.",
                         pv.err_text[0] ? pv.err_text : "no device");
                gui_app_set_status(app, msg);
                return RECORD_ERROR;
            }
            pv = gui_preview_get_status();
        }

        if (pv.state == PREVIEW_STATE_POPPED_OUT) {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "Reference video is on but the preview is in a separate window (pid %d). "
                     "Bring it back, or turn Reference video off.", pv.child_pid);
            gui_app_set_status(app, msg);
            return RECORD_ERROR;
        }
        /* STALLED is accepted: it only means no frame for a few seconds, which
         * is a paused tape as often as a fault. */
        if (pv.state != PREVIEW_STATE_STREAMING && pv.state != PREVIEW_STATE_STALLED &&
            pv.state != PREVIEW_STATE_CONNECTING) {
            char msg[320];
            snprintf(msg, sizeof(msg),
                     "Reference video is on but the preview is not running: %s. "
                     "Turn Reference video off to record without it.",
                     pv.err_text[0] ? pv.err_text : "not connected");
            gui_app_set_status(app, msg);
            return RECORD_ERROR;
        }
    }

    gui_record_session_t *ses = calloc(1, sizeof(*ses));
    if (!ses) {
        gui_app_set_status(app, "Out of memory starting recording");
        return RECORD_ERROR;
    }
    ses->app = app;
    ses->capture_a = app->settings.capture_a;
    ses->capture_b = app->settings.capture_b;
    if (ses->capture_a) {
        snprintf(ses->path_a, sizeof(ses->path_a), "%s", path_a);
    }
    if (ses->capture_b) {
        snprintf(ses->path_b, sizeof(ses->path_b), "%s", path_b);
    }
    s_active = ses;
    atomic_store(&app->recording_bytes, 0);
    atomic_store(&app->recording_raw_a, 0);
    atomic_store(&app->recording_raw_b, 0);
    atomic_store(&app->recording_compressed_a, 0);
    atomic_store(&app->recording_compressed_b, 0);
    app->last_recording_duration_s = 0.0;
    gui_record_spill_reset_all();
    // Clear per-channel write-error flags for the new recording session.
    for (int i = 0; i < GUI_RECORD_SPILL_CHANNELS; i++) {
        atomic_store(&s_record_write_error[i], false);
    }

    // Reset record buffers before starting
    gui_extract_reset_record_rbs(app);

#if LIBFLAC_ENABLED == 1
    if (app->settings.use_flac) {
        ses->use_flac = true;
        // Open FLAC files (respect per-channel enable)
        ses->file_a = app->settings.capture_a ? fopen(path_a, "wb") : NULL;
        ses->file_b = app->settings.capture_b ? fopen(path_b, "wb") : NULL;

        if ((app->settings.capture_a && !ses->file_a) || (app->settings.capture_b && !ses->file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (ses->file_a) fclose(ses->file_a);
            if (ses->file_b) fclose(ses->file_b);
            ses->file_a = ses->file_b = NULL;
            free(ses);
            s_active = NULL;
            return RECORD_ERROR;
        }
        gui_record_open_session_log(app, path_a, path_b);
        // Boost process priority before creating FLAC encoders/worker threads.
        proc_set_priority(PROC_PRIORITY_ABOVE);

        // Determine per-channel RF bit depth
        uint8_t bits_a = clamp_rf_bits_flac(app->settings.rf_bits_a);
        uint8_t bits_b = clamp_rf_bits_flac(app->settings.rf_bits_b);

        // Setup writer contexts
        ses->ctx_a.bufmgr = &app->buffers;
        ses->ctx_a.buf_id = BUF_RECORD_A;
        ses->ctx_a.file = ses->file_a;
        ses->ctx_a.channel = 0;
        ses->ctx_a.compressed_bytes = &app->recording_compressed_a;
        ses->ctx_a.flac_bits_per_sample = bits_a;
        ses->ctx_a.rf_bits = bits_a;
        ses->ctx_a.raw_bytes_per_sample = 2;  // input blocks are int16
        ses->ctx_a.enable_resample = app->settings.enable_resample_a;
        ses->ctx_a.resample_rate_khz = app->settings.resample_rate_a;
        ses->ctx_a.resample_quality = app->settings.resample_quality_a;
        ses->ctx_a.resample_gain_db = app->settings.resample_gain_a;
#if LIBSOXR_ENABLED
        ses->ctx_a.soxr = NULL;
        ses->ctx_a.soxr_rate_khz = 0.0f;
#endif
        ses->ctx_a.app = app;
        ses->ctx_a.ses = ses;

        ses->ctx_b.bufmgr = &app->buffers;
        ses->ctx_b.buf_id = BUF_RECORD_B;
        ses->ctx_b.file = ses->file_b;
        ses->ctx_b.channel = 1;
        ses->ctx_b.compressed_bytes = &app->recording_compressed_b;
        ses->ctx_b.flac_bits_per_sample = bits_b;
        ses->ctx_b.rf_bits = bits_b;
        ses->ctx_b.raw_bytes_per_sample = 2;  // input blocks are int16
        ses->ctx_b.enable_resample = app->settings.enable_resample_b;
        ses->ctx_b.resample_rate_khz = app->settings.resample_rate_b;
        ses->ctx_b.resample_quality = app->settings.resample_quality_b;
        ses->ctx_b.resample_gain_db = app->settings.resample_gain_b;
#if LIBSOXR_ENABLED
        ses->ctx_b.soxr = NULL;
        ses->ctx_b.soxr_rate_khz = 0.0f;
#endif
        ses->ctx_b.app = app;
        ses->ctx_b.ses = ses;

        // Configure FLAC writers using shared library
        flac_writer_config_t config_a = flac_writer_default_config();
        flac_writer_config_t config_b = flac_writer_default_config();

        // Sample rate is stored in kHz for RF capture (40000 = 40 MSPS)
        config_a.sample_rate = (app->settings.enable_resample_a && app->settings.resample_rate_a > 0.0f)
                                 ? (uint32_t)(app->settings.resample_rate_a)
                                 : 40000;
        config_b.sample_rate = (app->settings.enable_resample_b && app->settings.resample_rate_b > 0.0f)
                                 ? (uint32_t)(app->settings.resample_rate_b)
                                 : 40000;
        ses->sample_rate_a = config_a.sample_rate;
        ses->sample_rate_b = config_b.sample_rate;

        // bits_per_sample is set per-channel below
        config_a.bits_per_sample = 16;
        config_b.bits_per_sample = 16;
        config_a.compression_level = app->settings.flac_level;
        config_b.compression_level = app->settings.flac_level;
        config_a.verify = app->settings.flac_verification;
        config_b.verify = app->settings.flac_verification;
        // Auto (flac_threads == 0) resolves to a real parallel count (4-core
        // base, up to 8) so level-8 encode keeps up with 40 MSPS without forcing
        // the record spill file. A user override (>0) is honored verbatim.
        uint32_t resolved_flac_threads = gui_record_resolve_flac_threads(app->settings.flac_threads);
        config_a.num_threads = resolved_flac_threads;
        config_b.num_threads = resolved_flac_threads;
        config_a.affinity_enabled = app->settings.flac_affinity_enabled;
        config_b.affinity_enabled = app->settings.flac_affinity_enabled;
        snprintf(config_a.affinity_cpu_list, sizeof(config_a.affinity_cpu_list), "%s", app->settings.flac_affinity_cpu_list);
        snprintf(config_b.affinity_cpu_list, sizeof(config_b.affinity_cpu_list), "%s", app->settings.flac_affinity_cpu_list);
        config_a.enable_seektable = true;
        config_b.enable_seektable = true;

        // Create writer for channel A
        config_a.error_cb = gui_flac_error_callback;
        config_a.bytes_cb = gui_flac_bytes_callback;

        if (app->settings.capture_a) {
            config_a.bits_per_sample = ses->ctx_a.flac_bits_per_sample;
            config_a.callback_user_data = &ses->ctx_a;
            ses->flac_a = flac_writer_create_stream(ses->file_a, &config_a);
            if (!ses->flac_a) {
                gui_app_set_status(app, "Failed to create FLAC encoder A");
                gui_record_log_capture_event(app, "ERROR", "Failed to create FLAC encoder A",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (ses->file_a) fclose(ses->file_a);
                if (ses->file_b) fclose(ses->file_b);
                ses->file_a = ses->file_b = NULL;
                gui_record_close_session_log();
                free(ses);
                s_active = NULL;
                return RECORD_ERROR;
            }
            ses->ctx_a.writer = ses->flac_a;
        } else {
            ses->flac_a = NULL;
            ses->ctx_a.writer = NULL;
        }

        // Create writer for channel B
        if (app->settings.capture_b) {
            config_b.error_cb = gui_flac_error_callback;
            config_b.bytes_cb = gui_flac_bytes_callback;
            config_b.bits_per_sample = ses->ctx_b.flac_bits_per_sample;
            config_b.callback_user_data = &ses->ctx_b;
            ses->flac_b = flac_writer_create_stream(ses->file_b, &config_b);
            if (!ses->flac_b) {
                gui_app_set_status(app, "Failed to create FLAC encoder B");
                gui_record_log_capture_event(app, "ERROR", "Failed to create FLAC encoder B",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (ses->flac_a) { flac_writer_abort(ses->flac_a); ses->flac_a = NULL; }
                if (ses->file_a) fclose(ses->file_a);
                if (ses->file_b) fclose(ses->file_b);
                ses->file_a = ses->file_b = NULL;
                gui_record_close_session_log();
                free(ses);
                s_active = NULL;
                return RECORD_ERROR;
            }
            ses->ctx_b.writer = ses->flac_b;
        } else {
            ses->flac_b = NULL;
            ses->ctx_b.writer = NULL;
        }
        bool started_a = false;
        bool started_b = false;

        // Capture record-buffer backpressure stats at recording start
        ses->start_rec_a_waits = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
        ses->start_rec_a_drops = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
        ses->start_rec_b_waits = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
        ses->start_rec_b_drops = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);

        // Mark as recording
        app->is_recording = true;
        atomic_store(&ses->recording, true);
        app->recording_start_time = GetTime();
        ses->recording_start_time = app->recording_start_time;

        // Start writer threads BEFORE enabling recording in extraction thread
        // This ensures consumers are ready before producer starts filling buffers
        if (app->settings.capture_a) {
            if (thrd_create_with_priority(&ses->writer_thread_a,
                                          flac_writer_thread,
                                          &ses->ctx_a,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start FLAC writer A");
                gui_record_log_capture_event(app, "ERROR", "Failed to start FLAC writer A",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                app->is_recording = false;
                /* Removes the tap and asks the writer to drain. Does not join --
                 * this is the render thread; the join happens in finalize. */
                if (ses->video_started) gui_video_record_request_stop();

                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (ses->flac_a) { flac_writer_abort(ses->flac_a); ses->flac_a = NULL; }
                if (ses->flac_b) { flac_writer_abort(ses->flac_b); ses->flac_b = NULL; }
                if (ses->file_a) fclose(ses->file_a);
                if (ses->file_b) fclose(ses->file_b);
                ses->file_a = ses->file_b = NULL;
                gui_record_close_session_log();
                free(ses);
                s_active = NULL;
                return RECORD_ERROR;
            }
            started_a = true;
        }
        if (app->settings.capture_b) {
            if (thrd_create_with_priority(&ses->writer_thread_b,
                                          flac_writer_thread,
                                          &ses->ctx_b,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start FLAC writer B");
                gui_record_log_capture_event(app, "ERROR", "Failed to start FLAC writer B",
                                             GUI_ERROR_CLASS_SYSTEM, 1);
                app->is_recording = false;
                if (started_a) thrd_join(ses->writer_thread_a, NULL);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (ses->flac_a) { flac_writer_abort(ses->flac_a); ses->flac_a = NULL; }
                if (ses->flac_b) { flac_writer_abort(ses->flac_b); ses->flac_b = NULL; }
                if (ses->file_a) fclose(ses->file_a);
                if (ses->file_b) fclose(ses->file_b);
                ses->file_a = ses->file_b = NULL;
                gui_record_close_session_log();
                free(ses);
                s_active = NULL;
                return RECORD_ERROR;
            }
            started_b = true;
        }
        ses->writer_threads_running = started_a || started_b;

        gui_record_start_video_if_enabled(app);
        gui_record_flush_video_start_log(app);
#if defined(__APPLE__)
        /* Recording startup may create late helper threads in encoder/runtime
         * paths; promote them immediately so capture load stays P-core-biased. */
        macos_promote_all_task_threads();
#endif

        // Small delay to let writer threads initialize and start waiting on buffers
        thrd_sleep_ms(10);

        // Now enable recording in extraction thread - data will start flowing
        gui_extract_set_recording(true, true, bits_a, bits_b);

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, &app->buffers);

        gui_app_set_status(app, "Recording (FLAC)...");
    } else
#endif
    {
        // RAW recording (respect per-channel enable)
        ses->file_a = app->settings.capture_a ? fopen(path_a, "wb") : NULL;
        ses->file_b = app->settings.capture_b ? fopen(path_b, "wb") : NULL;

        if ((app->settings.capture_a && !ses->file_a) || (app->settings.capture_b && !ses->file_b)) {
            gui_app_set_status(app, "Failed to open output files");
            if (ses->file_a) fclose(ses->file_a);
            if (ses->file_b) fclose(ses->file_b);
            ses->file_a = ses->file_b = NULL;
            free(ses);
            s_active = NULL;
            return RECORD_ERROR;
        }

        uint8_t bits_a = rf_bits_for_raw(app->settings.rf_bits_a);
        uint8_t bits_b = rf_bits_for_raw(app->settings.rf_bits_b);

        ses->ctx_a.bufmgr = &app->buffers;
        ses->ctx_a.buf_id = BUF_RECORD_A;
        ses->ctx_a.file = ses->file_a;
        ses->ctx_a.channel = 0;
        ses->ctx_a.app = app;
        ses->ctx_a.ses = ses;
        ses->ctx_a.rf_bits = bits_a;
        ses->ctx_a.raw_bytes_per_sample = (bits_a == 8) ? 1 : 2;
        ses->ctx_a.enable_resample = app->settings.enable_resample_a;
        ses->ctx_a.resample_rate_khz = app->settings.resample_rate_a;
        ses->ctx_a.resample_quality = app->settings.resample_quality_a;
        ses->ctx_a.resample_gain_db = app->settings.resample_gain_a;
#if LIBSOXR_ENABLED
        ses->ctx_a.soxr = NULL;
        ses->ctx_a.soxr_rate_khz = 0.0f;
#endif

        ses->ctx_b.bufmgr = &app->buffers;
        ses->ctx_b.buf_id = BUF_RECORD_B;
        ses->ctx_b.file = ses->file_b;
        ses->ctx_b.channel = 1;
        ses->ctx_b.app = app;
        ses->ctx_b.ses = ses;
        ses->ctx_b.rf_bits = bits_b;
        ses->ctx_b.raw_bytes_per_sample = (bits_b == 8) ? 1 : 2;
        ses->ctx_b.enable_resample = app->settings.enable_resample_b;
        ses->ctx_b.resample_rate_khz = app->settings.resample_rate_b;
        ses->ctx_b.resample_quality = app->settings.resample_quality_b;
        ses->ctx_b.resample_gain_db = app->settings.resample_gain_b;
#if LIBSOXR_ENABLED
        ses->ctx_b.soxr = NULL;
        ses->ctx_b.soxr_rate_khz = 0.0f;
#endif
        bool started_a = false;
        bool started_b = false;

        // Capture record-buffer backpressure stats at recording start
        ses->start_rec_a_waits = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
        ses->start_rec_a_drops = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
        ses->start_rec_b_waits = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
        ses->start_rec_b_drops = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);

        // Boost process priority during recording
        proc_set_priority(PROC_PRIORITY_ABOVE);

        // Mark as recording
        app->is_recording = true;
        atomic_store(&ses->recording, true);
        app->recording_start_time = GetTime();
        ses->recording_start_time = app->recording_start_time;

        // Start writer threads BEFORE enabling recording in extraction thread
        // This ensures consumers are ready before producer starts filling buffers
        if (app->settings.capture_a) {
            if (thrd_create_with_priority(&ses->writer_thread_a,
                                          raw_writer_thread,
                                          &ses->ctx_a,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start RAW writer A");
                app->is_recording = false;
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (ses->file_a) fclose(ses->file_a);
                if (ses->file_b) fclose(ses->file_b);
                ses->file_a = ses->file_b = NULL;
                free(ses);
                s_active = NULL;
                return RECORD_ERROR;
            }
            started_a = true;
        }
        if (app->settings.capture_b) {
            if (thrd_create_with_priority(&ses->writer_thread_b,
                                          raw_writer_thread,
                                          &ses->ctx_b,
                                          THRD_PRIORITY_CRITICAL) != thrd_success) {
                gui_app_set_status(app, "Failed to start RAW writer B");
                app->is_recording = false;
                if (started_a) thrd_join(ses->writer_thread_a, NULL);
                proc_set_priority(PROC_PRIORITY_NORMAL);
                if (ses->file_a) fclose(ses->file_a);
                if (ses->file_b) fclose(ses->file_b);
                ses->file_a = ses->file_b = NULL;
                free(ses);
                s_active = NULL;
                return RECORD_ERROR;
            }
            started_b = true;
        }
        ses->writer_threads_running = started_a || started_b;

        gui_record_start_video_if_enabled(app);
        gui_record_flush_video_start_log(app);
#if defined(__APPLE__)
        /* Recording startup may create late helper threads in encoder/runtime
         * paths; promote them immediately so capture load stays P-core-biased. */
        macos_promote_all_task_threads();
#endif

        // Small delay to let writer threads initialize and start waiting on buffers
        thrd_sleep_ms(10);

        // Now enable recording in extraction thread - data will start flowing
        gui_extract_set_recording(true, false, bits_a, bits_b);

        // Start audio output/monitoring (if enabled)
        gui_audio_start(app, &app->buffers);
        gui_record_open_session_log(app, path_a, path_b);
        gui_record_flush_video_start_log(app);   /* RAW: log opens last */

        gui_app_set_status(app, "Recording (RAW)...");
    }

    return RECORD_OK;
}

// Stop recording - heavy finalization runs in background thread to keep UI responsive
static void gui_record_finalize_stop_sync(gui_record_session_t *ses) {
    gui_app_t *app = ses->app;
    double stop_request_time = ses->stop_request_time;
    // Wait for writer threads to drain and exit. Keyed off the session's
    // latched channel flags, never off live settings.
    if (ses->writer_threads_running) {
        if (ses->capture_a) thrd_join(ses->writer_thread_a, NULL);
        if (ses->capture_b) thrd_join(ses->writer_thread_b, NULL);
    }
    /* After the RF joins: the master reaches a consistent state first, and the
     * video finish can block for seconds flushing an FFV1 GOP. Keyed off the
     * latched flag, never off live settings. */
    if (ses->video_started) {
        gui_video_record_finish();
        gui_video_record_status_t vs = gui_video_record_get_status();
        gui_preview_hold_release();
        ses->video_started = false;

        char msg[420];
        snprintf(msg, sizeof(msg),
                 "Reference video: frames=%llu written=%llu dropped=%llu duped=%llu bytes=%llu",
                 (unsigned long long)vs.frames_submitted,
                 (unsigned long long)vs.frames_written,
                 (unsigned long long)vs.frames_dropped,
                 (unsigned long long)vs.frames_duped,
                 (unsigned long long)vs.output_bytes);
        gui_record_log_capture_event(app, vs.error ? "ERROR" : "INFO", msg,
                                     vs.error ? GUI_ERROR_CLASS_SYSTEM : GUI_ERROR_CLASS_NONE,
                                     vs.error ? 1 : 0);
        if (vs.error && vs.err_text[0]) {
            char emsg[256];
            snprintf(emsg, sizeof(emsg), "Reference video error: %s", vs.err_text);
            gui_record_log_capture_event(app, "ERROR", emsg, GUI_ERROR_CLASS_SYSTEM, 1);
        }
    }
    {
        ses->writer_threads_running = false;
    }
    gui_record_spill_reset_all();

    // Alert the user if a persistent output-file write error was active
    // during recording (e.g. file locked by another app for viewing). The
    // UI flashes the finalize icon red while this flag is set.
    if (gui_record_has_write_error()) {
        gui_record_log_writef("ERROR",
            "Output file write error during recording: one or more channels could not write to the output file (it may have been locked by another application). Some recorded data may be incomplete.");
        if (app) {
            gui_app_set_status(app, "Recording write error: output file was locked by another app (data may be incomplete)");
        }
    }

#if LIBFLAC_ENABLED == 1
    uint64_t flac_samples_a = 0;
    uint64_t flac_samples_b = 0;
    if (ses->flac_a) {
        flac_samples_a = flac_writer_get_samples_written(ses->flac_a);
    }
    if (ses->flac_b) {
        flac_samples_b = flac_writer_get_samples_written(ses->flac_b);
    }
    // Finalize FLAC writers (this also cleans them up)
    if (ses->flac_a) {
        flac_writer_finish(ses->flac_a);
        ses->flac_a = NULL;
    }
    if (ses->flac_b) {
        flac_writer_finish(ses->flac_b);
        ses->flac_b = NULL;
    }
#endif

    // Close files
    if (ses->file_a) {
        fclose(ses->file_a);
        ses->file_a = NULL;
    }
    if (ses->file_b) {
        fclose(ses->file_b);
        ses->file_b = NULL;
    }

#if LIBFLAC_ENABLED == 1
    // Embed finalized duration metadata in RF FLAC files for easier post handling.
    if (ses->use_flac) {
        if (ses->capture_a && ses->path_a[0]) {
            gui_record_finalize_flac_streaminfo(app, ses->path_a, "CH A",
                                                flac_samples_a);
            gui_record_embed_flac_duration_metadata(app, ses->path_a, "CH A",
                                                    flac_samples_a, ses->sample_rate_a);
        }
        if (ses->capture_b && ses->path_b[0]) {
            gui_record_finalize_flac_streaminfo(app, ses->path_b, "CH B",
                                                flac_samples_b);
            gui_record_embed_flac_duration_metadata(app, ses->path_b, "CH B",
                                                    flac_samples_b, ses->sample_rate_b);
        }
    }
#endif

    // Print recording summary with backpressure stats
    double stop_complete_time = GetTime();
    double total_duration = stop_complete_time - ses->recording_start_time;
    double capture_duration = stop_request_time - ses->recording_start_time;
    double processing_duration = stop_complete_time - stop_request_time;
    if (capture_duration < 0.0) capture_duration = 0.0;
    if (processing_duration < 0.0) processing_duration = 0.0;
    if (total_duration < 0.0) total_duration = 0.0;
    app->last_recording_duration_s = capture_duration;
    uint64_t raw_a = atomic_load(&app->recording_raw_a);
    uint64_t raw_b = atomic_load(&app->recording_raw_b);
    uint64_t comp_a = atomic_load(&app->recording_compressed_a);
    uint64_t comp_b = atomic_load(&app->recording_compressed_b);
    uint64_t raw_total = raw_a + raw_b;
    uint64_t comp_total = comp_a + comp_b;
    double ratio_total = (comp_total > 0) ? ((double)raw_total / (double)comp_total) : 0.0;
    uint32_t end_rec_a_waits = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_waits);
    uint32_t end_rec_a_drops = atomic_load(&app->buffers.stats[BUF_RECORD_A].write_drops);
    uint32_t end_rec_b_waits = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_waits);
    uint32_t end_rec_b_drops = atomic_load(&app->buffers.stats[BUF_RECORD_B].write_drops);
    uint32_t rec_a_waits = end_rec_a_waits - ses->start_rec_a_waits;
    uint32_t rec_a_drops = end_rec_a_drops - ses->start_rec_a_drops;
    uint32_t rec_b_waits = end_rec_b_waits - ses->start_rec_b_waits;
    uint32_t rec_b_drops = end_rec_b_drops - ses->start_rec_b_drops;
    uint32_t rec_waits = rec_a_waits + rec_b_waits;
    uint32_t rec_drops = rec_a_drops + rec_b_drops;

    char size_a[64], size_b[64], size_comp_a[64], size_comp_b[64], size_raw_total[64], size_comp_total[64];
    char total_hms[32], capture_hms[32], processing_hms[32];
    format_log_data_size_u64(raw_a, size_a, sizeof(size_a));
    format_log_data_size_u64(raw_b, size_b, sizeof(size_b));
    format_log_data_size_u64(comp_a, size_comp_a, sizeof(size_comp_a));
    format_log_data_size_u64(comp_b, size_comp_b, sizeof(size_comp_b));
    format_log_data_size_u64(raw_total, size_raw_total, sizeof(size_raw_total));
    format_log_data_size_u64(comp_total, size_comp_total, sizeof(size_comp_total));
    format_duration_hhmmss(total_duration, total_hms, sizeof(total_hms));
    format_duration_hhmmss(capture_duration, capture_hms, sizeof(capture_hms));
    format_duration_hhmmss(processing_duration, processing_hms, sizeof(processing_hms));

    fprintf(stderr, "[REC] Recording stopped: total=%.1fs (%s), capture=%.1fs (%s), processing=%.1fs (%s), A=%s, B=%s, waits=%u, drops=%u\n",
            total_duration, total_hms, capture_duration, capture_hms, processing_duration, processing_hms,
            size_a, size_b, rec_waits, rec_drops);
    fprintf(stderr, "[REC] Record buffers: A waits=%u drops=%u, B waits=%u drops=%u\n",
            rec_a_waits, rec_a_drops, rec_b_waits, rec_b_drops);

    if (rec_drops > 0) {
        fprintf(stderr, "[REC] WARNING: %u frames were dropped during recording due to backpressure!\n", rec_drops);
    }

    gui_record_log_writef("INFO", "Recording stopped: duration=%.2fs (%s) rawA=%s rawB=%s waits=%u drops=%u",
                          total_duration, total_hms, size_a, size_b, rec_waits, rec_drops);
    gui_record_log_writef("INFO", "Capture time: %.2fs (%s)", capture_duration, capture_hms);
    gui_record_log_writef("INFO", "Processing time: %.2fs (%s)", processing_duration, processing_hms);
    gui_record_log_writef("INFO", "Output data: compressedA=%s compressedB=%s",
                          size_comp_a, size_comp_b);
    if (comp_total > 0) {
        gui_record_log_writef("INFO", "Compression ratio: total_raw=%s total_compressed=%s ratio=%.3fx",
                              size_raw_total, size_comp_total, ratio_total);
    } else {
        gui_record_log_writef("INFO", "Compression ratio: N/A (non-compressed recording mode)");
    }
    if (rec_drops > 0) {
        gui_record_log_writef("WARN", "Backpressure drops detected: %u frame blocks dropped", rec_drops);
    }
    {
        char end_iso[32];
        gui_record_build_iso8601_timestamp(end_iso, sizeof(end_iso));
        gui_record_log_writef("INFO", "datetime_end: %s", end_iso[0] ? end_iso : "unknown");
    }
    gui_record_log_writef_ses(ses, "INFO", "Session complete");
    gui_record_close_session_log_ses(ses);
}

static int gui_record_finalize_thread(void *arg) {
    gui_record_session_t *ses = (gui_record_session_t *)arg;
    gui_record_finalize_stop_sync(ses);
    atomic_store(&s_record_stop_finalize_done, true);
    atomic_store(&s_record_stop_finalizing, false);
    return 0;
}

void gui_record_stop(gui_app_t *app) {
    if (!app->is_recording) {
        return;
    }
    if (atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running)) {
        gui_app_set_status(app, "Finalizing previous recording...");
        return;
    }
    gui_record_session_t *ses = s_active;
    if (!ses) {
        app->is_recording = false;
        return;
    }
    gui_record_reset_disk_guard_state();
    ses->stop_request_time = GetTime();

    // Disable recording in extraction thread first.
    gui_extract_set_recording(false, false, 16, 16);

    // Signal threads to stop FIRST so the audio restart cannot reopen WAVs in
    // record mode. The session flag switches this session's writer threads to
    // drain mode independently of any new recording's state.
    app->is_recording = false;
    atomic_store(&ses->recording, false);

    // Stop audio output/monitoring and restart monitor-only path if still capturing.
    gui_audio_stop(app);
    if (app->is_capturing) {
        (void)gui_audio_start(app, &app->buffers);
    }

    // Restore normal process priority
    proc_set_priority(PROC_PRIORITY_NORMAL);

    // Hand the session to the finalize thread; the module forgets it as the
    // active recording so a new session can be created.
    s_active = NULL;
    s_finalizing = ses;

    atomic_store(&s_record_stop_finalize_done, false);
    atomic_store(&s_record_stop_finalizing, true);

    if (thrd_create(&s_finalize_thread, gui_record_finalize_thread, ses) != thrd_success) {
        atomic_store(&s_record_stop_finalizing, false);
        gui_record_finalize_stop_sync(ses);
        free(ses);
        s_finalizing = NULL;
        gui_app_set_status(app, "Recording stopped");
        return;
    }

    atomic_store(&s_finalize_thread_running, true);
    gui_app_set_status(app, "Finalizing recording...");
}

bool gui_record_is_finalizing(void) {
    return atomic_load(&s_record_stop_finalizing) || atomic_load(&s_finalize_thread_running);
}
