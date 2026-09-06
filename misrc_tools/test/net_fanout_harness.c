/*
 * net_fanout_harness.c - drives misrc_gui/net/gui_net_fanout.c the way the
 * /rf and /baseband handlers do (one producer, subscribers reading into a
 * 64 KB buffer with a timeout) and measures what comes out: bytes produced vs
 * delivered vs dropped, ordering, wake-up latency, memory held, and the
 * shutdown hand-off.
 *
 * Build and run (ci_guard_tests.py "net fanout runtime" does exactly this):
 *
 *   cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
 *      -pthread -I misrc_tools/misrc_gui/net \
 *      misrc_tools/test/net_fanout_harness.c misrc_tools/misrc_gui/net/gui_net_fanout.c \
 *      -o net_fanout_harness && ./net_fanout_harness
 *
 * Exit status is the number of failed checks.
 */
#include "gui_net_fanout.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define KIB ((size_t)1024)
#define MIB ((size_t)1024 * 1024)

static int g_failed = 0;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void report(bool ok, const char *name, const char *fmt, ...) {
    va_list ap;
    printf("%s: %s - ", ok ? "PASS" : "FAIL", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) g_failed++;
}

/* Push one chunk whose every byte is `tag`. */
static void push_tagged(net_fanout_t *f, uint8_t tag, size_t len) {
    uint8_t *tmp = (uint8_t *)malloc(len);
    memset(tmp, tag, len);
    net_fanout_push(f, tmp, len);
    free(tmp);
}

/* Read until nothing more arrives within idle_ms (or cap is reached).
 * Returns the bytes collected. */
static size_t drain(net_fanout_sub_t *s, uint8_t *dst, size_t cap, int idle_ms) {
    size_t total = 0;
    while (total < cap) {
        int n = net_fanout_read(s, dst + total, cap - total, idle_ms);
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total;
}

/* True when dst holds chunks tagged first, first+1, ... in order, `chunk` bytes each. */
static bool tags_in_order(const uint8_t *dst, size_t len, size_t chunk, uint8_t first) {
    for (size_t i = 0; i < len; i++) {
        if (dst[i] != (uint8_t)(first + i / chunk)) return false;
    }
    return true;
}

typedef struct {
    net_fanout_sub_t *s;
    uint8_t *buf;
    size_t cap;
    int timeout_ms;
    int n;
    uint64_t dt_ms;
} single_read_t;

static void *single_read_main(void *arg) {
    single_read_t *r = (single_read_t *)arg;
    uint64_t t0 = now_ms();
    r->n = net_fanout_read(r->s, r->buf, r->cap, r->timeout_ms);
    r->dt_ms = now_ms() - t0;
    return NULL;
}

typedef struct {
    net_fanout_sub_t *s;
    uint8_t *buf;
    size_t cap;
    int idle_ms;
    size_t total;
} drain_thread_t;

static void *drain_main(void *arg) {
    drain_thread_t *d = (drain_thread_t *)arg;
    d->total = drain(d->s, d->buf, d->cap, d->idle_ms);
    return NULL;
}

/* ------------------------------------------------------------------------ */

/* Five chunks queued, then read: each must arrive once, in order. */
static void test_ordered_delivery_no_replay(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t buf[4096];
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &s);
    for (uint8_t k = 1; k <= 5; k++) push_tagged(&f, k, 16);
    size_t total = drain(&s, buf, sizeof(buf), 0);
    bool ordered = (total == 80) && tags_in_order(buf, total, 16, 1);
    report(ordered, __func__, "delivered %zu of 80 queued bytes%s",
           total, (total == 80 && !ordered) ? " (out of order or replayed)" : "");
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
}

/* Data queued before the read is returned without waiting for the timeout. */
static void test_queued_data_read_without_waiting(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t buf[64 * KIB];
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &s);
    for (uint8_t k = 1; k <= 3; k++) push_tagged(&f, k, 16);
    uint64_t t0 = now_ms();
    int n = net_fanout_read(&s, buf, sizeof(buf), 1000);
    uint64_t dt = now_ms() - t0;
    report(n == 48 && dt < 500, __func__,
           "read(timeout 1000 ms) with 48 bytes queued returned %d bytes after %" PRIu64 " ms",
           n, dt);
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
}

/* A reader blocked on an empty fanout returns as soon as one push lands,
 * not when its buffer is full or its timeout expires. */
static void test_push_wakes_blocked_reader_promptly(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t buf[64 * KIB];
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &s);
    single_read_t r = { &s, buf, sizeof(buf), 2000, 0, 0 };
    pthread_t t;
    pthread_create(&t, NULL, single_read_main, &r);
    sleep_ms(50);
    push_tagged(&f, 7, 16);
    pthread_join(t, NULL);
    report(r.n == 16 && r.dt_ms < 1000, __func__,
           "blocked read(timeout 2000 ms) returned %d bytes %" PRIu64 " ms after start (push at 50 ms)",
           r.n, r.dt_ms);
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
}

/* A reader that waits between pushes must not see earlier chunks again. */
static void test_no_replay_when_reader_waits_between_pushes(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t buf[4096];
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &s);
    drain_thread_t d = { &s, buf, sizeof(buf), 300, 0 };
    pthread_t t;
    pthread_create(&t, NULL, drain_main, &d);
    sleep_ms(20);
    for (uint8_t k = 1; k <= 5; k++) {
        push_tagged(&f, k, 16);
        sleep_ms(30);
    }
    pthread_join(t, NULL);
    bool ordered = (d.total == 80) && tags_in_order(buf, d.total, 16, 1);
    report(ordered, __func__, "5 x 16 bytes pushed 30 ms apart, reader collected %zu bytes%s",
           d.total, (d.total > 80) ? " (replayed)" : (ordered ? "" : " (out of order)"));
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
}

/* A subscriber that keeps up leaves nothing behind in the queue. */
static void test_keeping_up_consumer_holds_no_memory(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t *out = (uint8_t *)malloc(64 * KIB);
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &s);
    size_t got = 0;
    for (int i = 0; i < 1000; i++) {
        push_tagged(&f, (uint8_t)i, 64 * KIB);
        got += drain(&s, out, 64 * KIB, 0);
    }
    size_t chunks = 0, bytes = 0;
    uint64_t delivered = 0, dropped = 0;
    net_fanout_stats(&f, &chunks, &bytes, NULL);
    net_fanout_sub_stats(&s, &delivered, &dropped, NULL);
    report(got == 1000 * 64 * KIB && delivered == 1000 * 64 * KIB && dropped == 0 && chunks == 0, __func__,
           "1000 x 64 KiB pushed and read back: delivered %" PRIu64 " dropped %" PRIu64
           ", queue still holds %zu chunk(s) / %zu bytes (want 0)",
           delivered, dropped, chunks, bytes);
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
    free(out);
}

/* A subscriber that never reads is bounded by max_bytes: the oldest data is
 * dropped, the newest is kept contiguous, and delivered + dropped == pushed. */
static void test_slow_consumer_drops_oldest_with_exact_accounting(void) {
    const size_t chunk = 16 * KIB;
    const uint32_t count = 256;             /* 4 MiB pushed into a 1 MiB budget */
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t *tmp = (uint8_t *)malloc(chunk);
    uint8_t *out = (uint8_t *)malloc(count * chunk);
    net_fanout_init(&f, 1 * MIB);
    net_fanout_subscribe(&f, &s);
    for (uint32_t k = 0; k < count; k++) {
        memset(tmp, (int)(k & 0xFF), chunk);
        memcpy(tmp, &k, sizeof(k));
        net_fanout_push(&f, tmp, chunk);
    }
    size_t held_chunks = 0, held_bytes = 0;
    net_fanout_stats(&f, &held_chunks, &held_bytes, NULL);
    size_t total = drain(&s, out, count * chunk, 0);
    uint64_t delivered = 0, dropped = 0;
    net_fanout_sub_stats(&s, &delivered, &dropped, NULL);

    bool contiguous = (total % chunk) == 0 && total > 0;
    uint32_t first = 0, last = 0;
    for (size_t off = 0; contiguous && off < total; off += chunk) {
        uint32_t idx;
        memcpy(&idx, out + off, sizeof(idx));
        if (off == 0) first = idx;
        else if (idx != last + 1) contiguous = false;
        last = idx;
    }
    bool ok = held_bytes <= 1 * MIB && contiguous && last == count - 1 &&
              delivered == 1 * MIB && delivered + dropped == (uint64_t)count * chunk;
    report(ok, __func__,
           "4 MiB pushed into a 1 MiB budget with no reads: queue held %zu bytes, then delivered %"
           PRIu64 " + dropped %" PRIu64 " = %" PRIu64 " (want 4 MiB), chunks %" PRIu32 "..%" PRIu32 "%s",
           held_bytes, delivered, dropped, delivered + dropped, first, last,
           contiguous ? " contiguous" : " NOT contiguous");
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
    free(tmp);
    free(out);
}

/* Two subscribers each see the whole stream; one leaving does not disturb the other. */
static void test_two_subscribers_independent(void) {
    net_fanout_t f;
    net_fanout_sub_t a, b;
    uint8_t ba[4096], bb[4096];
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &a);
    net_fanout_subscribe(&f, &b);
    for (uint8_t k = 1; k <= 3; k++) push_tagged(&f, k, 16);
    size_t na = drain(&a, ba, sizeof(ba), 0);
    size_t nb = drain(&b, bb, sizeof(bb), 0);
    size_t held_after_both = 0;
    net_fanout_stats(&f, &held_after_both, NULL, NULL);
    net_fanout_unsubscribe(&a);
    for (uint8_t k = 4; k <= 5; k++) push_tagged(&f, k, 16);
    size_t nb2 = drain(&b, bb + nb, sizeof(bb) - nb, 0);
    size_t held_end = 0;
    net_fanout_stats(&f, &held_end, NULL, NULL);
    bool ok = na == 48 && nb == 48 && memcmp(ba, bb, 48) == 0 && tags_in_order(ba, 48, 16, 1) &&
              held_after_both == 0 && nb2 == 32 && tags_in_order(bb, 80, 16, 1) && held_end == 0;
    report(ok, __func__,
           "A got %zu, B got %zu of 48 (identical: %s); queue after both read: %zu chunk(s); "
           "after A left, B got %zu of 32 more; queue at end: %zu chunk(s)",
           na, nb, (na == nb && memcmp(ba, bb, na) == 0) ? "yes" : "no",
           held_after_both, nb2, held_end);
    net_fanout_unsubscribe(&b);
    net_fanout_destroy(&f);
}

/* No subscribers: pushes allocate nothing. A late subscriber sees only what
 * is pushed after it joined. */
static void test_subscriber_starts_at_live_edge(void) {
    net_fanout_t f;
    net_fanout_sub_t a, b;
    uint8_t ba[4096], bb[4096];
    net_fanout_init(&f, 0);
    for (uint8_t k = 1; k <= 3; k++) push_tagged(&f, k, 16);
    size_t held_no_subs = 0;
    net_fanout_stats(&f, &held_no_subs, NULL, NULL);
    net_fanout_subscribe(&f, &a);
    push_tagged(&f, 4, 16);
    push_tagged(&f, 5, 16);
    net_fanout_subscribe(&f, &b);
    push_tagged(&f, 6, 16);
    size_t nb = drain(&b, bb, sizeof(bb), 0);
    size_t na = drain(&a, ba, sizeof(ba), 0);
    bool ok = held_no_subs == 0 && nb == 16 && bb[0] == 6 && na == 48 && tags_in_order(ba, 48, 16, 4);
    report(ok, __func__,
           "queue after 3 pushes with no subscriber: %zu chunk(s) (want 0); late subscriber got %zu "
           "bytes (want 16, tag 6, got tag %u); early subscriber got %zu (want 48)",
           held_no_subs, nb, nb ? bb[0] : 0u, na);
    net_fanout_unsubscribe(&a);
    net_fanout_unsubscribe(&b);
    net_fanout_destroy(&f);
}

/* shutdown() wakes a blocked reader and reads report EOF (-1) from then on,
 * so the /rf handler loop exits instead of spinning on 0-byte timeouts. */
static void test_shutdown_wakes_blocked_reader_with_eof(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    uint8_t buf[64 * KIB];
    net_fanout_init(&f, 0);
    net_fanout_subscribe(&f, &s);
    single_read_t r = { &s, buf, sizeof(buf), 5000, 0, 0 };
    pthread_t t;
    pthread_create(&t, NULL, single_read_main, &r);
    sleep_ms(50);
    net_fanout_shutdown(&f);
    pthread_join(t, NULL);
    int again = net_fanout_read(&s, buf, sizeof(buf), 1000);
    report(r.n == -1 && r.dt_ms < 1000 && again == -1, __func__,
           "blocked read(timeout 5000 ms) returned %d after %" PRIu64 " ms (shutdown at 50 ms; want -1), "
           "next read returned %d (want -1)",
           r.n, r.dt_ms, again);
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
}

/* Producer/consumer stress: random chunk sizes at full speed through a 4 MiB
 * budget. Every delivered byte must match the stream at its absolute offset,
 * and delivered + dropped must equal pushed. */
static uint8_t pattern(uint64_t pos) {
    return (uint8_t)((pos * 31u) ^ (pos >> 8));
}

typedef struct {
    net_fanout_t *f;
    uint64_t pushed;
    int chunks;
    atomic_int done;
} producer_t;

static void *producer_main(void *arg) {
    producer_t *p = (producer_t *)arg;
    uint8_t *buf = (uint8_t *)malloc(48 * KIB);
    uint32_t lcg = 12345;
    uint64_t pos = 0;
    for (int i = 0; i < p->chunks; i++) {
        lcg = lcg * 1103515245u + 12345u;
        size_t len = 1 + (size_t)((lcg >> 8) % (48 * KIB));
        for (size_t j = 0; j < len; j++) buf[j] = pattern(pos + j);
        net_fanout_push(p->f, buf, len);
        pos += len;
    }
    p->pushed = pos;
    atomic_store(&p->done, 1);
    free(buf);
    return NULL;
}

typedef struct {
    net_fanout_sub_t *s;
    producer_t *p;
    uint64_t delivered;
    uint64_t mismatched;
    int reads;
} consumer_t;

static void *consumer_main(void *arg) {
    consumer_t *c = (consumer_t *)arg;
    uint8_t *buf = (uint8_t *)malloc(64 * KIB);
    bool finishing = false;
    for (;;) {
        int n = net_fanout_read(c->s, buf, 64 * KIB, finishing ? 0 : 100);
        if (n < 0) break;
        if (n == 0) {
            if (finishing) break;
            if (atomic_load(&c->p->done)) finishing = true;
            continue;
        }
        uint64_t pos = 0;
        net_fanout_sub_stats(c->s, NULL, NULL, &pos);
        uint64_t start = pos - (uint64_t)n;
        for (int j = 0; j < n; j++) {
            if (buf[j] != pattern(start + (uint64_t)j)) c->mismatched++;
        }
        c->delivered += (uint64_t)n;
        c->reads++;
    }
    free(buf);
    return NULL;
}

static void test_stress_producer_consumer(void) {
    net_fanout_t f;
    net_fanout_sub_t s;
    net_fanout_init(&f, 4 * MIB);
    net_fanout_subscribe(&f, &s);
    producer_t p = { &f, 0, 3000, 0 };
    consumer_t c = { &s, &p, 0, 0, 0 };
    pthread_t tp, tc;
    uint64_t t0 = now_ms();
    pthread_create(&tc, NULL, consumer_main, &c);
    pthread_create(&tp, NULL, producer_main, &p);
    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    uint64_t dt = now_ms() - t0;
    uint64_t delivered = 0, dropped = 0;
    net_fanout_sub_stats(&s, &delivered, &dropped, NULL);
    size_t held = 0;
    net_fanout_stats(&f, &held, NULL, NULL);
    bool ok = c.mismatched == 0 && delivered == c.delivered && delivered + dropped == p.pushed &&
              delivered > 0 && held == 0;
    report(ok, __func__,
           "%d chunks / %" PRIu64 " bytes pushed in %" PRIu64 " ms: delivered %" PRIu64 " + dropped %"
           PRIu64 " = %" PRIu64 " (%s), %" PRIu64 " mismatched byte(s), %d reads, queue holds %zu chunk(s)",
           p.chunks, p.pushed, dt, delivered, dropped, delivered + dropped,
           (delivered + dropped == p.pushed) ? "matches" : "MISMATCH", c.mismatched, c.reads, held);
    net_fanout_unsubscribe(&s);
    net_fanout_destroy(&f);
}

int main(void) {
    printf("net_fanout harness: gui_net_fanout.c driven the way the /rf handler drives it\n");
    test_ordered_delivery_no_replay();
    test_queued_data_read_without_waiting();
    test_push_wakes_blocked_reader_promptly();
    test_no_replay_when_reader_waits_between_pushes();
    test_keeping_up_consumer_holds_no_memory();
    test_slow_consumer_drops_oldest_with_exact_accounting();
    test_two_subscribers_independent();
    test_subscriber_starts_at_live_edge();
    test_shutdown_wakes_blocked_reader_with_eof();
    test_stress_producer_consumer();
    printf("%d check(s) failed\n", g_failed);
    return g_failed > 255 ? 255 : g_failed;
}
