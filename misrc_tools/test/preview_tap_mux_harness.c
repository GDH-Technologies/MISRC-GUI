/*
 * Unit harness for gui_preview_tap_mux.
 *
 * The mux exists because gui_preview_v4l2.c publishes its tap through a SINGLE
 * atomic slot: installing a second tap silently displaces the first. That makes
 * "start a stream while recording" a silent data-loss bug rather than an error,
 * which is exactly the kind of failure a test has to pin down.
 *
 * The seam is the module boundary. gui_preview_tap_install/remove are the only
 * preview symbols the mux touches, so this harness supplies them and stands in
 * for the capture thread by invoking whatever tap the mux installed.
 */

#include "gui_preview_tap_mux.h"   /* pulls in the tap contract */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- stand-in for gui_preview_v4l2.c's single tap slot ------------------- */

static const preview_tap_t *g_installed;
static int g_install_calls;
static int g_remove_calls;

void gui_preview_tap_install(const preview_tap_t *tap)
{
    g_installed = tap;
    g_install_calls++;
}

void gui_preview_tap_remove(void)
{
    g_installed = NULL;
    g_remove_calls++;
}

/* Stand in for the capture thread delivering one frame. */
static void deliver_frame(const uint8_t *yuyv, uint32_t w, uint32_t h)
{
    if (g_installed && g_installed->fn) {
        g_installed->fn(yuyv, (size_t)w * 2, w, h, g_installed->user);
    }
}

static void reset_preview_stub(void)
{
    g_installed = NULL;
    g_install_calls = 0;
    g_remove_calls = 0;
}

/* ---- consumers ----------------------------------------------------------- */

typedef struct {
    int      frames;
    uint32_t last_w, last_h;
    size_t   last_pitch;
    uint8_t  first_byte;
} consumer_t;

static void consumer_cb(const uint8_t *yuyv, size_t pitch,
                        uint32_t w, uint32_t h, void *user)
{
    consumer_t *c = (consumer_t *)user;
    c->frames++;
    c->last_w = w;
    c->last_h = h;
    c->last_pitch = pitch;
    c->first_byte = yuyv ? yuyv[0] : 0;
}

static int expect_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "ASSERTION FAILED: %s\n", message);
        return 1;
    }
    return 0;
}

static int expect_eq_int(int got, int want, const char *what)
{
    if (got != want) {
        fprintf(stderr, "ASSERTION FAILED: %s: expected=%d got=%d\n", what, want, got);
        return 1;
    }
    return 0;
}

/* ---- tests --------------------------------------------------------------- */

/* The bug this module exists to fix: with the raw preview tap, adding a second
 * consumer displaces the first and it silently stops receiving frames. */
static int test_two_consumers_both_receive_every_frame(void)
{
    reset_preview_stub();

    consumer_t rec = {0}, stream = {0};
    preview_tap_t rec_tap    = { consumer_cb, &rec };
    preview_tap_t stream_tap = { consumer_cb, &stream };

    if (expect_eq_int(gui_preview_mux_add(&rec_tap), 0, "add(recorder)") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_add(&stream_tap), 0, "add(streamer)") != 0) return 1;

    static const uint8_t frame[8] = { 0xAB, 0, 0, 0, 0, 0, 0, 0 };
    deliver_frame(frame, 4, 1);
    deliver_frame(frame, 4, 1);
    deliver_frame(frame, 4, 1);

    if (expect_eq_int(rec.frames, 3, "recorder frame count") != 0) return 1;
    if (expect_eq_int(stream.frames, 3, "streamer frame count") != 0) return 1;

    /* Payload must arrive intact, not merely arrive. */
    if (expect_eq_int((int)stream.last_w, 4, "streamer width") != 0) return 1;
    if (expect_eq_int((int)stream.last_h, 1, "streamer height") != 0) return 1;
    if (expect_true(stream.last_pitch == 8, "streamer pitch") != 0) return 1;
    if (expect_true(stream.first_byte == 0xAB, "streamer payload") != 0) return 1;

    gui_preview_mux_remove(&rec_tap);
    gui_preview_mux_remove(&stream_tap);
    puts("PASS: two consumers both receive every frame");
    return 0;
}

/* The mux must present gui_preview_v4l2.c with exactly the lifecycle it saw
 * when there was one consumer: installed once, removed once. */
static int test_underlying_tap_installed_once_removed_once(void)
{
    reset_preview_stub();

    consumer_t a = {0}, b = {0};
    preview_tap_t ta = { consumer_cb, &a };
    preview_tap_t tb = { consumer_cb, &b };

    gui_preview_mux_add(&ta);
    if (expect_eq_int(g_install_calls, 1, "install after first add") != 0) return 1;

    gui_preview_mux_add(&tb);
    if (expect_eq_int(g_install_calls, 1, "no re-install on second add") != 0) return 1;

    gui_preview_mux_remove(&ta);
    if (expect_eq_int(g_remove_calls, 0, "no remove while a consumer remains") != 0) return 1;

    gui_preview_mux_remove(&tb);
    if (expect_eq_int(g_remove_calls, 1, "remove after last consumer") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_count(), 0, "count after last remove") != 0) return 1;

    puts("PASS: underlying tap installed once, removed once");
    return 0;
}

/* Stopping a recording must not stop the stream. */
static int test_remaining_consumer_keeps_receiving_after_a_removal(void)
{
    reset_preview_stub();

    consumer_t rec = {0}, stream = {0};
    preview_tap_t rec_tap    = { consumer_cb, &rec };
    preview_tap_t stream_tap = { consumer_cb, &stream };
    static const uint8_t frame[8] = { 0x11, 0, 0, 0, 0, 0, 0, 0 };

    gui_preview_mux_add(&rec_tap);
    gui_preview_mux_add(&stream_tap);
    deliver_frame(frame, 4, 1);

    gui_preview_mux_remove(&rec_tap);
    deliver_frame(frame, 4, 1);
    deliver_frame(frame, 4, 1);

    if (expect_eq_int(rec.frames, 1, "removed consumer stopped at 1") != 0) return 1;
    if (expect_eq_int(stream.frames, 3, "remaining consumer kept receiving") != 0) return 1;

    gui_preview_mux_remove(&stream_tap);
    puts("PASS: remaining consumer keeps receiving after a removal");
    return 0;
}

/* A double add must not deliver every frame twice -- for the recorder that
 * would mean a corrupt timeline, and nothing would report it. */
static int test_duplicate_add_does_not_double_deliver(void)
{
    reset_preview_stub();

    consumer_t c = {0};
    preview_tap_t tap = { consumer_cb, &c };

    if (expect_eq_int(gui_preview_mux_add(&tap), 0, "first add") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_add(&tap), 0, "duplicate add is accepted") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_count(), 1, "duplicate add registers once") != 0) return 1;

    static const uint8_t frame[8] = { 0x22, 0, 0, 0, 0, 0, 0, 0 };
    deliver_frame(frame, 4, 1);

    if (expect_eq_int(c.frames, 1, "frames delivered per frame") != 0) return 1;

    /* One remove must fully deregister it. */
    gui_preview_mux_remove(&tap);
    if (expect_eq_int(gui_preview_mux_count(), 0, "count after remove") != 0) return 1;
    deliver_frame(frame, 4, 1);
    if (expect_eq_int(c.frames, 1, "no delivery after remove") != 0) return 1;

    puts("PASS: duplicate add does not double-deliver");
    return 0;
}

static int test_add_beyond_capacity_is_rejected(void)
{
    reset_preview_stub();

    consumer_t c[5] = {{0}};
    preview_tap_t taps[5];
    for (int i = 0; i < 5; i++) {
        taps[i].fn = consumer_cb;
        taps[i].user = &c[i];
    }

    for (int i = 0; i < 4; i++) {
        if (expect_eq_int(gui_preview_mux_add(&taps[i]), 0, "add within capacity") != 0) return 1;
    }
    if (expect_eq_int(gui_preview_mux_add(&taps[4]), -1, "add beyond capacity") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_count(), 4, "count stays at capacity") != 0) return 1;

    for (int i = 0; i < 4; i++) gui_preview_mux_remove(&taps[i]);
    puts("PASS: add beyond capacity is rejected");
    return 0;
}

static int test_rejects_unusable_taps_and_ignores_unknown_removes(void)
{
    reset_preview_stub();

    preview_tap_t no_fn = { NULL, NULL };
    if (expect_eq_int(gui_preview_mux_add(NULL), -1, "add(NULL)") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_add(&no_fn), -1, "add(tap without fn)") != 0) return 1;
    if (expect_eq_int(g_install_calls, 0, "no install for a rejected add") != 0) return 1;

    consumer_t c = {0};
    preview_tap_t never_added = { consumer_cb, &c };
    gui_preview_mux_remove(&never_added);   /* must not crash or unbalance */
    gui_preview_mux_remove(NULL);
    if (expect_eq_int(g_remove_calls, 0, "unknown remove does not touch the tap") != 0) return 1;
    if (expect_eq_int(gui_preview_mux_count(), 0, "count unchanged") != 0) return 1;

    puts("PASS: rejects unusable taps, ignores unknown removes");
    return 0;
}

/* The guarantee that makes removal safe: once gui_preview_mux_remove() returns,
 * the consumer's callback is not running, so the caller may free what `user`
 * points at. Without it, gui_video_record's teardown would be a use-after-free
 * that only shows up under load. */
static atomic_int q_in_callback;
static atomic_int q_finished;

static void slow_consumer_cb(const uint8_t *yuyv, size_t pitch,
                             uint32_t w, uint32_t h, void *user)
{
    (void)yuyv; (void)pitch; (void)w; (void)h; (void)user;
    atomic_store(&q_in_callback, 1);
    struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50ms, far longer than any race window */
    nanosleep(&ts, NULL);
    atomic_store(&q_finished, 1);
    atomic_store(&q_in_callback, 0);
}

static preview_tap_t q_tap = { slow_consumer_cb, NULL };

static void *deliver_thread(void *arg)
{
    (void)arg;
    static const uint8_t frame[8] = { 0x33, 0, 0, 0, 0, 0, 0, 0 };
    deliver_frame(frame, 4, 1);
    return NULL;
}

static int test_remove_waits_for_an_in_flight_callback(void)
{
    reset_preview_stub();
    atomic_store(&q_in_callback, 0);
    atomic_store(&q_finished, 0);

    if (expect_eq_int(gui_preview_mux_add(&q_tap), 0, "add slow consumer") != 0) return 1;

    pthread_t th;
    if (expect_eq_int(pthread_create(&th, NULL, deliver_thread, NULL), 0, "spawn capture thread") != 0) {
        return 1;
    }

    /* Wait until the callback is genuinely running, so remove() has something
     * to wait out rather than winning by arriving late. */
    while (atomic_load(&q_in_callback) == 0) {
        struct timespec ts = { 0, 100 * 1000 };
        nanosleep(&ts, NULL);
    }

    gui_preview_mux_remove(&q_tap);

    /* Read immediately: if remove() returned early this is still 0. */
    int finished_on_return = atomic_load(&q_finished);

    pthread_join(th, NULL);

    if (expect_true(finished_on_return == 1,
                    "remove() returned while the callback was still in flight") != 0) {
        return 1;
    }

    puts("PASS: remove waits for an in-flight callback");
    return 0;
}

int main(void)
{
    if (test_two_consumers_both_receive_every_frame() != 0) return 1;
    if (test_underlying_tap_installed_once_removed_once() != 0) return 1;
    if (test_remaining_consumer_keeps_receiving_after_a_removal() != 0) return 1;
    if (test_duplicate_add_does_not_double_deliver() != 0) return 1;
    if (test_add_beyond_capacity_is_rejected() != 0) return 1;
    if (test_rejects_unusable_taps_and_ignores_unknown_removes() != 0) return 1;
    if (test_remove_waits_for_an_in_flight_callback() != 0) return 1;

    puts("PASS: preview tap mux harness");
    return 0;
}
