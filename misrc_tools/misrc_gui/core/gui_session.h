/*
 * MISRC GUI - Session launch file (--session <path.json>)
 *
 * A caller (a capture scheduler, a tape database front-end, a script) can
 * launch the GUI pre-filled for ONE capture without touching the operator's
 * saved settings:
 *
 *   {"schema": "misrc-gui.session/1",
 *    "output_path": "/some/dir",
 *    "output_base_name": "Some_Tape",
 *    "operator": "Reece",
 *    "asset": {"asset_id": "a_019", "client_id": "c_7",
 *              "client_name": "Kuhn Family", "display_name": "Christmas 1994",
 *              "index": 3, "label": "Tape 3", "format": "VHS",
 *              "tape_speed": "SP", "video_system": "NTSC",
 *              "hifi_audio_equipped": true, "black_and_white": null,
 *              "notes": "..."}}
 *
 * Every key but "schema" is optional. output_path / output_base_name overlay
 * the settings in memory only: gui_settings_save() writes their PRE-overlay
 * values (they are session-scoped, even if the operator edits them). An
 * "asset" LINKS the capture (gui_capture_meta.h): its fields are read-only
 * for the run and reach the capture log, the capture-meta sidecar and the RF
 * FLAC tags. When "asset" is present, asset_id, client_id, client_name,
 * display_name, label and format are required and non-empty. Types are
 * strict (index is a JSON integer >= 0 or null; the two booleans are
 * true/false/null), values are UTF-8 within the field caps, and only notes
 * may hold a line break or tab (CRLF is stored as LF). Unknown keys --
 * including the retired "ingest" and "log_tags" -- are ignored with a note
 * on stderr.
 *
 * The file is applied whole or not at all: any refusal leaves the settings
 * AND the capture metadata exactly as they were.
 *
 * No raylib here.
 *
 * Licensed under GNU GPL v3 or later
 */

#ifndef GUI_SESSION_H
#define GUI_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include "gui_settings.h"
#include "gui_capture_meta.h"

#define GUI_SESSION_SCHEMA          "misrc-gui.session/1"
#define GUI_SESSION_MAX_FILE_BYTES  65536
#define GUI_SESSION_VALUE_MAX       512   /* incl. NUL: output_path / output_base_name */

/* A parsed, fully validated session file. Nothing in it has touched any
 * settings or the capture metadata yet. */
typedef struct {
    bool has_output_path;
    char output_path[GUI_SESSION_VALUE_MAX];
    bool has_output_base_name;
    char output_base_name[GUI_SESSION_VALUE_MAX];
    bool has_operator;
    char operator_name[GUI_META_OPERATOR_CAP];
    bool has_asset;
    gui_capture_meta_t asset;   /* the descriptor fields; linked is set on apply */
} gui_session_data_t;

/* Parse and validate session-file text. Returns false (and a reason naming
 * the field in err) on bad JSON, a missing or unknown "schema", a wrong value
 * type, a value over its cap, invalid UTF-8, a control character, a bad id,
 * a duplicate key, or a missing required asset field. */
bool gui_session_parse_text(const char *text, gui_session_data_t *out,
                            char *err, size_t errcap);

/* Read and parse path, then apply it: the settings overlay, then the capture
 * metadata link. All or nothing: on any failure the settings, the capture
 * metadata and the module state are untouched and err says why. On success
 * gui_settings_save() restores the overlaid fields' pre-overlay values in
 * what it writes. Call once, right after gui_settings_load() and
 * gui_capture_meta_init(). */
bool gui_session_apply_file(const char *path, gui_settings_t *settings,
                            char *err, size_t errcap);

/* Apply already-parsed data (gui_session_apply_file's second half).
 * session_path is recorded as the sidecar's session_file. */
bool gui_session_apply(const gui_session_data_t *data, gui_settings_t *settings,
                       const char *session_path, char *err, size_t errcap);

/* True after a successful apply. */
bool gui_session_active(void);

/* Deactivate: forget the saved originals and stop filtering saves. Settings
 * and capture metadata in memory are left as they are. For tests. */
void gui_session_clear(void);

/* --session-selftest: headless, no window. Exit code 0 = pass. Uses scratch
 * files under TMPDIR/TEMP and never the live settings file. */
int gui_session_selftest_main(void);

#endif /* GUI_SESSION_H */
