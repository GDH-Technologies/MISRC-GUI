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

    expect_true(gui_ui_scale_step_percent(100, 1) == 110,
                "keyboard zoom-in advances one scale step");
    expect_true(gui_ui_scale_step_percent(100, -1) == 90,
                "keyboard zoom-out retreats one scale step");
    expect_true(gui_ui_scale_step_percent(75, 1) == 80,
                "keyboard zoom-in preserves the 75-to-80 transition");
    expect_true(gui_ui_scale_step_percent(80, -1) == 75,
                "keyboard zoom-out preserves the 80-to-75 transition");
    expect_true(gui_ui_scale_step_percent(200, 1) == 200,
                "keyboard zoom-in stops at the upper bound");
    expect_true(gui_ui_scale_step_percent(75, -1) == 75,
                "keyboard zoom-out stops at the lower bound");
    expect_true(gui_ui_scale_step_percent(150, 0) == 150,
                "zero keyboard direction leaves scale unchanged");

    expect_float((float)GUI_UI_SCALE_HUD_DURATION_S,
                 1.5f,
                 "zoom HUD uses the requested 1.5-second lifetime");
    expect_float((float)GUI_UI_SCALE_HUD_FADE_S,
                 0.3f,
                 "zoom HUD reserves the final 0.3 seconds for fading");
    expect_true(GUI_UI_SCALE_HUD_DURATION_S > GUI_UI_SCALE_HUD_FADE_S,
                "zoom HUD lifetime is longer than its fade interval");
    expect_float(gui_ui_scale_hud_opacity(GUI_UI_SCALE_HUD_DURATION_S),
                 1.0f,
                 "zoom HUD begins fully opaque");
    expect_float(gui_ui_scale_hud_opacity(GUI_UI_SCALE_HUD_FADE_S),
                 1.0f,
                 "zoom HUD remains opaque until the fade interval");
    expect_float(gui_ui_scale_hud_opacity(GUI_UI_SCALE_HUD_FADE_S / 2.0),
                 0.5f,
                 "zoom HUD fades linearly near its deadline");
    expect_float(gui_ui_scale_hud_opacity(0.0),
                 0.0f,
                 "zoom HUD is hidden at its deadline");
    expect_float(gui_ui_scale_hud_opacity(-0.1),
                 0.0f,
                 "zoom HUD remains hidden after its deadline");

    expect_true(gui_ui_modal_max_extent(320, 1080) == 296,
                "wide modal is clamped inside a 320px logical viewport");
    expect_true(gui_ui_modal_max_extent(180, 780) == 156,
                "tall modal is clamped inside a 180px logical viewport");
    expect_true(gui_ui_modal_max_extent(1920, 1080) == 1080,
                "modal keeps its configured cap on a wide viewport");
    expect_true(gui_ui_modal_max_extent(10, 420) == 1,
                "modal extent remains valid on a degenerate viewport");

    expect_true(gui_ui_toolbar_uses_two_rows(1239, 1300),
                "2478px screenshot at 200 percent uses two toolbar rows");
    expect_true(gui_ui_toolbar_uses_two_rows(320, 500),
                "extremely narrow logical layouts keep the compact toolbar policy");
    expect_true(gui_ui_toolbar_uses_two_rows(1425, 1450),
                "long labels wrap when the default window cannot fit them");
    expect_true(!gui_ui_toolbar_uses_two_rows(1425, 1400),
                "default window stays one row when its current labels fit");
    expect_true(!gui_ui_toolbar_uses_two_rows(1920, 1500),
                "wide logical layouts keep the original single row");

    expect_true(gui_ui_get_status_layout_mode(1090, 700, false) ==
                    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
                "large-scale screenshot width uses compact single-row status");
    expect_true(gui_ui_get_status_layout_mode(
                    GUI_UI_STATUS_COMPACT_BREAKPOINT - 1, 900, false) ==
                    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
                "width below full status breakpoint uses compact labels");
    expect_true(gui_ui_get_status_layout_mode(
                    GUI_UI_STATUS_COMPACT_BREAKPOINT, 900, false) ==
                    GUI_UI_STATUS_LAYOUT_FULL_SINGLE,
                "full status breakpoint restores the original labels");
    expect_true(gui_ui_get_status_layout_mode(1425, 900, false) ==
                    GUI_UI_STATUS_LAYOUT_FULL_SINGLE,
                "default window keeps full status labels at stock size");
    expect_true(gui_ui_get_status_layout_mode(1425, 900, true) ==
                    GUI_UI_STATUS_LAYOUT_FULL_SINGLE,
                "recording keeps full status labels at stock size");
    expect_true(gui_ui_get_status_layout_mode(
                    GUI_UI_STATUS_RECORDING_MINIMAL_BREAKPOINT - 1,
                    900, true) == GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE,
                "narrow recording layout hides runway and lower-priority groups");
    expect_true(gui_ui_get_status_layout_mode(
                    GUI_UI_STATUS_RECORDING_MINIMAL_BREAKPOINT,
                    900, true) == GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
                "recording minimal breakpoint restores the compact row");
    expect_true(gui_ui_get_status_layout_mode(
                    GUI_UI_STATUS_RECORDING_FULL_BREAKPOINT, 900, true) ==
                    GUI_UI_STATUS_LAYOUT_FULL_SINGLE,
                "wide recording layout restores full status labels");
    expect_true(gui_ui_get_status_layout_mode(1000, 700, false) ==
                    GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE,
                "quarter-scale boundary preserves plot height with one minimal row");
    expect_true(gui_ui_get_status_layout_mode(1001, 700, false) ==
                    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
                "one pixel beyond quarter width stays a compact single row");
    expect_true(gui_ui_get_status_layout_mode(1000, 701, false) ==
                    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
                "one pixel beyond quarter height stays a compact single row");
    expect_true(gui_ui_get_status_layout_mode(759, 900, false) ==
                    GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE,
                "extremely narrow status bar remains a minimal single row");
    expect_true(gui_ui_get_status_layout_mode(760, 900, false) ==
                    GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE,
                "tiny breakpoint itself remains a compact single row");
    expect_true(gui_ui_status_uses_two_rows(GUI_UI_STATUS_LAYOUT_FULL_SINGLE, true),
                "wide status profile gives error text a dedicated row");
    expect_true(gui_ui_status_uses_two_rows(GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE, true),
                "compact status profile gives error text a dedicated row");
    expect_true(!gui_ui_status_uses_two_rows(GUI_UI_STATUS_LAYOUT_MINIMAL_SINGLE, true),
                "minimal profile remains one row even when showing an error");
    expect_true(!gui_ui_status_uses_two_rows(GUI_UI_STATUS_LAYOUT_COMPACT_SINGLE, false),
                "normal compact status remains one row");
    expect_true(!gui_ui_status_shows_extended_counters(
                    GUI_UI_STATUS_NARROW_BREAKPOINT - 1, false),
                "narrow status hides lower-priority counters");
    expect_true(gui_ui_status_shows_extended_counters(
                    GUI_UI_STATUS_NARROW_BREAKPOINT, false),
                "extended counters return at the narrow breakpoint");
    expect_true(!gui_ui_status_shows_extended_counters(
                    GUI_UI_STATUS_RECORDING_NARROW_BREAKPOINT - 1, true),
                "recording keeps extended counters hidden until they fit");
    expect_true(gui_ui_status_shows_extended_counters(
                    GUI_UI_STATUS_RECORDING_NARROW_BREAKPOINT, true),
                "recording restores extended counters at its wider breakpoint");

    gui_ui_zoom_result_t result =
        gui_ui_zoom_process(&state, 100, false, 0.25f, -0.5f);
    expect_true(!result.consumed && !result.changed && result.percent == 100,
                "plain wheel does not change UI scale");
    expect_float(result.passthrough_x, 0.25f,
                 "plain horizontal wheel passes through");
    expect_float(result.passthrough_y, -0.5f,
                 "plain vertical wheel passes through");

    result = gui_ui_zoom_process(&state, 100, true, 0.0f, 0.4f);
    expect_true(result.consumed && !result.step_attempted &&
                    !result.changed && result.percent == 100,
                "partial modified wheel is consumed but waits for a full step");
    result = gui_ui_zoom_process(&state, result.percent, true, 0.0f, 0.6f);
    expect_true(result.consumed && result.step_attempted &&
                    result.changed && result.percent == 110,
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
    expect_true(result.consumed && result.step_attempted &&
                    !result.changed && result.percent == 200,
                "upper-bound wheel attempt remains visible to HUD routing");
    result = gui_ui_zoom_process(&state, 75, true, 0.0f, -1.0f);
    expect_true(result.consumed && result.step_attempted &&
                    !result.changed && result.percent == 75,
                "lower-bound wheel attempt remains visible to HUD routing");
    result = gui_ui_zoom_process(&state, 75, true, 0.0f, 1.0f);
    expect_true(result.changed && result.percent == 80,
                "zooming in from 75 percent enters the 10-percent scale grid");

    result = gui_ui_zoom_process(&state, 100, true, 0.75f, 0.0f);
    expect_true(!result.consumed && !result.changed,
                "horizontal-only modified wheel keeps existing behavior");
    expect_float(result.passthrough_x, 0.75f,
                 "horizontal-only modified wheel passes through");

    state.wheel_remainder = 0.0f;
    (void)gui_ui_zoom_process(&state, 100, true, 0.0f, 0.5f);
    (void)gui_ui_zoom_process(&state, 100, true, 0.75f, 0.0f);
    result = gui_ui_zoom_process(&state, 100, true, 0.0f, 0.5f);
    expect_true(!result.step_attempted && !result.changed && result.percent == 100,
                "horizontal modified wheel clears stale vertical remainder");

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
