/*
 * Unit harness for the ordered record FIFO (gui_record_fifo.c), the RAW
 * hold-and-retry sink (gui_record_sink.c) and the ringbuffer counters.
 *
 * Everything runs on the REAL buffer manager + ringbuffer with small record
 * rings and a REAL spill file in a private temp directory. The stream is a
 * run of little-endian 64-bit words whose value is their own index, so the
 * reader verifies every byte: a gap, a repeat or a moved block is caught
 * exactly. Dropped pushes are recorded by the producer and must appear as
 * exactly those gaps and no others.
 *
 *   1. ring counters: a producer and a reader hammer a 64 KiB ring across
 *      ~10^4 wraps; every commit must succeed and the stream stay in order.
 *      A stress test of the grow-only counters, not a reproduction of the
 *      old rebase race: that needed the producer preempted between its two
 *      counter loads, which a loop like this almost never hits.
 *   2. random pressure: random push sizes, a bursty reader with random chunk
 *      caps; the output equals the input, through many ring/spill segments,
 *      with hole punching on and with recycle-only space reuse
 *   3. stop mid-episode: a 5.3 MiB tail behind a stopped reader drains in
 *      full after close; then 200 stops at random moments through the gate
 *   4. recycle + truncate stress (punch off): the drained spill is reused
 *      and truncated many times, stream intact
 *   5. segment log full (capacity 4): pushes are accepted or reported
 *      dropped, never reordered
 *   6. spill write failure: a failing spill write drops (or goes to the
 *      ring when it has room) and logs an ERROR; accepted bytes stay ordered
 *   7. sink: a failing output holds the unwritten remainder and retries it;
 *      after recovery the file is exact, with no duplicated bytes
 *   8. detector self-check: the OLD per-block spill algorithm replayed under
 *      the same pressure MUST be caught reordering, proving the verifier sees
 *      the bug this FIFO fixes
 *
 * Registered as the meson test 'gui_record_fifo' and compiled+run by
 * ci_guard_tests.py (check_record_fifo_runtime). Optional argument: seed.
 */

#include "../misrc_gui/output/gui_record_fifo.h"
#include "../misrc_gui/output/gui_record_sink.h"
#include "../common/threading.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(_WIN64)
#include <dirent.h>
#include <unistd.h>
#endif

static int g_checks;
static int g_failures;

static void expect_true(bool condition, const char *message) {
    g_checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        g_failures++;
    }
}

/* ---- deterministic RNG (per thread) ------------------------------------ */

typedef struct { uint64_t s; } rng_t;

static uint64_t rng_next(rng_t *r) {
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

static uint64_t rng_range(rng_t *r, uint64_t lo, uint64_t hi) {
    return lo + rng_next(r) % (hi - lo + 1);
}

static uint64_t g_seed = 0x9E3779B97F4A7C15ull;

/* ---- buffer manager with small record rings ---------------------------- */

static buffer_manager_t g_mgr;
static bool g_mgr_up;

static bool init_mgr(size_t record_size) {
    if (g_mgr_up) {
        bufmgr_cleanup(&g_mgr);
        g_mgr_up = false;
    }
    buffer_config_t cfg[BUF_COUNT];
    memset(&cfg, 0, sizeof(cfg));
    static const char *const names[BUF_COUNT] = {
        "capture_rf", "capture_audio", "record_a", "record_b", "display"
    };
    size_t sizes[BUF_COUNT] = {
        1024 * 1024, 256 * 1024, record_size, record_size, 256 * 1024
    };
    for (int i = 0; i < BUF_COUNT; i++) {
        cfg[i].name = names[i];
        cfg[i].size = sizes[i];
        cfg[i].lazy_init = true;
    }
    if (bufmgr_init_custom(&g_mgr, cfg) != 0) return false;
    g_mgr_up = true;
    return bufmgr_ensure_init(&g_mgr, BUF_RECORD_A) == 0;
}

/* ---- scratch directory --------------------------------------------------- */

static char g_dir[512];

static bool make_dir(void) {
#if defined(_WIN32) || defined(_WIN64)
    const char *tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(g_dir, sizeof(g_dir), "%s", tmp);
    return true;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/misrc_fifo_harness_XXXXXX", tmp);
    return mkdtemp(g_dir) != NULL;
#endif
}

static void remove_dir(void) {
#if !defined(_WIN32) && !defined(_WIN64)
    /* The spill files are unlinked at creation; only the dir remains. */
    DIR *d = opendir(g_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", g_dir, e->d_name);
            remove(p);
        }
        closedir(d);
    }
    rmdir(g_dir);
#endif
}

/* ---- logging sink ---------------------------------------------------------- */

static atomic_int g_log_errors;
static atomic_int g_log_warns;
static atomic_int g_log_lines;
static bool g_log_echo;

static void test_log(void *user, int channel, const char *level, const char *message) {
    (void)user;
    (void)channel;
    atomic_fetch_add(&g_log_lines, 1);
    if (strcmp(level, "ERROR") == 0) atomic_fetch_add(&g_log_errors, 1);
    if (strcmp(level, "WARN") == 0) atomic_fetch_add(&g_log_warns, 1);
    if (g_log_echo) fprintf(stderr, "  [%s] %s\n", level, message);
}

static void reset_log(void) {
    atomic_store(&g_log_errors, 0);
    atomic_store(&g_log_warns, 0);
    atomic_store(&g_log_lines, 0);
}

/* ---- sequence stream ------------------------------------------------------- */

/* Little-endian words whose value is their index. */
static void fill_words(uint8_t *dst, uint64_t first_byte, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint64_t pos = first_byte + i;
        uint64_t word = pos / 8;
        dst[i] = (uint8_t)(word >> (8 * (pos % 8)));
    }
}

#define MAX_DROPS 65536

typedef struct {
    /* Producer-recorded drops: stream byte ranges that will be missing. */
    uint64_t drop_start[MAX_DROPS];
    uint64_t drop_len[MAX_DROPS];
    atomic_uint drop_count;
    /* Reader state. */
    uint64_t expect_byte;   /* next stream byte expected */
    uint32_t drop_cursor;
    uint64_t bytes_ok;
    uint64_t mismatches;
    uint64_t first_bad;
    uint64_t reorders;      /* detector: a word lower than expected */
} verifier_t;

static void verifier_init(verifier_t *v) {
    memset(v, 0, sizeof(*v));
    v->first_bad = UINT64_MAX;
}

static void verifier_record_drop(verifier_t *v, uint64_t start, uint64_t len) {
    unsigned n = atomic_load_explicit(&v->drop_count, memory_order_relaxed);
    if (n >= MAX_DROPS) return;
    v->drop_start[n] = start;
    v->drop_len[n] = len;
    atomic_store_explicit(&v->drop_count, n + 1, memory_order_release);
}

/* Feed received bytes; bytes skipped by recorded drops are allowed gaps. */
static void verifier_feed(verifier_t *v, const uint8_t *p, size_t len) {
    unsigned drops = atomic_load_explicit(&v->drop_count, memory_order_acquire);
    for (size_t i = 0; i < len; i++) {
        while (v->drop_cursor < drops && v->drop_start[v->drop_cursor] == v->expect_byte) {
            v->expect_byte += v->drop_len[v->drop_cursor];
            v->drop_cursor++;
        }
        uint64_t pos = v->expect_byte;
        if (pos % 8 == 0 && i + 8 <= len) {
            /* Whole word: compare it as a word, and on a mismatch resync to
             * the word actually received, so a moved block shows up as a
             * forward jump followed by a backward one (a reorder). */
            uint64_t w = 0;
            for (int k = 7; k >= 0; k--) w = (w << 8) | p[i + (size_t)k];
            if (w == pos / 8) {
                v->bytes_ok += 8;
                v->expect_byte += 8;
            } else {
                if (v->first_bad == UINT64_MAX) v->first_bad = pos;
                v->mismatches += 8;
                if (w < pos / 8) v->reorders++;
                v->expect_byte = w * 8 + 8;
            }
            i += 7;
            continue;
        }
        uint8_t want = (uint8_t)((pos / 8) >> (8 * (pos % 8)));
        if (p[i] != want) {
            if (v->first_bad == UINT64_MAX) v->first_bad = pos;
            v->mismatches++;
        } else {
            v->bytes_ok++;
        }
        v->expect_byte++;
    }
}

/* ---- 1. ring counters across many wraps ---------------------------------- */

typedef struct {
    atomic_bool stop;
    atomic_uint_fast64_t commit_failures;
    uint64_t produced;
    uint64_t seed;
} ring_prod_t;

static int ring_producer(void *arg) {
    ring_prod_t *p = (ring_prod_t *)arg;
    rng_t r = { p->seed | 1 };
    uint8_t buf[4096];
    while (!atomic_load(&p->stop)) {
        size_t len = (size_t)rng_range(&r, 1, sizeof(buf));
        void *dst = bufmgr_try_write_begin(&g_mgr, BUF_RECORD_A, len);
        if (!dst) continue;
        fill_words(buf, p->produced, len);
        memcpy(dst, buf, len);
        if (bufmgr_write_commit(&g_mgr, BUF_RECORD_A, len)) {
            p->produced += len;
        } else {
            atomic_fetch_add(&p->commit_failures, 1);
        }
    }
    return 0;
}

static void test_ring_counters(void) {
    expect_true(init_mgr(64 * 1024), "ring counters: 64 KiB record ring");
    ring_prod_t prod;
    memset(&prod, 0, sizeof(prod));
    prod.seed = g_seed ^ 0x1111;
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    thrd_t t;
    expect_true(thrd_create(&t, ring_producer, &prod) == thrd_success, "ring counters: producer thread");
    rng_t r = { g_seed ^ 0x2222 };
    uint64_t consumed = 0;
    uint64_t t_end = get_time_ms() + 1500;
    while (get_time_ms() < t_end) {
        size_t fill = bufmgr_fill_level(&g_mgr, BUF_RECORD_A);
        if (fill == 0) continue;
        size_t want = (size_t)rng_range(&r, 1, fill);
        void *p = bufmgr_read_begin(&g_mgr, BUF_RECORD_A, want, 0);
        if (!p) continue;
        verifier_feed(v, (const uint8_t *)p, want);
        bufmgr_read_end(&g_mgr, BUF_RECORD_A, want);
        consumed += want;
    }
    atomic_store(&prod.stop, true);
    thrd_join(t, NULL);
    uint64_t wraps = consumed / (64 * 1024);
    printf("  ring counters: %llu bytes through, %llu wraps, %llu commit failures\n",
           (unsigned long long)consumed, (unsigned long long)wraps,
           (unsigned long long)atomic_load(&prod.commit_failures));
    expect_true(wraps > 1000, "ring counters: exercised over a thousand wraps");
    expect_true(atomic_load(&prod.commit_failures) == 0, "ring counters: no commit ever fails");
    expect_true(v->mismatches == 0, "ring counters: stream in order across wraps");
    free(v);
}

/* ---- FIFO test rig ---------------------------------------------------------- */

typedef struct {
    gui_record_fifo_t *f;
    gui_record_gate_t *gate;    /* NULL = no gate */
    verifier_t *v;
    uint64_t seed;
    size_t min_push, max_push;
    uint64_t duration_ms;       /* 0 = push target_bytes then stop */
    uint64_t target_bytes;
    uint64_t rate_bps;          /* pace to this many bytes/s; 0 = flat out */
    atomic_bool stop;
    /* results */
    uint64_t pushed;            /* stream bytes offered (accepted + dropped) */
    uint64_t accepted;
    uint64_t dropped;
    uint64_t ring_pushes, spill_pushes, drop_pushes;
} fifo_prod_t;

static int fifo_producer(void *arg) {
    fifo_prod_t *p = (fifo_prod_t *)arg;
    rng_t r = { p->seed | 1 };
    size_t cap = p->max_push;
    uint8_t *buf = (uint8_t *)malloc(cap);
    uint64_t t0 = get_time_ms();
    uint64_t t_end = p->duration_ms ? t0 + p->duration_ms : UINT64_MAX;
    while (!atomic_load(&p->stop) && get_time_ms() < t_end) {
        if (p->target_bytes && p->pushed >= p->target_bytes) break;
        if (p->rate_bps) {
            /* A capture is a steady source: stay on the schedule. */
            uint64_t due_ms = t0 + (p->pushed * 1000) / p->rate_bps;
            if (get_time_ms() < due_ms) {
                thrd_sleep_ms(1);
                continue;
            }
        }
        size_t len = (size_t)rng_range(&r, p->min_push / 8, p->max_push / 8) * 8;
        if (p->target_bytes && p->pushed + len > p->target_bytes) {
            len = (size_t)(p->target_bytes - p->pushed);
        }
        if (len == 0) break;
        bool gated_on = true;
        if (p->gate) gated_on = gui_record_gate_enter(p->gate);
        if (gated_on) {
            fill_words(buf, p->pushed, len);
            gui_record_push_result_t res = gui_record_fifo_push(p->f, buf, len, 0);
            if (res == GUI_RECORD_PUSH_DROPPED) {
                verifier_record_drop(p->v, p->pushed, len);
                p->dropped += len;
                p->drop_pushes++;
            } else {
                p->accepted += len;
                if (res == GUI_RECORD_PUSH_RING) p->ring_pushes++;
                if (res == GUI_RECORD_PUSH_SPILL) p->spill_pushes++;
            }
            p->pushed += len;
        }
        if (p->gate) {
            gui_record_gate_leave(p->gate);
            if (!gated_on) break;
        }
    }
    free(buf);
    return 0;
}

typedef struct {
    gui_record_fifo_t *f;
    verifier_t *v;
    uint64_t seed;
    bool bursty;            /* random pauses so the ring overflows repeatedly */
    atomic_bool paused;     /* externally held */
    uint64_t received;
    int end_rc;
} fifo_reader_t;

static int fifo_reader(void *arg) {
    fifo_reader_t *rd = (fifo_reader_t *)arg;
    rng_t r = { rd->seed | 1 };
    uint64_t next_pause = get_time_ms() + rng_range(&r, 5, 40);
    for (;;) {
        if (atomic_load(&rd->paused)) {
            thrd_sleep_ms(1);
            continue;
        }
        if (rd->bursty && get_time_ms() >= next_pause) {
            thrd_sleep_ms((int)rng_range(&r, 5, 60));
            next_pause = get_time_ms() + rng_range(&r, 5, 40);
        }
        size_t cap = (size_t)rng_range(&r, 1, 512 * 1024 / 2) * 2;
        gui_record_fifo_chunk_t c;
        int rc = gui_record_fifo_next(rd->f, cap, 10, &c);
        if (rc < 0) {
            rd->end_rc = rc;
            return 0;
        }
        if (rc == 0) continue;
        /* Sometimes consume only part of the chunk. */
        size_t take = c.len;
        if ((rng_next(&r) & 7) == 0 && c.len > 2) take = (size_t)rng_range(&r, 1, c.len / 2) * 2;
        verifier_feed(rd->v, (const uint8_t *)c.data, take);
        gui_record_fifo_consume(rd->f, take);
        rd->received += take;
    }
}

static bool fifo_setup(gui_record_fifo_t *f, size_t ring, bool no_punch, size_t H,
                       uint32_t seg_cap, uint64_t trunc,
                       bool (*hook)(void *, const void *, size_t, uint64_t), void *hook_user) {
    if (!init_mgr(ring)) return false;
    gui_record_fifo_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bufmgr = &g_mgr;
    cfg.buf_id = BUF_RECORD_A;
    cfg.channel = 0;
    cfg.sample_bytes = 2;
    cfg.sample_rate_hz = 40000000;
    cfg.max_push_bytes = 128 * 1024;
    cfg.max_read_bytes = 2 * 1024 * 1024;
    cfg.switch_back_free_bytes = H;
    cfg.spill_dir = g_dir;
    cfg.log = test_log;
    cfg.force_no_punch = no_punch;
    cfg.truncate_threshold = trunc;
    cfg.seg_cap_override = seg_cap;
    cfg.spill_write_hook = hook;
    cfg.hook_user = hook_user;
    char err[256] = {0};
    if (gui_record_fifo_init(f, &cfg, err, sizeof(err)) != 0) {
        fprintf(stderr, "fifo init: %s\n", err);
        return false;
    }
    return gui_record_fifo_spill_available(f);
}

/* Run producer + reader to completion; returns the final stats. */
static void fifo_run(gui_record_fifo_t *f, fifo_prod_t *p, fifo_reader_t *rd,
                     gui_record_fifo_stats_t *st) {
    thrd_t tp, tr;
    expect_true(thrd_create(&tr, fifo_reader, rd) == thrd_success, "reader thread");
    expect_true(thrd_create(&tp, fifo_producer, p) == thrd_success, "producer thread");
    thrd_join(tp, NULL);
    gui_record_fifo_close(f);
    thrd_join(tr, NULL);
    gui_record_fifo_get_stats(f, st);
}

/* ---- 2. random pressure ----------------------------------------------------- */

static void test_random_pressure(bool no_punch) {
    gui_record_fifo_t f;
    reset_log();
    expect_true(fifo_setup(&f, 1024 * 1024, no_punch, 0, 0, 0, NULL, NULL),
                "random pressure: FIFO with a spill file");
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    fifo_prod_t p;
    memset(&p, 0, sizeof(p));
    p.f = &f; p.v = v; p.seed = g_seed ^ (no_punch ? 0x3333 : 0x4444);
    p.min_push = 8; p.max_push = 128 * 1024; p.duration_ms = 2500; p.rate_bps = 150000000;
    fifo_reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.f = &f; rd.v = v; rd.seed = g_seed ^ 0x5555; rd.bursty = true;
    gui_record_fifo_stats_t st;
    fifo_run(&f, &p, &rd, &st);
    printf("  random pressure (%s): pushed %.1f MiB, ring %llu / spill %llu pushes, "
           "%llu segments, %llu episodes, peak backlog %.1f MiB, drops %llu, recycles %llu\n",
           no_punch ? "recycle" : (atomic_load(&f.punch_ok) ? "punch" : "recycle(no punch support)"),
           (double)p.pushed / 1048576.0, (unsigned long long)p.ring_pushes,
           (unsigned long long)p.spill_pushes, (unsigned long long)st.segments,
           (unsigned long long)st.episodes, (double)st.max_backlog_bytes / 1048576.0,
           (unsigned long long)p.drop_pushes, (unsigned long long)st.recycles);
    expect_true(rd.end_rc == -1, "random pressure: reader sees end of stream after close");
    expect_true(p.drop_pushes == 0, "random pressure: nothing dropped");
    expect_true(rd.received == p.accepted, "random pressure: every accepted byte received");
    expect_true(v->mismatches == 0, "random pressure: output identical to input, in order");
    /* Timing-dependent counts stay modest so a slow (sanitized, loaded)
     * runner still passes; the ordering checks above are what matter. */
    expect_true(st.segments >= 10, "random pressure: many ring/spill segments");
    expect_true(st.spill_bytes > 0 && st.ring_bytes > 0, "random pressure: both stores used");
    expect_true(st.consumed_bytes == st.accepted_bytes, "random pressure: consumed == accepted");
    expect_true(atomic_load(&g_log_errors) == 0, "random pressure: no ERROR logged");
    gui_record_fifo_destroy(&f);
    free(v);
}

/* ---- 3. stop mid-episode ---------------------------------------------------- */

static void test_stop_mid_episode(void) {
    gui_record_fifo_t f;
    reset_log();
    expect_true(fifo_setup(&f, 1024 * 1024, false, 0, 0, 0, NULL, NULL), "stop: FIFO");
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    /* Reader held: 5.3 MiB pushed into a 1 MiB ring -> 4.3 MiB spilled. */
    fifo_prod_t p;
    memset(&p, 0, sizeof(p));
    p.f = &f; p.v = v; p.seed = g_seed ^ 0x6666;
    p.min_push = 8; p.max_push = 131072; p.target_bytes = 5557452;  /* not a block multiple */
    p.target_bytes -= p.target_bytes % 8;
    thrd_t tp;
    expect_true(thrd_create(&tp, fifo_producer, &p) == thrd_success, "stop: producer");
    thrd_join(tp, NULL);
    gui_record_fifo_close(&f);
    gui_record_fifo_stats_t st;
    gui_record_fifo_get_stats(&f, &st);
    expect_true(st.spill_bytes > 4 * 1024 * 1024, "stop: the tail is mostly in the spill");
    fifo_reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.f = &f; rd.v = v; rd.seed = g_seed ^ 0x7777;
    fifo_reader(&rd);
    expect_true(rd.end_rc == -1, "stop: end of stream after the drain");
    expect_true(rd.received == p.target_bytes, "stop: the whole tail drains after close");
    expect_true(v->mismatches == 0, "stop: tail in order");
    gui_record_fifo_destroy(&f);
    free(v);

    /* 200 stops at random moments through the gate. */
    rng_t r = { g_seed ^ 0x8888 };
    int bad = 0;
    for (int i = 0; i < 200; i++) {
        gui_record_fifo_t g;
        if (!fifo_setup(&g, 256 * 1024, (i & 1) != 0, 0, 0, 0, NULL, NULL)) { bad++; continue; }
        verifier_t *w = (verifier_t *)calloc(1, sizeof(*w));
        verifier_init(w);
        gui_record_gate_t gate;
        atomic_init(&gate.enabled, false);
        atomic_init(&gate.inflight, 0);
        gui_record_gate_enable(&gate);
        fifo_prod_t q;
        memset(&q, 0, sizeof(q));
        q.f = &g; q.gate = &gate; q.v = w; q.seed = rng_next(&r);
        q.min_push = 8; q.max_push = 131072; q.duration_ms = 1000; q.rate_bps = 150000000;
        fifo_reader_t rr;
        memset(&rr, 0, sizeof(rr));
        rr.f = &g; rr.v = w; rr.seed = rng_next(&r); rr.bursty = true;
        thrd_t a, b;
        thrd_create(&b, fifo_reader, &rr);
        thrd_create(&a, fifo_producer, &q);
        thrd_sleep_ms((int)rng_range(&r, 0, 40));
        gui_record_gate_disable(&gate);
        gui_record_gate_wait_idle(&gate);
        gui_record_fifo_close(&g);
        thrd_join(a, NULL);
        thrd_join(b, NULL);
        if (rr.end_rc != -1 || rr.received != q.accepted || w->mismatches != 0 || q.drop_pushes != 0) {
            fprintf(stderr, "  stop #%d: end=%d received=%llu accepted=%llu mismatches=%llu drops=%llu\n",
                    i, rr.end_rc, (unsigned long long)rr.received, (unsigned long long)q.accepted,
                    (unsigned long long)w->mismatches, (unsigned long long)q.drop_pushes);
            bad++;
        }
        gui_record_fifo_destroy(&g);
        free(w);
    }
    expect_true(bad == 0, "stop: 200 random-moment stops each deliver exactly the accepted bytes");
}

/* ---- 4. recycle + truncate stress ------------------------------------------ */

static void test_recycle_stress(void) {
    gui_record_fifo_t f;
    reset_log();
    expect_true(fifo_setup(&f, 256 * 1024, true, 64 * 1024, 0, 512 * 1024, NULL, NULL),
                "recycle: FIFO (punch off)");
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    fifo_prod_t p;
    memset(&p, 0, sizeof(p));
    p.f = &f; p.v = v; p.seed = g_seed ^ 0x9999;
    p.min_push = 8; p.max_push = 32768; p.duration_ms = 2000; p.rate_bps = 100000000;
    fifo_reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.f = &f; rd.v = v; rd.seed = g_seed ^ 0xAAAA; rd.bursty = true;
    gui_record_fifo_stats_t st;
    fifo_run(&f, &p, &rd, &st);
    printf("  recycle: %llu recycles, %llu segments, %llu episodes\n",
           (unsigned long long)st.recycles, (unsigned long long)st.segments,
           (unsigned long long)st.episodes);
    expect_true(st.recycles >= 3, "recycle: the drained spill is reused");
    expect_true(rd.received == p.accepted && v->mismatches == 0, "recycle: stream intact");
    expect_true(p.drop_pushes == 0, "recycle: nothing dropped");
    gui_record_fifo_destroy(&f);
    free(v);
}

/* ---- 5. segment log full --------------------------------------------------- */

static void test_seg_log_full(void) {
    gui_record_fifo_t f;
    reset_log();
    expect_true(fifo_setup(&f, 256 * 1024, false, 16 * 1024, 4, 0, NULL, NULL),
                "segment log: FIFO with 4 slots");
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    fifo_prod_t p;
    memset(&p, 0, sizeof(p));
    p.f = &f; p.v = v; p.seed = g_seed ^ 0xBBBB;
    p.min_push = 8; p.max_push = 16384; p.duration_ms = 1500; p.rate_bps = 100000000;
    fifo_reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.f = &f; rd.v = v; rd.seed = g_seed ^ 0xCCCC; rd.bursty = true;
    gui_record_fifo_stats_t st;
    fifo_run(&f, &p, &rd, &st);
    printf("  segment log full: %llu segments, %llu drop(s)\n",
           (unsigned long long)st.segments, (unsigned long long)p.drop_pushes);
    expect_true(rd.received == p.accepted, "segment log: every accepted byte received");
    expect_true(v->mismatches == 0, "segment log: accepted bytes in order, drops are the only gaps");
    gui_record_fifo_destroy(&f);
    free(v);
}

/* ---- 6. spill write failure ------------------------------------------------ */

typedef struct { atomic_uint_fast64_t calls; uint64_t fail_from, fail_to; } fail_hook_t;

static bool failing_spill(void *user, const void *bytes, size_t len, uint64_t off) {
    (void)bytes; (void)len; (void)off;
    fail_hook_t *h = (fail_hook_t *)user;
    uint64_t n = atomic_fetch_add(&h->calls, 1);
    if (n >= h->fail_from && n < h->fail_to) {
        errno = ENOSPC;
        return false;
    }
    return true;
}

static void test_spill_failure(void) {
    gui_record_fifo_t f;
    fail_hook_t hook;
    memset(&hook, 0, sizeof(hook));
    hook.fail_from = 20;
    hook.fail_to = 60;
    reset_log();
    expect_true(fifo_setup(&f, 256 * 1024, false, 0, 0, 0, failing_spill, &hook),
                "spill failure: FIFO with a failing spill");
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    fifo_prod_t p;
    memset(&p, 0, sizeof(p));
    p.f = &f; p.v = v; p.seed = g_seed ^ 0xDDDD;
    p.min_push = 8192; p.max_push = 65536; p.duration_ms = 1500; p.rate_bps = 100000000;
    fifo_reader_t rd;
    memset(&rd, 0, sizeof(rd));
    rd.f = &f; rd.v = v; rd.seed = g_seed ^ 0xEEEE; rd.bursty = true;
    gui_record_fifo_stats_t st;
    fifo_run(&f, &p, &rd, &st);
    printf("  spill failure: %llu spill write error(s), %llu drop(s), %d ERROR line(s)\n",
           (unsigned long long)st.spill_write_errors, (unsigned long long)p.drop_pushes,
           atomic_load(&g_log_errors));
    expect_true(st.spill_write_errors > 0, "spill failure: the hook failed writes");
    expect_true(p.drop_pushes > 0, "spill failure: a failed spill with a full ring is a drop");
    expect_true(atomic_load(&g_log_errors) > 0, "spill failure: logged as ERROR");
    expect_true(rd.received == p.accepted && v->mismatches == 0,
                "spill failure: accepted bytes in order, drops are the only gaps");
    gui_record_fifo_destroy(&f);
    free(v);
}

/* ---- 7. sink hold and retry ------------------------------------------------- */

static void test_sink(void) {
#if defined(__linux__)
    char path[700];
    snprintf(path, sizeof(path), "%s/sink_out.bin", g_dir);
    FILE *bad = fopen("/dev/full", "wb");
    FILE *good = fopen(path, "w+b");
    expect_true(bad && good, "sink: /dev/full and a real file");
    if (!bad || !good) {
        if (bad) fclose(bad);
        if (good) fclose(good);
        return;
    }
    setvbuf(bad, NULL, _IONBF, 0);
    gui_record_sink_t s;
    gui_record_sink_init(&s, bad);
    enum { N = 300000 };
    uint8_t *data = (uint8_t *)malloc(N);
    fill_words(data, 0, N);
    uint64_t now = 1000;
    bool ok1 = gui_record_sink_write(&s, data, 100000, now);
    expect_true(!ok1 && gui_record_sink_failing(&s), "sink: a failing output is detected");
    expect_true(gui_record_sink_held(&s) == 100000, "sink: the unwritten bytes are held");
    expect_true(!gui_record_sink_retry(&s, now + 10, false), "sink: no retry before it is due");
    /* Output recovers. */
    s.fp = good;
    expect_true(gui_record_sink_retry(&s, now + GUI_RECORD_SINK_RETRY_MS, false),
                "sink: the due retry writes the held bytes");
    expect_true(gui_record_sink_write(&s, data + 100000, N - 100000, now + 3000),
                "sink: writing resumes after recovery");
    fflush(good);
    fseek(good, 0, SEEK_END);
    long size = ftell(good);
    rewind(good);
    uint8_t *back = (uint8_t *)malloc(N);
    size_t got = fread(back, 1, N, good);
    expect_true(size == N && got == N && memcmp(back, data, N) == 0,
                "sink: the file is exact -- nothing lost, nothing written twice");
    free(back);
    free(data);
    gui_record_sink_free(&s);
    fclose(bad);
    fclose(good);
    remove(path);
#else
    printf("  sink: skipped (needs /dev/full)\n");
#endif
}

/* ---- 8. detector self-check: the OLD algorithm reorders ------------------- */

typedef struct {
    uint8_t *data;
    size_t len, cap, head;
} mem_spill_t;

static void old_algorithm_replay(void) {
    expect_true(init_mgr(256 * 1024), "detector: 256 KiB ring");
    mem_spill_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.cap = 64 * 1024 * 1024;
    sp.data = (uint8_t *)malloc(sp.cap);
    verifier_t *v = (verifier_t *)calloc(1, sizeof(*v));
    verifier_init(v);
    rng_t r = { g_seed ^ 0xF0F0 };
    uint8_t blk[16384];
    uint64_t pushed = 0;
    const size_t B = sizeof(blk), RB = 65536;  /* producer block, reader block */
    for (int step = 0; step < 20000 && sp.len + B < sp.cap; step++) {
        /* Alternate a stalled writer (the ring fills, blocks spill) with a
         * fast one (the ring frees space while the spill still holds older
         * data) -- the shape of a real writer stall. */
        bool stalled = (step / 200) % 2 == 0;
        /* Producer (old): try the ring for every block, spill on failure. */
        int pushes = (int)rng_range(&r, 0, 6);
        for (int i = 0; i < pushes && sp.len + B <= sp.cap; i++) {
            fill_words(blk, pushed, B);
            void *dst = bufmgr_try_write_begin(&g_mgr, BUF_RECORD_A, B);
            if (dst) {
                memcpy(dst, blk, B);
                bufmgr_write_commit(&g_mgr, BUF_RECORD_A, B);
            } else {
                memcpy(sp.data + sp.len, blk, B);
                sp.len += B;
            }
            pushed += B;
        }
        /* Reader (old): a full block from the ring first, else the spill. */
        int reads = stalled ? 0 : (int)rng_range(&r, 0, 3);
        for (int i = 0; i < reads; i++) {
            void *p = bufmgr_read_begin(&g_mgr, BUF_RECORD_A, RB, 0);
            if (p) {
                verifier_feed(v, (const uint8_t *)p, RB);
                bufmgr_read_end(&g_mgr, BUF_RECORD_A, RB);
            } else if (sp.len - sp.head >= RB) {
                verifier_feed(v, sp.data + sp.head, RB);
                sp.head += RB;
            }
        }
    }
    printf("  detector: the old algorithm produced %llu reorder(s), %llu mismatched byte(s)\n",
           (unsigned long long)v->reorders, (unsigned long long)v->mismatches);
    expect_true(v->reorders > 0, "detector: the verifier catches the old per-block spill reordering");
    free(sp.data);
    free(v);
}

int main(int argc, char **argv) {
    if (argc > 1) g_seed = strtoull(argv[1], NULL, 0);
    if (g_seed == 0) g_seed = 1;
    printf("gui_record_fifo harness (seed 0x%llx)\n", (unsigned long long)g_seed);
    if (!make_dir()) {
        fprintf(stderr, "cannot create a scratch dir: %s\n", strerror(errno));
        return 2;
    }
    g_log_echo = getenv("FIFO_HARNESS_VERBOSE") != NULL;

    test_ring_counters();
    test_random_pressure(false);
    test_random_pressure(true);
    test_stop_mid_episode();
    test_recycle_stress();
    test_seg_log_full();
    test_spill_failure();
    test_sink();
    old_algorithm_replay();

    if (g_mgr_up) bufmgr_cleanup(&g_mgr);
    remove_dir();
    printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
