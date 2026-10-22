/* Sparse raw-RF stream boundaries. GPL-3.0-or-later. */
#include "gui_rf_stream_marks.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RF_MARK_CAPACITY 4096u
#define RF_PAIR_BYTES 4u

typedef struct {
    uint64_t position;
    uint32_t rates[2];
    uint64_t epochs[2];
    bool unsafe;
} rf_mark_t;

struct gui_rf_stream_marks {
    rf_mark_t queue[RF_MARK_CAPACITY];
    atomic_uint_fast64_t write_index;
    atomic_uint_fast64_t read_index;

    /* All snapshot members are atomic: a conventional seqlock with ordinary
     * C members would still contain data races when overflow is concurrent. */
    atomic_uint_fast64_t recovery_sequence;
    atomic_uint_fast64_t recovery_position;
    atomic_uint_fast32_t recovery_rates[2];
    atomic_uint_fast64_t recovery_epochs[2];
    atomic_bool recovery_unsafe;

    /* Producer-owned state. */
    rf_mark_t producer;
    uint64_t producer_end;
    bool producer_valid;

    /* Consumer-owned state. */
    rf_mark_t consumer;
    uint64_t consumed_end;
    uint64_t minimum_safe_position;
    uint64_t seen_recovery_sequence;
    bool consumer_valid;
    bool consumed_any;
    bool gap_pending;
};

gui_rf_stream_marks_t *gui_rf_stream_marks_create(void) {
    gui_rf_stream_marks_t *marks = calloc(1, sizeof(*marks));
    if (marks) gui_rf_stream_marks_reset(marks);
    return marks;
}

void gui_rf_stream_marks_destroy(gui_rf_stream_marks_t *marks) { free(marks); }

void gui_rf_stream_marks_reset(gui_rf_stream_marks_t *marks) {
    if (!marks) return;
    memset(marks->queue, 0, sizeof(marks->queue));
    memset(&marks->producer, 0, sizeof(marks->producer));
    memset(&marks->consumer, 0, sizeof(marks->consumer));
    atomic_init(&marks->write_index, 0);
    atomic_init(&marks->read_index, 0);
    atomic_init(&marks->recovery_sequence, 0);
    atomic_init(&marks->recovery_position, 0);
    atomic_init(&marks->recovery_unsafe, false);
    for (int ch = 0; ch < 2; ++ch) {
        atomic_init(&marks->recovery_rates[ch], 0);
        atomic_init(&marks->recovery_epochs[ch], 0);
    }
    marks->producer_end = 0;
    marks->consumed_end = 0;
    marks->minimum_safe_position = 0;
    marks->seen_recovery_sequence = 0;
    marks->producer_valid = marks->consumer_valid = marks->consumed_any = false;
    marks->gap_pending = true;
}

static bool rf_mark_changed(const rf_mark_t *a, const rf_mark_t *b) {
    return a->unsafe != b->unsafe || a->rates[0] != b->rates[0] ||
           a->rates[1] != b->rates[1] || a->epochs[0] != b->epochs[0] ||
           a->epochs[1] != b->epochs[1];
}

static void rf_publish_recovery(gui_rf_stream_marks_t *marks, const rf_mark_t *mark) {
    /* seq_cst prevents readers from accepting new payload under an old even
     * sequence. There is one writer; this never spins or overwrites a slot. */
    atomic_fetch_add(&marks->recovery_sequence, 1);
    atomic_store(&marks->recovery_position, mark->position);
    for (int ch = 0; ch < 2; ++ch) {
        atomic_store(&marks->recovery_rates[ch], mark->rates[ch]);
        atomic_store(&marks->recovery_epochs[ch], mark->epochs[ch]);
    }
    atomic_store(&marks->recovery_unsafe, mark->unsafe);
    atomic_fetch_add(&marks->recovery_sequence, 1);
}

void gui_rf_stream_marks_observe(gui_rf_stream_marks_t *marks,
                                 uint64_t byte_position, size_t bytes,
                                 const gui_rf_block_info_t info[2], bool ambiguous_gap) {
    if (!marks || !info || !bytes) return;
    bool invalid = (byte_position % RF_PAIR_BYTES) != 0 ||
                   (bytes % RF_PAIR_BYTES) != 0 || bytes > UINT64_MAX - byte_position;
    rf_mark_t mark = {.position = byte_position, .unsafe = ambiguous_gap || invalid};
    for (int ch = 0; ch < 2; ++ch) {
        mark.rates[ch] = info[ch].sample_rate_hz;
        mark.epochs[ch] = info[ch].epoch;
    }
    bool unexpected_position = marks->producer_valid && byte_position != marks->producer_end;
    if (unexpected_position) mark.unsafe = true;
    bool changed = !marks->producer_valid || rf_mark_changed(&marks->producer, &mark) ||
                   unexpected_position;
    /* Multiple adjacent ambiguous writes form one unsafe range. */
    marks->producer = mark;
    marks->producer_valid = true;
    marks->producer_end = invalid ? UINT64_MAX : byte_position + bytes;
    if (!changed) return;

    uint64_t write = atomic_load_explicit(&marks->write_index, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&marks->read_index, memory_order_acquire);
    if (write - read >= RF_MARK_CAPACITY) {
        rf_publish_recovery(marks, &mark);
        return;
    }
    marks->queue[write % RF_MARK_CAPACITY] = mark;
    atomic_store_explicit(&marks->write_index, write + 1, memory_order_release);
}

static bool rf_recover(gui_rf_stream_marks_t *marks) {
    uint64_t sequence = atomic_load(&marks->recovery_sequence);
    if (sequence == marks->seen_recovery_sequence) return true;
    if (sequence & 1u) return false;
    rf_mark_t mark = {.position = atomic_load(&marks->recovery_position),
                      .unsafe = atomic_load(&marks->recovery_unsafe)};
    for (int ch = 0; ch < 2; ++ch) {
        mark.rates[ch] = atomic_load(&marks->recovery_rates[ch]);
        mark.epochs[ch] = atomic_load(&marks->recovery_epochs[ch]);
    }
    if (sequence != atomic_load(&marks->recovery_sequence)) return false;

    /* Keep marks NEWER than this snapshot. A producer can publish them as
     * soon as slots are freed; discarding the whole queue would lose those
     * rate/gap changes during concurrent recovery. */
    uint64_t read = atomic_load_explicit(&marks->read_index, memory_order_relaxed);
    uint64_t write = atomic_load_explicit(&marks->write_index, memory_order_acquire);
    while (read != write && marks->queue[read % RF_MARK_CAPACITY].position <= mark.position) {
        ++read;
        atomic_store_explicit(&marks->read_index, read, memory_order_release);
    }
    marks->consumer = mark;
    marks->consumer_valid = true;
    marks->minimum_safe_position = mark.position;
    marks->seen_recovery_sequence = sequence;
    marks->gap_pending = true;
    return sequence == atomic_load(&marks->recovery_sequence);
}

bool gui_rf_stream_marks_read(gui_rf_stream_marks_t *marks,
                              uint64_t byte_position, size_t bytes,
                              gui_rf_block_info_t info[2]) {
    if (!marks || !info || !bytes || (byte_position % RF_PAIR_BYTES) != 0 ||
        (bytes % RF_PAIR_BYTES) != 0 || bytes > UINT64_MAX - byte_position) return false;
    uint64_t end = byte_position + bytes;
    bool recovered = rf_recover(marks);
    bool safe = recovered && byte_position >= marks->minimum_safe_position;
    if (marks->consumed_any && marks->consumed_end != byte_position) marks->gap_pending = true;
    if (marks->consumed_any && byte_position < marks->consumed_end) safe = false;

    uint64_t read = atomic_load_explicit(&marks->read_index, memory_order_relaxed);
    uint64_t write = atomic_load_explicit(&marks->write_index, memory_order_acquire);
    while (read != write) {
        rf_mark_t mark = marks->queue[read % RF_MARK_CAPACITY];
        if (mark.position >= end) break;
        if (mark.position > byte_position) safe = false;
        marks->consumer = mark;
        marks->consumer_valid = true;
        marks->gap_pending = true;
        ++read;
        atomic_store_explicit(&marks->read_index, read, memory_order_release);
    }
    safe = safe && marks->consumer_valid && !marks->consumer.unsafe;
    /* Unsafe at the block start must stay unsafe even if a clean boundary was
     * consumed within it (the crossing condition above also rejects it). */
    if (marks->seen_recovery_sequence != atomic_load(&marks->recovery_sequence)) safe = false;
    for (int ch = 0; ch < 2; ++ch) {
        info[ch].sample_rate_hz = marks->consumer.rates[ch];
        info[ch].epoch = marks->consumer.epochs[ch];
        info[ch].start_sample = byte_position / RF_PAIR_BYTES;
        info[ch].discontinuity = marks->gap_pending || !safe;
    }
    marks->consumed_end = end;
    marks->consumed_any = true;
    marks->gap_pending = !safe;
    return safe;
}
