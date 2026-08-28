#include "gui_ui_scale.h"

#include <math.h>
#include <stdio.h>

static int failures = 0;

static void expect_true(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void expect_float(float actual, float expected, const char *message)
{
    if (fabsf(actual - expected) < 0.0001f) return;
    fprintf(stderr, "FAIL: %s (actual %.4f, expected %.4f)\n",
            message, (double)actual, (double)expected);
    failures++;
}

int main(void)
{
    gui_ui_zoom_state_t state = {0};

    expect_true(gui_ui_scale_sanitize_percent(75) == 75,
                "minimum persisted scale remains valid");
    expect_true(gui_ui_scale_sanitize_percent(200) == 200,
                "maximum persisted scale remains valid");
    expect_true(gui_ui_scale_sanitize_percent(0) == 100,
                "invalid low persisted scale falls back to 100");
    expect_true(gui_ui_scale_sanitize_percent(999) == 100,
                "invalid high persisted scale falls back to 100");
    expect_true(gui_ui_scale_sanitize_percent(137) == 100,
                "persisted scale outside the supported steps falls back to 100");
    expect_true(gui_ui_scale_parse_percent("150") == 150,
                "valid persisted scale parses");
    expect_true(gui_ui_scale_parse_percent(NULL) == 100,
                "missing persisted scale falls back to 100");
    expect_true(gui_ui_scale_parse_percent("") == 100,
                "empty persisted scale falls back to 100");
    expect_true(gui_ui_scale_parse_percent("250") == 100,
                "out-of-range persisted scale falls back to 100");
    expect_true(gui_ui_scale_parse_percent("125junk") == 100,
                "persisted scale with trailing data is rejected");
    expect_true(gui_ui_scale_parse_percent("999999999999999999999") == 100,
                "overflowed persisted scale falls back to 100");

    gui_ui_zoom_result_t result =
        gui_ui_zoom_process(&state, 100, false, 0.25f, -0.5f);
    expect_true(!result.consumed && !result.changed && result.percent == 100,
                "plain wheel does not change UI scale");
    expect_float(result.passthrough_x, 0.25f,
                 "plain horizontal wheel passes through");
    expect_float(result.passthrough_y, -0.5f,
                 "plain vertical wheel passes through");

    result = gui_ui_zoom_process(&state, 100, true, 0.0f, 0.4f);
    expect_true(result.consumed && !result.changed && result.percent == 100,
                "partial modified wheel is consumed but waits for a full step");
    result = gui_ui_zoom_process(&state, result.percent, true, 0.0f, 0.6f);
    expect_true(result.consumed && result.changed && result.percent == 110,
                "trackpad wheel remainder produces one zoom step");
    expect_float(result.passthrough_y, 0.0f,
                 "modified vertical wheel is not passed through");

    result = gui_ui_zoom_process(&state, result.percent, true, 0.0f, -1.0f);
    expect_true(result.changed && result.percent == 100,
                "negative modified wheel zooms out");

    result = gui_ui_zoom_process(&state, 100, true, 0.0f, 3.0f);
    expect_true(result.changed && result.percent == 130,
                "large modified wheel delta can cross multiple steps");

    result = gui_ui_zoom_process(&state, 200, true, 0.0f, 1.0f);
    expect_true(result.consumed && !result.changed && result.percent == 200,
                "wheel remains consumed at the upper bound");
    result = gui_ui_zoom_process(&state, 75, true, 0.0f, -1.0f);
    expect_true(result.consumed && !result.changed && result.percent == 75,
                "wheel remains consumed at the lower bound");
    result = gui_ui_zoom_process(&state, 75, true, 0.0f, 1.0f);
    expect_true(result.changed && result.percent == 80,
                "zooming in from 75 percent enters the 10-percent scale grid");

    result = gui_ui_zoom_process(&state, 100, true, 0.75f, 0.0f);
    expect_true(!result.consumed && !result.changed,
                "horizontal-only modified wheel keeps existing behavior");
    expect_float(result.passthrough_x, 0.75f,
                 "horizontal-only modified wheel passes through");

    (void)gui_ui_zoom_process(&state, 100, true, 0.0f, 0.5f);
    result = gui_ui_zoom_process(&state, 100, true, 1.0f, 0.05f);
    expect_true(!result.consumed && !result.changed && result.percent == 100,
                "horizontal-dominant diagonal gesture passes through");
    expect_float(result.passthrough_x, 1.0f,
                 "horizontal-dominant gesture preserves horizontal input");
    expect_float(result.passthrough_y, 0.05f,
                 "horizontal-dominant gesture preserves vertical noise");
    result = gui_ui_zoom_process(&state, 100, true, 0.0f, 0.5f);
    expect_true(!result.changed && result.percent == 100,
                "horizontal-dominant gesture clears prior zoom remainder");

    state.wheel_remainder = 0.0f;
    (void)gui_ui_zoom_process(&state, 100, true, 0.0f, 0.5f);
    (void)gui_ui_zoom_process(&state, 100, false, 0.0f, 0.0f);
    result = gui_ui_zoom_process(&state, 100, true, 0.0f, 0.5f);
    expect_true(!result.changed && result.percent == 100,
                "releasing the modifier clears a partial wheel gesture");

    if (failures != 0) {
        fprintf(stderr, "%d UI scale policy assertion(s) failed\n", failures);
        return 1;
    }

    puts("UI scale policy assertions passed");
    return 0;
}
