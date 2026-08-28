/*
 * MISRC GUI - Real-time Spectrogram / Waterfall panel implementation
 *
 * Shared engine for the Waterfall (PANEL_VIEW_WATERFALL) and Spectrograph
 * (PANEL_VIEW_SPECTROGRAPH) panels. See gui_spectrogram.h for the high-level
 * description and threading model.
 *
 * Rendering strategy:
 *   A CPU RGBA8 history buffer is kept in "display order" (the row/column that
 *   represents the newest time slice is always at a fixed screen edge). Each
 *   render frame we shift the history by one slice, write the latest FFT
 *   magnitude frame (resampled to SPEC_FREQ_BINS, mapped through a 256-entry
 *   heatmap LUT that replicates the GPU phosphor intensityToHeatmap ramp) into
 *   the freed slot, upload the whole buffer to a Texture2D, and draw it with
 *   DrawTexturePro using a negative source height so buffer row 0 maps to the
 *   top of the screen.
 *
 *   Waterfall:    freq on X (tex_w = SPEC_FREQ_BINS), time on Y scrolling down,
 *                 newest written at buffer row 0 (drawn at the top).
 *   Spectrograph: time on X scrolling left (tex_w = SPEC_TIME_BINS), freq on Y,
 *                 newest written at buffer col (tex_w-1) (drawn at the right),
 *                 high freq at buffer row 0 (drawn at the top).
 */

#include "gui_spectrogram.h"
#include "gui_fft.h"
#include "gui_text.h"
#include "../core/gui_app.h"
#include "../ui/gui_ui.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>

//-----------------------------------------------------------------------------
// Configuration (tunable; lowering these reduces per-frame upload bandwidth)
//-----------------------------------------------------------------------------

#define SPEC_FREQ_BINS   1024   // Fixed frequency resolution of the history texture
#define SPEC_TIME_BINS   256    // History depth (time slices)

#define SPEC_LABEL_H     16     // Waterfall: bottom strip reserved for freq labels (px)
#define SPEC_LABEL_W     34     // Spectrograph: left strip reserved for freq labels (px)

#define SPEC_AXIS_DIVS   8      // Number of frequency grid divisions

//-----------------------------------------------------------------------------
// Engine State
//-----------------------------------------------------------------------------

typedef struct spectro_engine {
    fft_state_t fft;                 // Reusable FFT engine (display thread writes back buffer)

    bool time_on_x;                  // false = Waterfall (freq X, time Y), true = Spectrograph (time X, freq Y)
    int  freq_bins;                  // == SPEC_FREQ_BINS (frequency axis length in the texture)
    int  time_bins;                  // == SPEC_TIME_BINS (time axis length in the texture)

    int  tex_w;                      // Texture width  (depends on orientation)
    int  tex_h;                      // Texture height (depends on orientation)

    unsigned char *pixels;           // CPU RGBA8 history buffer: tex_w * tex_h * 4 bytes
    float         *frame;            // Latest resampled magnitude frame (freq_bins floats, render thread)

    unsigned char  lut[256][4];      // Heatmap color LUT (matches phosphor intensityToHeatmap)

    Texture2D tex;                   // GPU texture (created lazily in render thread)
    bool tex_valid;
    bool needs_full_upload;          // True after clear/init -> upload whole texture once

    bool frame_valid;                // True once at least one FFT frame has been consumed
    uint32_t last_sample_rate;       // Sample rate snapshot for axis labels
} spectro_engine_t;

//-----------------------------------------------------------------------------
// Availability
//-----------------------------------------------------------------------------

bool gui_spectrogram_available(void) {
    return gui_fft_available();
}

//-----------------------------------------------------------------------------
// Heatmap LUT (replicates gui_phosphor_rt.c intensityToHeatmap exactly so the
// colors match the existing FFT/oscilloscope phosphor display)
//-----------------------------------------------------------------------------

static void build_lut(unsigned char lut[256][4]) {
    for (int i = 0; i < 256; i++) {
        float intensity = (float)i / 255.0f;
        float r, g, b;
        if (intensity < 0.25f) {
            float t = intensity / 0.25f;
            r = 0.0f; g = 0.078f * t; b = 0.392f + 0.608f * t;
        } else if (intensity < 0.5f) {
            float t = (intensity - 0.25f) / 0.25f;
            r = 0.0f; g = 0.078f + 0.922f * t; b = 1.0f - 0.784f * t;
        } else if (intensity < 0.75f) {
            float t = (intensity - 0.5f) / 0.25f;
            r = t; g = 1.0f; b = 0.216f - 0.216f * t;
        } else {
            float t = (intensity - 0.75f) / 0.25f;
            r = 1.0f; g = 1.0f - 0.706f * t; b = 0.0f;
        }
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        if (g < 0.0f) g = 0.0f;
        if (g > 1.0f) g = 1.0f;
        if (b < 0.0f) b = 0.0f;
        if (b > 1.0f) b = 1.0f;
        lut[i][0] = (unsigned char)(r * 255.0f + 0.5f);
        lut[i][1] = (unsigned char)(g * 255.0f + 0.5f);
        lut[i][2] = (unsigned char)(b * 255.0f + 0.5f);
        lut[i][3] = 255;
    }
}

static inline const unsigned char *lut_lookup(spectro_engine_t *e, float m) {
    if (m < 0.0f) m = 0.0f;
    if (m > 1.0f) m = 1.0f;
    int idx = (int)(m * 255.0f + 0.5f);
    if (idx < 0) idx = 0;
    if (idx > 255) idx = 255;
    return e->lut[idx];
}

//-----------------------------------------------------------------------------
// Resample (linear) the dynamic fft_bins magnitude frame into freq_bins slots
//-----------------------------------------------------------------------------

static void resample_magnitude(const float *src, int n_src, float *dst, int n_dst) {
    if (!dst || n_dst <= 0) return;
    if (!src || n_src <= 0) {
        for (int i = 0; i < n_dst; i++) dst[i] = 0.0f;
        return;
    }
    if (n_src == 1) {
        for (int i = 0; i < n_dst; i++) dst[i] = src[0];
        return;
    }
    if (n_dst == 1) {
        dst[0] = src[n_src / 2];
        return;
    }
    for (int i = 0; i < n_dst; i++) {
        float pos = (float)i * (float)(n_src - 1) / (float)(n_dst - 1);
        if (pos < 0.0f) pos = 0.0f;
        int i0 = (int)pos;
        if (i0 >= n_src - 1) { dst[i] = src[n_src - 1]; continue; }
        float frac = pos - (float)i0;
        dst[i] = src[i0] * (1.0f - frac) + src[i0 + 1] * frac;
    }
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void *spectro_create(bool time_on_x) {
    if (!gui_spectrogram_available()) return NULL;

    spectro_engine_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->time_on_x = time_on_x;
    e->freq_bins = SPEC_FREQ_BINS;
    e->time_bins = SPEC_TIME_BINS;
    e->tex_w = time_on_x ? e->time_bins : e->freq_bins;
    e->tex_h = time_on_x ? e->freq_bins : e->time_bins;

    e->pixels = calloc((size_t)e->tex_w * e->tex_h, 4);
    e->frame  = calloc((size_t)e->freq_bins, sizeof(float));
    if (!e->pixels || !e->frame) {
        free(e->pixels);
        free(e->frame);
        free(e);
        return NULL;
    }

    build_lut(e->lut);

    if (!gui_fft_init(&e->fft)) {
        free(e->pixels);
        free(e->frame);
        free(e);
        return NULL;
    }

    e->tex_valid = false;
    e->needs_full_upload = true;
    e->frame_valid = false;
    e->last_sample_rate = 0;
    return e;
}

static void *waterfall_vtable_create(void)    { return spectro_create(false); }
static void *spectrograph_vtable_create(void) { return spectro_create(true); }

static void spectro_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (e->tex_valid) {
        UnloadTexture(e->tex);
        e->tex_valid = false;
    }
    gui_fft_cleanup(&e->fft);
    free(e->pixels);
    free(e->frame);
    free(e);
}

static void spectro_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    if (e->pixels) memset(e->pixels, 0, (size_t)e->tex_w * e->tex_h * 4);
    e->needs_full_upload = true;
    e->frame_valid = false;
    // gui_fft_clear() only touches CPU buffers + the unused phosphor state
    // (phosphor render textures are never created for this panel, so the
    // phosphor clear is a no-op and no GL calls occur here).
    gui_fft_clear(&e->fft);
}

//-----------------------------------------------------------------------------
// Processing (display thread, no GL)
//-----------------------------------------------------------------------------

static void spectro_vtable_process(void *state_ptr, const int16_t *samples,
                                    size_t count, uint32_t sample_rate) {
    if (!state_ptr || !samples || count == 0) return;
    spectro_engine_t *e = (spectro_engine_t *)state_ptr;
    // Compute FFT into the embedded back buffer and set magnitude_ready.
    gui_fft_process_raw(&e->fft, samples, count, sample_rate);
}

//-----------------------------------------------------------------------------
// History append (render thread)
//-----------------------------------------------------------------------------

static inline void put_pixel(spectro_engine_t *e, int col, int row, const unsigned char *c) {
    unsigned char *p = &e->pixels[((size_t)row * e->tex_w + col) * 4];
    p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
}

static void spectro_append_frame(spectro_engine_t *e) {
    if (!e->frame_valid) return;

    if (!e->time_on_x) {
        // Waterfall: time on Y (rows), freq on X (cols). Newest at row 0 (top).
        // Shift rows down by one (discard oldest at bottom).
        memmove(e->pixels + (size_t)e->tex_w * 4,
                e->pixels,
                (size_t)(e->tex_h - 1) * e->tex_w * 4);
        // Write new frame as row 0.
        unsigned char *row0 = e->pixels;
        for (int f = 0; f < e->freq_bins; f++) {
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            unsigned char *p = &row0[(size_t)f * 4];
            p[0] = c[0]; p[1] = c[1]; p[2] = c[2]; p[3] = c[3];
        }
    } else {
        // Spectrograph: time on X (cols), freq on Y (rows). Newest at col (tex_w-1) (right).
        // High freq at row 0 (top), low freq at row (tex_h-1) (bottom).
        // Shift columns left by one (discard oldest at left), per row.
        for (int r = 0; r < e->tex_h; r++) {
            unsigned char *row = &e->pixels[(size_t)r * e->tex_w * 4];
            memmove(row, row + 4, (size_t)(e->tex_w - 1) * 4);
        }
        // Write new frame as the rightmost column.
        int col = e->tex_w - 1;
        for (int f = 0; f < e->freq_bins; f++) {
            int row = e->freq_bins - 1 - f;   // f = freq index (0 = DC/low). high freq -> row 0 (top)
            const unsigned char *c = lut_lookup(e, e->frame[f]);
            put_pixel(e, col, row, c);
        }
    }
}

//-----------------------------------------------------------------------------
// Overlay: frequency axis grid + labels + "now" edge marker
//-----------------------------------------------------------------------------

static void format_freq(char *buf, size_t bufsz, double hz) {
    if (hz >= 1.0e6)      snprintf(buf, bufsz, "%.1fM", hz / 1.0e6);
    else if (hz >= 1.0e3) snprintf(buf, bufsz, "%.0fk", hz / 1.0e3);
    else                  snprintf(buf, bufsz, "%.0f", hz);
}

static void spectro_draw_overlay(spectro_engine_t *e, Rectangle bounds, Rectangle plot) {
    (void)plot;  // Labels are drawn inside bounds, so plot == bounds.
    uint32_t sr = e->last_sample_rate;
    double nyq = (double)sr * 0.5;
    if (nyq <= 0.0) nyq = 1.0;   // before first data, avoid divide-by-zero / nonsense labels

    const int divs = SPEC_AXIS_DIVS;
    const float pad = 3.0f;      // Inset from the border so labels sit inside the window
    const float label_h = (float)FONT_SIZE_OSC_SCALE;

    if (!e->time_on_x) {
        // Waterfall: frequency on X (0 at left, Nyquist at right).
        // Labels drawn just inside the bottom edge.
        float label_y = bounds.y + bounds.height - label_h - pad;
        for (int d = 0; d <= divs; d++) {
            float frac = (float)d / (float)divs;
            float px = bounds.x + bounds.width * frac;
            DrawLineEx((Vector2){px, bounds.y}, (Vector2){px, bounds.y + bounds.height}, 1.0f, COLOR_GRID);
            char buf[16];
            format_freq(buf, sizeof(buf), nyq * (double)frac);
            int tw = gui_text_measure_mono(buf, FONT_SIZE_OSC_SCALE);
            float lx = px - tw / 2.0f;
            // Keep label horizontally inside bounds.
            if (lx < bounds.x + pad) lx = bounds.x + pad;
            if (lx + tw > bounds.x + bounds.width - pad) lx = bounds.x + bounds.width - tw - pad;
            gui_text_draw_mono(buf, lx, label_y, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
        }
        // "Now" marker at the top edge (newest slice).
        DrawLineEx((Vector2){bounds.x, bounds.y}, (Vector2){bounds.x + bounds.width, bounds.y},
                   2.0f, COLOR_GRID_MAJOR);
    } else {
        // Spectrograph: frequency on Y (0 at bottom, Nyquist at top).
        // Labels drawn just inside the left edge, vertically centered on each gridline.
        float label_x = bounds.x + pad;
        for (int d = 0; d <= divs; d++) {
            float frac = (float)d / (float)divs;          // 0 = 0 Hz, 1 = Nyquist
            float py = bounds.y + bounds.height * (1.0f - frac);  // frac=1 -> top
            DrawLineEx((Vector2){bounds.x, py}, (Vector2){bounds.x + bounds.width, py}, 1.0f, COLOR_GRID);
            char buf[16];
            format_freq(buf, sizeof(buf), nyq * (double)frac);
            float ly = py - label_h / 2.0f;
            // Keep label vertically inside bounds.
            if (ly < bounds.y + pad) ly = bounds.y + pad;
            if (ly + label_h > bounds.y + bounds.height - pad) ly = bounds.y + bounds.height - label_h - pad;
            gui_text_draw_mono(buf, label_x, ly, FONT_SIZE_OSC_SCALE, COLOR_TEXT);
        }
        // "Now" marker at the right edge (newest slice).
        DrawLineEx((Vector2){bounds.x + bounds.width, bounds.y},
                   (Vector2){bounds.x + bounds.width, bounds.y + bounds.height},
                   2.0f, COLOR_GRID_MAJOR);
    }

    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
}

//-----------------------------------------------------------------------------
// Rendering (render thread, GL available)
//-----------------------------------------------------------------------------

static void spectro_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                   Rectangle bounds, Color channel_color) {
    (void)channel;
    (void)channel_color;
    if (!state_ptr || !app) return;

    spectro_engine_t *e = (spectro_engine_t *)state_ptr;

    if (!e->fft.initialized) {
        const char *text = gui_spectrogram_available()
            ? "Spectrogram Initializing..."
            : "Spectrogram Not Available (FFTW)";
        int tw = gui_text_measure(text, FONT_SIZE_OSC_MSG);
        gui_text_draw(text, bounds.x + bounds.width / 2 - tw / 2,
                 bounds.y + bounds.height / 2 - 12,
                 FONT_SIZE_OSC_MSG, COLOR_TEXT_DIM);
        DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
        return;
    }

    // Create the GPU texture lazily (needs an OpenGL context, i.e. render thread).
    if (!e->tex_valid) {
        Image img = GenImageColor(e->tex_w, e->tex_h, BLACK);
        e->tex = LoadTextureFromImage(img);
        UnloadImage(img);
        if (e->tex.id == 0) {
            const char *text = "Spectrogram: texture alloc failed";
            int tw = gui_text_measure(text, FONT_SIZE_OSC_MSG);
            gui_text_draw(text, bounds.x + bounds.width / 2 - tw / 2,
                     bounds.y + bounds.height / 2 - 12,
                     FONT_SIZE_OSC_MSG, COLOR_CLIP_RED);
            DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);
            return;
        }
        SetTextureFilter(e->tex, TEXTURE_FILTER_BILINEAR);
        e->tex_valid = true;
        e->needs_full_upload = true;
    }

    // Consume the latest FFT frame from the display thread (no EMA: each time
    // slice is a distinct, crisp spectrum).
    if (atomic_exchange(&e->fft.magnitude_ready, 0)) {
        int bins = e->fft.fft_bins;
        if (bins > 0 && e->fft.magnitude_back) {
            resample_magnitude(e->fft.magnitude_back, bins, e->frame, e->freq_bins);
        }
        e->last_sample_rate = atomic_load(&e->fft.sample_rate);
        e->frame_valid = true;
    }

    // Scroll one slice and write the latest frame (repeats the last frame if no
    // new data arrived this render tick -> smooth 60 fps scroll).
    spectro_append_frame(e);

    // Upload the whole history buffer (no-flip; the draw uses negative source
    // height so buffer row 0 lands at the top of the screen).
    UpdateTexture(e->tex, e->pixels);
    e->needs_full_upload = false;

    // Plot fills the whole bounds now; labels are drawn inside it on top of
    // the spectrogram (no reserved strip), so they read as part of the window.
    Rectangle plot = bounds;

    // Background + spectrogram.
    DrawRectangleRec(bounds, COLOR_METER_BG);
    DrawTexturePro(e->tex,
                   (Rectangle){0, 0, (float)e->tex_w, -(float)e->tex_h},
                   (Rectangle){plot.x, plot.y, plot.width, plot.height},
                   (Vector2){0, 0}, 0.0f, WHITE);

    spectro_draw_overlay(e, bounds, plot);
}

//-----------------------------------------------------------------------------
// Vtables + Registration
//-----------------------------------------------------------------------------

static const panel_vtable_t s_waterfall_vtable = {
    .name = "Waterfall",
    .create = waterfall_vtable_create,
    .destroy = spectro_vtable_destroy,
    .clear = spectro_vtable_clear,
    .process = spectro_vtable_process,
    .render = spectro_vtable_render,
    .render_overlay = NULL,
    .handle_click = NULL,
    .handle_scroll = NULL,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

static const panel_vtable_t s_spectrograph_vtable = {
    .name = "Spectro",
    .create = spectrograph_vtable_create,
    .destroy = spectro_vtable_destroy,
    .clear = spectro_vtable_clear,
    .process = spectro_vtable_process,
    .render = spectro_vtable_render,
    .render_overlay = NULL,
    .handle_click = NULL,
    .handle_scroll = NULL,
    .get_menu_count = NULL,
    .get_menu = NULL,
};

void gui_waterfall_panel_register(void) {
    panel_register(PANEL_VIEW_WATERFALL, &s_waterfall_vtable);
}

void gui_spectrograph_panel_register(void) {
    panel_register(PANEL_VIEW_SPECTROGRAPH, &s_spectrograph_vtable);
}
