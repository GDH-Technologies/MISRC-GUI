/* Sparse RF boundary tests. No decoder, GUI, or capture hardware is needed. */
#include "../misrc_gui/processing/gui_rf_stream_marks.h"
#include "../common/threading.h"

#include <stdatomic.h>
#include <stdio.h>

#define RAW_BLOCK_BYTES 262144u
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); ++failures; \
} } while (0)

static int failures;

static void make_info(gui_rf_block_info_t info[2], uint32_t rate, uint64_t epoch) {
    for (int ch = 0; ch < 2; ++ch) {
        info[ch].sample_rate_hz = rate + (uint32_t)ch;
        info[ch].epoch = epoch + (uint64_t)ch;
        info[ch].start_sample = 999;
        info[ch].discontinuity = false;
    }
}

static void test_backlog_and_unsafe(void) {
    gui_rf_stream_marks_t *marks = gui_rf_stream_marks_create();
    CHECK(marks != NULL);
    if (!marks) return;
    gui_rf_block_info_t in[2], out[2];
    make_info(in, 20000000u, 1);
    /* Producer is ahead: the gap must not reset the older queued RF blocks. */
    gui_rf_stream_marks_observe(marks, 0, RAW_BLOCK_BYTES * 3u, in, false);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES * 3u, RAW_BLOCK_BYTES, in, true);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES * 4u, RAW_BLOCK_BYTES * 2u, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 0, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity && out[0].start_sample == 0);
    CHECK(out[1].sample_rate_hz == 20000001u && out[1].epoch == 2);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(!out[0].discontinuity && out[0].start_sample == 65536);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 2u, RAW_BLOCK_BYTES, out));
    CHECK(!out[0].discontinuity);
    CHECK(!gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 3u, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 4u, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 5u, RAW_BLOCK_BYTES, out));
    CHECK(!out[0].discontinuity);
    gui_rf_stream_marks_destroy(marks);
}

static void test_midblock_and_rate_change(void) {
    gui_rf_stream_marks_t *marks = gui_rf_stream_marks_create();
    CHECK(marks != NULL);
    if (!marks) return;
    gui_rf_block_info_t in[2], out[2];
    make_info(in, 40000000u, 10);
    gui_rf_stream_marks_observe(marks, 0, RAW_BLOCK_BYTES + 100u, in, false);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES + 100u, 100u, in, true);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES + 200u, RAW_BLOCK_BYTES - 200u, in, false);
    make_info(in, 80000000u, 20);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES * 2u, RAW_BLOCK_BYTES, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 0, RAW_BLOCK_BYTES, out));
    CHECK(!gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 40000000u && out[0].epoch == 10);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 2u, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 80000000u && out[0].epoch == 20 && out[0].discontinuity);

    gui_rf_stream_marks_reset(marks);
    make_info(in, 20000000u, 30);
    gui_rf_stream_marks_observe(marks, 0, RAW_BLOCK_BYTES / 2u, in, false);
    make_info(in, 40000000u, 31);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES / 2u, RAW_BLOCK_BYTES * 2u, in, false);
    CHECK(!gui_rf_stream_marks_read(marks, 0, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 40000000u && out[0].epoch == 31);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity && out[0].start_sample == 65536);
    /* Consumer-side skipped input also makes a gap, without a producer mark. */
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 3u, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity);
    gui_rf_stream_marks_destroy(marks);
}

static void test_channel_b_presence_backlog(void) {
    gui_rf_stream_marks_t *marks = gui_rf_stream_marks_create();
    CHECK(marks != NULL);
    if (!marks) return;
    gui_rf_block_info_t in[2], out[2];
    make_info(in, 20000000u, 50);
    in[1].sample_rate_hz = 0; /* This queued payload contains only channel A. */
    gui_rf_stream_marks_observe(marks, 0, RAW_BLOCK_BYTES * 2u + RAW_BLOCK_BYTES / 2u, in, false);
    in[1].sample_rate_hz = 20000000u;
    in[0].epoch = in[1].epoch = 51;
    /* Producer has already discovered B while the consumer is still at A-only
     * backlog. The first AB write straddles the consumer's third RF block. */
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES * 2u + RAW_BLOCK_BYTES / 2u,
                                RAW_BLOCK_BYTES / 2u, in, true);
    gui_rf_stream_marks_observe(marks, RAW_BLOCK_BYTES * 3u, RAW_BLOCK_BYTES * 2u, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 0, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 20000000u && out[1].sample_rate_hz == 0 && out[0].epoch == 50);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[1].sample_rate_hz == 0 && !out[0].discontinuity);
    CHECK(!gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 2u, RAW_BLOCK_BYTES, out));
    CHECK(out[1].sample_rate_hz == 20000000u && out[0].epoch == 51 && out[0].discontinuity);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 3u, RAW_BLOCK_BYTES, out));
    CHECK(out[1].sample_rate_hz == 20000000u && out[0].discontinuity && out[1].discontinuity);
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES * 4u, RAW_BLOCK_BYTES, out));
    CHECK(!out[0].discontinuity && !out[1].discontinuity);
    gui_rf_stream_marks_destroy(marks);
}

static void test_sparse_and_overflow(void) {
    gui_rf_stream_marks_t *marks = gui_rf_stream_marks_create();
    CHECK(marks != NULL);
    if (!marks) return;
    gui_rf_block_info_t in[2], out[2];
    make_info(in, 20000000u, 1);
    for (uint64_t i = 0; i < 10000; ++i)
        gui_rf_stream_marks_observe(marks, i * RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 0, RAW_BLOCK_BYTES, out));
    CHECK(gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(!out[0].discontinuity); /* Stable writes cannot fill a sparse queue. */

    gui_rf_stream_marks_reset(marks);
    for (uint64_t i = 0; i < 9000; ++i) {
        make_info(in, (i & 1u) ? 40000000u : 20000000u, i + 1);
        gui_rf_stream_marks_observe(marks, i * RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, false);
    }
    /* Overflow must not let old data use the latest producer's metadata. */
    CHECK(!gui_rf_stream_marks_read(marks, 0, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 40000000u && out[0].epoch == 9000);
    CHECK(!gui_rf_stream_marks_read(marks, RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    /* Recovery snapshot is at a certain, aligned clean producer boundary. */
    CHECK(gui_rf_stream_marks_read(marks, 8999u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity && out[0].start_sample == 8999u * 65536u);
    make_info(in, 80000000u, 10000);
    gui_rf_stream_marks_observe(marks, 9000u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 9000u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 80000000u && out[0].epoch == 10000 && out[0].discontinuity);
    gui_rf_stream_marks_observe(marks, 9001u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 9001u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(!out[0].discontinuity);

    gui_rf_stream_marks_reset(marks);
    for (uint64_t i = 0; i < 5000; ++i) {
        make_info(in, 20000000u, i + 1);
        gui_rf_stream_marks_observe(marks, i * RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, i == 4999);
    }
    CHECK(!gui_rf_stream_marks_read(marks, 4999u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    gui_rf_stream_marks_observe(marks, 5000u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, false);
    CHECK(gui_rf_stream_marks_read(marks, 5000u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[0].discontinuity);
    gui_rf_stream_marks_destroy(marks);
}

typedef struct {
    gui_rf_stream_marks_t *marks;
    atomic_uint_fast64_t published_blocks;
    atomic_bool start_consumer;
    atomic_uint bad_metadata;
    atomic_uint safe_reads;
    atomic_uint unsafe_reads;
} concurrent_test_t;

static int concurrent_producer(void *context) {
    concurrent_test_t *test = context;
    gui_rf_block_info_t in[2];
    for (uint64_t i = 0; i < 30000; ++i) {
        make_info(in, (i & 1u) ? 80000000u : 20000000u, i + 1);
        gui_rf_stream_marks_observe(test->marks, i * RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, (i % 7u) == 3u);
        /* This release represents publishing the corresponding raw bytes. */
        atomic_store_explicit(&test->published_blocks, i + 1, memory_order_release);
        if (i == 6000) atomic_store(&test->start_consumer, true);
        if ((i % 512u) == 511u && i > 6000) thrd_sleep_ms(1);
    }
    atomic_store(&test->start_consumer, true);
    return 0;
}

static int concurrent_consumer(void *context) {
    concurrent_test_t *test = context;
    while (!atomic_load(&test->start_consumer)) thrd_sleep_ms(1);
    for (uint64_t i = 0; i < 30000; ++i) {
        while (atomic_load_explicit(&test->published_blocks, memory_order_acquire) <= i)
            thrd_sleep_ms(1);
        gui_rf_block_info_t out[2];
        bool safe = gui_rf_stream_marks_read(test->marks, i * RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out);
        if (safe) {
            atomic_fetch_add(&test->safe_reads, 1);
            uint32_t rate = (i & 1u) ? 80000000u : 20000000u;
            if ((i % 7u) == 3u || out[0].sample_rate_hz != rate || out[1].sample_rate_hz != rate + 1u ||
                out[0].epoch != i + 1 || out[1].epoch != i + 2 || out[0].start_sample != i * 65536u)
                atomic_fetch_add(&test->bad_metadata, 1);
        } else {
            atomic_fetch_add(&test->unsafe_reads, 1);
        }
    }
    return 0;
}

static void test_concurrent(void) {
    concurrent_test_t test = {.marks = gui_rf_stream_marks_create()};
    CHECK(test.marks != NULL);
    if (!test.marks) return;
    atomic_init(&test.published_blocks, 0);
    atomic_init(&test.start_consumer, false);
    atomic_init(&test.bad_metadata, 0);
    atomic_init(&test.safe_reads, 0);
    atomic_init(&test.unsafe_reads, 0);
    thrd_t producer, consumer;
    int producer_result = thrd_create(&producer, concurrent_producer, &test);
    int consumer_result = producer_result == thrd_success
                        ? thrd_create(&consumer, concurrent_consumer, &test) : -1;
    CHECK(producer_result == thrd_success && consumer_result == thrd_success);
    if (producer_result == thrd_success) thrd_join(producer, NULL);
    if (consumer_result == thrd_success) thrd_join(consumer, NULL);
    CHECK(atomic_load(&test.bad_metadata) == 0);
    /* Even an unfair scheduler must accept the final published clean block;
     * do not require a particular number of intermediate scheduled reads. */
    CHECK(atomic_load(&test.safe_reads) > 0);
    CHECK(atomic_load(&test.unsafe_reads) > 1000);
    gui_rf_block_info_t in[2], out[2];
    make_info(in, 40000000u, 60000);
    gui_rf_stream_marks_observe(test.marks, 30000u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, in, false);
    CHECK(gui_rf_stream_marks_read(test.marks, 30000u * (uint64_t)RAW_BLOCK_BYTES, RAW_BLOCK_BYTES, out));
    CHECK(out[0].sample_rate_hz == 40000000u && out[0].epoch == 60000 && out[0].discontinuity);
    gui_rf_stream_marks_destroy(test.marks);
}

int main(void) {
    test_backlog_and_unsafe();
    test_midblock_and_rate_change();
    test_channel_b_presence_backlog();
    test_sparse_and_overflow();
    test_concurrent();
    if (failures) fprintf(stderr, "%d RF boundary test failures\n", failures);
    else puts("RF stream boundary tests passed");
    return failures ? 1 : 0;
}
