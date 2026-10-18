#ifndef GUI_RF_PREVIEW_H
#define GUI_RF_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GUI_RF_DECODE_RATE_HZ 20000000u
#define GUI_RF_FRAME_MAX_WIDTH 768
/* Full-frame mode weaves every field line: PAL needs 2x313 = 626 rows. */
#define GUI_RF_FRAME_MAX_HEIGHT 640

/* Studio-swing display mapping for the ACTIVE area (Active and Full modes
 * alike). tbc-tools' makeYuvRangeScale convention — black lifted above the
 * display floor like a production monitor, white with headroom, sync below
 * black clamped at the floor — punched up from the pure 16..235 transfer per
 * user GUI feedback (the picture read as washed out): blacks sit a little
 * deeper and whites a little hotter while ~4% footroom and headroom remain
 * (sync tip -40 IRE still clamps at 0, ~110 IRE overshoots still reach 255).
 * Single source shared by the renderer and the synthetic-test oracle so the
 * two can never drift apart. */
#define RF_DISPLAY_BLACK_255 10.0
#define RF_DISPLAY_WHITE_255 245.0
static inline double rf_display_level(double y01) {
    if (y01 < 0.0) y01 = 0.0;
    double level = (RF_DISPLAY_BLACK_255
                    + (RF_DISPLAY_WHITE_255 - RF_DISPLAY_BLACK_255) * y01) / 255.0;
    return level > 1.0 ? 1.0 : level;
}

typedef enum { GUI_RF_PAL = 0, GUI_RF_NTSC = 1 } gui_rf_format_t;
typedef struct gui_rf_decoder gui_rf_decoder_t;

typedef struct {
    bool h_locked;
    bool v_locked;
    bool color_locked;
    uint64_t hsync_count;
    uint64_t vsync_count;
    uint64_t fields_decoded;
    double line_period_us;
    uint64_t frames_decoded;
    /* Reset-scoped diagnostic events. A V candidate is three consecutive
     * broad pulses at half-line cadence. Accepted fields have a measured line
     * map; rejected counts discarded candidate/reconstruction attempts, with
     * mutually exclusive cadence, parity/reference, and line-map reasons.
     * Color losses count transitions from locked to unlocked, not gray rows.
     * The worker accumulates deltas across resets for session totals. */
    uint64_t vsync_candidates;
    uint64_t fields_accepted;
    uint64_t fields_rejected;
    uint64_t reject_cadence;
    uint64_t reject_parity;
    uint64_t reject_line_map;
    uint64_t color_losses;
    char last_reject_reason[96];
    char last_color_loss_reason[96];
    char reason[96];
} gui_rf_decode_status_t;

/* RGBA is borrowed for the duration of the callback. Rows retain their
 * original field parity: no bob, interpolation, or deinterlacing is applied. */
typedef void (*gui_rf_frame_callback_t)(void *context, const uint8_t *rgba,
                                      int width, int height, uint64_t end_sample,
                                      const gui_rf_decode_status_t *status);

gui_rf_decoder_t *gui_rf_decoder_create(gui_rf_format_t format);
void gui_rf_decoder_destroy(gui_rf_decoder_t *decoder);
void gui_rf_decoder_reset(gui_rf_decoder_t *decoder, gui_rf_format_t format);
/* Frame display mode: Active (profile active lines/horizontal window, the
 * default) or Full (whole frame from the field boundary including the VBI,
 * with the horizontal window widened to the full line so the sync tip and
 * the color burst render with their decoded chroma). Full weaves every
 * field line, so the frame height is twice the taller parity's line count
 * (PAL 626, NTSC 526 rows). Unmeasured VBI lines (EQ/broad pulse rows
 * carry no H anchors) use nominal line positions. */
void gui_rf_decoder_set_frame_mode(gui_rf_decoder_t *decoder, bool full_frame);
void gui_rf_decoder_process(gui_rf_decoder_t *decoder, const float *samples,
                            size_t count, uint64_t first_sample,
                            gui_rf_frame_callback_t callback, void *context);
void gui_rf_decoder_get_status(const gui_rf_decoder_t *decoder,
                               gui_rf_decode_status_t *status);

#endif
