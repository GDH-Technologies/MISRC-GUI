/*
 * MISRC GUI - ordered record FIFO (RAM ring first, disk spill for overflow)
 *
 * One FIFO per record channel carries the RF byte stream from its single
 * producer (the extraction thread, or the CXADC capture thread's direct tap)
 * to the channel's writer thread, through two stores:
 *
 *   - the channel's record ring (BUF_RECORD_A/B) while it has room, and
 *   - a spill file on LOCAL disk for whatever does not fit.
 *
 * The contract: every byte push() accepts reaches next() exactly once, in
 * push order, including the partial tail after close(). A byte is lost only
 * by an explicit drop, which push() returns, logs and counts.
 *
 * How order survives two stores: a segment log. The stream is a sequence of
 * segments, each wholly in one store; the producer publishes a segment's
 * {store, first stream byte, spill file offset} BEFORE the segment's first
 * byte, and the reader consumes segments in log order, each capped at the
 * next segment's first byte. The ring therefore keeps taking data whenever it
 * has space (RAM stays in use; only true overflow touches the disk), and a
 * spilled block can never be written after a newer one -- the defect that
 * reordered the old per-block spill.
 *
 *   producer:  ring full  -> open a SPILL segment, append to the file
 *              in SPILL   -> back to a RING segment once the ring has
 *                            switch_back_free_bytes free (hysteresis)
 *   reader:    load closed, then pub_seq, then seg_tail (in that order:
 *              see gui_record_fifo_next), read the current segment's store
 *
 * Capture-side safety: push() never waits. It touches RAM and the spill
 * file only, and the spill directory must be on a local filesystem (the
 * caller resolves it; gui_record_fifo_fs_kind helps). Logging goes through
 * the injected callback, which must not block either.
 *
 * Spill file: positional I/O on one unlinked file (no stdio, no lock); the
 * producer is its only writer and truncater. Space the reader has consumed is
 * returned with hole punching where the filesystem supports it; otherwise
 * the file is reused from offset 0 once fully drained.
 *
 * Raylib-free and GUI-free, so the contract is unit-tested
 * (test/gui_record_fifo_harness.c).
 */

#ifndef GUI_RECORD_FIFO_H
#define GUI_RECORD_FIFO_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../common/buffer_manager.h"

/* ---- producer gate ------------------------------------------------------ */

/* Start/stop handshake for every producer of a recording. A producer calls
 * enter() before its pushes and leave() after (always, on every path); the
 * stopper calls disable() then wait_idle(), after which no push is running
 * or will run. enter() increments inflight BEFORE it reads enabled and the
 * stopper clears enabled BEFORE it reads inflight (both seq_cst), so a push
 * can never slip past the stop. inflight is never reset: a producer may be
 * between enter and leave when a recording is armed. */
typedef struct {
    atomic_bool enabled;
    atomic_int inflight;
} gui_record_gate_t;

void gui_record_gate_enable(gui_record_gate_t *g);
void gui_record_gate_disable(gui_record_gate_t *g);
bool gui_record_gate_enter(gui_record_gate_t *g);   /* returns enabled */
void gui_record_gate_leave(gui_record_gate_t *g);
void gui_record_gate_wait_idle(gui_record_gate_t *g);

/* ---- filesystem kind (for choosing a spill directory) ------------------- */

typedef enum {
    GUI_RECORD_FS_UNKNOWN = 0,
    GUI_RECORD_FS_LOCAL,
    GUI_RECORD_FS_NETWORK,  /* NFS, SMB/CIFS, AFP, WebDAV, FUSE, UNC/remote drive */
    GUI_RECORD_FS_RAM,      /* tmpfs/ramfs: spilling there takes RAM, not disk */
} gui_record_fs_kind_t;

/* Classifies the filesystem holding path (which must exist). name receives
 * a short type name for the log ("btrfs", "nfs", "0x6969", ...). */
gui_record_fs_kind_t gui_record_fifo_fs_kind(const char *path, char *name, size_t name_size);

/* ---- the FIFO ------------------------------------------------------------ */

typedef enum {
    GUI_RECORD_PUSH_IDLE = 0,   /* nothing recorded (empty push / no FIFO) */
    GUI_RECORD_PUSH_RING,       /* accepted into the record ring */
    GUI_RECORD_PUSH_SPILL,      /* accepted into the spill file */
    GUI_RECORD_PUSH_DROPPED,    /* lost: logged and counted */
} gui_record_push_result_t;

enum { GUI_RECORD_SEG_RING = 1, GUI_RECORD_SEG_SPILL = 2 };

typedef void (*gui_record_fifo_log_fn)(void *user, int channel, const char *level,
                                       const char *message);

typedef struct {
    buffer_manager_t *bufmgr;    /* required */
    buffer_id_t buf_id;          /* BUF_RECORD_A or BUF_RECORD_B */
    int channel;                 /* 0 = A, 1 = B (log text) */
    size_t sample_bytes;         /* bytes per sample: 2 (int16), 1 (direct u8) */
    uint64_t sample_rate_hz;     /* for t= in log lines; 0 omits it */
    size_t max_push_bytes;       /* largest single push (sizes the hysteresis) */
    size_t max_read_bytes;       /* reader chunk cap (0 = 2 MiB) */
    size_t switch_back_free_bytes; /* SPILL->RING threshold (0 = default) */
    const char *spill_dir;       /* local dir for the spill; NULL/"" = RAM only */
    gui_record_fifo_log_fn log;  /* non-blocking sink; may be NULL */
    void *log_user;

    /* Test hooks; zero in production. */
    bool (*spill_write_hook)(void *hook_user, const void *bytes, size_t len, uint64_t off);
    void *hook_user;
    bool force_no_punch;         /* recycle-only spill space reuse */
    uint64_t truncate_threshold; /* recycle mode: truncate a drained file above this (0 = 256 MiB) */
    uint32_t seg_cap_override;   /* segment log capacity (power of two; 0 = sized from the ring) */
} gui_record_fifo_config_t;

typedef struct {
    const void *data;
    size_t len;
    uint32_t src;                /* GUI_RECORD_SEG_RING / _SPILL */
} gui_record_fifo_chunk_t;

/* Totals, readable from any thread at any time. */
typedef struct {
    uint64_t accepted_bytes;     /* pushed and accepted (ring + spill) */
    uint64_t ring_bytes;
    uint64_t spill_bytes;
    uint64_t spill_pushes;
    uint64_t dropped_bytes;
    uint64_t dropped_pushes;
    uint64_t consumed_bytes;     /* handed to and consumed by the reader */
    uint64_t read_lost_bytes;    /* spill bytes the reader could not read back */
    uint64_t segments;
    uint64_t episodes;
    uint64_t max_backlog_bytes;  /* peak spill backlog */
    uint64_t spill_write_errors;
    uint64_t recycles;
} gui_record_fifo_stats_t;

typedef struct gui_record_fifo_seg {
    uint32_t src;
    uint32_t pad;
    uint64_t start_seq;          /* first stream byte of the segment */
    uint64_t file_off;           /* SPILL: spill file offset of that byte */
} gui_record_fifo_seg_t;

typedef struct gui_record_spill_file {
#if defined(_WIN32) || defined(_WIN64)
    void *wh;                    /* HANDLE: producer (write) */
    void *rh;                    /* HANDLE: reader (read), own file object */
#else
    int fd;
#endif
    bool open;
    char path[1024];
} gui_record_spill_file_t;

typedef struct gui_record_fifo {
    gui_record_fifo_config_t cfg;
    size_t ring_size;
    size_t H;                    /* switch-back free-space threshold */
    size_t max_read;
    uint32_t seg_cap;
    gui_record_fifo_seg_t *segs;
    gui_record_spill_file_t spill;
    bool spill_ok;
    uint64_t truncate_threshold;

    /* Producer-owned (single producer at a time; the gate serializes). */
    uint64_t p_seq;
    uint32_t p_src;
    uint64_t p_spill_cursor;     /* next spill file offset */
    uint64_t p_spill_hw;         /* spill file high-water mark */
    uint64_t p_spill_produced;
    uint64_t p_log_mark;         /* next backlog log step */
    bool ep_open;
    uint64_t ep_start_seq, ep_end_seq, ep_start_ms, ep_last_spill_ms;
    uint64_t ep_spilled, ep_peak, ep_drops;
    uint32_t ep_segs;
    uint64_t drop_log_count;

    /* Shared: producer publishes, reader consumes. */
    _Alignas(64) atomic_uint_fast64_t pub_seq;
    atomic_uint_fast64_t seg_tail;
    atomic_uint_fast64_t spill_produced;
    _Alignas(64) atomic_uint_fast64_t seg_head;
    atomic_uint_fast64_t spill_consumed;
    atomic_uint_fast64_t cons_seq;
    atomic_bool closed;
    atomic_bool punch_ok;

    /* Reader-owned. */
    _Alignas(64) uint64_t c_seq;
    uint64_t c_seg;
    uint64_t c_spill_consumed;
    uint64_t c_punched;          /* file bytes below this are punched */
    uint8_t *c_buf;              /* spill read buffer (max_read bytes) */
    size_t c_out;                /* bytes handed out by the last next() */
    uint32_t c_out_src;

    /* Stats. */
    atomic_uint_fast64_t st_ring_bytes, st_spill_bytes, st_spill_pushes;
    atomic_uint_fast64_t st_drop_bytes, st_drop_pushes, st_read_lost;
    atomic_uint_fast64_t st_segments, st_episodes, st_max_backlog;
    atomic_uint_fast64_t st_spill_write_errors, st_recycles;
} gui_record_fifo_t;

/* Sets the FIFO up and opens the spill file in cfg->spill_dir (unless it is
 * NULL/empty). Returns 0, or -1 with err filled; a spill that cannot be
 * opened is not an error -- the FIFO runs RAM-only and *spill_error says why.
 * The record ring is initialized here (never lazily on the capture side). */
int gui_record_fifo_init(gui_record_fifo_t *f, const gui_record_fifo_config_t *cfg,
                         char *err, size_t err_size);

/* Releases everything; the spill file disappears with its handle. */
void gui_record_fifo_destroy(gui_record_fifo_t *f);

/* Producer. Never blocks; bytes must be a whole number of samples. */
gui_record_push_result_t gui_record_fifo_push(gui_record_fifo_t *f, const void *bytes,
                                              size_t len, uint32_t frame_index);

/* Producer: no more pushes will come (call after the gate is idle). */
void gui_record_fifo_close(gui_record_fifo_t *f);

/* Reader. Up to max_bytes (0 = the configured cap) of the oldest unread
 * stream bytes, contiguous: 1 = chunk filled, 0 = nothing within timeout_ms,
 * -1 = end of stream (closed and fully consumed). The chunk stays valid
 * until gui_record_fifo_consume, which must follow every 1 with len <=
 * chunk.len (a shorter len re-offers the rest next time). */
int gui_record_fifo_next(gui_record_fifo_t *f, size_t max_bytes, int timeout_ms,
                         gui_record_fifo_chunk_t *chunk);
void gui_record_fifo_consume(gui_record_fifo_t *f, size_t len);

/* Bytes in the spill not yet consumed (any thread). */
uint64_t gui_record_fifo_spill_backlog(const gui_record_fifo_t *f);
/* Bytes accepted but not yet consumed, ring + spill (any thread). */
uint64_t gui_record_fifo_backlog(const gui_record_fifo_t *f);
bool gui_record_fifo_spill_available(const gui_record_fifo_t *f);
const char *gui_record_fifo_spill_path(const gui_record_fifo_t *f);
void gui_record_fifo_get_stats(const gui_record_fifo_t *f, gui_record_fifo_stats_t *out);

/* Producer thread (or after close): ends an open overflow episode whose spill
 * has drained and that has had no spill push for idle_ms, logging its
 * summary. push() calls it; the record layer also calls it from the producer
 * on quiet stretches and once after close. */
void gui_record_fifo_maybe_end_episode(gui_record_fifo_t *f, uint64_t now_ms, uint64_t idle_ms);

#endif /* GUI_RECORD_FIFO_H */
