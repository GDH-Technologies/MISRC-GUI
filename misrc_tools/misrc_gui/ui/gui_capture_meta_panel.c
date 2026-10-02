/* The Capture Metadata panel. See gui_capture_meta_panel.h. */

#include "gui_capture_meta_panel.h"
#include "gui_ui.h"
#include "gui_ui_scale.h"

#include "../core/gui_app.h"
#include "../core/gui_capture_meta.h"
#include "../net/gui_net.h"

#include <clay.h>
#include <raylib.h>
#include <stdio.h>
#include <string.h>

/* Local copies, as ui/gui_usbref_settings.c keeps its own. */
static inline Clay_Color to_clay_color(Color c)
{
    return (Clay_Color){ (float)c.r, (float)c.g, (float)c.b, (float)c.a };
}

static Clay_String make_string(const char *str)
{
    return (Clay_String){ .isStaticallyAllocated = false,
                          .length = (int32_t)strlen(str),
                          .chars = str };
}

/* Linked: a muted take on the text-selection blue. Amber: the record
 * button's finalizing colour, the app's "look at this" tone. */
#define META_COLOR_ACCENT   (Color){ 56, 92, 168, 255 }
#define META_COLOR_AMBER    (Color){ 184, 118, 20, 255 }
#define META_COLOR_AMBER_TX (Color){ 236, 170, 70, 255 }
#define META_COLOR_EDIT_BG  (Color){ 25, 25, 30, 255 }
#define META_COLOR_LOCK_BG  (Color){ 48, 48, 56, 255 }

#define META_LABEL_WIDTH 150
#define META_ROW_HEIGHT  32

static bool s_open = false;

/* Formatted lines Clay draws after the layout pass returns: static, never
 * stack (the "Clay text outlives the layout pass" guard). */
static char s_banner_title[GUI_META_NAME_CAP * 2 + 32];

void gui_capture_meta_panel_open(void)  { s_open = true; }
void gui_capture_meta_panel_close(void) { s_open = false; }
bool gui_capture_meta_panel_is_open(void) { return s_open; }

bool gui_capture_meta_panel_toggle(void)
{
    s_open = !s_open;
    return s_open;
}

/* A net client that forwards Record to the server never records here, so
 * a value typed here would reach no file. */
static bool meta_seat_records_elsewhere(const gui_app_t *app)
{
    return gui_net_is_client(app) && !app->settings.net_client_record_local;
}

bool gui_capture_meta_panel_can_edit(const gui_app_t *app)
{
    if (!app || !s_open) return false;
    if (gui_capture_meta_get()->linked) return false;
    if (gui_app_effective_recording(app)) return false;
    if (meta_seat_records_elsewhere(app)) return false;
    return true;
}

Color gui_capture_meta_panel_toolbar_color(const gui_app_t *app)
{
    if (gui_capture_meta_get()->linked) return META_COLOR_ACCENT;
    if (gui_capture_meta_used_since_edit() && app && !gui_app_effective_recording(app)) {
        return META_COLOR_AMBER;
    }
    return COLOR_BUTTON;
}

/* Linked notes, one wrapped text element per line: Clay's word wrap does
 * not restart its line width at an embedded newline. The lines live in a
 * static copy (Clay reads them after the layout pass returns). */
static char s_note_lines[GUI_META_NOTES_CAP + 1];

static void render_wrapped_lines(const char *text)
{
    snprintf(s_note_lines, sizeof(s_note_lines), "%s", text);
    char *line = s_note_lines;
    int k = 0;
    for (;;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = '\0';
        for (char *t = line; *t; t++) if (*t == '\t' || *t == '\r') *t = ' ';
        CLAY(CLAY_IDI("MetaNoteLine", k++), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) } }
        }) {
            CLAY_TEXT(line[0] ? make_string(line) : CLAY_STRING(" "),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1,
                                         .wrapMode = CLAY_TEXT_WRAP_WORDS,
                                         .textColor = to_clay_color(COLOR_TEXT) }));
        }
        if (!nl) break;
        line = nl + 1;
    }
}

static const char *tri_text(int8_t v)
{
    return v == GUI_META_TRI_TRUE ? "Yes" : v == GUI_META_TRI_FALSE ? "No" : "-";
}

static void render_banner_line(const char *text, Color color, int font_size)
{
    CLAY_TEXT(make_string(text),
              CLAY_TEXT_CONFIG({ .fontSize = font_size, .textColor = to_clay_color(color) }));
}

/* One row: label, then the value as an editor, a locked box, or for the two
 * booleans either three buttons or a locked box. */
static void render_row(size_t i, const gui_capture_meta_field_t *f,
                       const gui_capture_meta_t *m, bool can_edit)
{
    bool editable = can_edit && (f->flags & GUI_META_F_UNLINKED_EDITABLE);
    bool wrap = m->linked && (f->flags & GUI_META_F_MULTILINE);
    ui_text_field_t tf = (ui_text_field_t)(UI_TEXT_FIELD_META_FIRST + (int)i);

    CLAY(CLAY_IDI("MetaRow", (int)i), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0),
                        wrap ? CLAY_SIZING_FIT(.min = META_ROW_HEIGHT) : CLAY_SIZING_FIXED(META_ROW_HEIGHT) },
            .layoutDirection = CLAY_LEFT_TO_RIGHT,
            .childAlignment = { .y = wrap ? CLAY_ALIGN_Y_TOP : CLAY_ALIGN_Y_CENTER },
            .childGap = 10
        }
    }) {
        CLAY(CLAY_IDI("MetaLabel", (int)i), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(META_LABEL_WIDTH), CLAY_SIZING_FIT(0) },
                        .padding = { 0, 0, wrap ? 7 : 0, 0 } }
        }) {
            CLAY_TEXT(make_string(f->label),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        if (f->type == GUI_META_TRI && editable) {
            static const char *const labels[3] = { "-", "Yes", "No" };
            static const int8_t values[3] = { GUI_META_TRI_UNSET, GUI_META_TRI_TRUE, GUI_META_TRI_FALSE };
            int8_t cur = gui_capture_meta_tri(m, f);
            for (int k = 0; k < 3; k++) {
                CLAY(CLAY_IDI("MetaTri", (int)i * 3 + k), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(72), CLAY_SIZING_FIXED(28) },
                                .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = to_clay_color(cur == values[k] ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
                    .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(labels[k]),
                              CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }
        } else {
        /* (No early return anywhere in here: CLAY() closes its element at the
         * end of its block, and leaving the block skips that.)
         *
         * Locked rows keep normal-colour text on a flat, dimmer box; editable
         * rows are the darker inset field with a border, like every other
         * text field in the app. */
        CLAY(CLAY_IDI("MetaField", (int)i), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0),
                            wrap ? CLAY_SIZING_FIT(.min = META_ROW_HEIGHT) : CLAY_SIZING_FIXED(META_ROW_HEIGHT) },
                .layoutDirection = wrap ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT,
                .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                .padding = { 8, 8, wrap ? 7 : 0, wrap ? 7 : 0 },
                .childGap = 2
            },
            .backgroundColor = to_clay_color(editable ? META_COLOR_EDIT_BG : META_COLOR_LOCK_BG),
            .cornerRadius = CLAY_CORNER_RADIUS(4),
            .border = { .width = { editable ? 1 : 0, editable ? 1 : 0, editable ? 1 : 0, editable ? 1 : 0 },
                        .color = to_clay_color(COLOR_BUTTON_ACTIVE) }
        }) {
            const char *v;
            bool dim = false;
            bool active = false;
            if (f->type == GUI_META_TRI) {
                int8_t t = gui_capture_meta_tri(m, f);
                v = tri_text(t);
                dim = (t == GUI_META_TRI_UNSET);
            } else {
                v = gui_capture_meta_str(m, f);
                active = editable && gui_ui_is_text_field_active(tf);
                if (!active && !v[0]) {
                    dim = true;
                    if ((f->flags & GUI_META_F_ID) && !m->linked) v = "- (not linked)";
                    else if (editable) v = (f->type == GUI_META_INDEX) ? "(unset)" : "(empty)";
                    else v = "-";
                }
            }
            if (active) {
                gui_ui_render_active_text(tf, v, FONT_SIZE_STATS, 1, COLOR_TEXT);
            } else if (wrap && !dim) {
                render_wrapped_lines(v);
            } else {
                CLAY_TEXT(make_string(v),
                          CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1,
                                             .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE,
                                             .textColor = to_clay_color(dim ? COLOR_TEXT_DIM : COLOR_TEXT) }));
            }
        }
        }
    }
}

void gui_capture_meta_panel_render(gui_app_t *app)
{
    if (!s_open || !app) return;

    const gui_capture_meta_t *m = gui_capture_meta_get();
    bool recording = gui_app_effective_recording(app);
    bool elsewhere = meta_seat_records_elsewhere(app);
    bool can_edit = gui_capture_meta_panel_can_edit(app);

    int max_w = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 840);
    int min_w = gui_ui_clamp_int(max_w, 1, 640);
    int max_h = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 780);

    CLAY(CLAY_ID("MetadataBackdrop"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP }
        },
        .backgroundColor = (Clay_Color){ 0, 0, 0, 140 }
    }) {}

    CLAY(CLAY_ID("MetadataWindow"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(.min = min_w, .max = max_w), CLAY_SIZING_FIT(.max = max_h) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 16, 16, 16, 16 },
            .childGap = 8
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER }
        },
        .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        CLAY(CLAY_ID("MetadataHeader"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 }
        }) {
            CLAY_TEXT(CLAY_STRING("Capture Metadata"),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_TITLE, .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY(CLAY_ID("MetadataHeaderSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
            CLAY(CLAY_ID("MetadataCloseButton"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"),
                          CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        CLAY(CLAY_ID("MetadataBanner"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 4,
                        .padding = { 10, 10, 8, 8 } },
            .backgroundColor = to_clay_color(m->linked ? (Color){ 34, 44, 66, 255 } : (Color){ 58, 44, 22, 255 }),
            .cornerRadius = CLAY_CORNER_RADIUS(4)
        }) {
            if (m->linked) {
                snprintf(s_banner_title, sizeof(s_banner_title), "From toolkit: %s \xC2\xB7 %s",
                         m->client_name, m->display_name);
                render_banner_line(s_banner_title, COLOR_TEXT, FONT_SIZE_NORMAL);
                render_banner_line("Read-only - corrections are made in the toolkit", COLOR_TEXT_DIM, FONT_SIZE_STATS);
            } else {
                render_banner_line("Not linked to an asset - these values apply to this run only and are never saved",
                                   META_COLOR_AMBER_TX, FONT_SIZE_STATS);
            }
            if (recording) {
                render_banner_line("Locked while recording", META_COLOR_AMBER_TX, FONT_SIZE_STATS);
            } else if (elsewhere && !m->linked) {
                render_banner_line("Locked: this seat records on the server", META_COLOR_AMBER_TX, FONT_SIZE_STATS);
            }
        }

        size_t n = 0;
        const gui_capture_meta_field_t *fields = gui_capture_meta_fields(&n);
        for (size_t i = 0; i < n; i++) {
            render_row(i, &fields[i], m, can_edit);
        }
    }
}

gui_meta_panel_click_t gui_capture_meta_panel_handle_click(gui_app_t *app)
{
    if (!s_open) return GUI_META_PANEL_CLICK_NONE;
    if (Clay_PointerOver(CLAY_ID("MetadataCloseButton")) ||
        Clay_PointerOver(CLAY_ID("MetadataBackdrop"))) {
        s_open = false;
        return GUI_META_PANEL_CLICK_CLOSED;
    }
    bool can_edit = gui_capture_meta_panel_can_edit(app);
    size_t n = 0;
    const gui_capture_meta_field_t *fields = gui_capture_meta_fields(&n);
    for (size_t i = 0; can_edit && i < n; i++) {
        const gui_capture_meta_field_t *f = &fields[i];
        if (!(f->flags & GUI_META_F_UNLINKED_EDITABLE)) continue;
        if (f->type == GUI_META_TRI) {
            static const int8_t values[3] = { GUI_META_TRI_UNSET, GUI_META_TRI_TRUE, GUI_META_TRI_FALSE };
            for (int k = 0; k < 3; k++) {
                if (Clay_PointerOver(CLAY_IDI("MetaTri", (int)i * 3 + k))) {
                    gui_capture_meta_set_tri(i, values[k]);
                    return GUI_META_PANEL_CLICK_CONSUMED;
                }
            }
        } else if (Clay_PointerOver(CLAY_IDI("MetaField", (int)i))) {
            gui_ui_begin_text_edit(app, (ui_text_field_t)(UI_TEXT_FIELD_META_FIRST + (int)i),
                                   CLAY_IDI("MetaField", (int)i), 8.0f, 8.0f);
            return GUI_META_PANEL_CLICK_CONSUMED;
        }
    }
    if (Clay_PointerOver(CLAY_ID("MetadataWindow"))) return GUI_META_PANEL_CLICK_CONSUMED;
    return GUI_META_PANEL_CLICK_NONE;
}
