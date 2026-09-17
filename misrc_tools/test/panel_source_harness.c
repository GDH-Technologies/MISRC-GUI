/*
 * CPU-only checks for per-panel source routing: each of the four panels is fed
 * the channel its source names, a B-sourced panel is skipped when the capture
 * has no channel B, and changing a panel's source gives it fresh state.
 * Include the production registry and panel helpers; LTO drops the renderers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../misrc_gui/visualization/panel_registry.c"
#include "../misrc_gui/visualization/gui_panel.c"

static int checks;
static int failures;
static const char *test_case;
static gui_app_t test_app;

static void expect_true(bool condition, const char *message)
{
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL [%s]: %s\n", test_case, message);
        failures++;
    }
}

typedef struct {
    int serial;
    const int16_t *last;
    int calls;
} fake_state_t;

static int created;
static int destroyed;

static void *fake_create(void)
{
    fake_state_t *state = calloc(1, sizeof(*state));
    if (state) state->serial = ++created;
    return state;
}

static void fake_destroy(void *state)
{
    destroyed++;
    free(state);
}

static void fake_process(void *state_ptr, const int16_t *samples, size_t count, uint32_t sample_rate)
{
    (void)count;
    (void)sample_rate;
    fake_state_t *state = state_ptr;
    state->last = samples;
    state->calls++;
}

static const panel_vtable_t fake_vtable = {
    .name = "fake",
    .create = fake_create,
    .destroy = fake_destroy,
    .process = fake_process,
};

static fake_state_t *pane(channel_panel_config_t *config, bool right)
{
    return right ? config->right_state : config->left_state;
}

static void setup_row(channel_panel_config_t *config, int left_source, int right_source)
{
    memset(config, 0, sizeof(*config));
    config->split = true;
    config->left_view = PANEL_VIEW_WAVEFORM;
    config->right_view = PANEL_VIEW_FFT;
    config->left_state = panel_create_view_state(config->left_view);
    config->right_state = panel_create_view_state(config->right_view);
    config->left_source = left_source;
    config->right_source = right_source;
}

static void reset_all(void)
{
    memset(&test_app, 0, sizeof(test_app));
    atomic_flag_clear(&test_app.panel_config_lock);
    setup_row(&test_app.panel_config_a, 0, 1);
    setup_row(&test_app.panel_config_b, 0, 1);
}

static void cleanup_all(void)
{
    panel_config_cleanup(&test_app.panel_config_a);
    panel_config_cleanup(&test_app.panel_config_b);
}

int main(void)
{
    static int16_t samples_a[16];
    static int16_t samples_b[16];
    panel_register(PANEL_VIEW_WAVEFORM, &fake_vtable);
    panel_register(PANEL_VIEW_FFT, &fake_vtable);

    test_case = "routing";
    reset_all();
    panel_process_all(&test_app, samples_a, samples_b, 16, 40000000);
    expect_true(pane(&test_app.panel_config_a, false)->last == samples_a, "top-left (A) gets A");
    expect_true(pane(&test_app.panel_config_a, true)->last == samples_b, "top-right (B) gets B");
    expect_true(pane(&test_app.panel_config_b, false)->last == samples_a, "bottom-left sourced A gets A");
    expect_true(pane(&test_app.panel_config_b, true)->last == samples_b, "bottom-right (B) gets B");
    cleanup_all();

    test_case = "no channel B";
    reset_all();
    panel_process_all(&test_app, samples_a, NULL, 16, 40000000);
    expect_true(pane(&test_app.panel_config_a, false)->calls == 1, "A-sourced top panel still runs");
    expect_true(pane(&test_app.panel_config_b, false)->calls == 1, "A-sourced bottom panel still runs");
    expect_true(pane(&test_app.panel_config_a, true)->calls == 0, "B-sourced top panel is skipped");
    expect_true(pane(&test_app.panel_config_b, true)->calls == 0, "B-sourced bottom panel is skipped");
    cleanup_all();

    test_case = "single layout";
    reset_all();
    panel_config_set_split(&test_app.panel_config_b, false);
    panel_process_all(&test_app, samples_a, samples_b, 16, 40000000);
    expect_true(test_app.panel_config_b.right_state == NULL, "leaving split drops the right state");
    expect_true(pane(&test_app.panel_config_b, false)->calls == 1, "the single panel still runs");
    cleanup_all();

    test_case = "source change";
    reset_all();
    int before_created = created;
    int before_destroyed = destroyed;
    int old_serial = pane(&test_app.panel_config_b, true)->serial;
    panel_config_set_source(&test_app.panel_config_b, true, 1);
    expect_true(created == before_created && destroyed == before_destroyed,
                "setting the same source keeps the state");
    panel_config_set_source(&test_app.panel_config_b, true, 0);
    expect_true(panel_config_source(&test_app.panel_config_b, true) == 0, "the source is stored");
    expect_true(destroyed == before_destroyed + 1 && created == before_created + 1,
                "a new source recreates the state");
    expect_true(pane(&test_app.panel_config_b, true)->serial != old_serial, "the panel has fresh state");
    panel_process_all(&test_app, samples_a, samples_b, 16, 40000000);
    expect_true(pane(&test_app.panel_config_b, true)->last == samples_a, "bottom-right now gets A");
    panel_config_set_source(&test_app.panel_config_b, false, 7);
    expect_true(panel_config_source(&test_app.panel_config_b, false) == 0, "an out-of-range source means A");
    cleanup_all();

    test_case = "source while single";
    reset_all();
    panel_config_set_split(&test_app.panel_config_a, false);
    panel_config_set_source(&test_app.panel_config_a, true, 0);
    expect_true(test_app.panel_config_a.right_state == NULL, "a hidden right panel gets no state");
    panel_config_set_split(&test_app.panel_config_a, true);
    panel_process_all(&test_app, samples_a, samples_b, 16, 40000000);
    expect_true(pane(&test_app.panel_config_a, true)->last == samples_a,
                "the source set while hidden applies once split");
    cleanup_all();

    test_case = "views with a source";
    expect_true(panel_view_type_has_source(PANEL_VIEW_WAVEFORM), "waveform has a source");
    expect_true(panel_view_type_has_source(PANEL_VIEW_FFT), "FFT has a source");
    expect_true(!panel_view_type_has_source(PANEL_VIEW_DEMOD), "demod reads both channels");
    expect_true(!panel_view_type_has_source(PANEL_VIEW_PREVIEW), "preview reads V4L2");

    expect_true(created == destroyed, "every state created was destroyed");
    printf("panel source: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
