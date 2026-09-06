/*
 * MISRC GUI - Settings: the persisted struct and its descriptor table.
 *
 * Lives apart from gui_app.h so the settings code can be compiled without
 * raylib (the settings round-trip guard does exactly that).
 */
#ifndef GUI_SETTINGS_H
#define GUI_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef MAX_FILENAME_LEN
#define MAX_FILENAME_LEN 256
#endif

// GUI settings (bound to UI controls) - mirrors all CLI options
typedef struct {
    // Basic settings
    int device_index;
    char output_filename_a[MAX_FILENAME_LEN];
    char output_filename_b[MAX_FILENAME_LEN];
    char output_path[MAX_FILENAME_LEN];        // Default save path (Desktop)
    bool capture_a;
    bool capture_b;

    // Auto naming
    bool auto_names_enabled;                   // If true, derive filenames from output_base_name + parameters
    char output_base_name[MAX_FILENAME_LEN];   // Base name for outputs (no extension)

    // Timestamp behavior
    // If true, append a system-time timestamp captured at *record start* to the base name when generating output filenames.
    // This does not mutate output_base_name; it only affects derived filenames.
    bool append_timestamp_on_capture_start;

    // Capture duration limit (0 = no limit)
    uint32_t capture_limit_seconds;

    // Recording duration limit (0 = no limit)
    uint32_t record_limit_seconds;

    // RF bit depth selection (per-channel)
    // FLAC supports 8/12/16; RAW supports 8/16 (12 is disabled in RAW UI).
    uint8_t rf_bits_a;
    uint8_t rf_bits_b;
    // CXADC capture mode per card: false=8-bit @ base 40 MSPS,
    // true=driver tenbit (16-bit samples @ base 20 MSPS).
    // [0]=card A (cxadc0), [1]=card B (cxadc1).
    bool cxadc_tenbit_mode_card[2];
    // Optional per-channel RF tags used in auto naming (e.g. "luma", "chroma")
    char rf_channel_tags[2][32];

    // Capture control
    uint64_t sample_count;                     // Number of samples (0 = infinite)
    char capture_time[32];                     // Time string (e.g., "5:30" or "1:30:00")
    bool overwrite_files;                      // Overwrite without asking

    // Output files (used when auto_names_enabled=false, or as fallbacks)
    char aux_filename[MAX_FILENAME_LEN];
    char raw_filename[MAX_FILENAME_LEN];
    char audio_4ch_filename[MAX_FILENAME_LEN];
    char audio_2ch_12_filename[MAX_FILENAME_LEN];
    char audio_2ch_34_filename[MAX_FILENAME_LEN];
    char audio_1ch_filenames[4][MAX_FILENAME_LEN]; // Individual channel files

    // Processing options
    bool pad_lower_bits;                       // Pad lower 4 bits instead of upper
    bool show_peak_levels;                     // Display peak levels
    bool suppress_clip_a;                      // Suppress clipping messages A
    bool suppress_clip_b;                      // Suppress clipping messages B

    // Legacy flags (kept for backward-compatible settings load)
    bool reduce_8bit_a;                        // Reduce A to 8-bit
    bool reduce_8bit_b;                        // Reduce B to 8-bit

    // Resampling (if SOXR enabled)
    bool enable_resample_a;
    bool enable_resample_b;
    float resample_rate_a;                     // kHz
    float resample_rate_b;                     // kHz
    int resample_quality_a;                    // 0-4
    int resample_quality_b;                    // 0-4
    float resample_gain_a;                     // dB
    float resample_gain_b;                     // dB
#ifdef ENABLE_DDD
    uint8_t ddd_decimation;                    // Firmware 3.1 only: 1=40, 2=20 MSPS
#endif

    // FLAC compression
    bool use_flac;
    bool flac_12bit;                           // Legacy (kept for backward-compatible load)
    int flac_level;                            // 0-8
    bool flac_verification;                    // Verify encoder output
    int flac_threads;                          // Number of threads
    bool flac_affinity_enabled;                // Linux-only: pin FLAC work to selected CPU cores
    char flac_affinity_cpu_list[64];           // Linux-only CPU list/range string, e.g. "10-17,20"

    // Audio output options
    bool enable_audio_4ch;
    bool enable_audio_2ch_12;
    bool enable_audio_2ch_34;
    bool enable_audio_1ch[4];                  // Individual channel enables

    // Audio monitoring
    bool audio_monitor_playback;               // If true, play monitored audio to system output
    bool audio_monitor_ch34;                   // If true, monitor CH3/4; if false, monitor CH1/2
    bool misrc_mode;                           // If true, MISRC mode (default) with A/B channel swap
    bool misrc_v15_v25_ab_swap;               // If true, invert MISRC A/B mapping for V1.5/V2.5 hardware swap variants
    bool stop_on_dropout;                      // If true, automatically stop capture when stream dropout is detected

    // Level autostop: stop capture/recording when signal level stays below a
    // configurable percentage for a configurable duration (tape-end detection).
    // Independent from the digital dropout (frame error/missed frame) logic above.
    bool level_autostop_enabled;               // Enable/disable the level-based autostop
    char level_autostop_level_str[16];         // Signal level threshold as a percent string (e.g. "33")
    char level_autostop_duration_str[16];      // Sustain duration as a seconds string (e.g. "5.0")

    // Per-channel audio labels (for auto naming, e.g. "linear", "baseband")
    char audio_1ch_labels[4][32];
    // Optional tags for non-mono audio outputs: [0]=4ch, [1]=stereo ch1/2, [2]=stereo ch3/4
    char audio_output_tags[3][32];
    // Ingest metadata (saved to settings and written to capture log at record start)
    char ingest_project[128];
    char ingest_tape_id[128];
    char ingest_tape_format[128];
    char ingest_tape_size[128];
    char ingest_tape_speed[128];
    char ingest_tape_condition[128];
    char ingest_operator[128];
    char ingest_location[128];
    char ingest_notes[256];

    // Display settings (existing)
    bool show_grid;
    float time_scale;         // Time per division (ms)
    float amplitude_scale;    // Amplitude scale factor
    int ui_scale_percent;     // Ctrl/Cmd+wheel or +/- UI zoom, persisted as 75-200

    // Device discovery: V4L2/simple_capture device enumeration is opt-in.
    // Disabled by default since most users use hsdaoh/CXADC/DdD/FX3 backends;
    // enabling it lists OS video capture devices (e.g. MS2130 HDMI capture)
    // in the device dropdown.
    bool discover_simple_capture;
    // Advanced settings visibility: show/hide core-pinning controls in Settings.
    bool show_core_pinning_in_settings;

    // Max total capture/playback buffer RAM budget in GB (1-16). Applied at
    // buffer-manager init and re-applied on change when idle. Default 4 GB
    // mirrors the older code's ~4 GB target. Record A/B get the remainder
    // after fixed RF/Audio/Display allocations; record buffers stay lazy.
    uint32_t memory_budget_gb;

    // Update check metadata (GitHub releases). last_check_unix_s is persisted
    // so the automatic checker runs at most once every 7 days.
    uint64_t update_last_check_unix_s;
    char update_last_release_tag[64];
    bool update_available_cached;

    // RTL-SDR settings (only relevant when an RTL-SDR device is selected)
    uint64_t rtlsdr_freq_hz;              // Center frequency (Hz), default 100.0 MHz
    int     rtlsdr_gain_mode;             // 0 = auto, 1 = manual
    int     rtlsdr_gain_tenths_db;        // Manual gain in tenths of dB (0 = auto when auto mode)
    uint32_t rtlsdr_sample_rate_hz;       // Sample rate (Hz), default 2400000 (2.4 MSPS)
    bool    rtlsdr_agc;                   // RTL2832 AGC on/off
    bool    rtlsdr_offset_corr;           // RTL offset tuning correction on/off

    // Demod view settings (device-agnostic; apply to the Demod panel)
    int     demod_mode;                   // 0=WFM, 1=NFM, 2=AM, 3=USB, 4=LSB
    int     demod_bandwidth_hz;           // Approx target bandwidth (Hz), 0 = mode default
    float   demod_squelch;                // 0.0-1.0 squelch level (0 = open)
    float   demod_volume;                 // 0.0-2.0 output volume (1.0 = unity)
    int     demod_output_pair;            // 0 = CH1/2, 1 = CH3/4 (audio monitor output pair)

    // Playback settings
    char playback_file_a[MAX_FILENAME_LEN];   // FLAC file for channel A playback
    char playback_file_b[MAX_FILENAME_LEN];   // FLAC file for channel B playback

    // Server/Client networking (cxadc_vhs_server-style peer mode).
    // net_mode: 0=Local (default, no networking), 1=Server (host/master),
    // 2=Client (connect to a host server, slave).
    int  net_mode;
    uint16_t net_server_port;                 // Port to listen on when Server.
    char net_server_port_str[16];             // Editable string mirror of net_server_port.
    char net_client_host[128];                // Host to connect to when Client.
    char net_client_port_str[16];             // Editable string mirror of net_client_port.
    uint16_t net_client_port;                 // Port to connect to when Client.
} gui_settings_t;

/* ----------------------------------------------------------------------------
 * Persistence (gui_settings.c: platform paths and file I/O)
 * -------------------------------------------------------------------------- */

// Settings persistence
void gui_settings_load(gui_settings_t *settings);
void gui_settings_save(const gui_settings_t *settings);
void gui_settings_init_defaults(gui_settings_t *settings);

/* Regenerate every auto-derived output filename from the current base name and
 * tags. Deliberately does NOT append the record-start timestamp -- that is the
 * one intended difference from gui_record_apply_auto_names(), which does. The
 * two are separate implementations that must otherwise stay in lockstep;
 * --video-name-test checks they have not drifted. */
void gui_settings_refresh_auto_names(gui_settings_t *settings);
// Override the settings file path (e.g. from --config <path>). When set,
// gui_settings_load/save use this path instead of the platform default.
void gui_settings_set_override_path(const char *path);
// True when a --config override path is active (so startup logic can
// respect the config instead of forcing defaults like Local mode).
bool gui_settings_override_active(void);
const char* gui_settings_get_desktop_path(void);
// True when path names an existing directory.
bool gui_settings_path_is_dir(const char *path);

// Best-effort folder picker for output_path. Returns true if changed.
bool gui_settings_choose_output_folder(gui_settings_t *settings);

// Best-effort file picker for playback_file_{a,b}. channel: 0=A, 1=B.
// Returns true if changed.
bool gui_settings_choose_playback_file(gui_settings_t *settings, int channel);

/* ----------------------------------------------------------------------------
 * Descriptor table (gui_settings_table.c)
 *
 * One entry per persisted key. The file writer, the file reader, the JSON
 * snapshot and the key/value setter all walk this table, so a setting cannot
 * exist in one of them without existing in the others.
 * -------------------------------------------------------------------------- */

/* A settings file larger than this is ignored on load (defaults are used),
 * and the writer never produces one: the whole table formats into a buffer
 * of this size. */
#define GUI_SETTINGS_MAX_FILE_BYTES 32768

typedef enum {
    GS_BOOL = 0,
    GS_INT,
    GS_U8,
    GS_U16,
    GS_U32,
    GS_U64,
    GS_FLOAT,
    GS_STR,
} gui_setting_type_t;

enum {
    /* Describes the machine running this GUI (display, monitor, its own
     * network role), never the capture: not published to a net client and
     * never sent to a server. */
    GS_CLIENT_LOCAL    = 1u << 0,
    /* Written to the file for compatibility but never read back: the value
     * in the file has no effect (this is how these keys behaved before the
     * table existed). Never published. */
    GS_WRITE_ONLY      = 1u << 1,
    /* Legacy alias: parsed from older files, never written or published. */
    GS_LOAD_ONLY       = 1u << 2,
    /* Published but not settable over the network (device_index: use the
     * /device command, which validates against the live device list). */
    GS_NO_REMOTE_SET   = 1u << 3,
    /* Changing it changes the auto-derived output names, so the caller
     * re-runs gui_settings_refresh_auto_names() afterwards. */
    GS_AUTO_NAME_INPUT = 1u << 4,
};

typedef struct gui_setting_desc gui_setting_desc_t;

/* Parse + validate + store `value` (the bare text: no quotes) into the field
 * this entry describes. strict is the network path: refuse instead of
 * clamping/truncating where the file loader would silently accept. On
 * failure write a short reason to err and return false. */
typedef bool (*gui_setting_parse_fn)(const gui_setting_desc_t *d, gui_settings_t *s,
                                     const char *value, bool strict,
                                     char *err, size_t errcap);

struct gui_setting_desc {
    const char *key;            /* key in the file and on the wire */
    uint8_t type;               /* gui_setting_type_t */
    uint8_t prec;               /* GS_FLOAT: printf precision */
    uint16_t flags;             /* GS_* */
    uint32_t offset;            /* offsetof(gui_settings_t, field) */
    uint32_t cap;               /* GS_STR: buffer size incl. NUL */
    gui_setting_parse_fn parse; /* NULL = generic by type */
};

/* The table and its length. */
const gui_setting_desc_t *gui_settings_table(size_t *count);
/* The entry for key, or NULL. */
const gui_setting_desc_t *gui_settings_find(const char *key);

/* Format the settings file (the exact layout gui_settings_save writes) into
 * buf. Returns the length the full text needs, like snprintf: a result >= cap
 * means it was truncated and must not be written. */
size_t gui_settings_format_file(const gui_settings_t *s, char *buf, size_t cap);

/* Apply every key found in content (settings-file syntax) over s, then run
 * the post-load migrations. Unknown keys are ignored, missing keys keep
 * their current value, the first occurrence of a key wins. */
void gui_settings_parse_text(gui_settings_t *s, const char *content);

/* The post-load migrations alone (forced-zero limits, port string mirrors,
 * legacy bit-depth derivation, base name default, auto-name refresh). */
void gui_settings_post_load(gui_settings_t *s);

/* Find "key": in content and copy the bare value (no quotes) into out.
 * json=false is the settings-file dialect (raw strings, no escapes);
 * json=true also decodes backslash escapes. Returns false when the key is
 * absent or the value is unterminated. */
bool gui_settings_find_value(const char *content, const char *key,
                             char *out, size_t cap, bool json);

/* Compact JSON object of every entry whose flags have none of exclude_flags
 * set (GS_LOAD_ONLY entries are never emitted). Strings are escaped. Returns
 * the needed length like snprintf. */
size_t gui_settings_to_json(const gui_settings_t *s, unsigned exclude_flags,
                            char *buf, size_t cap);

/* JSON-escape text into out (quotes, backslashes, control characters; no
 * surrounding quotes). Returns the needed length like snprintf. */
size_t gui_settings_json_escape(const char *text, char *out, size_t cap);

/* Parse and store one key. Returns 0 on success, -1 for an unknown key, -2
 * for an invalid value (err describes why). strict selects the network
 * rules (see gui_setting_parse_fn). */
int gui_settings_apply_key(gui_settings_t *s, const char *key, const char *value,
                           bool strict, char *err, size_t errcap);

/* The bare value text of one field, as apply_key would accept it. */
bool gui_settings_format_value(const gui_settings_t *s, const gui_setting_desc_t *d,
                               char *out, size_t cap);

/* True when a and b hold the same value for d's field. */
bool gui_settings_field_equal(const gui_settings_t *a, const gui_settings_t *b,
                              const gui_setting_desc_t *d);

/* Copy every table field with (or, when client_local is false, without) the
 * GS_CLIENT_LOCAL flag from src to dst. */
void gui_settings_copy_fields(gui_settings_t *dst, const gui_settings_t *src,
                              bool client_local);
/* Copy one table field from src to dst. */
void gui_settings_copy_field(gui_settings_t *dst, const gui_settings_t *src,
                             const gui_setting_desc_t *d);

/* A counter that gui_settings_save() bumps, so a peer can tell whether the
 * snapshot it holds is current. Starts at 1. */
uint32_t gui_settings_generation(void);
void gui_settings_bump_generation(void);

/* While suspended, gui_settings_save() only records that a save was asked
 * for. Un-suspending returns that flag (and clears it) so the caller can
 * save once, with the struct it actually wants persisted. */
bool gui_settings_suspend_save(bool suspend);
bool gui_settings_save_suspended(void);
void gui_settings_note_save_requested(void);

#endif /* GUI_SETTINGS_H */
