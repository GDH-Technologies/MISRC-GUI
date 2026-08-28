#ifndef GUI_UI_SCALE_H
#define GUI_UI_SCALE_H

#include <stdbool.h>

#define GUI_UI_SCALE_MIN_PERCENT 75
#define GUI_UI_SCALE_MAX_PERCENT 200
#define GUI_UI_SCALE_DEFAULT_PERCENT 100
#define GUI_UI_SCALE_STEP_PERCENT 10

typedef struct gui_ui_zoom_state {
    float wheel_remainder;
} gui_ui_zoom_state_t;

typedef struct gui_ui_zoom_result {
    int percent;
    float passthrough_x;
    float passthrough_y;
    bool consumed;
    bool changed;
} gui_ui_zoom_result_t;

// Invalid persisted values fall back to 100% so a damaged settings file cannot
// leave the UI permanently too small or too large to operate.
int gui_ui_scale_sanitize_percent(int percent);

// Parses the integer JSON value used by settings persistence. Malformed,
// trailing, or out-of-range input returns the safe 100% default.
int gui_ui_scale_parse_percent(const char *text);

// Routes one frame of wheel input. Vertical Ctrl/Cmd+wheel is accumulated into
// discrete scale steps and consumed so it cannot also scroll Clay or a panel.
gui_ui_zoom_result_t gui_ui_zoom_process(gui_ui_zoom_state_t *state,
                                         int current_percent,
                                         bool primary_modifier_down,
                                         float wheel_x,
                                         float wheel_y);

#endif // GUI_UI_SCALE_H
