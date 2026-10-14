/* MISRC GUI VHS RF preview wrapper. GPL-3.0-or-later. */
#ifndef GUI_VHS_FM_H
#define GUI_VHS_FM_H
#include "../core/gui_app.h"
#include "../processing/gui_rf_preview_worker.h"
#include "raylib.h"
#include <stdatomic.h>

typedef enum { VHS_FM_FORMAT_UNKNOWN, VHS_FM_FORMAT_PAL, VHS_FM_FORMAT_NTSC } vhs_fm_format_t;
typedef struct vhs_fm_decoder {
    gui_rf_preview_worker_t *worker; // Borrowed from the channel/system pool.
    int bound_channel, bound_system;
    uint64_t displayed_frames, display_window_frames, display_window_ms, displayed_generation;
    double display_fps;
    uint8_t *rgba;
    Texture2D texture;
    int width, height;
    bool texture_valid, texture_dirty;
    uint64_t generation;
    gui_rf_preview_stats_t stats;
    atomic_bool input_supported;
    vhs_fm_format_t format;
    struct {
        Rectangle button_rect, format_rect, budget_rect;
        Rectangle stats_btn_rect, stats_opts_rect[3];
        Rectangle frame_rect;
        bool is_visible, dropdown_open, stats_dropdown_open;
        int selected_system;
        int stats_view;               // 0 = All, 1 = Basic, 2 = Off
        int frame_mode;               // 0 = Active, 1 = Full (VBI + burst view)
        bool controls_hidden;         // Double-click on the panel toggles ALL
                                       // overlay chrome (buttons + stats text).
        uint64_t last_click_ms;      // Double-click detection (ms + position)
        float last_click_x, last_click_y;
    } overlay;
} vhs_fm_decoder_t;
bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder);
bool gui_vhs_fm_bind_worker(vhs_fm_decoder_t *decoder, gui_rf_preview_worker_t *worker);
void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder, float x, float y, float width, float height);
void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select);
// Frame display mode: 0 = Active (default), 1 = Full (VBI + burst with
// decoded chroma). Persists across worker rebinds.
void gui_vhs_fm_set_frame_mode(vhs_fm_decoder_t *decoder, int mode_select);
vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder);
const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder);
void gui_vhs_fm_panel_register(void);
#endif
