/*
 * MISRC GUI - Broadcast fanout feeding the /rf and /baseband streams.
 *
 * One producer (the capture tap, on the capture thread) pushes chunks; any
 * number of subscribers (one per streaming HTTP client, each on its own
 * thread) read them back in order. The fanout is a sequence-numbered chunk
 * queue with a per-subscriber cursor:
 *
 *   - every chunk gets a sequence number and an absolute stream offset at
 *     push; a subscriber's cursor is (seq, offset within chunk), so a wake-up
 *     resumes exactly where the previous read stopped -- nothing is replayed;
 *   - a chunk is freed as soon as every subscriber has passed it; with no
 *     subscribers a push allocates nothing at all;
 *   - the queue is bounded by max_bytes. When a subscriber falls further
 *     behind than that, the oldest chunks are dropped and its cursor jumps to
 *     the oldest chunk still held, counting the skipped bytes as dropped. A
 *     slow client therefore loses data instead of stalling the capture thread
 *     or growing memory without bound;
 *   - a read returns queued data before it ever waits, so data that arrived
 *     while the subscriber was busy sending is never slept through, and it
 *     returns as soon as any data is available rather than waiting to fill
 *     the caller's buffer;
 *   - a new subscriber starts at the live edge: it sees only what is pushed
 *     after it subscribed;
 *   - net_fanout_shutdown() wakes every reader and makes reads return -1 from
 *     then on. net_fanout_destroy() must only run once every subscriber has
 *     unsubscribed (server_stop() joins the client threads first).
 *
 * All state is protected by one mutex per fanout. Subscribers copy out under
 * the lock; the producer copies in under the lock. Neither blocks on the
 * other for longer than a memcpy.
 *
 * Testable standalone: misrc_tools/test/net_fanout_harness.c drives this file
 * with the /rf handler's own read loop (ci_guard_tests.py "net fanout runtime").
 */
#ifndef GUI_NET_FANOUT_H
#define GUI_NET_FANOUT_H

#include "gui_net_sync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Queue budget when net_fanout_init() is given 0: about 200 ms of dual-channel
 * 40 MSps RF (160 MB/s), enough to ride out a client's send() stall without
 * letting a stuck client pin memory. */
#define NET_FANOUT_DEFAULT_MAX_BYTES ((size_t)32 * 1024 * 1024)

typedef struct net_chunk net_chunk_t;
struct net_fanout_sub;

typedef struct net_fanout {
    net_mutex_t mtx;
    net_cond_t cond;
    net_chunk_t *head;             /* oldest chunk still held (NULL when empty) */
    net_chunk_t *tail;             /* newest chunk */
    uint64_t head_seq;             /* sequence number of head (== next_seq when empty) */
    uint64_t next_seq;             /* sequence number the next push receives */
    uint64_t head_off;             /* absolute stream offset of head's first byte */
    uint64_t pushed_bytes;         /* absolute stream offset of next_seq's first byte */
    size_t live_bytes;             /* bytes currently held */
    size_t live_chunks;            /* chunks currently held */
    size_t max_bytes;              /* drop-oldest above this */
    struct net_fanout_sub *subs;   /* subscriber list (for trimming) */
    int subscribers;
    bool shutdown;
    bool initialized;
} net_fanout_t;

typedef struct net_fanout_sub {
    net_fanout_t *f;
    struct net_fanout_sub *next;   /* fanout's subscriber list */
    net_chunk_t *cur;              /* cached chunk pointer, valid while cur_seq >= f->head_seq */
    uint64_t cur_seq;              /* sequence number of the chunk cur points at */
    uint64_t seq;                  /* next chunk to read */
    size_t off;                    /* byte offset within it */
    uint64_t pos;                  /* absolute stream offset of the next byte to deliver */
    uint64_t delivered;            /* bytes returned to the caller */
    uint64_t dropped;              /* bytes skipped because the queue trimmed them */
} net_fanout_sub_t;

/* max_bytes == 0 selects NET_FANOUT_DEFAULT_MAX_BYTES. */
void net_fanout_init(net_fanout_t *f, size_t max_bytes);

/* Wake every blocked reader; from now on reads return -1 and pushes are dropped.
 * Safe to call more than once. Does not free anything. */
void net_fanout_shutdown(net_fanout_t *f);

/* Free the queue and the sync primitives. Every subscriber must already have
 * unsubscribed. Implies net_fanout_shutdown(). */
void net_fanout_destroy(net_fanout_t *f);

/* Producer: append a copy of data. Dropped without allocating when there are
 * no subscribers or after shutdown. Never blocks beyond the lock + memcpy. */
void net_fanout_push(net_fanout_t *f, const void *data, size_t len);

/* Subscriber lifecycle. subscribe() starts at the live edge. */
void net_fanout_subscribe(net_fanout_t *f, net_fanout_sub_t *s);
void net_fanout_unsubscribe(net_fanout_sub_t *s);

/* Read up to cap bytes into dst. Returns the byte count as soon as any data is
 * available (queued data is returned without waiting); 0 if nothing arrived
 * within timeout_ms; -1 once the fanout is shut down. */
int net_fanout_read(net_fanout_sub_t *s, void *dst, size_t cap, int timeout_ms);

/* Snapshots for logging and for the harness. Any out pointer may be NULL. */
void net_fanout_stats(net_fanout_t *f, size_t *live_chunks, size_t *live_bytes,
                      uint64_t *pushed_bytes);
void net_fanout_sub_stats(const net_fanout_sub_t *s, uint64_t *delivered,
                          uint64_t *dropped, uint64_t *pos);

#endif /* GUI_NET_FANOUT_H */
