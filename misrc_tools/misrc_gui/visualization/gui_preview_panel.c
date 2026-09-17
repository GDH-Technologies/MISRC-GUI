/*
 * MISRC GUI - USB video preview panel.
 *
 * Everything about the stream lives in gui_preview_v4l2; this file is the
 * panel-shaped wrapper around it. Per-panel state is UI only, because up to
 * four panel slots can show the same single device at once.
 */

#include "gui_preview_panel.h"
#include "gui_panel.h"
#include "panel_interface.h"
#include "gui_text.h"
#include "../core/gui_app.h"
#include "../input/gui_preview_v4l2.h"
#include "../output/gui_video_record.h"
#include "../ui/gui_dropdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PV_BTN_H        18.0f
#define PV_GAP           4.0f
#define PV_OPT_H        18.0f
#define PV_MAX_OPTS     16
#define PV_COMPACT_W   400.0f   /* below this, collapse the two pickers */
#define PV_CONFIRM_SECS  2.0

typedef struct {
    bool      dev_menu_open;
    bool      fmt_menu_open;

    /* Sampled during render so handle_click, which runs a frame later, can
     * tell whether this click was the one that dismissed the gear popover. */
    bool      gear_open_at_render;

    double    confirm_until;
    int       confirm_which;    /* 0 none, 1 disconnect, 2 popout */

    /* Analog controls, present only for an SDTV device. They sit on their own
     * row rather than competing for width with the device and mode pickers: a
     * jack selector that vanishes in a split pane is a jack selector you cannot
     * use, and switching Composite to S-Video mid-tape is the main thing this
     * panel is for on such a device. */
    bool      in_menu_open;
    bool      std_menu_open;

    Rectangle r_dev, r_fmt, r_action, r_pop, r_in, r_std;
    Rectangle r_dev_opt[PV_MAX_OPTS];
    Rectangle r_fmt_opt[PV_MAX_OPTS];
    Rectangle r_in_opt[PV_MAX_OPTS];
    Rectangle r_std_opt[PV_MAX_OPTS];
    int       n_dev_opt, n_fmt_opt, n_in_opt, n_std_opt;
    bool      compact;
} preview_panel_t;

/* Snapshot the analog selections so the next launch restores them. Stored by
 * name for the same reason the device is stored by path: an index into a
 * device-specific list means nothing once the device changes. */
static void pv_remember_analog(gui_app_t *app)
{
    if (!app) return;
    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    int sel = gui_preview_selected_device();
    if (sel < 0 || (size_t)sel >= n) return;
    const preview_device_t *d = &devs[sel];

    int in = gui_preview_selected_input();
    if (in >= 0 && in < d->n_inputs) {
        snprintf(app->settings.preview_input, sizeof(app->settings.preview_input),
                 "%s", d->inputs[in].name);
    }
    if (d->std_index >= 0 && d->std_index < d->n_stds) {
        snprintf(app->settings.preview_standard, sizeof(app->settings.preview_standard),
                 "%s", d->stds[d->std_index].name);
    }
    gui_preview_mode_spec(app->settings.preview_mode_spec,
                          sizeof(app->settings.preview_mode_spec));
    gui_settings_save(&app->settings);
}

/* ------------------------------------------------------------------ helpers */

static void pv_draw_button(Rectangle r, const char *label, bool active, bool enabled)
{
    Color bg = active ? (Color){ 60, 90, 140, 230 } : (Color){ 40, 40, 48, 210 };
    if (!enabled) bg = (Color){ 34, 34, 38, 170 };
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1.0f, (Color){ 90, 90, 100, 200 });

    int fs = 11;
    int tw = MeasureText(label, fs);
    Color fg = enabled ? (Color){ 220, 220, 225, 255 } : (Color){ 120, 120, 125, 255 };
    DrawText(label, (int)(r.x + r.width / 2 - tw / 2), (int)(r.y + r.height / 2 - fs / 2), fs, fg);
}

/* ------------------------------------------------------------------- vtable */

static void *preview_vtable_create(void)
{
    preview_panel_t *pp = calloc(1, sizeof(preview_panel_t));
    if (!pp) return NULL;
    gui_preview_panel_attach();
    return pp;
}

/* Runs while panel_config_lock is held, so it must not join a thread, issue an
 * ioctl or touch GL. Detach only records that a viewer went away; the reader's
 * per-frame tick performs any teardown. */
static void preview_vtable_destroy(void *state)
{
    if (!state) return;
    gui_preview_panel_detach();
    free(state);
}

static void preview_vtable_clear(void *state)
{
    preview_panel_t *pp = state;
    if (!pp) return;
    pp->dev_menu_open = false;
    pp->fmt_menu_open = false;
    pp->in_menu_open = false;
    pp->std_menu_open = false;
    pp->confirm_which = 0;
}

void gui_preview_close_overlays(void *state)
{
    preview_vtable_clear(state);
}

static const char *pv_action_label(preview_status_t *st, preview_panel_t *pp, char *buf, size_t cap)
{
    if (gui_video_record_is_running() &&
        (st->state == PREVIEW_STATE_STREAMING || st->state == PREVIEW_STATE_STALLED)) {
        return "REC";   /* the stream is owned by a recording */
    }
    if (pp->confirm_which == 1) return "Confirm?";
    if (pp->confirm_which == 2) return "Confirm?";
    switch (st->state) {
        case PREVIEW_STATE_NO_DEVICE:   return "Rescan";
        case PREVIEW_STATE_CONNECTING:  return "Cancel";
        case PREVIEW_STATE_ERROR:       return "Reconnect";
        case PREVIEW_STATE_POPPED_OUT:  return "Bring back";
        case PREVIEW_STATE_STREAMING:
        case PREVIEW_STATE_STALLED:
            if (st->viewers > 1) {
                snprintf(buf, cap, "Disconnect (%d)", st->viewers);
                return buf;
            }
            return "Disconnect";
        default:                        return "Connect";
    }
}

/* Controls sit top-RIGHT: the channel gear button floats over the top-left of
 * the same canvas, and its popover opens downward from there. */
static void preview_render_overlay(preview_panel_t *pp, Rectangle bounds)
{
    preview_status_t st = gui_preview_get_status();
    size_t n_devs = 0;
    const preview_device_t *devs = gui_preview_devices(&n_devs);
    int sel_dev = gui_preview_selected_device();
    int sel_mode = gui_preview_selected_mode();

    if (pp->confirm_which && GetTime() > pp->confirm_until) pp->confirm_which = 0;

    pp->compact = (bounds.width < PV_COMPACT_W);
    float y = bounds.y + 6.0f;
    float x_right = bounds.x + bounds.width - 8.0f;

    char action_buf[32];
    const char *action = pv_action_label(&st, pp, action_buf, sizeof(action_buf));

    float pop_w = 60.0f;
    float act_w = (float)MeasureText(action, 11) + 16.0f;
    if (act_w < 74.0f) act_w = 74.0f;

    pp->r_pop = (Rectangle){ x_right - pop_w, y, pop_w, PV_BTN_H };
    pp->r_action = (Rectangle){ pp->r_pop.x - PV_GAP - act_w, y, act_w, PV_BTN_H };

    if (pp->compact) {
        /* A split half-panel at the minimum window width has no room for the
         * pickers; collapse them so the actions stay reachable. */
        pp->r_fmt = (Rectangle){ pp->r_action.x - PV_GAP - 24.0f, y, 24.0f, PV_BTN_H };
        pp->r_dev = (Rectangle){ 0, 0, 0, 0 };
    } else {
        pp->r_fmt = (Rectangle){ pp->r_action.x - PV_GAP - 104.0f, y, 104.0f, PV_BTN_H };
        pp->r_dev = (Rectangle){ pp->r_fmt.x - PV_GAP - 110.0f, y, 110.0f, PV_BTN_H };
    }

    bool busy = (st.state == PREVIEW_STATE_STREAMING || st.state == PREVIEW_STATE_STALLED ||
                 st.state == PREVIEW_STATE_CONNECTING || st.state == PREVIEW_STATE_POPPED_OUT);
    bool can_pick = !busy && n_devs > 0;

    if (pp->compact) {
        pv_draw_button(pp->r_fmt, "...", pp->dev_menu_open || pp->fmt_menu_open, can_pick);
    } else {
        const char *dev_label = (n_devs > 0 && sel_dev < (int)n_devs) ? devs[sel_dev].path
                                                                      : "(no device)";
        const char *fmt_label = "(no mode)";
        if (n_devs > 0 && sel_dev < (int)n_devs && sel_mode < devs[sel_dev].n_modes) {
            fmt_label = devs[sel_dev].modes[sel_mode].label;
        }
        pv_draw_button(pp->r_dev, dev_label, pp->dev_menu_open, can_pick);
        pv_draw_button(pp->r_fmt, fmt_label, pp->fmt_menu_open, can_pick);
    }

    pv_draw_button(pp->r_action, action, pp->confirm_which != 0, n_devs > 0);
    pv_draw_button(pp->r_pop, st.state == PREVIEW_STATE_POPPED_OUT ? "Popped" : "Pop-out",
                   st.state == PREVIEW_STATE_POPPED_OUT, busy || st.state == PREVIEW_STATE_POPPED_OUT);

    /* ---- analog row: which jack, which video standard ---------------------- */
    pp->r_in = (Rectangle){ 0, 0, 0, 0 };
    pp->r_std = (Rectangle){ 0, 0, 0, 0 };

    const preview_device_t *adev = (n_devs > 0 && sel_dev < (int)n_devs) ? &devs[sel_dev] : NULL;
    bool sdtv = adev && adev->is_sdtv;
    /* A recording and the RTSP stream are both tees off this device. Changing
     * the standard resizes the raster, so it is refused while either holds the
     * stream -- the same reason the settings dialog refuses a device swap. */
    bool tee_live = gui_video_record_is_running();
    /* The jack, by contrast, is switchable live: V4L2 allows S_INPUT while
     * streaming and it does not change geometry, so the encoders keep running. */
    bool can_pick_input = sdtv && adev->n_inputs > 1 && st.state != PREVIEW_STATE_POPPED_OUT;
    bool can_pick_std   = sdtv && adev->n_stds > 1 && can_pick && !tee_live;

    if (sdtv) {
        float y2 = y + PV_BTN_H + PV_GAP;
        float std_w = 86.0f, in_w = 104.0f;
        pp->r_std = (Rectangle){ x_right - std_w, y2, std_w, PV_BTN_H };
        pp->r_in  = (Rectangle){ pp->r_std.x - PV_GAP - in_w, y2, in_w, PV_BTN_H };

        /* Prefer what the device reported back over what was merely selected:
         * until it is streaming there is nothing to read back, and after that
         * the readback is the truth. */
        int sel_in = gui_preview_selected_input();
        const char *in_label = "Input";
        if (st.input_name[0]) in_label = st.input_name;
        else if (sel_in >= 0 && sel_in < adev->n_inputs) in_label = adev->inputs[sel_in].name;

        const char *std_label = "Standard";
        if (st.std_name[0]) std_label = st.std_name;
        else if (adev->std_index >= 0 && adev->std_index < adev->n_stds) {
            std_label = adev->stds[adev->std_index].name;
        }

        pv_draw_button(pp->r_in, in_label, pp->in_menu_open, can_pick_input);
        pv_draw_button(pp->r_std, std_label, pp->std_menu_open, can_pick_std);
    }

    /* Option lists, right-aligned under their button. */
    pp->n_dev_opt = 0;
    pp->n_fmt_opt = 0;
    pp->n_in_opt = 0;
    pp->n_std_opt = 0;

    if (pp->dev_menu_open && can_pick) {
        Rectangle anchor = pp->compact ? pp->r_fmt : pp->r_dev;
        float ow = 180.0f;
        float ox = anchor.x + anchor.width - ow;
        float oy = anchor.y + anchor.height + 2.0f;
        for (size_t i = 0; i < n_devs && i < PV_MAX_OPTS; i++) {
            Rectangle r = { ox, oy + (float)i * PV_OPT_H, ow, PV_OPT_H };
            pp->r_dev_opt[pp->n_dev_opt++] = r;
            char line[96];
            snprintf(line, sizeof(line), "%s  %s", devs[i].path, devs[i].card);
            pv_draw_button(r, line, (int)i == sel_dev, true);
        }
    }
    if (pp->fmt_menu_open && can_pick && sel_dev < (int)n_devs) {
        Rectangle anchor = pp->r_fmt;
        float ow = 140.0f;
        float ox = anchor.x + anchor.width - ow;
        float oy = anchor.y + anchor.height + 2.0f;
        int count = devs[sel_dev].n_modes;
        if (count > PV_MAX_OPTS) count = PV_MAX_OPTS;
        for (int i = 0; i < count; i++) {
            Rectangle r = { ox, oy + (float)i * PV_OPT_H, ow, PV_OPT_H };
            pp->r_fmt_opt[pp->n_fmt_opt++] = r;
            pv_draw_button(r, devs[sel_dev].modes[i].label, i == sel_mode, true);
        }
    }
    if (pp->in_menu_open && can_pick_input && adev) {
        float ow = 140.0f;
        float ox = pp->r_in.x + pp->r_in.width - ow;
        float oy = pp->r_in.y + pp->r_in.height + 2.0f;
        int count = adev->n_inputs;
        if (count > PV_MAX_OPTS) count = PV_MAX_OPTS;
        int sel_in = gui_preview_selected_input();
        for (int i = 0; i < count; i++) {
            Rectangle r = { ox, oy + (float)i * PV_OPT_H, ow, PV_OPT_H };
            pp->r_in_opt[pp->n_in_opt++] = r;
            pv_draw_button(r, adev->inputs[i].name, i == sel_in, true);
        }
    }
    if (pp->std_menu_open && can_pick_std && adev) {
        float ow = 120.0f;
        float ox = pp->r_std.x + pp->r_std.width - ow;
        float oy = pp->r_std.y + pp->r_std.height + 2.0f;
        int count = adev->n_stds;
        if (count > PV_MAX_OPTS) count = PV_MAX_OPTS;
        for (int i = 0; i < count; i++) {
            Rectangle r = { ox, oy + (float)i * PV_OPT_H, ow, PV_OPT_H };
            pp->r_std_opt[pp->n_std_opt++] = r;
            pv_draw_button(r, adev->stds[i].name, i == adev->std_index, true);
        }
    }
}

static void preview_vtable_render(void *state, gui_app_t *app, int channel,
                                  Rectangle bounds, Color channel_color)
{
    (void)app; (void)channel_color;

    /* Idempotent within a frame: the first preview panel to render consumes
     * the published frame and does the single texture upload, the rest just
     * draw the texture that is already current. */
    gui_preview_frame_sync();
    gui_preview_draw(bounds, true);

    preview_panel_t *pp = state;
    if (!pp) return;   /* render is reached with a NULL state; draw and bail */

    pp->gear_open_at_render = gui_dropdown_is_open(DROPDOWN_CHANNEL_GEAR, (uint32_t)channel);
    preview_render_overlay(pp, bounds);
}

static bool preview_vtable_handle_click(void *state, gui_app_t *app, int channel,
                                        Vector2 click, Rectangle bounds)
{
    (void)channel; (void)bounds;   /* app is used to persist the analog selections */
    preview_panel_t *pp = state;
    if (!pp) return false;

    /* The gear's outside-click dismissal deliberately does not consume, so this
     * click may be the one that closed its popover. Non-destructive controls
     * are fine to act on; destructive ones must not fire on that frame. */
    bool dismissal = pp->gear_open_at_render;
    pp->gear_open_at_render = false;

    /* Option lists first: they are drawn on top. */
    for (int i = 0; i < pp->n_dev_opt; i++) {
        if (CheckCollisionPointRec(click, pp->r_dev_opt[i])) {
            gui_preview_select(i, 0);
            pp->dev_menu_open = false;
            return true;
        }
    }
    for (int i = 0; i < pp->n_fmt_opt; i++) {
        if (CheckCollisionPointRec(click, pp->r_fmt_opt[i])) {
            gui_preview_select(gui_preview_selected_device(), i);
            pp->fmt_menu_open = false;
            pv_remember_analog(app);
            return true;
        }
    }
    for (int i = 0; i < pp->n_in_opt; i++) {
        if (CheckCollisionPointRec(click, pp->r_in_opt[i])) {
            /* Applied to the hardware immediately when streaming, so the
             * picture changes under the operator's hand rather than at the next
             * reconnect. A device that refuses leaves the old selection. */
            gui_preview_select_input(i);
            pp->in_menu_open = false;
            pv_remember_analog(app);
            return true;
        }
    }
    for (int i = 0; i < pp->n_std_opt; i++) {
        if (CheckCollisionPointRec(click, pp->r_std_opt[i])) {
            /* Only reachable while disconnected: every SDTV driver refuses
             * S_STD during streaming, and it resizes the raster besides. */
            gui_preview_select_standard(i);
            pp->std_menu_open = false;
            pv_remember_analog(app);
            return true;
        }
    }

    preview_status_t st = gui_preview_get_status();
    bool busy = (st.state == PREVIEW_STATE_STREAMING || st.state == PREVIEW_STATE_STALLED ||
                 st.state == PREVIEW_STATE_CONNECTING || st.state == PREVIEW_STATE_POPPED_OUT);

    if (CheckCollisionPointRec(click, pp->r_dev) ||
        (pp->compact && CheckCollisionPointRec(click, pp->r_fmt) && !pp->fmt_menu_open)) {
        if (!busy) { pp->dev_menu_open = !pp->dev_menu_open; pp->fmt_menu_open = false; }
        return true;
    }
    if (CheckCollisionPointRec(click, pp->r_fmt)) {
        if (!busy) { pp->fmt_menu_open = !pp->fmt_menu_open; pp->dev_menu_open = false; }
        return true;
    }
    /* The zero rectangles a non-SDTV device leaves behind must not swallow a
     * click at the panel's origin, so test for a real button first. */
    if (pp->r_in.width > 0.0f && CheckCollisionPointRec(click, pp->r_in)) {
        pp->in_menu_open = !pp->in_menu_open;
        pp->dev_menu_open = pp->fmt_menu_open = pp->std_menu_open = false;
        return true;
    }
    if (pp->r_std.width > 0.0f && CheckCollisionPointRec(click, pp->r_std)) {
        if (!busy) {
            pp->std_menu_open = !pp->std_menu_open;
            pp->dev_menu_open = pp->fmt_menu_open = pp->in_menu_open = false;
        }
        return true;
    }

    /* A live reference-video recording owns this stream. Letting Disconnect or
     * Pop-out through here would end the recording's source mid-file. */
    bool video_recording = gui_video_record_is_running();

    if (CheckCollisionPointRec(click, pp->r_action)) {
        pp->dev_menu_open = pp->fmt_menu_open = false;
        if (video_recording && (st.state == PREVIEW_STATE_STREAMING ||
                                st.state == PREVIEW_STATE_STALLED)) {
            return true;   /* swallowed: recording in progress */
        }
        switch (st.state) {
            case PREVIEW_STATE_NO_DEVICE:
                gui_preview_refresh_devices();
                break;
            case PREVIEW_STATE_STREAMING:
            case PREVIEW_STATE_STALLED:
                /* Destructive: never on a gear-dismiss frame, and always with
                 * a confirm step when other panels are watching the stream. */
                if (dismissal) break;
                if (pp->confirm_which == 1) {
                    pp->confirm_which = 0;
                    gui_preview_disconnect();
                } else if (st.viewers > 1) {
                    pp->confirm_which = 1;
                    pp->confirm_until = GetTime() + PV_CONFIRM_SECS;
                } else {
                    gui_preview_disconnect();
                }
                break;
            case PREVIEW_STATE_CONNECTING:
                if (!dismissal) gui_preview_disconnect();
                break;
            case PREVIEW_STATE_POPPED_OUT:
                if (!dismissal) gui_preview_reclaim();
                break;
            default:
                gui_preview_connect();
                break;
        }
        return true;
    }

    if (CheckCollisionPointRec(click, pp->r_pop)) {
        pp->dev_menu_open = pp->fmt_menu_open = false;
        if (video_recording) return true;   /* swallowed: recording in progress */
        if (dismissal) return true;
        if (st.state == PREVIEW_STATE_POPPED_OUT) {
            gui_preview_reclaim();
        } else if (busy) {
            if (pp->confirm_which == 2) {
                pp->confirm_which = 0;
                gui_preview_popout();
            } else {
                pp->confirm_which = 2;
                pp->confirm_until = GetTime() + PV_CONFIRM_SECS;
            }
        }
        return true;
    }

    /* A click anywhere else in the panel closes the menus but is not consumed,
     * so it still does whatever it would normally have done. */
    if (pp->dev_menu_open || pp->fmt_menu_open) {
        pp->dev_menu_open = pp->fmt_menu_open = false;
    }
    return false;
}

static const panel_vtable_t s_preview_vtable = {
    .name           = "Preview",
    .create         = preview_vtable_create,
    .destroy        = preview_vtable_destroy,
    .clear          = preview_vtable_clear,
    .process        = NULL,   /* keeps the display thread out entirely */
    .render         = preview_vtable_render,
    .render_overlay = NULL,   /* drawn at the tail of render, as CVBS does */
    .handle_click   = preview_vtable_handle_click,
    .handle_scroll  = NULL,
    .get_menu_count = NULL,
    .get_menu       = NULL,
};

void gui_preview_panel_register(void)
{
    panel_register(PANEL_VIEW_PREVIEW, &s_preview_vtable);
}
