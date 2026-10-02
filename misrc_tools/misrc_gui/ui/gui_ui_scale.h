#ifndef GUI_UI_SCALE_H
#define GUI_UI_SCALE_H

#include <stdbool.h>

// The display-scale grid: what detection snaps to, and the range of the
// persisted ui_scale_percent, which builds before the desktop-relative zoom
// read as the whole scale and still do on a rollback.
#define GUI_UI_SCALE_MIN_PERCENT 75
#define GUI_UI_SCALE_MAX_PERCENT 300
// The user's zoom is relative to the desktop's own scale: 100% draws at the
// size every other app on the desktop does, whatever the display density.
#define GUI_UI_ZOOM_MIN_PERCENT 50
#define GUI_UI_ZOOM_MAX_PERCENT 200
#define GUI_UI_ZOOM_DEFAULT_PERCENT 100
// The effective scale, desktop x zoom, is what the renderer applies. 400% is
// a 200% zoom on a 2x desktop; the 128px font atlas covers the 26px title
// there (104px).
#define GUI_UI_SCALE_EFFECTIVE_MIN_PERCENT 50
#define GUI_UI_SCALE_EFFECTIVE_MAX_PERCENT 400
// Density the fixed pixel sizes throughout the UI were authored against, and
// the millimetre conversion used to turn a monitor's physical size into one.
#define GUI_UI_SCALE_REFERENCE_DPI 96.0f
#define GUI_UI_SCALE_MM_PER_INCH 25.4f
#define GUI_UI_SCALE_DEFAULT_PERCENT 100
#define GUI_UI_SCALE_STEP_PERCENT 10
#define GUI_UI_SCALE_HUD_DURATION_S 1.5
#define GUI_UI_SCALE_HUD_FADE_S 0.3
#define GUI_UI_MODAL_MARGIN 12
#define GUI_UI_VU_BAR_WIDTH 35.0f
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

// Returns true when auto-follow should adopt a newly detected desktop scale.
// Following is the "Follow desktop" setting; zooming never turns it off, since
// the zoom is relative to whatever the desktop scale is.
bool gui_ui_scale_should_follow(int applied_percent,
                                int detected_percent,
                                bool auto_enabled);

// Applies one step on the display-scale grid while preserving the special
// 75%-80% transition and the grid's bounds. A zero direction is a no-op.
int gui_ui_scale_step_percent(int current_percent, int direction);

// The desktop-relative zoom: 50-200% on the 10% grid. Anything else,
// including 0 (no zoom saved yet), falls back to 100%.
int gui_ui_zoom_sanitize_percent(int percent);

// Parses a persisted zoom with the same strictness as
// gui_ui_scale_parse_percent; malformed or out-of-range input returns 100%.
int gui_ui_zoom_parse_percent(const char *text);

// One keyboard/wheel/stepper zoom step, clamped to the zoom bounds. A zero
// direction is a no-op.
int gui_ui_zoom_step_percent(int current_percent, int direction);

// The scale the renderer applies: desktop_percent x zoom_percent / 100,
// rounded and clamped to the effective bounds. A non-positive desktop scale
// counts as 100%, an invalid zoom as 100%.
int gui_ui_scale_effective_percent(int desktop_percent, int zoom_percent);

// Clamps an effective scale to its bounds. Unlike the display grid, any
// integer in range is valid: a 230% panel at 110% zoom is 253%.
int gui_ui_scale_clamp_effective_percent(int percent);

// The effective scale snapped onto the display grid, for ui_scale_percent:
// a build from before the desktop-relative zoom reads that key as the whole
// scale and rejects anything off the grid.
int gui_ui_scale_legacy_percent(int effective_percent);

// Only the channel stats panel width grows by 60% of the effective scale
// above 100%. Return its width relative to the globally scaled UI; text and
// controls keep the global scale. Zoom-out and invalid values stay unchanged.
float gui_ui_stats_width_scale(int percent);

typedef struct gui_ui_channel_spacing {
    float vu_column_width;
    int horizontal_gap;
} gui_ui_channel_spacing_t;

// Follow the status bar's viewport-only compact-label boundary, so live
// values and capture state cannot resize the channel area. Compact channels
// cap empty margins in physical pixels without shrinking the logical VU bar;
// render_scale_x includes OS backing scale. Full labels keep the old spacing.
gui_ui_channel_spacing_t gui_ui_get_channel_spacing(bool compact,
                                                     float render_scale_x);

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

// Add a row only when the complete error and reserved readouts cannot fit.
// Minimal layouts remain single-row to protect the remaining plot height.
bool gui_ui_status_uses_two_rows(gui_ui_status_layout_mode_t layout_mode,
                                 bool status_is_error, int content_width,
                                 int single_row_required_width);

// Extended frame/missed/error counters are hidden below this width so a
// normal compact status bar can remain on one line.
bool gui_ui_status_shows_extended_counters(int layout_width,
                                           bool is_recording);

// Nonzero stream faults still need a compact indicator when the individual
// counters are hidden by the measured budget, not a width breakpoint.
// Critical stop messages retain their existing priority.
bool gui_ui_status_uses_fault_summary(bool has_missed, bool has_errors,
                                      bool show_missed, bool show_errors);

// Routes one frame of wheel input. Vertical Ctrl/Cmd+wheel is accumulated into
// discrete zoom steps and consumed so it cannot also scroll Clay or a panel.
// current_percent and the result are the desktop-relative zoom.
gui_ui_zoom_result_t gui_ui_zoom_process(gui_ui_zoom_state_t *state,
                                         int current_percent,
                                         bool primary_modifier_down,
                                         float wheel_x,
                                         float wheel_y);

#endif // GUI_UI_SCALE_H
