/*
 * MISRC GUI - Direct native RAW record path (implementation)
 *
 * See gui_record_direct.h for the contract this file implements. Depends only
 * on the record FIFO, the RAW output sink and threading helpers, so the unit
 * harness compiles it without the GUI.
 */

#include "gui_record_direct.h"
#include "gui_record_sink.h"

#include "../../common/threading.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gui_record_direct_log(const gui_record_direct_ctx_t *ctx,
                                  const char *level, const char *fmt, ...) {
    if (!ctx || ctx->cb.log == NULL) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';
    ctx->cb.log(ctx->cb.user, ctx->channel, level ? level : "INFO", msg);
}

static void gui_record_direct_notify_write_error(const gui_record_direct_ctx_t *ctx, bool active) {
    if (ctx && ctx->cb.set_write_error) {
        ctx->cb.set_write_error(ctx->cb.user, ctx->channel, active);
    }
}

static void gui_record_direct_account_write(gui_record_direct_ctx_t *ctx, uint64_t bytes) {
    if (!ctx || bytes == 0) {
        return;
    }
    if (ctx->bytes_written_total) {
        atomic_fetch_add(ctx->bytes_written_total, bytes);
    }
    if (ctx->bytes_written_channel) {
        atomic_fetch_add(ctx->bytes_written_channel, bytes);
    }
}

static gui_record_direct_push_result_t gui_record_direct_map(gui_record_push_result_t r) {
    switch (r) {
        case GUI_RECORD_PUSH_RING:    return GUI_RECORD_DIRECT_PUSH_RING;
        case GUI_RECORD_PUSH_SPILL:   return GUI_RECORD_DIRECT_PUSH_SPILL;
        case GUI_RECORD_PUSH_DROPPED: return GUI_RECORD_DIRECT_PUSH_DROPPED;
        default:                      return GUI_RECORD_DIRECT_PUSH_IDLE;
    }
}

// Push body shared by the live-gated push and the iteration-latched push.
// No gate management here: the callers own the handshake.
static gui_record_direct_push_result_t gui_record_direct_push_inner(
    gui_record_direct_ctx_t *ctx, const uint8_t *bytes, size_t len,
    uint32_t frame_index) {
    if (ctx->bytes_read_total) {
        atomic_fetch_add(ctx->bytes_read_total, len);
    }
    if (!ctx->fifo) {
        return GUI_RECORD_DIRECT_PUSH_IDLE;
    }
    return gui_record_direct_map(gui_record_fifo_push(ctx->fifo, bytes, len, frame_index));
}

gui_record_direct_push_result_t gui_record_direct_push(gui_record_direct_ctx_t *ctx,
                                                       const uint8_t *bytes, size_t len,
                                                       uint32_t frame_index) {
    if (!ctx || !ctx->gate || !bytes || len == 0) {
        return GUI_RECORD_DIRECT_PUSH_IDLE;
    }
    // Handshake: mark in-flight, THEN check enabled, so the stopper's
    // disable + wait-idle sequence can never miss an active push.
    bool on = gui_record_gate_enter(ctx->gate);
    gui_record_direct_push_result_t result = GUI_RECORD_DIRECT_PUSH_IDLE;
    if (on) {
        result = gui_record_direct_push_inner(ctx, bytes, len, frame_index);
    }
    gui_record_gate_leave(ctx->gate);
    return result;
}

bool gui_record_direct_tap_iteration_enter(gui_record_direct_ctx_t *ctx) {
    if (!ctx || !ctx->gate) return false;
    return gui_record_gate_enter(ctx->gate);
}

void gui_record_direct_tap_iteration_leave(gui_record_direct_ctx_t *ctx) {
    if (!ctx || !ctx->gate) return;
    gui_record_gate_leave(ctx->gate);
}

gui_record_direct_push_result_t gui_record_direct_push_latched(
    gui_record_direct_ctx_t *ctx, bool latched_on,
    const uint8_t *bytes, size_t len, uint32_t frame_index) {
    if (!ctx || !bytes || len == 0 || !latched_on) {
        return GUI_RECORD_DIRECT_PUSH_IDLE;
    }
    // The paired tap_iteration_enter already marked the gate in-flight (the
    // stopper's wait-idle covers this push); honor the iteration-latched
    // decision instead of the live enabled flag so a record start/stop
    // handshake landing between the per-channel pushes cannot split the
    // channels by one chunk.
    return gui_record_direct_push_inner(ctx, bytes, len, frame_index);
}

int gui_record_direct_writer_thread(void *ctx_ptr) {
    gui_record_direct_ctx_t *ctx = (gui_record_direct_ctx_t *)ctx_ptr;
    if (!ctx || !ctx->fifo || !ctx->file) {
        return 0;
    }

    // Boost thread priority to avoid backpressure when window is minimized.
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    fprintf(stderr, "[DIRECT] Writer thread %c started\n", ctx->channel == 0 ? 'A' : 'B');

    gui_record_sink_t sink;
    gui_record_sink_init(&sink, ctx->file);
    uint64_t logged_failures = 0;
    uint64_t accounted = 0;   // sink.written already added to the counters

    for (;;) {
        bool exiting = ctx->exit_flag != NULL && atomic_load(ctx->exit_flag) != 0;
        sink.fp = ctx->file;  /* the record layer never swaps it; the harness does */

        if (gui_record_sink_failing(&sink)) {
            // The output refused bytes: they are held (the oldest unread
            // data) and nothing more is read, so nothing can reorder; the
            // FIFO backs up behind them.
            bool closed = atomic_load(&ctx->fifo->closed);
            bool recovered = gui_record_sink_retry(&sink, get_time_ms(), closed || exiting);
            gui_record_direct_account_write(ctx, sink.written - accounted);
            accounted = sink.written;
            uint64_t held = gui_record_sink_held(&sink);
            if (recovered) {
                gui_record_direct_notify_write_error(ctx, false);
                gui_record_direct_log(ctx, "INFO", "Direct RAW output file write recovered");
                continue;
            }
            if (sink.failures != logged_failures &&
                (sink.failures <= 5 || sink.failures % 100 == 0)) {
                logged_failures = sink.failures;
                gui_record_direct_log(ctx, "ERROR",
                                      "Direct RAW write error (#%llu): %s; %.2f MiB held, retrying every %us",
                                      (unsigned long long)sink.failures, strerror(sink.last_errno),
                                      (double)held / (1024.0 * 1024.0),
                                      (unsigned)(GUI_RECORD_SINK_RETRY_MS / 1000));
            }
            if (closed || exiting) {
                // The last forced attempt failed: the held bytes and anything
                // still queued are lost. Drain the FIFO so it ends cleanly,
                // and say exactly how much.
                uint64_t lost = held;
                gui_record_fifo_chunk_t c;
                while (gui_record_fifo_next(ctx->fifo, 0, 0, &c) > 0) {
                    lost += c.len;
                    gui_record_fifo_consume(ctx->fifo, c.len);
                }
                if (ctx->bytes_lost_total) atomic_fetch_add(ctx->bytes_lost_total, lost);
                gui_record_direct_log(ctx, "ERROR",
                                      "Direct RAW output never recovered: %.2f MiB at the end of the recording were not written",
                                      (double)lost / (1024.0 * 1024.0));
                break;
            }
            thrd_sleep_ms(50);
            continue;
        }

        gui_record_fifo_chunk_t c;
        int rc = gui_record_fifo_next(ctx->fifo, GUI_RECORD_DIRECT_MAX_BLOCK_BYTES, 10, &c);
        if (rc < 0) {
            break;  // closed and fully written
        }
        if (rc == 0) {
            if (exiting) break;
            continue;
        }
        bool ok = gui_record_sink_write(&sink, c.data, c.len, get_time_ms());
        // The chunk is consumed either way: what the output did not take is
        // held by the sink, in order, ahead of anything read later.
        gui_record_fifo_consume(ctx->fifo, c.len);
        gui_record_direct_account_write(ctx, sink.written - accounted);
        accounted = sink.written;
        if (!ok) {
            gui_record_direct_notify_write_error(ctx, true);
        }
    }

    gui_record_sink_free(&sink);
    fprintf(stderr, "[DIRECT] Writer thread %c exiting\n", ctx->channel == 0 ? 'A' : 'B');
    return 0;
}
