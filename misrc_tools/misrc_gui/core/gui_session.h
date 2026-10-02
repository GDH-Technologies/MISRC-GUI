/*
 * MISRC GUI - Session launch file (--session <path.json>)
 *
 * A caller (a capture scheduler, a script, a tape database front-end) can
 * launch the GUI pre-filled for ONE capture without touching the operator's
 * saved settings:
 *
 *   {"schema": "misrc-gui.session/1",
 *    "output_path": "/some/dir",
 *    "output_base_name": "Some_Tape",
 *    "ingest": {"project": "...", "tape_id": "...", "tape_format": "VHS",
 *               "tape_speed": "SP", "tape_size": "...", "tape_condition": "...",
 *               "operator": "...", "location": "...", "notes": "..."},
 *    "log_tags": {"key": "value", ...}}
 *
 * Every key but "schema" is optional; only the keys present overlay. The
 * overlay lives in memory only: gui_settings_save() writes every overlaid
 * field with its PRE-overlay value (overlaid fields are session-scoped, even
 * if the operator edits them during the session). log_tags are opaque
 * key/value pairs, in file order, written to the capture log at record start
 * as "Session tag <key>: <value>".
 *
 * No raylib here, so a harness can compile it with the settings table alone.
 *
 * Licensed under GNU GPL v3 or later
 */

#ifndef GUI_SESSION_H
#define GUI_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include "gui_settings.h"

#define GUI_SESSION_SCHEMA          "misrc-gui.session/1"
#define GUI_SESSION_MAX_FILE_BYTES  65536
#define GUI_SESSION_MAX_TAGS        16
#define GUI_SESSION_TAG_KEY_MAX     64    /* incl. NUL: keys are 1-63 chars of [A-Za-z0-9_.-] */
#define GUI_SESSION_TAG_VALUE_MAX   256   /* incl. NUL */
#define GUI_SESSION_INGEST_COUNT    9
#define GUI_SESSION_VALUE_MAX       512   /* incl. NUL: parse buffer for overlay values */

typedef struct {
    char key[GUI_SESSION_TAG_KEY_MAX];
    char value[GUI_SESSION_TAG_VALUE_MAX];
} gui_session_tag_t;

/* A parsed session file. Nothing in it has touched any settings yet. */
typedef struct {
    bool has_output_path;
    char output_path[GUI_SESSION_VALUE_MAX];
    bool has_output_base_name;
    char output_base_name[GUI_SESSION_VALUE_MAX];
    bool has_ingest[GUI_SESSION_INGEST_COUNT];
    char ingest[GUI_SESSION_INGEST_COUNT][GUI_SESSION_VALUE_MAX];
    size_t tag_count;
    gui_session_tag_t tags[GUI_SESSION_MAX_TAGS];
} gui_session_data_t;

/* Parse session-file text. Returns false (and a reason in err) on bad JSON,
 * a missing or unknown "schema", a wrong value type, an over-long value, a
 * control character in a value, or a bad/duplicate/excess log tag. Unknown
 * keys are ignored with a note on stderr. */
bool gui_session_parse_text(const char *text, gui_session_data_t *out,
                            char *err, size_t errcap);

/* Read and parse path, then overlay it onto settings. All or nothing: on any
 * failure settings and the module state are untouched and err says why.
 * On success the session is active: its log tags are available, and
 * gui_settings_save() restores the overlaid fields' pre-overlay values in
 * what it writes. Call once, right after gui_settings_load(). */
bool gui_session_apply_file(const char *path, gui_settings_t *settings,
                            char *err, size_t errcap);

/* Overlay already-parsed data (gui_session_apply_file's second half). */
bool gui_session_apply(const gui_session_data_t *data, gui_settings_t *settings,
                       char *err, size_t errcap);

/* True after a successful apply. */
bool gui_session_active(void);

/* The log tags of the active session, in file order (0 when none). */
size_t gui_session_tag_count(void);
const gui_session_tag_t *gui_session_tag_at(size_t index);

/* Deactivate: forget the tags and the saved originals and stop filtering
 * saves. Settings in memory are left as they are. For tests. */
void gui_session_clear(void);

/* --session-selftest: headless, no window. Exit code 0 = pass. Uses scratch
 * files under TMPDIR/TEMP and never the live settings file. */
int gui_session_selftest_main(void);

#endif /* GUI_SESSION_H */
