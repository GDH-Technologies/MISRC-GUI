/*
 * ringbuffer_mirror_harness.c - proves the real ringbuffer (common/ringbuffer.c)
 * can be created on this host: the anonymous shared-memory object, the
 * double mapping, and the mirror (a byte written at [i] reads back at
 * [size + i]). Every capture path in the GUI and CLI sits on this; a host
 * where rb_init() fails cannot capture, record or ingest anything, and
 * --smoke-test does not reach it.
 *
 * Build (ci_guard_tests.py "ringbuffer mirror runtime"):
 *   cc -std=c11 -Wall -Wextra -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_GNU_SOURCE \
 *      -I misrc_tools/common misrc_tools/test/ringbuffer_mirror_harness.c \
 *      misrc_tools/common/ringbuffer.c -o ringbuffer_mirror_harness && ./ringbuffer_mirror_harness
 */
#include "ringbuffer.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed = 0;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_failed++;
}

static int create_and_verify(size_t size, const char *label) {
    ringbuffer_t rb;
    memset(&rb, 0, sizeof(rb));
    errno = 0;
    int rc = rb_init(&rb, "harness", size);
    int saved = errno;
    if (rc != 0) {
        printf("FAIL: %s: rb_init(%zu bytes) returned %d, errno %d (%s)\n",
               label, size, rc, saved, saved ? strerror(saved) : "-");
        g_failed++;
        return -1;
    }
    check(rb.buffer != NULL && rb.buffer_size == size, label);

    /* The two halves must alias: write low, read high, and the reverse. */
    rb.buffer[0] = 0x5A;
    rb.buffer[size - 1] = 0xA5;
    check(rb.buffer[size] == 0x5A && rb.buffer[2 * size - 1] == 0xA5, "mirror reads back what the first half holds");
    rb.buffer[size + 7] = 0x77;
    check(rb.buffer[7] == 0x77, "first half reads back what the mirror holds");

    /* A write that straddles the seam lands contiguously, which is the whole
     * point of the mirror: reserve near the end, fill across, read via the
     * wrapped offsets. */
    size_t chunk = 4096;
    atomic_store(&rb.head, size - chunk / 2);
    atomic_store(&rb.tail, size - chunk / 2);
    uint8_t *w = (uint8_t *)rb_write_ptr(&rb, chunk);
    check(w != NULL, "write region reserved across the seam");
    if (w) {
        for (size_t i = 0; i < chunk; i++) w[i] = (uint8_t)(i * 7);
        check(rb_write_finished(&rb, chunk) == 0, "write across the seam committed");
        int ok = 1;
        for (size_t i = 0; i < chunk; i++) {
            size_t off = (size - chunk / 2 + i) % size;
            if (rb.buffer[off] != (uint8_t)(i * 7)) { ok = 0; break; }
        }
        check(ok, "bytes written across the seam are readable at their wrapped offsets");
    }
    rb_close(&rb);
    return 0;
}

int main(void) {
    printf("ringbuffer mirror harness (real common/ringbuffer.c on this host)\n");
    create_and_verify((size_t)1 * 1024 * 1024, "1 MiB ring created");
    /* Twice, so a leaked or non-unlinked object cannot make the second one fail. */
    create_and_verify((size_t)1 * 1024 * 1024, "second 1 MiB ring created after the first was closed");
    create_and_verify((size_t)64 * 1024 * 1024, "64 MiB ring created");
    printf("%d check(s) failed\n", g_failed);
    return g_failed;
}
