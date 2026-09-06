/*
 * bufmgr_write_tap_harness.c - proves the buffer manager's producer-side
 * write tap: a function registered with bufmgr_set_write_tap() is called
 * from bufmgr_write_end() on the writer's thread with exactly the region
 * write_begin() handed out, once those bytes are visible to readers. The
 * GUI's network server hangs its /rf and /baseband fan-out on this, so every
 * capture backend streams without knowing the server exists.
 *
 * Build (ci_guard_tests.py "buffer manager write tap runtime"):
 *   cc -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *      -I misrc_tools/common misrc_tools/test/bufmgr_write_tap_harness.c \
 *      misrc_tools/common/buffer_manager.c -o bufmgr_write_tap_harness
 */
#include "buffer_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failed++;
}

/* ----- ringbuffer stubs: a flat heap buffer, no mirroring, no wrap ----- */

int rb_init(ringbuffer_t *rb, char *name, size_t size) {
    (void)name;
    rb->buffer = (uint8_t *)calloc(1, size);
    if (!rb->buffer) return 1;
#ifdef _WIN32
    rb->_buffer2 = NULL;
#endif
    rb->buffer_size = size;
    rb->fd = -1;
    atomic_store(&rb->head, 0);
    atomic_store(&rb->tail, 0);
    return 0;
}

int rb_put(ringbuffer_t *rb, void *data, size_t size) {
    (void)rb;
    (void)data;
    (void)size;
    return 0;
}

void *rb_read_ptr(ringbuffer_t *rb, size_t size) {
    size_t head = atomic_load(&rb->head);
    size_t tail = atomic_load(&rb->tail);
    if (tail - head < size) return NULL;
    return rb->buffer + head;
}

int rb_read_finished(ringbuffer_t *rb, size_t size) {
    rb->head += size;
    return 0;
}

void *rb_write_ptr(ringbuffer_t *rb, size_t size) {
    size_t tail = atomic_load(&rb->tail);
    if (rb->buffer_size - tail < size) return NULL;
    return rb->buffer + tail;
}

int rb_write_finished(ringbuffer_t *rb, size_t size) {
    rb->tail += size;
    return 0;
}

void rb_close(ringbuffer_t *rb) {
    free(rb->buffer);
    rb->buffer = NULL;
}

/* ----- event stubs ----- */

int rb_event_init(rb_event_t *event) {
    event->initialized = true;
    return 0;
}

void rb_event_signal(rb_event_t *event) {
    (void)event;
}

void rb_event_wait(rb_event_t *event) {
    (void)event;
}

bool rb_event_wait_timeout(rb_event_t *event, uint32_t timeout_ms) {
    (void)event;
    (void)timeout_ms;
    return false;
}

void rb_event_destroy(rb_event_t *event) {
    event->initialized = false;
}

/* ----- the tap under test ----- */

typedef struct {
    buffer_manager_t *mgr;
    int calls;
    buffer_id_t last_id;
    const void *last_data;
    size_t last_bytes;
    void *last_ctx;
    size_t fill_at_call;     /* what a reader could see when the tap ran */
    unsigned char first_byte;
} tap_trace_t;

static tap_trace_t g_trace;

static void tap(void *ctx, buffer_id_t id, const void *data, size_t bytes) {
    g_trace.calls++;
    g_trace.last_id = id;
    g_trace.last_data = data;
    g_trace.last_bytes = bytes;
    g_trace.last_ctx = ctx;
    g_trace.fill_at_call = bufmgr_fill_level(g_trace.mgr, id);
    g_trace.first_byte = ((const unsigned char *)data)[0];
}

static void small_configs(buffer_config_t *cfg) {
    static const char *names[BUF_COUNT] = { "capture_rf", "capture_audio", "record_a", "record_b", "display" };
    for (int i = 0; i < BUF_COUNT; i++) {
        cfg[i].name = names[i];
        cfg[i].size = 64 * 1024;
        cfg[i].lazy_init = false;
    }
}

int main(void) {
    buffer_manager_t mgr;
    buffer_config_t cfg[BUF_COUNT];
    int token = 42;
    small_configs(cfg);
    if (bufmgr_init_custom(&mgr, cfg) != 0) {
        fprintf(stderr, "bufmgr_init_custom failed\n");
        return 1;
    }
    memset(&g_trace, 0, sizeof(g_trace));
    g_trace.mgr = &mgr;

    check(bufmgr_get_write_tap(&mgr, BUF_CAPTURE_RF) == NULL, "fresh manager has no tap");

    /* A write_begin/write_end pair reaches the tap with the committed region. */
    bufmgr_set_write_tap(&mgr, BUF_CAPTURE_RF, tap, &token);
    void *p = bufmgr_write_begin(&mgr, BUF_CAPTURE_RF, 100, NULL);
    check(p != NULL, "write_begin(100) hands out a region");
    if (p) memset(p, 0xAB, 100);
    bufmgr_write_end(&mgr, BUF_CAPTURE_RF, 100);
    check(g_trace.calls == 1, "write_end called the tap exactly once");
    check(g_trace.last_id == BUF_CAPTURE_RF, "tap received the buffer id");
    check(g_trace.last_data == p, "tap received the pointer write_begin returned");
    check(g_trace.last_bytes == 100, "tap received the committed byte count");
    check(g_trace.last_ctx == &token, "tap received its context");
    check(g_trace.first_byte == 0xAB, "tap saw the producer's bytes");
    check(g_trace.fill_at_call == 100, "bytes were already visible to readers when the tap ran");

    /* A buffer with no tap installed is untouched. */
    void *a = bufmgr_write_begin(&mgr, BUF_CAPTURE_AUDIO, 50, NULL);
    if (a) bufmgr_write_end(&mgr, BUF_CAPTURE_AUDIO, 50);
    check(a != NULL && g_trace.calls == 1, "a buffer without a tap does not call it");

    /* The convenience writer goes through the same path. */
    unsigned char twenty[20];
    memset(twenty, 0xCD, sizeof(twenty));
    check(bufmgr_write(&mgr, BUF_CAPTURE_RF, twenty, sizeof(twenty)) == 0, "bufmgr_write succeeds");
    check(g_trace.calls == 2 && g_trace.last_bytes == 20 && g_trace.first_byte == 0xCD,
          "bufmgr_write() also reaches the tap");
    check(g_trace.last_data == (const unsigned char *)p + 100, "second region follows the first");

    /* Clearing the tap stops the calls; the getter reports it. */
    check(bufmgr_get_write_tap(&mgr, BUF_CAPTURE_RF) == tap, "getter returns the installed tap");
    bufmgr_set_write_tap(&mgr, BUF_CAPTURE_RF, NULL, NULL);
    check(bufmgr_get_write_tap(&mgr, BUF_CAPTURE_RF) == NULL, "getter returns NULL after clearing");
    void *q = bufmgr_write_begin(&mgr, BUF_CAPTURE_RF, 10, NULL);
    if (q) bufmgr_write_end(&mgr, BUF_CAPTURE_RF, 10);
    check(q != NULL && g_trace.calls == 2, "a cleared tap is not called");

    /* Re-initialising the manager (memory budget change) drops the tap; the
     * server re-installs it from its poll, so this must be observable. */
    bufmgr_set_write_tap(&mgr, BUF_CAPTURE_RF, tap, &token);
    bufmgr_cleanup(&mgr);
    if (bufmgr_init_custom(&mgr, cfg) != 0) {
        fprintf(stderr, "bufmgr_init_custom (second) failed\n");
        return 1;
    }
    check(bufmgr_get_write_tap(&mgr, BUF_CAPTURE_RF) == NULL, "re-init clears the tap");
    bufmgr_cleanup(&mgr);

    printf("%d check(s) failed\n", g_failed);
    return g_failed;
}
