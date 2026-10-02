/*
 * MISRC GUI - Capture metadata sidecar ({base}_{dateTag}_capture_meta.json)
 *
 * One small JSON file per recording, next to the capture log and with the
 * same stem, that says what the capture is of (the asset from the toolkit,
 * or what the operator typed for an unlinked run), who ran it, and which
 * files it produced. A machine reads it; nobody has to parse the log.
 *
 * gui_record.c writes it twice: with state "recording" just before record
 * start returns RECORD_OK (so a crash still leaves the link on disk), and
 * with state "complete" at the end of finalize, when the end time, the
 * duration, the sample counts and the file sizes are known. Both writes are
 * atomic (temp file, fsync, rename), so a reader sees one whole version or
 * the other, never half of one. A failed write is a warning in the capture
 * log; it never stops a recording.
 *
 * Every file name in it is a basename: the files sit next to the sidecar.
 *
 * No raylib here: the meson harness compiles this file on its own.
 *
 * Licensed under GNU GPL v3 or later
 */

#ifndef GUI_CAPTURE_SIDECAR_H
#define GUI_CAPTURE_SIDECAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/gui_capture_meta.h"

#define GUI_CAPTURE_SIDECAR_SCHEMA  "misrc-gui.capture-meta/1"
#define GUI_CAPTURE_SIDECAR_SUFFIX  "_capture_meta.json"

/* files.audio's keys, in order. */
#define GUI_CAPTURE_SIDECAR_AUDIO_COUNT 7
extern const char *const gui_capture_sidecar_audio_keys[GUI_CAPTURE_SIDECAR_AUDIO_COUNT];

typedef struct {
    const char *name;           /* basename; NULL = channel not recorded (JSON null) */
    unsigned bits;
    uint64_t sample_rate_hz;
    bool have_samples;          /* false = null (while recording) */
    uint64_t samples;
    bool have_bytes;            /* false = null (while recording) */
    uint64_t bytes;
} gui_capture_sidecar_rf_t;

typedef struct {
    bool complete;                          /* "complete" vs "recording" */
    const gui_capture_meta_t *meta;         /* the record-start snapshot */
    const char *misrc_gui_version;
    const char *computer_name;
    const char *device_name;
    const char *device_type;
    const char *capture_format;             /* "FLAC" / "RAW" */
    const char *started_at;                 /* UTC "YYYY-MM-DDTHH:MM:SSZ" */
    const char *ended_at;                   /* NULL = null */
    bool have_capture_seconds;
    double capture_seconds;
    const char *output_path;
    const char *base_name;
    const char *log_file;                   /* basename */
    /* "rf_channels": {"requested": {a, b} | null, "recorded": {a, b}}.
     * requested comes from meta (null when the session asked for nothing);
     * recorded is what the recording latched at start. */
    bool rf_recorded_a, rf_recorded_b;
    gui_capture_sidecar_rf_t rf_a, rf_b;
    const char *video;                      /* basename or NULL */
    const char *closed_captions;            /* basename or NULL */
    const char *audio[GUI_CAPTURE_SIDECAR_AUDIO_COUNT];  /* basenames or NULL */
    bool have_result;                       /* false = "result": null */
    uint64_t drops;
    uint64_t waits;
} gui_capture_sidecar_t;

/* Format the sidecar JSON into buf (NUL-terminated, truncated if cap is
 * short). Returns the length the whole text needs, excluding the NUL. */
size_t gui_capture_sidecar_format(const gui_capture_sidecar_t *s, char *buf, size_t cap);

/* The same, into a malloc'd buffer the caller frees. NULL on no memory. */
char *gui_capture_sidecar_format_alloc(const gui_capture_sidecar_t *s, size_t *len_out);

/* Write text to path atomically: path.tmp.<pid>, fflush, fsync, rename
 * (MoveFileExA REPLACE_EXISTING|WRITE_THROUGH on Windows). On failure the
 * temp file is removed, path is untouched, and err says why. */
bool gui_capture_sidecar_write_atomic(const char *path, const char *text, size_t len,
                                      char *err, size_t errcap);

/* The part of a path after its last separator ('/' everywhere, also '\\' on
 * Windows). Never NULL for a non-NULL path. */
const char *gui_capture_sidecar_basename(const char *path);

#endif /* GUI_CAPTURE_SIDECAR_H */
