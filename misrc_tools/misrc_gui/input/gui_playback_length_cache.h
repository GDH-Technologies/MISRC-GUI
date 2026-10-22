/* Persistent grammar-validated completed-scan sample counts. GPL-3.0-or-later. */
#ifndef GUI_PLAYBACK_LENGTH_CACHE_H
#define GUI_PLAYBACK_LENGTH_CACHE_H

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>

/* Native identity and high-resolution modification time plus bounded content
 * fingerprints. Padding is never serialized or compared. This is a cache key,
 * not a cryptographic integrity proof of the complete input file. */
typedef struct {
    uint64_t file_size;
    uint64_t file_device;
    uint64_t file_id;
    int64_t mtime_seconds;
    uint32_t mtime_nanoseconds;
    uint32_t head_bytes;
    uint32_t tail_bytes;
    uint64_t head_hash;
    uint64_t tail_hash;
} gui_playback_length_cache_key_t;

/* Reads at most the first and last 4096 bytes. Rejects nonregular/empty files
 * and observed metadata changes during these reads. Never modifies input. */
bool gui_playback_length_cache_snapshot(const char *filepath,
                                       gui_playback_length_cache_key_t *key);
bool gui_playback_length_cache_keys_equal(const gui_playback_length_cache_key_t *a,
                                         const gui_playback_length_cache_key_t *b);

/* Compares identity/size/mtime of the already-open regular input with a key.
 * Does not read, seek, flush, close, or check fingerprints. The caller keeps
 * file alive for the call. Use this before accepting a pathname snapshot for
 * a decoder that could still hold the previous file after path replacement. */
bool gui_playback_length_cache_matches_open_file(FILE *file,
                                                const gui_playback_length_cache_key_t *key);

/* Miss/error/disabled leaves *samples=0. MISRC_PLAYBACK_CACHE_DIR overrides the
 * user cache directory; an existing empty value disables lookup and store.
 * V2 records/default playback-length-v2 directories isolate former header-only
 * V1 scan results; V1 records are rejected even in an override directory.
 * Store only a positive count from complete frame-grammar/integrity validation
 * after comparing input snapshots taken before and after the scan.
 * Cancellation/errors must not be
 * stored. Both operations tolerate concurrent callers and atomic replacement.
 * Cache loss/corruption/unwritable directories only cause a miss/store failure. */
bool gui_playback_length_cache_lookup(const gui_playback_length_cache_key_t *key,
                                     uint64_t *samples);
bool gui_playback_length_cache_store(const gui_playback_length_cache_key_t *key,
                                    uint64_t samples);

#endif
