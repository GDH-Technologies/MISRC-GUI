#include "gui_preview_tap_mux.h"

#include <stdatomic.h>
#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
/* Yield. Windows never has an in-flight tap -- gui_preview_v4l2.c stubs the
 * whole reader off Linux -- so this loop exits on its first test. */
static void mux_backoff(void) { Sleep(0); }
#else
#include <time.h>
/* 200us, the same backoff gui_preview_tap_remove() uses to wait out a callback. */
static void mux_backoff(void)
{
    struct timespec ts = { 0, 200 * 1000 };
    nanosleep(&ts, NULL);
}
#endif

/* Four is generous: the recorder and the stream are the only consumers, and a
 * fixed array keeps the dispatch loop allocation-free on the capture thread. */
#define MUX_MAX_CONSUMERS 4

static _Atomic(const preview_tap_t *) g_slots[MUX_MAX_CONSUMERS];
static atomic_int g_inflight[MUX_MAX_CONSUMERS];
static atomic_int g_count;

/* Runs on the capture thread, between VIDIOC_DQBUF and VIDIOC_QBUF. */
static void mux_dispatch(const uint8_t *yuyv, size_t pitch,
                         uint32_t w, uint32_t h, void *user)
{
    (void)user;
    for (int i = 0; i < MUX_MAX_CONSUMERS; i++) {
        if (atomic_load_explicit(&g_slots[i], memory_order_acquire) == NULL) {
            continue;
        }
        atomic_fetch_add_explicit(&g_inflight[i], 1, memory_order_acquire);
        /* Re-load inside the counter: add/remove is not atomic with it, so the
         * slot may have been cleared between the test above and here. This is
         * the same guard gui_preview_v4l2.c applies to its single slot. */
        const preview_tap_t *t = atomic_load_explicit(&g_slots[i], memory_order_acquire);
        if (t && t->fn) {
            t->fn(yuyv, pitch, w, h, t->user);
        }
        atomic_fetch_sub_explicit(&g_inflight[i], 1, memory_order_release);
    }
}

static const preview_tap_t g_mux_tap = { mux_dispatch, NULL };

int gui_preview_mux_add(const preview_tap_t *tap)
{
    if (tap == NULL || tap->fn == NULL) {
        return -1;
    }
    /* Idempotent. Registering the same tap twice would deliver every frame
     * twice -- for the recorder that is a corrupt timeline, and nothing would
     * report it. Add and remove are render-thread only, so this scan does not
     * race a concurrent add. */
    for (int i = 0; i < MUX_MAX_CONSUMERS; i++) {
        if (atomic_load_explicit(&g_slots[i], memory_order_acquire) == tap) {
            return 0;
        }
    }
    for (int i = 0; i < MUX_MAX_CONSUMERS; i++) {
        const preview_tap_t *empty = NULL;
        if (!atomic_compare_exchange_strong_explicit(&g_slots[i], &empty, tap,
                                                     memory_order_release,
                                                     memory_order_relaxed)) {
            continue;
        }
        /* Publish the slot before the underlying tap, so no frame can arrive
         * for a consumer that is not yet visible to the dispatch loop. */
        if (atomic_fetch_add_explicit(&g_count, 1, memory_order_acq_rel) == 0) {
            gui_preview_tap_install(&g_mux_tap);
        }
        return 0;
    }
    return -1;
}

void gui_preview_mux_remove(const preview_tap_t *tap)
{
    if (tap == NULL) {
        return;
    }
    for (int i = 0; i < MUX_MAX_CONSUMERS; i++) {
        if (atomic_load_explicit(&g_slots[i], memory_order_acquire) != tap) {
            continue;
        }
        atomic_store_explicit(&g_slots[i], NULL, memory_order_release);
        /* Wait this slot out, so the caller may free what `user` points at. */
        while (atomic_load_explicit(&g_inflight[i], memory_order_acquire) != 0) {
            mux_backoff();
        }
        if (atomic_fetch_sub_explicit(&g_count, 1, memory_order_acq_rel) == 1) {
            gui_preview_tap_remove();
        }
        return;
    }
}

int gui_preview_mux_count(void)
{
    return atomic_load_explicit(&g_count, memory_order_acquire);
}
