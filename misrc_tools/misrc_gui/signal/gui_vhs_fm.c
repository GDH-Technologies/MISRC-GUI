/* MISRC GUI VHS RF preview wrapper. GPL-3.0-or-later.
 * DSP and resampling run exclusively in the bounded preview worker.
 * Raylib resources and the front RGBA copy belong exclusively to the UI.
 */
#include "gui_vhs_fm.h"
#include "../visualization/gui_text.h"
#include "../ui/gui_ui.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../../common/threading.h"
#define RF_RGBA_BYTES ((size_t)GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4)

bool gui_vhs_fm_init(vhs_fm_decoder_t *decoder) {
    if (!decoder) return false;
    memset(decoder, 0, sizeof(*decoder));
    atomic_init(&decoder->input_supported, true);
    decoder->format = VHS_FM_FORMAT_PAL;
    decoder->bound_channel = -1;
    return true; // No RF allocation or worker until this panel selects VHS FM.
}
bool gui_vhs_fm_bind_worker(vhs_fm_decoder_t *decoder, gui_rf_preview_worker_t *worker) {
    if (!decoder) return false;
    if (worker && !decoder->rgba) {
        decoder->rgba = calloc(1, RF_RGBA_BYTES);
        if (!decoder->rgba) return false;
    }
    if (!worker) {
        if (decoder->texture_valid) UnloadTexture(decoder->texture);
        decoder->texture_valid = false;
        free(decoder->rgba); decoder->rgba = NULL;
    }
    decoder->worker = worker;
    // Re-apply the stored frame display mode to each newly bound worker
    // (the DSP decoder preserves it across its own resets, but a fresh
    // worker starts in Active mode).
    if (worker && decoder->overlay.frame_mode == 1) {
        gui_rf_preview_worker_set_frame_mode(worker, true);
    }
    decoder->generation = 0;
    decoder->width = decoder->height = 0;
    decoder->texture_dirty = false;
    decoder->displayed_frames = decoder->display_window_frames = decoder->display_window_ms = 0;
    decoder->displayed_generation = 0; decoder->display_fps = 0;
    memset(&decoder->stats,0,sizeof(decoder->stats));
    return true;
}
void gui_vhs_fm_cleanup(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;
    // Pool teardown releases the borrowed worker before this UI-only cleanup.
    if (decoder->texture_valid) UnloadTexture(decoder->texture);
    free(decoder->rgba);
    decoder->worker = NULL;
    decoder->rgba = NULL;
    decoder->texture_valid = false;
}
void gui_vhs_fm_reset(vhs_fm_decoder_t *decoder) {
    if (!decoder) return;
    gui_rf_preview_worker_reset(decoder->worker);
}
void gui_vhs_fm_set_format(vhs_fm_decoder_t *decoder, int format_select) {
    if (!decoder) return;
    decoder->overlay.selected_system = format_select == 1;
    decoder->format = format_select == 1 ? VHS_FM_FORMAT_NTSC : VHS_FM_FORMAT_PAL;
    // The UI route update rebinds this subscriber; other subscribers keep their system.
}
void gui_vhs_fm_set_frame_mode(vhs_fm_decoder_t *decoder, int mode_select) {
    if (!decoder) return;
    decoder->overlay.frame_mode = (mode_select == 1) ? 1 : 0;
    if (decoder->worker) {
        gui_rf_preview_worker_set_frame_mode(decoder->worker,
                                            decoder->overlay.frame_mode == 1);
    }
}
vhs_fm_format_t gui_vhs_fm_get_format(vhs_fm_decoder_t *decoder) {
    return decoder ? decoder->format : VHS_FM_FORMAT_UNKNOWN;
}
const char *gui_vhs_fm_get_format_name(vhs_fm_decoder_t *decoder) {
    return gui_vhs_fm_get_format(decoder) == VHS_FM_FORMAT_NTSC ? "NTSC" : "PAL";
}
void gui_vhs_fm_swap_buffers(vhs_fm_decoder_t *decoder) {
    if (!decoder || !decoder->worker) return;
    if (gui_rf_preview_worker_take_frame(decoder->worker, decoder->rgba, RF_RGBA_BYTES,
                                         &decoder->generation, &decoder->width, &decoder->height,
                                         &decoder->stats)) {
        decoder->texture_dirty = true;
    } else {
        gui_rf_preview_worker_get_stats(decoder->worker, &decoder->stats);
    }
}
// Draw one stats line, truncating with "..." when it is wider than the
// panel (physical-pixel budget) so long counters stay readable on narrow or
// split panels instead of vanishing entirely.
static void vhs_draw_stat_fitted(const char *text, float dx, float dy, int font_px,
                                 float max_w_px, Color color) {
    if (max_w_px <= 0.0f) return;
    char fit[224];
    size_t len = strlen(text);
    if (len >= sizeof(fit)) len = sizeof(fit) - 1;
    memcpy(fit, text, len);
    fit[len] = '\0';
    if ((float)gui_text_measure_mono(fit, font_px) > max_w_px) {
        while (len > 4) {
            fit[--len] = '\0';
            snprintf(fit + len - 3, 4, "...");
            if ((float)gui_text_measure_mono(fit, font_px) <= max_w_px) break;
        }
    }
    gui_text_draw_mono(fit, dx, dy, font_px, color);
}

void gui_vhs_fm_render_frame(vhs_fm_decoder_t *decoder, float x, float y, float width, float height) {
    DrawRectangleRec((Rectangle){x, y, width, height}, (Color){20, 20, 20, 255});
    if (!decoder) return;
    const gui_rf_decode_status_t *status = &decoder->stats.decode;
    bool supported = atomic_load(&decoder->input_supported);
    if (decoder->texture_dirty && decoder->width > 0 && decoder->height > 0) {
        if (!decoder->texture_valid || decoder->texture.width != decoder->width ||
            decoder->texture.height != decoder->height) {
            if (decoder->texture_valid) UnloadTexture(decoder->texture);
            Image image = {.data = decoder->rgba, .width = decoder->width, .height = decoder->height,
                           .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            decoder->texture = LoadTextureFromImage(image);
            decoder->texture_valid = decoder->texture.id != 0;
            if (decoder->texture_valid) SetTextureFilter(decoder->texture, TEXTURE_FILTER_BILINEAR);
        } else {
            UpdateTexture(decoder->texture, decoder->rgba);
        }
        decoder->texture_dirty = false;
    }
    // Display both original fields, in their original alternating rows.
    if (decoder->texture_valid && supported && (status->v_locked || decoder->stats.paused)) {
        if (decoder->generation != decoder->displayed_generation) {
            decoder->displayed_generation = decoder->generation;
            decoder->displayed_frames++;
        }
        float draw_w = width, draw_h = width * 0.75f;
        if (draw_h > height) { draw_h = height; draw_w = height * 4.0f / 3.0f; }
        DrawTexturePro(decoder->texture,
                       (Rectangle){0, 0, (float)decoder->width, (float)decoder->height},
                       (Rectangle){x + (width-draw_w)*0.5f, y + (height-draw_h)*0.5f, draw_w, draw_h},
                       (Vector2){0, 0}, 0, WHITE);
    }
    uint64_t now = get_time_ms();
    if (!decoder->display_window_ms) decoder->display_window_ms = now;
    if (now < decoder->display_window_ms || now-decoder->display_window_ms >= 1000) {
        uint64_t elapsed = now > decoder->display_window_ms ? now-decoder->display_window_ms : 0;
        decoder->display_fps = elapsed ? (decoder->displayed_frames-decoder->display_window_frames)*1000.0/elapsed : 0;
        decoder->display_window_ms = now;
        decoder->display_window_frames = decoder->displayed_frames;
    }
    // --- Stats readout (selectable view, UI-scale-aware metrics) ---
    // Panel bounds are LOGICAL units; gui_text_* font sizes are PHYSICAL
    // pixels drawn under the global UI render-scale transform (Clay applies
    // rlScalef(ui_scale) around all custom panel drawing). The font therefore
    // stays a FIXED physical size — the transform scales it with UI zoom,
    // exactly like the overlay buttons (multiplying by ui_scale here would
    // double-scale and rendered wrong at any zoom other than 100%). The
    // LOGICAL line height derives from that fixed font. Measured widths are
    // physical pixels, so compare them against the logical budget * scale.
    // Layout is bottom-stacked: the reason line owns the lowest slot and the
    // stats lines stack directly above it — no gap, no overlap, every view.
    // Long lines truncate with "..." instead of vanishing on narrow panels.
    // A double-click on the panel (gui_cvbs.c) hides ALL overlay chrome —
    // buttons, stats and the reason line — for a clean video preview.
    float ui_scale = gui_ui_get_scale_factor();
    if (!(ui_scale > 0.05f)) ui_scale = 1.0f;
    const int font_px = FONT_SIZE_DROPDOWN_OPT;               // 16: matches overlay buttons
    const float line_h = ((float)font_px + 6.0f) / ui_scale;   // logical line height
    const float pad = 8.0f;
    const float max_w_px = (width - pad * 2.0f) * ui_scale;    // physical text budget

    char line[224];
    int stats_view = decoder->overlay.stats_view;
    if (stats_view < 0 || stats_view > 2) stats_view = 0;
    bool show_stats = !decoder->overlay.controls_hidden && stats_view != 2;
    bool show_basic_only = stats_view == 1;
    const int stat_lines = show_stats ? (show_basic_only ? 2 : 5) : 0;

    if (show_stats) {
        float ly = y + height - pad - line_h * (float)(stat_lines + 1); // above the reason line
        snprintf(line, sizeof(line), "%s VHS SP | %.3f -> 20 MHz | H:%s V:%s Color:%s",
                 gui_vhs_fm_get_format_name(decoder), decoder->stats.source_rate_hz / 1000000.0,
                 status->h_locked ? "LOCK" : "SCAN", status->v_locked ? "LOCK" : "SCAN",
                 status->color_locked ? "LOCK" : "OFF");
        if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_px, max_w_px, (Color){230,230,230,255});
        ly += line_h;
        snprintf(line,sizeof(line),"Decode %.1f fps | Display %.1f fps | DSP %.1f ms | Age %.1f ms",
                 decoder->stats.fps,decoder->display_fps,decoder->stats.decode_ms,decoder->stats.latency_ms);
        if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_px, max_w_px, (Color){230,230,230,255});
        if (!show_basic_only) {
            ly += line_h;
            snprintf(line,sizeof(line),"V candidates %llu | Fields %llu | Reject %llu (cad %llu/par %llu/map %llu)",
                     (unsigned long long)decoder->stats.total_vsync_candidates,
                     (unsigned long long)decoder->stats.total_fields_accepted,
                     (unsigned long long)decoder->stats.total_fields_rejected,
                     (unsigned long long)decoder->stats.total_reject_cadence,
                     (unsigned long long)decoder->stats.total_reject_parity,
                     (unsigned long long)decoder->stats.total_reject_line_map);
            if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_px, max_w_px, (Color){230,230,230,255});
            ly += line_h;
            snprintf(line,sizeof(line),"Queue drops %llu | Pressure skips %llu | Color loss %llu | CPU %.0f%% core",
                     (unsigned long long)decoder->stats.blocks_dropped,
                     (unsigned long long)decoder->stats.pause_drops,
                     (unsigned long long)decoder->stats.total_color_losses,decoder->stats.cpu_percent);
            if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_px, max_w_px, (Color){230,230,230,255});
            ly += line_h;
            snprintf(line,sizeof(line),"Last field rejection: %s",
                     status->last_reject_reason[0] ? status->last_reject_reason : "none");
            if (ly >= y + pad) vhs_draw_stat_fitted(line, x + pad, ly, font_px, max_w_px, (Color){230,230,230,255});
        }
    }
    // The reason/sync line is part of the overlay chrome: double-click hides
    // it too (it once lingered after the controls were hidden).
    if (!decoder->overlay.controls_hidden) {
        const char *reason = !supported ? "Mixed-rate pair: independent input reader required" :
                         decoder->stats.paused ? "Preview paused: capture/recording pressure (frozen frame)" :
                         decoder->stats.budget_waiting ? "Preview yielding to the shared processing budget" : status->reason;
        float ry = y + height - pad - line_h;
        if (ry >= y + pad) vhs_draw_stat_fitted(reason, x + pad, ry, font_px, max_w_px, (Color){255,220,120,255});
    }
}
void gui_vhs_fm_panel_register(void) {
    // Video panels subscribe to pooled RF workers fed continuously by extraction.
}
