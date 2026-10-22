/* Exercise the production callback watchdog and active playback predicate.
 * Only the clock is deterministic; LTO removes unrelated backend/UI code.
 * Playback EOF here is the producer's paused-at-end state, not a media decode.
 */
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include "../common/threading.h"

static uint64_t watchdog_test_now;
static uint64_t watchdog_test_clock_ms(void) { return watchdog_test_now; }
#define get_time_ms watchdog_test_clock_ms
#include "../misrc_gui/input/gui_capture.c"
#include "../misrc_gui/input/gui_playback.c"
#undef get_time_ms

static gui_app_t watchdog_test_app;

static void watchdog_test_hardware(void) {
    gui_app_t *app=&watchdog_test_app;
    atomic_store(&s_playback->running,false);
    app->is_capturing=true;
    watchdog_test_now=10000;
    atomic_store(&app->last_callback_time_ms,10000);
    assert(!gui_capture_device_timeout(app,2000));
    watchdog_test_now=12000;
    assert(!gui_capture_device_timeout(app,2000));
    watchdog_test_now=12001;
    assert(gui_capture_device_timeout(app,2000));

    /* Clock rollback retains the production reset of the hardware baseline. */
    watchdog_test_now=9999;
    assert(!gui_capture_device_timeout(app,2000));
    assert(atomic_load(&app->last_callback_time_ms)==9999);
    watchdog_test_now=12000;
    assert(gui_capture_device_timeout(app,2000));
    app->is_capturing=false;
    assert(!gui_capture_device_timeout(app,2000));
    puts("Hardware watchdog: fresh, exact threshold, timeout, rollback, idle PASS");
}

static void watchdog_test_playback(void) {
    gui_app_t *app=&watchdog_test_app;
    playback_ctx_t ctx={0};
    ctx.app=app;
    s_playback=&ctx;
    app->is_capturing=true;
    app->device_count=1;
    app->selected_device=0;
    app->devices[0].type=DEVICE_TYPE_HSDAOH;
    atomic_store(&app->last_callback_time_ms,1);
    watchdog_test_now=3600000;
    atomic_store(&ctx.running,true);
    atomic_store(&ctx.state,PLAYBACK_STATE_PLAYING);
    assert(gui_playback_is_running(app));
    assert(!gui_capture_device_timeout(app,2000));
    assert(atomic_load(&app->last_callback_time_ms)==1);

    gui_playback_pause(app);
    assert(gui_playback_get_state(app)==PLAYBACK_STATE_PAUSED);
    assert(gui_playback_is_running(app));
    assert(!gui_capture_device_timeout(app,2000));

    /* The actual producer leaves running=true and state=PAUSED at Loop Off
     * EOF; setting its position to the endpoint represents that stable state. */
    atomic_store(&ctx.loop_enabled,false);
    atomic_store(&ctx.current_sample,64);
    atomic_store(&ctx.current_sample_a,64);
    atomic_store(&ctx.channel_total_samples[0],64);
    assert(!gui_capture_device_timeout(app,2000));

    /* A stale UI playback selection cannot exempt a real hardware stream. */
    atomic_store(&ctx.running,false);
    app->devices[0].type=DEVICE_TYPE_PLAYBACK;
    assert(gui_capture_device_timeout(app,2000));
    s_playback=&s_idle_playback;
    app->is_capturing=false;
    puts("Playback watchdog: playing, paused, EOF, active identity PASS");
}

int main(void) {
    watchdog_test_hardware();
    watchdog_test_playback();
    return 0;
}
