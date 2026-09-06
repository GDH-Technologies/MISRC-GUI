#ifndef GUI_APP_H
#define GUI_APP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stddef.h>
#include <time.h>
#include "raylib.h"
#include "../../common/buffer_manager.h"
#ifdef ENABLE_DDD
#include "../../common/ddd_protocol.h"
#endif

// Forward declarations
typedef struct hsdaoh_dev hsdaoh_dev_t;
typedef struct sc_handle sc_handle_t;
typedef struct phosphor_rt phosphor_rt_t;
typedef struct display_thread display_thread_t;

//-----------------------------------------------------------------------------
// Panel View Types (defined here to avoid circular includes)
//-----------------------------------------------------------------------------

typedef enum {
    PANEL_VIEW_WAVEFORM,           // Waveform oscilloscope (line or phosphor mode)
    PANEL_VIEW_FFT,                // FFT spectrum analysis
    PANEL_VIEW_CVBS,               // CVBS luma decoder view
    PANEL_VIEW_HISTOGRAM,          // Amplitude histogram
    PANEL_VIEW_WATERFALL,          // Scrolling FFT spectrogram (freq X, time Y, newest at top)
    PANEL_VIEW_SPECTROGRAPH,       // Scrolling FFT spectrogram (time X, freq Y)
    PANEL_VIEW_DEMOD,              // Demodulator (WFM/NFM/AM/USB/LSB) view on I/Q samples
    PANEL_VIEW_COUNT
    // Future: PANEL_VIEW_XY
} panel_view_type_t;

// Waveform render modes (selected via dropdown on panel)
typedef enum {
    WAVEFORM_MODE_LINE,            // Simple line waveform (fast)
    WAVEFORM_MODE_PHOSPHOR,        // Digital phosphor with persistence
    WAVEFORM_MODE_COUNT
} waveform_render_mode_t;

// Per-Channel Panel Configuration
typedef struct channel_panel_config {
    bool split;                    // false = single panel, true = split view
    panel_view_type_t left_view;   // View for left panel (or only panel if not split)
    panel_view_type_t right_view;  // View for right panel (only used if split)
    void *left_state;              // View-specific state (created via panel_create_view_state)
    void *right_state;             // View-specific state for right panel
    Rectangle left_bounds;         // Cached bounds from last render (for click handling)
    Rectangle right_bounds;        // Cached bounds for right panel
} channel_panel_config_t;

// Display buffer size (samples per channel for oscilloscope)
#define DISPLAY_BUFFER_SIZE 4096
#define MAX_DEVICES 16
#define MAX_FILENAME_LEN 256

// Waveform display sample - resampled via libsoxr with anti-aliasing
// Values are normalized floats in range -1.0 to 1.0
typedef struct {
    float value;              // Resampled waveform value (via libsoxr)
} waveform_sample_t;

// VU meter state - tracks positive and negative separately for AC signals
typedef struct vu_meter_state {
    float level_pos;          // Current smoothed positive level (0-1)
    float level_neg;          // Current smoothed negative level (0-1)
    float peak_pos;           // Peak hold positive (0-1)
    float peak_neg;           // Peak hold negative (0-1)
    float peak_hold_time_pos; // Time since positive peak was captured
    float peak_hold_time_neg; // Time since negative peak was captured
} vu_meter_state_t;

// Oscilloscope display modes (per-channel)
typedef enum {
    SCOPE_MODE_LINE,      // Basic line waveform (fast, simple)
    SCOPE_MODE_PHOSPHOR,  // Digital phosphor with heatmap persistence
    SCOPE_MODE_SPLIT,     // Split view: waveform left, FFT waterfall right
    SCOPE_MODE_CVBS,      // CVBS luma decoder view
    SCOPE_MODE_COUNT      // Number of modes (for cycling)
} scope_display_mode_t;

// Phosphor color modes
typedef enum {
    PHOSPHOR_COLOR_HEATMAP,   // Blue-green-yellow-red heatmap based on intensity
    PHOSPHOR_COLOR_OPACITY,   // Channel color with intensity as opacity
    PHOSPHOR_COLOR_COUNT
} phosphor_color_mode_t;

// Trigger modes (per-channel)
typedef enum {
    TRIGGER_MODE_RISING,      // Rising edge crossing level
    TRIGGER_MODE_FALLING,     // Falling edge crossing level
    TRIGGER_MODE_SYNC,        // Sync-locked trigger (CH3 headswitch phase lock)
    TRIGGER_MODE_CVBS_HSYNC,  // CVBS horizontal sync (auto PAL/NTSC)
    TRIGGER_MODE_COUNT
} trigger_mode_t;
// Trigger source selection (absolute channels)
typedef enum {
    TRIGGER_SOURCE_CH1,       // RF channel 1 / Channel A
    TRIGGER_SOURCE_CH2,       // RF channel 2 / Channel B
    TRIGGER_SOURCE_CH3,       // Clockgen headswitch channel
    TRIGGER_SOURCE_COUNT
} trigger_source_t;

// Per-channel trigger configuration and state
typedef struct {
    bool enabled;              // Trigger enabled for this channel
    int16_t level;             // Trigger level (-2048 to +2047, 12-bit range)
    float zoom_scale;          // Samples per pixel (1.0 = max zoom, higher = more zoomed out)
    int trigger_display_pos;   // Where trigger appears in display buffer (-1 if not triggered)
    atomic_int display_width;  // Actual pixel width of oscilloscope display (updated by renderer, read by extraction thread)
    scope_display_mode_t scope_mode;       // Display mode for this channel (line or phosphor)
    trigger_mode_t trigger_mode;           // Trigger mode (rising edge, falling edge, CVBS)
    trigger_source_t trigger_source;       // Trigger source channel (CH1/CH2/CH3)
    phosphor_color_mode_t phosphor_color;  // Phosphor color mode (heatmap or opacity)

    // Resampler state (managed by gui_oscilloscope.c)
    void *resampler;           // soxr_t handle (NULL if not initialized)
    float resampler_ratio;     // Current decimation ratio the resampler is configured for
} channel_trigger_t;

// Zoom limits
#define ZOOM_SCALE_MIN 1.0f    // 1 sample per pixel (max zoom in)
#define ZOOM_SCALE_MAX 128.0f  // 128 samples per pixel (max zoom out)
#define ZOOM_SCALE_DEFAULT 32.0f

// Default sample rate (40 MSPS per channel)
#define DEFAULT_SAMPLE_RATE 40000000

// Digital phosphor display settings
#define PHOSPHOR_MAX_WIDTH 4096   // Maximum phosphor buffer width (pixels)
#define PHOSPHOR_MAX_HEIGHT 512   // Maximum phosphor buffer height (pixels)
#define PHOSPHOR_DECAY_RATE 0.80f // Base decay multiplier per frame (0-1, higher = slower fade)
#define PHOSPHOR_HIT_INCREMENT 0.5f // Intensity added per waveform hit (0-1)

// Device info for enumeration
// Device type enumeration
typedef enum {
    DEVICE_TYPE_HSDAOH,                  // Hardware device via hsdaoh
    DEVICE_TYPE_SIMPLE_CAPTURE,           // OS video capture
    DEVICE_TYPE_CXADC,                   // CXADC RF capture card(s)
    DEVICE_TYPE_MISRC_CLOCKGEN,          // MISRC Clockgen: pure USB-audio clockgen
                                         // (MISRC v1.5 + shared-clock clockgen),
                                         // distinct from CXADC (no PCI RF cards,
                                         // no DC-offset/tenbit profile)
    DEVICE_TYPE_SIMULATED,               // Simulated device for testing
    DEVICE_TYPE_PLAYBACK,                // Playback from recorded FLAC files
#ifdef ENABLE_FX3
    DEVICE_TYPE_FX3,                     // Cypress FX3 USB device
#endif
#ifdef ENABLE_DDD
    DEVICE_TYPE_DDD,                     // DomesdayDuplicator USB device
#endif
#ifdef ENABLE_RTLSDR
    DEVICE_TYPE_RTLSDR,                  // RTL-SDR (RTL2832) USB receiver
#endif
} device_type_t;

typedef struct {
    char name[64];
    char serial[64];
    device_type_t type;
    int index;
#ifdef ENABLE_DDD
    ddd_device_profile_t ddd_profile;
    uint16_t ddd_vendor_id;
    uint16_t ddd_product_id;
    uint16_t ddd_bcd_device;
    char ddd_usb_path[DDD_STABLE_ID_MAX];
    bool ddd_capture_supported;
    bool ddd_clockgen;
#endif
} device_info_t;

// GUI settings (bound to UI controls): gui_settings_t and its descriptor table.
#include "gui_settings.h"

typedef enum {
    GUI_DROPOUT_NONE = 0,
    GUI_DROPOUT_MISSED_FRAME = 1,
    GUI_DROPOUT_FRAME_ERROR = 2,
    GUI_DROPOUT_ERROR_BURST = 3,
    GUI_DROPOUT_CALLBACK_GAP = 4,
    GUI_DROPOUT_DEVICE_ERROR = 5,
    GUI_DROPOUT_BACKPRESSURE = 6,
    GUI_DROPOUT_DISK_SPACE = 7,
    GUI_DROPOUT_LOW_SIGNAL = 8,   // Level autostop: sustained low/no signal (tape end)
} gui_dropout_reason_t;

// Main application state
typedef struct gui_app {
    // Device handles
    hsdaoh_dev_t *hs_dev;
    sc_handle_t *sc_dev;

    // Simulated device state
    void *sim_thread;          // Simulated capture thread handle
    atomic_bool sim_running;   // Flag to stop simulated capture

    // Playback device state
    atomic_bool playback_running;  // Flag for playback mode

#ifdef ENABLE_FX3
    // FX3 device state
    void *fx3_dev;                 // FX3 device handle (cyusb_handle *)
    void *fx3_thread;              // FX3 capture thread handle
    atomic_bool fx3_running;       // Flag for FX3 capture mode
#endif

#ifdef ENABLE_DDD
    // DdD device state
    void *ddd_dev;                 // DdD device handle (libusb_device_handle *)
    void *ddd_thread;              // DdD capture thread handle
    atomic_bool ddd_running;       // Flag for DdD capture mode
#endif

    // Capture state
    bool is_capturing;
    bool is_recording;
    bool user_capture_mode_misrc;      // Authoritative user-selected mode (changes only via mode toggle)
    bool capture_mode_runtime_misrc;   // Mode latched at recording start (stable for recording session)
    bool capture_backend_upstream;     // Active backend for current capture session (true=upstream callback)
    bool capture_has_channel_b;        // Runtime capability flag used by extraction/display mapping
    // The V1.5/V2.5 wiring inversion the extraction applies to the MISRC A/B
    // swap. Set on the main thread: from settings locally, from the server's
    // snapshot on a net client, so the extraction thread never reads a
    // settings field that a client-mode view swap may be replacing.
    bool capture_ab_swap_invert;

    // Device enumeration
    device_info_t devices[MAX_DEVICES];
    int device_count;
    int selected_device;

    // Per-channel display buffers for waveform
    waveform_sample_t display_samples_a[DISPLAY_BUFFER_SIZE];
    waveform_sample_t display_samples_b[DISPLAY_BUFFER_SIZE];
    size_t display_samples_available_a;
    size_t display_samples_available_b;

    // VU meter state (updated on main thread from atomic values)
    vu_meter_state_t vu_a;
    vu_meter_state_t vu_b;

    // Atomic values from capture thread (separate pos/neg for AC signals)
    atomic_uint_fast16_t peak_a_pos;  // Maximum positive sample (0-2047)
    atomic_uint_fast16_t peak_a_neg;  // Maximum negative sample (0-2048, stored as positive)
    atomic_uint_fast16_t peak_b_pos;
    atomic_uint_fast16_t peak_b_neg;

    // Statistics (atomic, updated by capture thread)
    atomic_uint_fast64_t total_samples;
    atomic_uint_fast64_t samples_a;         // Per-channel sample count
    atomic_uint_fast64_t samples_b;
    atomic_uint_fast32_t frame_count;
    atomic_uint_fast32_t missed_frame_count;  // Missed frames from sync events
    atomic_uint_fast32_t error_count;       // Combined total (parser + system events)
    atomic_uint_fast32_t parser_error_count; // Frame/parser error totals
    atomic_uint_fast32_t system_error_count; // FLAC/device/sync/system event totals
    atomic_uint_fast32_t error_count_a;     // Per-channel error counts
    atomic_uint_fast32_t error_count_b;
    atomic_uint_fast32_t clip_count_a_pos;  // Positive clipping (sample >= 2047)
    atomic_uint_fast32_t clip_count_a_neg;  // Negative clipping (sample <= -2048)
    atomic_uint_fast32_t clip_count_b_pos;
    atomic_uint_fast32_t clip_count_b_neg;
    atomic_bool stream_synced;
    atomic_uint_fast32_t sample_rate;
    atomic_uint_fast32_t audio_sample_rate;

    // Audio monitoring peaks (24-bit audio magnitude, per channel 1..4)
    atomic_uint_fast32_t audio_peak[4];

    // Buffer manager (centralized ringbuffer management)
    buffer_manager_t buffers;

    // Display thread (decoupled from recording path)
    display_thread_t *display_thread;

    // Backpressure statistics (for debugging buffer contention)
    atomic_uint_fast32_t rb_wait_count;     // Times callback had to wait for buffer space
    atomic_uint_fast32_t rb_drop_count;     // Frames dropped due to full buffer (after timeout)

    // Recording state
    double recording_start_time;
    double last_recording_duration_s;

    // Capture session timing
    double capture_start_time;
    char capture_timestamp[32];                  // yyyy.mm.dd_hh.mm.ss at capture start (empty if not set)
    atomic_uint_fast64_t recording_bytes;        // Total raw bytes recorded
    atomic_uint_fast64_t recording_raw_a;        // Raw input bytes channel A
    atomic_uint_fast64_t recording_raw_b;        // Raw input bytes channel B
    atomic_uint_fast64_t recording_compressed_a; // Compressed output bytes channel A
    atomic_uint_fast64_t recording_compressed_b; // Compressed output bytes channel B

    // GUI settings
    gui_settings_t settings;

    // UI state
    bool settings_panel_open;
    char status_message[256];
    double status_message_time;

    // hsdaoh status message cache (written from hsdaoh thread, applied by UI thread)
    // Support hsdaoh-rp2350 error handling & stats
    atomic_bool hs_msg_pending;
    atomic_int hs_msg_level;
    atomic_flag hs_msg_lock;
    char hs_msg_buf[512];

    // UI-side poll timing (UI thread only)
    uint64_t hs_ui_last_poll_ms;

    // Auto-reconnect state
    bool auto_reconnect_enabled;
    bool reconnect_pending;
    double reconnect_attempt_time;
    int reconnect_attempts;

    // Stop-on-dropout request path (set from capture thread, consumed on main thread)
    atomic_bool dropout_stop_requested;
    atomic_uint_fast32_t dropout_stop_reason;

    // Level autostop state (main thread only): tracks sustained low signal
    float low_signal_time;     // Seconds signal has stayed below the level threshold
    bool low_signal_armed;     // True once a real signal level has been seen above the threshold

    // Device disconnect detection (timestamp of last successful callback)
    atomic_uint_fast64_t last_callback_time_ms;

    // Fonts
    Font *fonts;

    // Per-channel trigger configuration (includes zoom level per channel)
    channel_trigger_t trigger_a;
    channel_trigger_t trigger_b;

    // Digital phosphor - uses shared phosphor_rt module
    phosphor_rt_t *phosphor_a;         // Phosphor render texture for channel A
    phosphor_rt_t *phosphor_b;         // Phosphor render texture for channel B

    // Panel configuration (per-channel, each panel owns its state)
    // FFT state is now owned by panel_config_*.left_state or right_state
    // CVBS decoder state is also owned by panel's left_state or right_state
    // NOTE: Accessed from both UI thread and display thread; protect with panel_config_lock.
    channel_panel_config_t panel_config_a, panel_config_b;

    // Protects panel_config_{a,b} and their *state pointers against concurrent access
    // between UI interactions and the display thread.
    atomic_flag panel_config_lock;

    // Server/Client networking state (opaque; implemented in net/gui_net.c).
    // NULL when net_mode == Local. Owned by gui_app_t; created/destroyed by
    // gui_net_apply_mode(). Accessed from the UI/main thread only for control;
    // the server/client worker threads read gui_app_t atomics + this handle.
    void *net_state;

    // Net control command queue: HTTP endpoints (server) and the client mirror
    // thread set these atomic flags; the main loop polls them and executes the
    // corresponding control action on the main thread (safe, like
    // dropout_stop_requested). Pending integer arguments follow each flag.
    atomic_bool net_cmd_start;          // /start requested
    atomic_bool net_cmd_stop;           // /stop requested
    atomic_bool net_cmd_record_on;      // /record?on=1 requested
    atomic_bool net_cmd_record_off;     // /record?on=0 requested
    atomic_bool net_cmd_select_device;  // /device?N requested (arg in net_cmd_device_index)
    atomic_int  net_cmd_device_index;   // Device index argument for select_device.

    // Net mirrored state snapshot (written by client mirror thread, read by UI).
    // Server mode also writes these so the local UI reflects remote-driven state.
    atomic_int  net_peer_state;         // 0=idle,1=capturing,2=recording
    atomic_int  net_peer_sample_rate;   // Hz reported by peer /stats.
    atomic_int  net_peer_device_count;  // device count from peer /devices.
    atomic_int  net_peer_selected;      // selected_device from peer.
    atomic_bool net_connected;          // server listening / client connected.
    atomic_bool net_error;              // last operation failed (see net_status).

    // Dedicated network status line, shown ONLY in the info window's Network
    // section. Net code writes this via gui_net_set_status() and must NEVER
    // touch the bottom status bar (app->status_message), which is owned by the
    // capture/record/device path. This keeps server/client activity from
    // clobbering the bottom bar.
    char net_status[160];
    double net_status_time;  // unused placeholder (mirrors status_message pattern)

    // The server's own bottom-bar message, relayed to a net client by
    // gui_net_poll_mirror() (main thread). Never copied into status_message:
    // the bar's reader consults it in client mode and prefixes it, so the
    // capture/record path stays the only writer of the bar on either side.
    char net_peer_status[256];
    uint32_t net_peer_status_seq;   // bumps when the server's message changes
    double net_peer_status_time;    // GetTime() when it last changed here
} gui_app_t;

static inline void gui_app_count_parser_errors(gui_app_t *app, uint32_t count) {
    if (!app || count == 0) return;
    atomic_fetch_add(&app->parser_error_count, count);
    atomic_fetch_add(&app->error_count, count);
}

static inline void gui_app_count_system_errors(gui_app_t *app, uint32_t count) {
    if (!app || count == 0) return;
    atomic_fetch_add(&app->system_error_count, count);
    atomic_fetch_add(&app->error_count, count);
}

// Application lifecycle
void gui_app_init(gui_app_t *app);
void gui_app_cleanup(gui_app_t *app);

// Device management
void gui_app_enumerate_devices(gui_app_t *app);
int gui_app_start_capture(gui_app_t *app);
/* Android-only: run gui_app_start_capture() on a worker thread so the render
 * loop stays responsive during hsdaoh_open + stream start. On non-Android
 * builds this is not declared/defined. */
#if defined(__ANDROID__)
void gui_app_start_capture_async(gui_app_t *app);
void gui_app_stop_capture_async(gui_app_t *app);
int gui_app_capture_busy(void);
#endif
void gui_app_stop_capture(gui_app_t *app);
int gui_app_start_recording(gui_app_t *app);
void gui_app_stop_recording(gui_app_t *app);
/* "Are we recording" for every readout and control: is_recording locally,
 * the server's recording state on a net client (which never sets its own
 * is_recording; that would start local WAV writers). */
bool gui_app_effective_recording(const gui_app_t *app);
/* "Is there a capture the controls act on": is_capturing locally, the
 * server's capture state on a net client (where is_capturing only means the
 * ingest is connected). */
bool gui_app_control_capturing(const gui_app_t *app);

// Update functions (called each frame)
void gui_app_update_vu_meters(gui_app_t *app, float dt);
void gui_app_update_display_buffer(gui_app_t *app);

// Clear display (called when device disconnects)
void gui_app_clear_display(gui_app_t *app);

// Status messages
void gui_app_set_status(gui_app_t *app, const char *message);

// Settings persistence and the descriptor table: see gui_settings.h.

// Constants for VU meter
#define VU_ATTACK_TIME 0.01f      // 10ms attack
#define VU_RELEASE_TIME 0.3f      // 300ms release
#define PEAK_HOLD_DURATION 2.0f   // 2 second peak hold
#define PEAK_DECAY_RATE 0.5f      // Decay rate after hold

// Note: Color definitions are in gui_ui.h

#endif // GUI_APP_H
