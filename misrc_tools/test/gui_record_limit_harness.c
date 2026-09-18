/*
 * CPU-only checks for the record time limit (the toolbar timer).
 * Includes gui_ui.c for its private timer state and drives the real Arm path
 * and runtime tick against a fake GetTime(), no window and no capture. LTO
 * discards the unrelated UI and backend paths, as in the layout harnesses.
 *
 * The case that matters: arming mid-recording counts the limit from the Arm
 * click. It used to count from the recording's start, so a limit only a little
 * longer than the time already recorded stopped the recording seconds later.
 */
#include <stdio.h>
#include <string.h>

#include "../misrc_gui/ui/gui_ui.c"

static int checks;
static int failures;
static const char *test_case;
static double fake_now;
static int stop_calls;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s (t=%.1f)\n", test_case, message, fake_now);
        failures++;
    }
}

double GetTime(void) { return fake_now; }

void gui_app_stop_recording(gui_app_t *app)
{
    stop_calls++;
    app->is_recording = false;
}

void gui_app_set_status(gui_app_t *app, const char *message)
{
    (void)app;
    (void)message;
}

void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t error_count)
{
    (void)app;
    (void)level;
    (void)message;
    (void)error_class;
    (void)error_count;
}

static void reset(gui_app_t *app, const char *name)
{
    test_case = name;
    memset(app, 0, sizeof(*app));
    fake_now = 1.0;
    stop_calls = 0;
    s_record_limit_armed = false;
    s_record_limit_timecode_edit = false;
    s_record_limit_seconds = 0;
    s_record_limit_session_seen = false;
    s_record_limit_deadline_active = false;
    s_record_limit_deadline_s = 0.0;
    s_record_limit_anchor_s = 0.0;
    snprintf(s_record_limit_timecode, sizeof(s_record_limit_timecode), "00:00:00");
}

static void start_recording(gui_app_t *app)
{
    app->is_recording = true;
    app->recording_start_time = fake_now;
}

static void set_timecode(const char *tc)
{
    snprintf(s_record_limit_timecode, sizeof(s_record_limit_timecode), "%s", tc);
}

/* Advance the clock in 0.1 s frames, ticking the timer each frame, and return
 * the time the recording stopped (or -1 if it never did). */
static double run_until(gui_app_t *app, double until)
{
    while (fake_now < until) {
        fake_now += 0.1;
        gui_record_limit_runtime_tick(app);
        if (!app->is_recording) return fake_now;
    }
    return -1.0;
}

int main(void)
{
    gui_app_t app;
    double stopped;

    reset(&app, "arm mid-recording, limit just past elapsed");
    fake_now = 100.0;
    start_recording(&app);
    gui_record_limit_runtime_tick(&app);
    fake_now = 125.0;                       /* 25 s recorded */
    set_timecode("00:00:30");
    gui_record_limit_arm(&app);
    stopped = run_until(&app, 200.0);
    expect_true(stop_calls == 1, "timer stops the recording exactly once");
    expect_true(stopped >= 154.95 && stopped <= 155.15,
                "stops 30 s after Arm, not 30 s after the start");

    reset(&app, "arm mid-recording, limit shorter than elapsed");
    fake_now = 100.0;
    start_recording(&app);
    gui_record_limit_runtime_tick(&app);
    fake_now = 700.0;                       /* 10 min recorded */
    set_timecode("00:05:00");
    gui_record_limit_arm(&app);
    gui_record_limit_runtime_tick(&app);
    expect_true(app.is_recording, "arming never stops the recording at once");
    stopped = run_until(&app, 1100.0);
    expect_true(stopped >= 999.95 && stopped <= 1000.15,
                "a limit shorter than elapsed still runs its full length from Arm");

    reset(&app, "arm while typing, mid-recording");
    fake_now = 50.0;
    start_recording(&app);
    gui_record_limit_runtime_tick(&app);
    fake_now = 80.0;
    s_record_limit_timecode_edit = true;    /* typed, Arm clicked before Enter */
    snprintf(s_record_limit_timecode_edit_buffer, sizeof(s_record_limit_timecode_edit_buffer),
             "00:01:00");
    gui_record_limit_arm(&app);
    expect_true(!s_record_limit_timecode_edit, "Arm commits the time being typed");
    stopped = run_until(&app, 200.0);
    expect_true(stopped >= 139.95 && stopped <= 140.15, "typed limit counts from Arm");

    reset(&app, "armed before the recording");
    set_timecode("00:01:00");
    gui_record_limit_arm(&app);
    fake_now = 200.0;
    start_recording(&app);
    stopped = run_until(&app, 400.0);
    expect_true(stopped >= 259.95 && stopped <= 260.15,
                "armed while idle counts from the recording's start");

    reset(&app, "stays armed for the next recording");
    set_timecode("00:00:20");
    gui_record_limit_arm(&app);
    fake_now = 10.0;
    start_recording(&app);
    stopped = run_until(&app, 100.0);
    expect_true(stopped >= 29.95 && stopped <= 30.15, "first recording stops at its limit");
    gui_record_limit_runtime_tick(&app);    /* a frame with no recording */
    fake_now = 500.0;
    start_recording(&app);
    stopped = run_until(&app, 600.0);
    expect_true(stopped >= 519.95 && stopped <= 520.15,
                "the next recording is timed from its own start, not the old anchor");

    reset(&app, "disarm mid-recording");
    fake_now = 10.0;
    start_recording(&app);
    set_timecode("00:00:10");
    gui_record_limit_arm(&app);
    run_until(&app, 15.0);
    s_record_limit_armed = false;           /* what the Disarm click does */
    stopped = run_until(&app, 60.0);
    expect_true(stopped < 0.0 && stop_calls == 0, "a disarmed timer never stops the recording");

    reset(&app, "longer limit while running");
    fake_now = 10.0;
    start_recording(&app);
    fake_now = 20.0;
    set_timecode("00:00:10");
    gui_record_limit_arm(&app);             /* deadline 30 */
    fake_now = 25.0;
    set_timecode("00:00:20");               /* Enter with a longer limit: 20 s from Arm */
    stopped = run_until(&app, 100.0);
    expect_true(stopped >= 39.95 && stopped <= 40.15,
                "a longer limit extends from the same Arm moment");

    printf("record limit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
