/*
 * MISRC GUI - USB video preview panel
 *
 * Wraps the gui_preview_v4l2 singleton in a panel view. The stream, the frame
 * slots and the texture all live in the reader; this holds only per-panel UI
 * state, because several panel slots may show the same one device.
 */

#ifndef GUI_PREVIEW_PANEL_H
#define GUI_PREVIEW_PANEL_H

void gui_preview_panel_register(void);

/* Close this panel's own overlay dropdowns. They live in private panel state
 * rather than the gui_dropdown registry, so opening the channel gear does not
 * close them and they would paint through its popover. */
void gui_preview_close_overlays(void *state);

#endif /* GUI_PREVIEW_PANEL_H */
