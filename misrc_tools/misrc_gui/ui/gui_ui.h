#ifndef GUI_UI_H
#define GUI_UI_H

#include "../core/gui_app.h"
#include "clay.h"
#include "gui_ui_scale.h"

// UI colors
#define COLOR_BG              (Color){ 30, 30, 35, 255 }
#define COLOR_PANEL_BG        (Color){ 40, 40, 48, 255 }
#define COLOR_TOOLBAR_BG      (Color){ 50, 50, 58, 255 }
#define COLOR_BUTTON          (Color){ 65, 65, 75, 255 }
#define COLOR_BUTTON_HOVER    (Color){ 80, 80, 92, 255 }
#define COLOR_BUTTON_ACTIVE   (Color){ 95, 95, 110, 255 }
#define COLOR_TEXT            (Color){ 220, 220, 230, 255 }
#define COLOR_TEXT_DIM        (Color){ 140, 140, 155, 255 }
#define COLOR_CHANNEL_A       (Color){ 80, 220, 100, 255 }
#define COLOR_CHANNEL_B       (Color){ 220, 200, 80, 255 }
#define COLOR_SYNC_GREEN      (Color){ 50, 200, 80, 255 }
#define COLOR_SYNC_RED        (Color){ 200, 60, 60, 255 }
#define COLOR_CLIP_RED        (Color){ 255, 50, 50, 255 }
#define COLOR_GRID            (Color){ 90, 90, 110, 130 }
#define COLOR_GRID_MAJOR      (Color){ 130, 130, 160, 180 }
#define COLOR_METER_BG        (Color){ 25, 25, 30, 255 }
#define COLOR_METER_GREEN     (Color){ 0, 255, 0, 255 }
#define COLOR_METER_YELLOW    (Color){ 255, 230, 0, 255 }
#define COLOR_METER_RED       (Color){ 255, 0, 0, 255 }

// Font sizes - adjust these to change all UI text sizes
#define FONT_SIZE_TITLE        26    // Main title "MISRC Capture"
#define FONT_SIZE_HEADING      20    // Section headings like "Statistics"
#define FONT_SIZE_NORMAL       18    // Normal UI text, buttons, labels
#define FONT_SIZE_DROPDOWN     18    // Dropdown menu headers
#define FONT_SIZE_DROPDOWN_OPT 16   // Dropdown menu option items
#define FONT_SIZE_STATUS       18    // Status bar text
#define FONT_SIZE_OSC_LABEL    20    // Oscilloscope channel labels "CH A", "CH B"
#define FONT_SIZE_OSC_SCALE    16    // Oscilloscope scale ticks "+1", "0", "-1"
#define FONT_SIZE_OSC_DIV      18    // Oscilloscope division labels "1ms/div", "1kHz/div"
#define FONT_SIZE_OSC_MSG      26    // Oscilloscope messages like "No Signal"
#define FONT_SIZE_VU_SCALE     16    // VU meter scale ticks
#define FONT_SIZE_VU_CLIP      14    // VU meter clip indicators "+CLIP", "-CLIP"
#define FONT_SIZE_STATS_LABEL  18    // Per-channel stats panel channel label
#define FONT_SIZE_STATS        16    // Per-channel stats panel text

// UI Layout functions
void gui_render_layout(gui_app_t *app);
void gui_handle_interactions(gui_app_t *app);
// Reconcile mode/device constraints immediately after device selection changes.
void gui_ui_sync_capture_mode_state(gui_app_t *app);
void gui_ui_sync_android_keyboard_state(void);

/* Server side of the net setter (GET /set): apply one key on the main thread
 * with the same refusals and side effects the settings panel's click
 * handlers have. Returns the HTTP status for the reply: 200 with the
 * canonical value in msg, or 400/409/422 with the reason. */
int gui_ui_apply_remote_setting(gui_app_t *app, const char *key, const char *value,
                                char *msg, size_t msg_cap);
/* True while a settings text field is being typed into (the client-mode
 * view swap defers sending string fields until the edit ends). */
bool gui_ui_text_edit_active(void);

// Application-controlled UI zoom. Layout and pointer coordinates remain in
// logical units while the renderer scales them into the window framebuffer.
void gui_ui_set_scale_percent(int percent);
float gui_ui_get_scale_factor(void);
// Physical framebuffer pixels per logical UI unit. This includes both the
// application zoom and any OS backing scale such as macOS Retina.
Vector2 gui_ui_get_render_scale(void);

// Asks the platform what scale the current display wants, as a percent on the
// supported steps. Wraps the pure policy in gui_ui_scale.h with the raylib
// queries it must not depend on directly (that file is compiled standalone by
// the CI guard). Safe to call every frame; requires an open window.
int gui_ui_detect_display_scale_percent(void);
void gui_ui_show_scale_hud(int percent);
int gui_ui_get_layout_width(void);
int gui_ui_get_layout_height(void);
Vector2 gui_ui_get_mouse_position(void);

// Check if UI consumed the current frame's click (prevents click-through to oscilloscope)
bool gui_ui_click_consumed(void);

// Text measurement function (from clay_renderer_raylib.c)
Clay_Dimensions Raylib_MeasureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData);

/* Draws a gear of outer radius r centred on (cx, cy), with raylib primitives
 * rather than a glyph or a texture. Exported because the preview panel's
 * overlay is drawn straight to the canvas and cannot use the Clay custom
 * element the toolbar and channel gears go through. */
void gui_draw_gear_icon(float cx, float cy, float r, Color col);

// Raylib render function (from clay_renderer_raylib.c)
void Clay_Raylib_Render(Clay_RenderCommandArray renderCommands, Font* fonts);

/* ---- editable text fields ------------------------------------------------
 *
 * The edit state (which field, caret, selection) stays private to gui_ui.c;
 * only the identity of a field and the three calls a renderer needs are
 * exported. A settings surface that lives outside gui_ui.c -- the USB
 * Reference Video dialog -- has editable tags like any other, and could not
 * draw them otherwise.
 *
 * ui_text_field_t is an identity, not an index: gui_ui_text_field_can_edit()
 * decides per field whether editing is allowed right now, which is where a
 * field belonging to a window other than the settings panel is handled. */
typedef enum {
    UI_TEXT_FIELD_NONE = 0,
    UI_TEXT_FIELD_OUTPUT_BASE_NAME,
    UI_TEXT_FIELD_OUTPUT_PATH,
    UI_TEXT_FIELD_FLAC_AFFINITY,
    UI_TEXT_FIELD_RF_TAG_A,
    UI_TEXT_FIELD_RF_TAG_B,
    UI_TEXT_FIELD_AUDIO_TAG_4CH,
    UI_TEXT_FIELD_VIDEO_TAG,
    UI_TEXT_FIELD_CC_TAG,
    UI_TEXT_FIELD_AUDIO_TAG_12,
    UI_TEXT_FIELD_AUDIO_TAG_34,
    UI_TEXT_FIELD_AUDIO_LABEL_1,
    UI_TEXT_FIELD_AUDIO_LABEL_2,
    UI_TEXT_FIELD_AUDIO_LABEL_3,
    UI_TEXT_FIELD_AUDIO_LABEL_4,
    UI_TEXT_FIELD_LEVEL_AUTOSTOP_LEVEL,    // Level autostop threshold (normalized 0.1-0.8)
    UI_TEXT_FIELD_LEVEL_AUTOSTOP_DURATION,  // Level autostop sustain seconds
    UI_TEXT_FIELD_INGEST_PROJECT,
    UI_TEXT_FIELD_INGEST_TAPE_ID,
    UI_TEXT_FIELD_INGEST_TAPE_FORMAT,
    UI_TEXT_FIELD_INGEST_TAPE_SIZE,
    UI_TEXT_FIELD_INGEST_TAPE_SPEED,
    UI_TEXT_FIELD_INGEST_TAPE_CONDITION,
    UI_TEXT_FIELD_INGEST_OPERATOR,
    UI_TEXT_FIELD_INGEST_LOCATION,
    UI_TEXT_FIELD_INGEST_NOTES,
    UI_TEXT_FIELD_RTLSDR_FREQ,         // RTL-SDR center frequency (Hz, digits only)
    UI_TEXT_FIELD_NET_SERVER_PORT,      // Network server port (digits only)
    UI_TEXT_FIELD_NET_CLIENT_HOST,      // Network client server host (IP/hostname)
    UI_TEXT_FIELD_NET_CLIENT_PORT,      // Network client server port (digits only)
} ui_text_field_t;

bool gui_ui_is_text_field_active(ui_text_field_t field);
void gui_ui_render_active_text(ui_text_field_t field, const char *text,
                               int font_size, int font_id, Color text_color);
void gui_ui_begin_text_edit(gui_app_t *app, ui_text_field_t field,
                            Clay_ElementId element_id, float pad_left, float pad_right);

/* True while a settings surface must refuse edits: no capture may be running,
 * and on a net client the server must be reachable and idle. */
bool gui_ui_settings_locked(const gui_app_t *app);

int gui_ui_clamp_int(int value, int min_value, int max_value);

/* Control on Linux and Windows, Command on macOS. */
bool gui_ui_primary_mod_down(void);

#endif // GUI_UI_H
