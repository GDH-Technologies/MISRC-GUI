/*
 * MISRC GUI - the USB Reference Video settings dialog.
 *
 * Everything about the USB capture dongle in one window: which device and jack,
 * the video standard and picture shape, the RTSP stream, and closed captions.
 *
 * Why it is its own module rather than more rows in the settings panel:
 *
 *   - ui/gui_ui.c is UPSTREAM's file. The fork had accumulated roughly 800 lines
 *     of reference-video and streaming UI inside it -- markup, click handlers,
 *     two sub-windows and a run of file-scope helpers -- and every one of those
 *     lines is a conflict waiting for the next upstream merge. None of it is
 *     upstream's concern: upstream's settings file knows 109 keys and not one of
 *     them is a preview, stream or caption key.
 *   - The controls were split across two surfaces, the settings panel and the
 *     preview panel's overlay, so half of them were wherever you were not.
 *
 * The dialog is deliberately NOT modal. It has no backdrop, so the picture keeps
 * rendering behind it -- crop and jack changes are things you judge by looking at
 * the tape, and a dimmed, inert preview would make them guesswork. It dismisses
 * on an outside click the same way the channel gear popover does: the click
 * closes the window without being consumed, so it dismisses and acts at once.
 *
 * Shaped like ui/gui_popup.c -- own state, own render, own interaction handler
 * that reports whether it consumed the click -- because that is the established
 * way a UI surface lives outside gui_ui.c here.
 */

#ifndef GUI_USBREF_SETTINGS_H
#define GUI_USBREF_SETTINGS_H

#include <stdbool.h>

typedef struct gui_app gui_app_t;

/* Open/close. The opener is exported rather than a static toggle because two
 * places raise this window -- the gear beside the settings-panel section label
 * and the gear on the preview panel overlay -- and the panel lives in another
 * translation unit entirely. */
void gui_usbref_settings_open(void);
void gui_usbref_settings_close(void);
bool gui_usbref_settings_is_open(void);

/* Call from gui_render_layout(), after the other modal windows. */
void gui_usbref_settings_render(gui_app_t *app);

/* Call from gui_handle_interactions() on a left press, before the settings-panel
 * block. Returns true if the click belongs to this window and must not be seen
 * by anything underneath. A click outside the window closes it and returns
 * false, so the same press still reaches whatever was clicked. */
bool gui_usbref_settings_handle_interactions(gui_app_t *app);

/* ESC. Closes the innermost thing that is open -- the codec sheet or the LAN
 * confirmation before the dialog itself -- and returns true if it closed
 * something, so the caller stops looking for another window to dismiss. */
bool gui_usbref_settings_handle_escape(gui_app_t *app);

/* Start or stop the RTSP stream. Lives here with the rest of the streaming UI,
 * but stays exported because gui_ui_apply_remote_setting() drives the same
 * transition when a net client flips the switch remotely. */
bool gui_usbref_set_rtsp_stream(gui_app_t *app, bool want);

#endif /* GUI_USBREF_SETTINGS_H */
