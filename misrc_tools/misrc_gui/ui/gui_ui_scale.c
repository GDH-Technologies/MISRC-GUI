#include "gui_ui_scale.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

int gui_ui_scale_sanitize_percent(int percent)
{
    if (percent == GUI_UI_SCALE_MIN_PERCENT) return percent;
    if (percent >= 80 && percent <= GUI_UI_SCALE_MAX_PERCENT &&
        (percent % GUI_UI_SCALE_STEP_PERCENT) == 0) return percent;
    return GUI_UI_SCALE_DEFAULT_PERCENT;
}

int gui_ui_scale_parse_percent(const char *text)
{
    if (!text || text[0] == '\0') return GUI_UI_SCALE_DEFAULT_PERCENT;

    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        return GUI_UI_SCALE_DEFAULT_PERCENT;
    }

    return gui_ui_scale_sanitize_percent((int)parsed);
}

gui_ui_zoom_result_t gui_ui_zoom_process(gui_ui_zoom_state_t *state,
                                         int current_percent,
                                         bool primary_modifier_down,
                                         float wheel_x,
                                         float wheel_y)
{
    gui_ui_zoom_result_t result = {
        .percent = gui_ui_scale_sanitize_percent(current_percent),
        .passthrough_x = wheel_x,
        .passthrough_y = wheel_y,
        .consumed = false,
        .changed = false,
    };

    if (!state) return result;

    if (!primary_modifier_down) {
        state->wheel_remainder = 0.0f;
        return result;
    }

    // Idle frames keep a partial trackpad gesture alive until more vertical
    // motion arrives. A horizontal-dominant gesture remains normal scrolling
    // and cancels any earlier vertical remainder.
    if (wheel_y == 0.0f) return result;
    if (fabsf(wheel_x) > fabsf(wheel_y)) {
        state->wheel_remainder = 0.0f;
        return result;
    }

    result.consumed = true;
    result.passthrough_x = 0.0f;
    result.passthrough_y = 0.0f;

    // Do not make a small trackpad movement in the opposite direction fight a
    // previously accumulated gesture.
    if ((state->wheel_remainder > 0.0f && wheel_y < 0.0f) ||
        (state->wheel_remainder < 0.0f && wheel_y > 0.0f)) {
        state->wheel_remainder = 0.0f;
    }
    state->wheel_remainder += wheel_y;

    while (state->wheel_remainder >= 1.0f ||
           state->wheel_remainder <= -1.0f) {
        int direction = (state->wheel_remainder > 0.0f) ? 1 : -1;
        int next_percent;
        if (direction > 0 && result.percent == GUI_UI_SCALE_MIN_PERCENT) {
            next_percent = 80;
        } else if (direction < 0 && result.percent == 80) {
            next_percent = GUI_UI_SCALE_MIN_PERCENT;
        } else {
            next_percent = result.percent + direction * GUI_UI_SCALE_STEP_PERCENT;
        }
        if (next_percent < GUI_UI_SCALE_MIN_PERCENT) {
            next_percent = GUI_UI_SCALE_MIN_PERCENT;
        }
        if (next_percent > GUI_UI_SCALE_MAX_PERCENT) {
            next_percent = GUI_UI_SCALE_MAX_PERCENT;
        }

        if (next_percent == result.percent) {
            state->wheel_remainder = 0.0f;
            break;
        }

        result.percent = next_percent;
        result.changed = true;
        state->wheel_remainder -= (float)direction;

        if (result.percent == GUI_UI_SCALE_MIN_PERCENT ||
            result.percent == GUI_UI_SCALE_MAX_PERCENT) {
            state->wheel_remainder = 0.0f;
            break;
        }
    }

    return result;
}
