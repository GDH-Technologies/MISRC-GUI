#ifndef GUI_UI_SCALE_H
#define GUI_UI_SCALE_H

#include <stdbool.h>

#define GUI_UI_SCALE_MIN_PERCENT 75
#define GUI_UI_SCALE_MAX_PERCENT 300
// Density the fixed pixel sizes throughout the UI were authored against, and
// the millimetre conversion used to turn a monitor's physical size into one.
#define GUI_UI_SCALE_REFERENCE_DPI 96.0f
#define GUI_UI_SCALE_MM_PER_INCH 25.4f
#define GUI_UI_SCALE_DEFAULT_PERCENT 100
#define GUI_UI_SCALE_STEP_PERCENT 10
#define GUI_UI_SCALE_HUD_DURATION_S 1.5
#define GUI_UI_SCALE_HUD_FADE_S 0.3
#define GUI_UI_MODAL_MARGIN 12
// Compact (short-label) readouts kick in below this width. Keep this below
// the default startup window so full labels remain visible at stock size and
// compact labels only appear once width pressure is real.
#define GUI_UI_STATUS_COMPACT_BREAKPOINT 1360
#define GUI_UI_STATUS_RECORDING_FULL_BREAKPOINT 1425
#define GUI_UI_STATUS_RECORDING_MINIMAL_BREAKPOINT 920
#define GUI_UI_STATUS_NARROW_BREAKPOINT 960
#define GUI_UI_STATUS_RECORDING_NARROW_BREAKPOINT 1100
#define GUI_UI_STATUS_TINY_BREAKPOINT 760
#define GUI_UI_STATUS_QUARTER_MAX_WIDTH 1000
#define GUI_UI_STATUS_QUARTER_MAX_HEIGHT 700

typedef enum gui_ui_status_layout_mode {
    GUI_UI_STATUS_LAYOUT_FULL_SINGLE,
    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
    GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE,
} gui_ui_status_layout_mode_t;

typedef struct gui_ui_zoom_state {
    float wheel_remainder;
} gui_ui_zoom_state_t;

typedef struct gui_ui_zoom_result {
    int percent;
    float passthrough_x;
    float passthrough_y;
    bool consumed;
    bool step_attempted;
    bool changed;
} gui_ui_zoom_result_t;

// Invalid persisted values fall back to 100% so a damaged settings file cannot
// leave the UI permanently too small or too large to operate.
int gui_ui_scale_sanitize_percent(int percent);

// Parses the integer JSON value used by settings persistence. Malformed,
// trailing, or out-of-range input returns the safe 100% default.
int gui_ui_scale_parse_percent(const char *text);

// Rounds an arbitrary scale factor onto the supported percent steps. Values
// outside the range clamp to it, and the off-grid 75% step wins anything below
// the midpoint between it and 80%. A non-finite or non-positive factor returns
// the safe 100% default.
int gui_ui_scale_snap_percent(float factor);

// Chooses a startup/auto-follow scale from what the platform reports about the
// display, in priority order:
//   1. content_scale, the desktop's own stated scale (Xft.dpi on X11/XWayland,
//      the compositor on Wayland). Honoured first so the app matches the rest
//      of the desktop instead of second-guessing it.
//   2. Physical density, monitor_px_w / (monitor_mm_w / 25.4), for platforms
//      that report no content scale but do report a panel size.
//      Known limitation: X11 and XWayland expose content scale as a single
//      global value (Xft.dpi) shared by every display, so on a mixed-density
//      X11 desktop rule 1 answers the same for all monitors and auto-follow
//      cannot tell them apart. That is deliberate -- matching the desktop's
//      own scale keeps the app consistent with every other window -- and the
//      manual zoom and Settings control cover the odd panel out. Per-monitor
//      following works where the compositor reports per-window content scale.
//   3. 100%. A monitor that reports a zero physical size -- some HDMI panels
//      report 0mm x 0mm -- lands here rather than dividing by zero.
// backing_scale is the framebuffer-to-logical ratio raylib has already applied
// (macOS Retina, or any backend honouring FLAG_WINDOW_HIGHDPI); the candidate
// is divided by it so an already-magnified framebuffer is not compensated for
// twice. Detection only ever scales up: a low-density panel returns 100% so a
// TV or projector never shrinks the UI. Manual zoom can still go below it.
int gui_ui_scale_from_display(float content_scale,
                              int monitor_px_w,
                              int monitor_mm_w,
                              float backing_scale);

// Returns true when auto-follow should adopt a newly detected scale. A user who
// has pinned the scale by zooming manually clears auto_enabled, so their choice
// is never silently overridden when the window moves to another display.
bool gui_ui_scale_should_follow(int applied_percent,
                                int detected_percent,
                                bool auto_enabled);

// Applies one keyboard/wheel zoom step while preserving the special 75%-80%
// transition and the configured scale bounds. A zero direction is a no-op.
int gui_ui_scale_step_percent(int current_percent, int direction);

// Returns the transient zoom HUD opacity from its remaining display time.
// The HUD stays opaque until the final fade interval, then reaches zero at
// the deadline.
float gui_ui_scale_hud_opacity(double remaining_seconds);

// Caps a modal extent to the scale-adjusted logical viewport while retaining
// a small margin on both sides. The result is always at least one pixel.
int gui_ui_modal_max_extent(int layout_extent, int configured_max);

// Returns true when the measured single-row toolbar would exceed the
// scale-adjusted logical viewport.
bool gui_ui_toolbar_uses_two_rows(int layout_width,
                                  int single_row_required_width);

// Chooses a deterministic status-bar layout from scale-adjusted logical
// dimensions. Recording reserves extra width for its timer/runway, while
// labels compact and lower-priority counters disappear before overflow. Very
// small/short layouts keep one minimal row to preserve plot height.
gui_ui_status_layout_mode_t gui_ui_get_status_layout_mode(int layout_width,
                                                          int layout_height,
                                                          bool is_recording);

// Error text is the only state important enough to add a second row. Minimal
// layouts remain single-row to protect the remaining plot height.
bool gui_ui_status_uses_two_rows(gui_ui_status_layout_mode_t layout_mode,
                                 bool status_is_error);

// Extended frame/missed/error counters are hidden below this width so a
// normal compact status bar can remain on one line.
bool gui_ui_status_shows_extended_counters(int layout_width,
                                           bool is_recording);

// Routes one frame of wheel input. Vertical Ctrl/Cmd+wheel is accumulated into
// discrete scale steps and consumed so it cannot also scroll Clay or a panel.
gui_ui_zoom_result_t gui_ui_zoom_process(gui_ui_zoom_state_t *state,
                                         int current_percent,
                                         bool primary_modifier_down,
                                         float wheel_x,
                                         float wheel_y);

#endif // GUI_UI_SCALE_H
