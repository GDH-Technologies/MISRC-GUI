/*
 * MISRC GUI - Direct native RAW record path
 *
 * Byte-exact passthrough recording for capture backends whose native device
 * stream is already the output format (CXADC with FLAC off and per-channel
 * resampling off). The device capture thread pushes the exact bytes it read
 * into the channel's ordered record FIFO (gui_record_fifo.h: record ring
 * first, local spill for overflow) via gui_record_direct_push();
 * gui_record_direct_writer_thread fwrites those bytes unchanged, so the
 * output file is the card's byte stream 1:1 (8-bit mode -> unsigned .u8
 * bytes, tenbit mode -> unsigned .u16 words).
 *
 * Ordering and tail drain are the FIFO's contract: every accepted byte is
 * written once, in order, including the final partial block after stop.
 * The FIFO is shared with the converted paths, so there is one ordering
 * implementation for every record mode.
 *
 * Output failures: the writer goes through gui_record_sink, which holds only
 * the unwritten remainder of a failed fwrite and retries it every ~2 s while
 * the FIFO backs up behind it -- nothing is reordered or written twice.
 *
 * The module has no raylib/gui_app dependency so this contract is
 * unit-testable (test/gui_record_direct_harness.c): every session
 * interaction (FIFO, gate, capture log, accounting atomics, write-error
 * flag) is injected through the context.
 */

#ifndef GUI_RECORD_DIRECT_H
#define GUI_RECORD_DIRECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>

#include "../../common/buffer_manager.h"
#include "gui_record_fifo.h"

// Writer read granularity cap (matches the legacy RAW writer block size).
#define GUI_RECORD_DIRECT_MAX_BLOCK_BYTES ((size_t)65536 * 32)

// Result of one producer push.
typedef enum {
    GUI_RECORD_DIRECT_PUSH_IDLE = 0,   // tap disabled: nothing recorded
    GUI_RECORD_DIRECT_PUSH_RING,       // accepted into the record ringbuffer
    GUI_RECORD_DIRECT_PUSH_SPILL,      // accepted into the spill file
    GUI_RECORD_DIRECT_PUSH_DROPPED,    // lost (logged and counted by the FIFO)
} gui_record_direct_push_result_t;

// Injected session interactions. Production implementations live in
// gui_record.c; the unit harness provides counting equivalents.
typedef struct gui_record_direct_callbacks {
    // Output-file write-error flag (drives the UI "write error" indicator).
    void (*set_write_error)(void *user, int channel, bool active);
    // Capture-log sink. May be NULL (no logging).
    void (*log)(void *user, int channel, const char *level, const char *message);
    void *user;
} gui_record_direct_callbacks_t;

typedef struct gui_record_direct_ctx {
    int channel;               // 0 = A, 1 = B
    FILE *file;                // output file (required)
    gui_record_fifo_t *fifo;   // the channel's record FIFO (required)
    gui_record_gate_t *gate;   // the recording's producer gate (required)

    // Global exit flag (do_exit, declared `volatile atomic_int`). Optional
    // (NULL = unused): an app exit ends the writer once the FIFO is empty
    // even when no stop closed it.
    const volatile atomic_int *exit_flag;

    // Accounting targets (optional; NULL = skip).
    atomic_uint_fast64_t *bytes_read_total;      // producer: bytes accepted from the device
    atomic_uint_fast64_t *bytes_written_total;    // writer: total output bytes
    atomic_uint_fast64_t *bytes_written_channel;  // writer: per-channel output bytes
    atomic_uint_fast64_t *bytes_lost_total;       // writer: bytes a failed output never took

    gui_record_direct_callbacks_t cb;
} gui_record_direct_ctx_t;

// Producer side (device capture thread). Pushes the exact bytes read from
// the card into the channel's FIFO (no decode, no pairing, no truncation):
// ring when it has room, local spill otherwise, a counted drop only when
// both fail. Never blocks. Gated: IDLE unless the recording's gate is on.
gui_record_direct_push_result_t gui_record_direct_push(gui_record_direct_ctx_t *ctx,
                                                        const uint8_t *bytes, size_t len,
                                                        uint32_t frame_index);

// Iteration-scoped gate (multi-channel lockstep capture threads): the CXADC
// capture thread must decide ONCE per iteration whether that iteration's
// chunks are recorded, so a record start/stop handshake landing between the
// per-channel pushes cannot split the channels by one chunk (observed: a
// 64 KiB A/B count difference). enter() marks the gate in-flight and returns
// the latched enabled state; the caller pushes every gated channel with
// push_latched(latched_on) or none, then leaves. The stopper's disable +
// wait-idle waits for the whole gated iteration, so no latched push can run
// after the writers' final drain.
bool gui_record_direct_tap_iteration_enter(gui_record_direct_ctx_t *ctx);
void gui_record_direct_tap_iteration_leave(gui_record_direct_ctx_t *ctx);
gui_record_direct_push_result_t gui_record_direct_push_latched(
    gui_record_direct_ctx_t *ctx, bool latched_on,
    const uint8_t *bytes, size_t len, uint32_t frame_index);

// Writer thread (started by the record layer instead of the converting RAW
// writer for direct channels). fwrites the bytes unchanged; variable-length
// reads; drains the FIFO to its end (closed + empty) including the final
// partial block. On output-file write failure it holds the unwritten bytes
// and retries every ~2 s, preserving order; one forced attempt at the end.
int gui_record_direct_writer_thread(void *ctx_ptr);

#endif // GUI_RECORD_DIRECT_H
