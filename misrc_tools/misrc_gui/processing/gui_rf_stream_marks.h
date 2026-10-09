/* Sparse raw-RF stream boundaries. GPL-3.0-or-later. */
#ifndef GUI_RF_STREAM_MARKS_H
#define GUI_RF_STREAM_MARKS_H

#include "gui_rf_preview_worker.h"

typedef struct gui_rf_stream_marks gui_rf_stream_marks_t;

gui_rf_stream_marks_t *gui_rf_stream_marks_create(void);
void gui_rf_stream_marks_destroy(gui_rf_stream_marks_t *marks);
/* Reset/destroy are only valid while both producer and consumer are stopped. */
void gui_rf_stream_marks_reset(gui_rf_stream_marks_t *marks);

/* Exactly one producer and one consumer may use this object concurrently.
 * Positions are absolute FIFO byte positions, not modulo-buffer offsets.
 * Input is packed 4-byte A/B sample pairs; positions and sizes must be aligned
 * to 4 bytes. Successful writes have contiguous, increasing positions; reset
 * this object when restarting/resetting the FIFO. Observe EVERY successful
 * write before publishing its raw bytes.
 * The caller supplies rate/epoch; start_sample and discontinuity are derived
 * here. Only rate/epoch changes, unsafe ranges, and recovery boundaries occupy
 * queue slots. Ambiguous writes are excluded from preview until a clean write.
 * Never allocates, waits for the consumer, or changes recording/raw data. */
void gui_rf_stream_marks_observe(gui_rf_stream_marks_t *marks,
                                 uint64_t byte_position, size_t bytes,
                                 const gui_rf_block_info_t info[2],
                                 bool ambiguous_gap);

/* Call after acquiring raw data and before releasing that FIFO read.
 * A block crossing a boundary, overlapping an unsafe range, or preceding a
 * known overflow-recovery boundary returns false. Metadata and the consumer
 * cursor still advance, and the next safe block has discontinuity=true.
 * Typical extraction reads are 262144 bytes (65536 packed sample pairs).
 * A safe block's start_sample is byte_position / 4, independent of its rate. */
bool gui_rf_stream_marks_read(gui_rf_stream_marks_t *marks,
                              uint64_t byte_position, size_t bytes,
                              gui_rf_block_info_t info[2]);

#endif
