/*
 * MISRC GUI - RAW record output with hold-and-retry
 *
 * The RAW writers (converted .s8/.s16 and direct native .u8/.u16) write
 * through this. When a write to the output file fails -- disk full, the file
 * locked by another program on Windows, an NFS server that stopped answering
 * with a soft mount -- the bytes fwrite did not take are held, in order, and
 * retried every GUI_RECORD_SINK_RETRY_MS; the writer stops pulling from its
 * record FIFO meanwhile, so the backlog waits in the ring and the local spill
 * and nothing is reordered. Only the unwritten remainder is kept, so a short
 * write is never written twice.
 *
 * Raylib-free and GUI-free (unit-tested by test/gui_record_fifo_harness.c).
 */

#ifndef GUI_RECORD_SINK_H
#define GUI_RECORD_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define GUI_RECORD_SINK_RETRY_MS 2000u

typedef struct {
    FILE *fp;
    uint8_t *pend;           /* held bytes, oldest first */
    size_t pend_len;
    size_t pend_cap;
    uint64_t next_retry_ms;
    uint64_t written;        /* bytes that reached fwrite successfully */
    uint64_t failures;       /* failed write attempts */
    int last_errno;
    bool failing;
} gui_record_sink_t;

void gui_record_sink_init(gui_record_sink_t *s, FILE *fp);
void gui_record_sink_free(gui_record_sink_t *s);

/* Writes bytes after anything held. Returns true when both reached the file;
 * false when the sink is (now) failing, with the unwritten remainder held.
 * Never call while failing: retry first (the writer must not read more). */
bool gui_record_sink_write(gui_record_sink_t *s, const void *bytes, size_t len, uint64_t now_ms);

/* While failing: write the held bytes again when a retry is due (or force).
 * Returns true when nothing is held any more. */
bool gui_record_sink_retry(gui_record_sink_t *s, uint64_t now_ms, bool force);

static inline bool gui_record_sink_failing(const gui_record_sink_t *s) {
    return s && s->failing;
}

/* Bytes currently held (lost if the final forced retry fails). */
static inline size_t gui_record_sink_held(const gui_record_sink_t *s) {
    return s ? s->pend_len : 0;
}

#endif /* GUI_RECORD_SINK_H */
