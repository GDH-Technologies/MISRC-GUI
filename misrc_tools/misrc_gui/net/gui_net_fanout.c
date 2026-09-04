/*
 * MISRC GUI - Broadcast fanout feeding the /rf and /baseband streams.
 * See gui_net_fanout.h for the model.
 */
#include "gui_net_fanout.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct net_chunk {
    struct net_chunk *next;
    uint64_t seq;       /* position in the chunk sequence */
    uint64_t off;       /* absolute stream offset of data[0] */
    size_t len;
    uint8_t data[];
};

void net_fanout_init(net_fanout_t *f, size_t max_bytes) {
    memset(f, 0, sizeof(*f));
    f->max_bytes = max_bytes ? max_bytes : NET_FANOUT_DEFAULT_MAX_BYTES;
    net_mutex_init(&f->mtx);
    net_cond_init(&f->cond);
    f->initialized = true;
}

/* Lock held. Free the oldest chunk. Chunks only ever leave from the head, so
 * "seq >= head_seq" is the liveness test for any cached chunk pointer. */
static void fanout_pop_head(net_fanout_t *f) {
    net_chunk_t *c = f->head;
    f->head = c->next;
    if (!f->head) f->tail = NULL;
    f->head_seq = c->seq + 1;
    f->head_off = c->off + c->len;
    f->live_bytes -= c->len;
    f->live_chunks--;
    free(c);
}

/* Lock held. Free every chunk that no subscriber still needs: everything
 * below the lowest cursor, or everything when nobody is subscribed. */
static void fanout_trim(net_fanout_t *f) {
    uint64_t low = f->next_seq;
    for (const net_fanout_sub_t *s = f->subs; s; s = s->next) {
        if (s->seq < low) low = s->seq;
    }
    while (f->head && f->head->seq < low) fanout_pop_head(f);
}

/* Lock held; requires head_seq <= s->seq < next_seq. Returns the chunk the
 * cursor is in, reusing the cached pointer when it is still alive. */
static net_chunk_t *fanout_locate(net_fanout_t *f, net_fanout_sub_t *s) {
    net_chunk_t *c = s->cur;
    if (c && s->cur_seq >= f->head_seq) {
        if (s->cur_seq == s->seq) return c;
        if (s->cur_seq + 1 == s->seq && c->next) {
            s->cur = c->next;
            s->cur_seq = s->seq;
            return s->cur;
        }
    }
    for (c = f->head; c && c->seq < s->seq; c = c->next) {}
    s->cur = c;
    s->cur_seq = c ? c->seq : 0;
    return c;
}

void net_fanout_shutdown(net_fanout_t *f) {
    if (!f || !f->initialized) return;
    net_mutex_lock(&f->mtx);
    f->shutdown = true;
    net_cond_broadcast(&f->cond);
    net_mutex_unlock(&f->mtx);
}

void net_fanout_destroy(net_fanout_t *f) {
    if (!f || !f->initialized) return;
    net_mutex_lock(&f->mtx);
    f->shutdown = true;
    net_cond_broadcast(&f->cond);
    if (f->subscribers > 0) {
        /* Contract violation: the caller must join its reader threads first. */
        fprintf(stderr, "[NET] fanout destroyed with %d subscriber(s) still attached\n",
                f->subscribers);
    }
    while (f->head) fanout_pop_head(f);
    net_mutex_unlock(&f->mtx);
    net_cond_destroy(&f->cond);
    net_mutex_destroy(&f->mtx);
    f->initialized = false;
}

void net_fanout_push(net_fanout_t *f, const void *data, size_t len) {
    if (!f || !f->initialized || !data || len == 0) return;
    net_mutex_lock(&f->mtx);
    if (f->shutdown || f->subscribers <= 0) {
        net_mutex_unlock(&f->mtx);
        return;
    }
    net_chunk_t *c = (net_chunk_t *)malloc(sizeof(*c) + len);
    if (!c) {
        net_mutex_unlock(&f->mtx);
        return;
    }
    c->next = NULL;
    c->seq = f->next_seq;
    c->off = f->pushed_bytes;
    c->len = len;
    memcpy(c->data, data, len);
    if (f->tail) f->tail->next = c; else f->head = c;
    f->tail = c;
    f->next_seq++;
    f->pushed_bytes += len;
    f->live_bytes += len;
    f->live_chunks++;
    /* Over budget: drop the oldest chunks until this one fits. A cursor that
     * pointed into dropped data is re-synced by its owner's next read. */
    while (f->live_bytes > f->max_bytes && f->head != c) fanout_pop_head(f);
    net_cond_broadcast(&f->cond);
    net_mutex_unlock(&f->mtx);
}

void net_fanout_subscribe(net_fanout_t *f, net_fanout_sub_t *s) {
    memset(s, 0, sizeof(*s));
    if (!f || !f->initialized) return;   /* s->f == NULL: reads return -1 */
    s->f = f;
    net_mutex_lock(&f->mtx);
    s->seq = f->next_seq;          /* live edge */
    s->pos = f->pushed_bytes;
    s->next = f->subs;
    f->subs = s;
    f->subscribers++;
    net_mutex_unlock(&f->mtx);
}

void net_fanout_unsubscribe(net_fanout_sub_t *s) {
    if (!s || !s->f) return;
    net_fanout_t *f = s->f;
    net_mutex_lock(&f->mtx);
    for (net_fanout_sub_t **pp = &f->subs; *pp; pp = &(*pp)->next) {
        if (*pp == s) { *pp = s->next; break; }
    }
    if (f->subscribers > 0) f->subscribers--;
    s->f = NULL;
    s->next = NULL;
    s->cur = NULL;
    fanout_trim(f);                /* frees everything once nobody is left */
    net_mutex_unlock(&f->mtx);
}

int net_fanout_read(net_fanout_sub_t *s, void *dst, size_t cap, int timeout_ms) {
    if (!s || !s->f || !dst) return -1;
    net_fanout_t *f = s->f;
    if (cap > (size_t)INT_MAX) cap = (size_t)INT_MAX;
    size_t copied = 0;
    net_mutex_lock(&f->mtx);
    for (;;) {
        if (f->shutdown) break;
        if (s->seq < f->head_seq) {
            /* Fell further behind than the budget: skip to the oldest chunk
             * still held and account for what was dropped underneath us. */
            s->dropped += f->head_off - s->pos;
            s->seq = f->head_seq;
            s->off = 0;
            s->pos = f->head_off;
            s->cur = NULL;
        }
        if (s->seq < f->next_seq) {
            net_chunk_t *c = fanout_locate(f, s);
            if (!c) break;         /* cannot happen while the invariants hold */
            size_t avail = c->len - s->off;
            size_t room = cap - copied;
            size_t n = (avail < room) ? avail : room;
            memcpy((uint8_t *)dst + copied, c->data + s->off, n);
            s->off += n;
            s->pos += n;
            copied += n;
            if (s->off == c->len) {
                s->seq++;
                s->off = 0;
                fanout_trim(f);
            }
            if (copied == cap) break;
            continue;              /* return whatever else is already queued */
        }
        /* Nothing queued for this cursor. */
        if (copied > 0 || timeout_ms <= 0) break;
        if (net_cond_timedwait_ms(&f->cond, &f->mtx, timeout_ms) != 0) break;
        /* Woken: re-check the predicate (spurious wake-ups are harmless). */
    }
    s->delivered += copied;
    int rc = (int)copied;
    if (copied == 0 && f->shutdown) rc = -1;
    net_mutex_unlock(&f->mtx);
    return rc;
}

void net_fanout_stats(net_fanout_t *f, size_t *live_chunks, size_t *live_bytes,
                      uint64_t *pushed_bytes) {
    size_t chunks = 0, bytes = 0;
    uint64_t pushed = 0;
    if (f && f->initialized) {
        net_mutex_lock(&f->mtx);
        chunks = f->live_chunks;
        bytes = f->live_bytes;
        pushed = f->pushed_bytes;
        net_mutex_unlock(&f->mtx);
    }
    if (live_chunks) *live_chunks = chunks;
    if (live_bytes) *live_bytes = bytes;
    if (pushed_bytes) *pushed_bytes = pushed;
}

void net_fanout_sub_stats(const net_fanout_sub_t *s, uint64_t *delivered,
                          uint64_t *dropped, uint64_t *pos) {
    uint64_t d = 0, dr = 0, p = 0;
    if (s && s->f) {
        net_mutex_lock(&s->f->mtx);
        d = s->delivered;
        dr = s->dropped;
        p = s->pos;
        net_mutex_unlock(&s->f->mtx);
    }
    if (delivered) *delivered = d;
    if (dropped) *dropped = dr;
    if (pos) *pos = p;
}
