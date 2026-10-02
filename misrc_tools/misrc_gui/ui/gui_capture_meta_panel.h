/*
 * MISRC GUI - the Capture Metadata panel (toolbar scroll badge).
 *
 * Shows what the next recording is OF, from core/gui_capture_meta.h:
 *
 *   - Linked (a --session asset): every row read-only, under a banner that
 *     names the client and title and says corrections are made in the toolkit.
 *   - Unlinked: an amber banner -- these values apply to this run only and
 *     are never saved -- and editable rows for the descriptive fields
 *     (free text, index digits only, "- / Yes / No" for the two booleans).
 *     asset_id / client_id stay locked ("- (not linked)"); the operator is
 *     always read-only.
 *   - While recording (this machine's or, on a net client, the effective
 *     recording) everything is locked: the start snapshot is what the files
 *     carry, and an edit now would only mislead.
 *   - When a --session asked for RF channels (rf_channels) and the current
 *     capture_a / capture_b differ from it, the banner says so; when it
 *     asked for B and this seat has no channel B, it says that instead. It
 *     never changes the channels: the operator's toggles stay free.
 *
 * The rows come from the descriptor table, so the panel never names a field.
 * It replaces upstream's render_metadata_window in ui/gui_ui.c, which kept
 * nine ingest strings in the settings file; that file keeps only the wiring
 * (open/close, render call, click call, the text-field hooks). Edits never
 * call gui_settings_save().
 *
 * Shaped like ui/gui_usbref_settings.c: own state, own render, own click
 * handler that reports whether it consumed the press.
 */

#ifndef GUI_CAPTURE_META_PANEL_H
#define GUI_CAPTURE_META_PANEL_H

#include <stdbool.h>
#include <raylib.h>

typedef struct gui_app gui_app_t;

void gui_capture_meta_panel_open(void);
void gui_capture_meta_panel_close(void);
bool gui_capture_meta_panel_is_open(void);
/* Flip open/closed; returns the new state. */
bool gui_capture_meta_panel_toggle(void);

/* May the operator change a value right now? The panel is open, the
 * capture is not linked, nothing is recording (gui_app_effective_recording),
 * and this seat is not a net client that records on the server. */
bool gui_capture_meta_panel_can_edit(const gui_app_t *app);

/* Call from gui_render_layout(), with the other modal windows. */
void gui_capture_meta_panel_render(gui_app_t *app);

typedef enum {
    GUI_META_PANEL_CLICK_NONE = 0,   /* not ours: let it through */
    GUI_META_PANEL_CLICK_CONSUMED,   /* ours */
    GUI_META_PANEL_CLICK_CLOSED      /* ours, and the panel closed */
} gui_meta_panel_click_t;

/* Call from gui_handle_interactions() on a left press while the panel is
 * open, before the toolbar. A press anywhere on the window is consumed; one
 * on the backdrop or the close button closes the panel. */
gui_meta_panel_click_t gui_capture_meta_panel_handle_click(gui_app_t *app);

/* The toolbar badge's background: an accent colour while linked; amber when
 * unlinked and a recording has used the current values since the last edit
 * (stale per-run values from the last tape); otherwise the normal button. */
Color gui_capture_meta_panel_toolbar_color(const gui_app_t *app);

#endif /* GUI_CAPTURE_META_PANEL_H */
