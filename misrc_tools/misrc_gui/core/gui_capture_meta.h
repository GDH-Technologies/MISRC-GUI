/*
 * MISRC GUI - Capture metadata (what the capture is OF, and who ran it)
 *
 * One in-memory record per GUI run, deliberately OUTSIDE gui_settings_t: it
 * is never saved to the settings file, never sent to or taken from a net
 * peer, and never survives a restart.
 *
 *   - Linked: a --session file named an asset (gui_session.c). Every asset
 *     field is read-only for the run; corrections are made in the caller (the
 *     tape database), never here, so the files cannot disagree with it.
 *   - Unlinked: no asset. The operator may type values for this run; they are
 *     written to the capture log and the sidecar and then forgotten.
 *
 * At record start gui_record.c takes a snapshot, so an edit made after the
 * start can never reach that recording's log, sidecar or FLAC tags.
 *
 * ONE descriptor table (gui_capture_meta_fields) drives the session parser,
 * the capture-log block, the sidecar's asset object and the panel's rows, so
 * a field exists in all of them or in none.
 *
 * No raylib here: the meson harness compiles this file on its own.
 *
 * Licensed under GNU GPL v3 or later
 */

#ifndef GUI_CAPTURE_META_H
#define GUI_CAPTURE_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Buffer sizes, NUL included. The contract's caps are these minus one. */
#define GUI_META_ID_CAP        64     /* asset_id, client_id: [A-Za-z0-9_.-]{1,63} */
#define GUI_META_NAME_CAP      256    /* client_name, display_name, label */
#define GUI_META_SHORT_CAP     32     /* format, tape_speed, video_system */
#define GUI_META_NOTES_CAP     8192   /* notes (may hold \n \r \t) */
#define GUI_META_OPERATOR_CAP  128    /* operator */
#define GUI_META_INDEX_CAP     24     /* index as decimal text, "" = unset */
#define GUI_META_PATH_CAP      1024   /* the --session file's path */

/* The largest index accepted (2^53 - 1: exact in every JSON reader). */
#define GUI_META_INDEX_MAX     9007199254740991ULL

/* Tri-state booleans (hifi_audio_equipped, black_and_white). */
#define GUI_META_TRI_UNSET     ((int8_t)-1)
#define GUI_META_TRI_FALSE     ((int8_t)0)
#define GUI_META_TRI_TRUE      ((int8_t)1)

typedef enum {
    GUI_META_OPERATOR_OS_LOGIN = 0,   /* nobody named one: the OS login */
    GUI_META_OPERATOR_SESSION         /* the --session file's "operator" */
} gui_meta_operator_source_t;

typedef struct {
    bool linked;
    char asset_id[GUI_META_ID_CAP];
    char client_id[GUI_META_ID_CAP];
    char client_name[GUI_META_NAME_CAP];
    char display_name[GUI_META_NAME_CAP];
    char index_text[GUI_META_INDEX_CAP];
    char label[GUI_META_NAME_CAP];
    char format[GUI_META_SHORT_CAP];
    char tape_speed[GUI_META_SHORT_CAP];
    char video_system[GUI_META_SHORT_CAP];
    int8_t hifi_audio_equipped;           /* GUI_META_TRI_* */
    int8_t black_and_white;               /* GUI_META_TRI_* */
    char notes[GUI_META_NOTES_CAP];
    char operator_name[GUI_META_OPERATOR_CAP];
    gui_meta_operator_source_t operator_source;
    char session_file[GUI_META_PATH_CAP]; /* "" = no session linked it */
    uint32_t generation;                  /* bumped on every change */
} gui_capture_meta_t;

typedef enum {
    GUI_META_STR = 0,    /* char[] at offset, cap bytes incl. NUL */
    GUI_META_INDEX,      /* char[] of decimal digits, "" = unset (JSON int|null) */
    GUI_META_TRI         /* int8_t GUI_META_TRI_*  (JSON true|false|null) */
} gui_meta_type_t;

/* Field flags */
#define GUI_META_F_ID                0x01u  /* [A-Za-z0-9_.-]{1,63} when non-empty */
#define GUI_META_F_REQUIRED_LINKED   0x02u  /* a session's asset must give it, non-empty */
#define GUI_META_F_UNLINKED_EDITABLE 0x04u  /* the operator may set it when unlinked */
#define GUI_META_F_MULTILINE         0x08u  /* may hold \n \r \t */
#define GUI_META_F_TOP_LEVEL         0x10u  /* a session's top-level key, not under "asset" */

typedef struct {
    const char *key;       /* session / sidecar / log key */
    const char *label;     /* panel label */
    gui_meta_type_t type;
    size_t offset;         /* into gui_capture_meta_t */
    size_t cap;            /* buffer size incl. NUL (0 for TRI) */
    unsigned flags;
} gui_capture_meta_field_t;

#define GUI_META_FIELD_COUNT 13   /* rows in the descriptor table (asserted) */

/* The descriptor table, in its one order: client_name, display_name, index,
 * label, format, tape_speed, video_system, hifi_audio_equipped,
 * black_and_white, notes, asset_id, client_id, operator. */
const gui_capture_meta_field_t *gui_capture_meta_fields(size_t *count);
const gui_capture_meta_field_t *gui_capture_meta_find_field(const char *key);

/* Field accessors on any record (a snapshot or the live one). */
const char *gui_capture_meta_str(const gui_capture_meta_t *m, const gui_capture_meta_field_t *f);
char *gui_capture_meta_str_mut(gui_capture_meta_t *m, const gui_capture_meta_field_t *f);
int8_t gui_capture_meta_tri(const gui_capture_meta_t *m, const gui_capture_meta_field_t *f);
/* The index as a number; false when it is unset (or not digits). */
bool gui_capture_meta_index_value(const gui_capture_meta_t *m, uint64_t *out);

/* Reset to unlinked and empty, operator = the OS login. Call once at startup,
 * before a --session file is applied. */
void gui_capture_meta_init(void);

/* The live record. Main thread only; recording code takes a snapshot. */
const gui_capture_meta_t *gui_capture_meta_get(void);
void gui_capture_meta_snapshot(gui_capture_meta_t *out);

/* Link to an asset (gui_session.c, after the whole file validated): copies
 * every descriptor field from src and sets linked. An empty operator keeps
 * the OS login (source os_login); a non-empty one is source "session". */
void gui_capture_meta_set_linked(const gui_capture_meta_t *src, const char *session_file);

/* A session with no asset: stays unlinked; records the session file and its
 * operator (an empty one keeps the OS login). */
void gui_capture_meta_set_unlinked_session(const char *operator_name, const char *session_file);

/* The panel's edit buffer for descriptor slot `slot` (a STR or INDEX field).
 * False while linked, or when the field is not UNLINKED_EDITABLE. Edits go
 * straight into the live record; call gui_capture_meta_touch() after one. */
bool gui_capture_meta_text_buffer(size_t slot, char **dst, size_t *cap);

/* Set a TRI field (false while linked / not editable / not TRI). Touches. */
bool gui_capture_meta_set_tri(size_t slot, int8_t value);

/* Record that an unlinked value changed. Clears the "used by a recording since
 * the last edit" mark the toolbar shows. */
void gui_capture_meta_touch(void);

/* Record start: the current values were used by a recording. The toolbar
 * shows it (unlinked only) until the next edit, so stale per-run values from
 * the last tape are noticed before the next one. */
void gui_capture_meta_mark_used(void);
bool gui_capture_meta_used_since_edit(void);

/* The OS login: getpwuid(geteuid())->pw_name, then $USER, $LOGNAME, then
 * "unknown" (Windows: %USERNAME%, then "unknown"). */
void gui_capture_meta_os_login(char *dst, size_t cap);

/* One-line escape for the capture log: \\ -> \\\\, \n -> \n, \r -> \r,
 * \t -> \t, any other C0 byte or DEL -> \xHH. Returns the length the escaped
 * text needs (excluding NUL); out is truncated (on a byte boundary that never
 * splits an escape) when cap is smaller. */
size_t gui_capture_meta_log_escape(const char *in, char *out, size_t cap);

/* Strict UTF-8: no overlongs, no surrogates, nothing above U+10FFFF. */
bool gui_capture_meta_utf8_valid(const char *s, size_t len);

/* The capture-log value of field f: "(empty)" for an empty string, "(unset)"
 * for an unset index/tri, "true"/"false" for a tri, the escaped text
 * otherwise. Returns the length needed, like gui_capture_meta_log_escape. */
size_t gui_capture_meta_format_log_value(const gui_capture_meta_t *m,
                                         const gui_capture_meta_field_t *f,
                                         char *out, size_t cap);

/* "session" / "os_login" */
const char *gui_capture_meta_operator_source_name(gui_meta_operator_source_t src);

#endif /* GUI_CAPTURE_META_H */
