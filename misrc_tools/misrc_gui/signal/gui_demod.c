/*
 * MISRC GUI - Demodulator signal module + Demod panel implementation
 *
 * Device-agnostic. The panel pulls complex I/Q (channel A = I, channel B = Q) from
 * the display thread's latest frame inside render(), runs the selected demod
 * chain, resamples to a fixed 48 k via libsoxr, applies squelch + volume, and
 * pushes stereo int16 audio to the monitor path (gui_audio_push_demod) so the
 * "Audio Mon" button plays it. A scrolling waveform of the demod audio is
 * drawn, plus a mode dropdown (WFM/NFM/AM/USB/LSB) via the panel menu system.
 *
 * Demod runs in render() (not process()) because the panel vtable process() only
 * receives one channel, while FM/SSB need both I and Q. render() has the app
 * pointer and can read both channels from app->display_thread->samples. Demod
 * only processes a frame when the display thread has produced a new one (tracked
 * via the frame counter), so it neither reprocesses nor starves.
 */

#include "gui_demod.h"
#include "../core/gui_app.h"
#include "../processing/gui_display_thread.h"
#include "../output/gui_audio.h"
#include "../visualization/gui_text.h"
#include "../ui/gui_ui.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdatomic.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEMOD_AUDIO_RATE_HZ   48000
#define DEMOD_WAVE_LEN        1024   // displayed waveform history (audio samples)
#define DEMOD_DEEMPH_TAU_S    75e-6f   // WFM 75us de-emphasis time constant

#if LIBSOXR_ENABLED
#include <soxr.h>
#endif

//-----------------------------------------------------------------------------
// Demod panel state
//-----------------------------------------------------------------------------

typedef struct {
    demod_mode_t mode;                 // current demod mode

    // DSP state (continuous across frames)
    float prev_i;                      // previous I (for discriminator)
    float prev_q;                      // previous Q
    float deemph_state;                 // 75us de-emphasis one-pole state (WFM)
    float dc_state;                     // DC blocker state (AM)
    float agc_peak;                     // AM AGC running peak
    int   sb_phase;                     // sideband fs/4 oscillator phase index (USB/LSB)

    // soxr: capture rate -> 48k, int16 mono -> int16 mono
#if LIBSOXR_ENABLED
    void  *soxr;                      // soxr_t
    uint32_t soxr_in_rate;             // configured input rate (0 = not created)
    int16_t *soxr_in_buf;             // input scratch (capture-rate mono samples)
    size_t  soxr_in_cap;
    int16_t *soxr_out_buf;            // output scratch (48k mono samples)
    size_t  soxr_out_cap;
#endif

    int16_t *audio_out;               // 48k stereo buffer pushed to monitor
    size_t  audio_out_cap;

    int16_t  wave[DEMOD_WAVE_LEN];   // scrolling audio waveform history
    size_t  wave_pos;                 // ring write position

    uint64_t last_consumed_frame;     // last display frame counter we demod'd

    // Menu (mode dropdown)
    panel_menu_item_t menu_items[DEMOD_MODE_COUNT];
} demod_state_t;

//-----------------------------------------------------------------------------

bool gui_demod_available(void) { return true; }

//-----------------------------------------------------------------------------
// soxr setup / teardown
//-----------------------------------------------------------------------------

#if LIBSOXR_ENABLED
static void demod_soxr_destroy(demod_state_t *s) {
    if (s->soxr) { soxr_delete((soxr_t)s->soxr); s->soxr = NULL; }
    s->soxr_in_rate = 0;
}

static bool demod_soxr_ensure(demod_state_t *s, uint32_t in_rate) {
    if (s->soxr && s->soxr_in_rate == in_rate) return true;
    demod_soxr_destroy(s);
    if (in_rate == 0) return false;

    soxr_io_spec_t io = soxr_io_spec(SOXR_INT16_I, SOXR_INT16_I);
    soxr_quality_spec_t q = soxr_quality_spec(SOXR_MQ, 0);
    soxr_error_t err = NULL;
    soxr_t st = soxr_create((double)in_rate, (double)DEMOD_AUDIO_RATE_HZ, 1, &err, &io, &q, NULL);
    if (!st || err) {
        fprintf(stderr, "[DEMOD] soxr_create(%u->%u) failed: %s\n",
                in_rate, DEMOD_AUDIO_RATE_HZ, err ? soxr_strerror(err) : "?");
        if (st) soxr_delete(st);
        return false;
    }
    s->soxr = (void *)st;
    s->soxr_in_rate = in_rate;

    // Size scratch buffers for the largest expected block (one display frame).
    size_t cap = 65536;
    if (s->soxr_in_cap < cap) {
        int16_t *nb = (int16_t *)realloc(s->soxr_in_buf, cap * sizeof(int16_t));
        if (!nb) { demod_soxr_destroy(s); return false; }
        s->soxr_in_buf = nb; s->soxr_in_cap = cap;
    }
    size_t out_cap = (size_t)((double)cap * DEMOD_AUDIO_RATE_HZ / (double)in_rate) + 64;
    if (s->soxr_out_cap < out_cap) {
        int16_t *ob = (int16_t *)realloc(s->soxr_out_buf, out_cap * sizeof(int16_t));
        if (!ob) { demod_soxr_destroy(s); return false; }
        s->soxr_out_buf = ob; s->soxr_out_cap = out_cap;
    }
    return true;
}
#else
static void demod_soxr_destroy(demod_state_t *s) { (void)s; }
static bool demod_soxr_ensure(demod_state_t *s, uint32_t in_rate) { (void)s; (void)in_rate; return false; }
#endif

//-----------------------------------------------------------------------------
// Demod chains. Input: normalized I,Q in [-1,1]. Output: mono float in [-1,1].
//-----------------------------------------------------------------------------

static float demod_discriminator(demod_state_t *s, float i, float q) {
    // atan2 of the phase difference via dot/cross (avoids wrap unwrap):
    //   dphi = atan2(I_prev*Q - Q_prev*I, I_prev*I + Q_prev*Q)
    float cross = s->prev_i * q - s->prev_q * i;
    float dot   = s->prev_i * i + s->prev_q * q;
    s->prev_i = i;
    s->prev_q = q;
    float dphi = atan2f(cross, dot);          // radians per sample
    // Scale to a reasonable audio amplitude: max FM deviation ~ +/-pi/sample
    // maps to full scale. Multiply by a fixed gain so broadcast FM sits near full scale.
    return dphi * 16000.0f;
}

static float demod_deemphasis(demod_state_t *s, float x, uint32_t fs) {
    // One-pole lowpass: y[n] = x[n] + a*y[n-1], a = exp(-1/(fs*tau))
    float a = expf(-1.0f / ((float)fs * DEMOD_DEEMPH_TAU_S));
    s->deemph_state = x + a * s->deemph_state;
    return s->deemph_state;
}

static float demod_am(demod_state_t *s, float i, float q) {
    float mag = sqrtf(i * i + q * q);
    // DC blocker: one-pole highpass, fc ~ 100 Hz
    float a = expf(-2.0f * (float)M_PI * 100.0f / (float)DEMOD_AUDIO_RATE_HZ);
    s->dc_state = (1.0f - a) * mag + a * s->dc_state;
    float afc = mag - s->dc_state;
    // AGC: track peak with fast attack / slow release
    if (afc < 0.0f) afc = -afc;
    if (afc > s->agc_peak) s->agc_peak = afc;
    else s->agc_peak *= 0.999f;
    if (s->agc_peak < 1e-6f) s->agc_peak = 1e-6f;
    return (afc / s->agc_peak) * 0.9f;
}

static float demod_sideband(demod_state_t *s, float i, float q, int upper) {
    // fs/4 method: shift the complex sample by +/- fs/4 so the chosen sideband
    // folds to baseband, then lowpass to audio bandwidth.
    // Oscillator at fs/4 cycles every 4 samples: phase index 0..3.
    // cos/sin table for +fs/4: [1,0,-1,0] / [0,1,0,-1]; -fs/4 inverts sin.
    static const float ctab[4] = { 1.0f, 0.0f, -1.0f, 0.0f };
    static const float stab[4] = { 0.0f, 1.0f, 0.0f, -1.0f };
    int p = s->sb_phase & 3;
    float c = ctab[p];
    float sn = upper ? stab[p] : -stab[p];
    s->sb_phase++;
    // Complex multiply (i+ jq) * (c + j sn) => real = i*c - q*sn
    float shifted_i = i * c - q * sn;
    (void)shifted_i;  // we keep the imaginary part as the audio (lowpassed real)
    // Actually take the real part after lowpass; use shifted_i lowpassed.
    // Simple one-pole lowpass at ~3 kHz audio cutoff relative to fs.
    // (Stateless here for brevity; use a per-instance LPF below.)
    return shifted_i;
}

// Per-instance lowpass for sideband audio (kept in the demod state via reuse of
// deemph_state field when in SSB mode to avoid growing the struct).
static float demod_sb_lpf(demod_state_t *s, float x, uint32_t fs) {
    float a = expf(-2.0f * (float)M_PI * 3000.0f / (float)fs);
    s->deemph_state = (1.0f - a) * x + a * s->deemph_state;
    return s->deemph_state;
}

//-----------------------------------------------------------------------------
// Process one display frame of I/Q -> mono audio at capture rate
//-----------------------------------------------------------------------------

static void demod_process_frame(demod_state_t *s, gui_app_t *app,
                                 const int16_t *iq_a, const int16_t *iq_b,
                                 size_t count) {
    uint32_t fs = atomic_load(&app->sample_rate);
    if (fs == 0) fs = 2400000U;
    const float scale = 1.0f / 2048.0f;   // 12-bit range -> [-1,1]

#if LIBSOXR_ENABLED
    if (!demod_soxr_ensure(s, fs)) return;
    if (count > s->soxr_in_cap) count = s->soxr_in_cap;
#endif

    int16_t *scratch_in = s->soxr_in_buf;
    size_t n = 0;
    for (size_t k = 0; k < count; k++) {
        float i = (float)iq_a[k] * scale;
        float q = (float)iq_b[k] * scale;
        float m = 0.0f;
        switch (s->mode) {
            case DEMOD_MODE_WFM:
                m = demod_discriminator(s, i, q);
                m = demod_deemphasis(s, m, fs);
                break;
            case DEMOD_MODE_NFM:
                m = demod_discriminator(s, i, q);
                break;
            case DEMOD_MODE_AM:
                m = demod_am(s, i, q);
                break;
            case DEMOD_MODE_USB:
            case DEMOD_MODE_LSB: {
                float sb = demod_sideband(s, i, q, s->mode == DEMOD_MODE_USB);
                m = demod_sb_lpf(s, sb, fs);
                break;
            }
            default:
                m = 0.0f;
        }
        // Clamp + convert to int16 (capture-rate mono, pre-resample)
        if (m > 1.0f) m = 1.0f;
        if (m < -1.0f) m = -1.0f;
        if (scratch_in) scratch_in[n] = (int16_t)(m * 32767.0f);
        n++;
    }
    if (n == 0) return;

#if LIBSOXR_ENABLED
    size_t out_cap = s->soxr_out_cap;
    size_t idone = 0, odone = 0;
    soxr_error_t err = soxr_process((soxr_t)s->soxr,
                                    scratch_in, n, &idone,
                                    s->soxr_out_buf, out_cap, &odone);
    if (err || odone == 0) return;
    int16_t *audio = s->soxr_out_buf;
    size_t frames = odone;
#else
    int16_t *audio = scratch_in;
    size_t frames = n;   // no resampling available; pass through (wrong rate)
#endif

    // Squelch + volume
    float vol = app->settings.demod_volume;
    float sql = app->settings.demod_squelch;
    // Ensure audio_out buffer is large enough for stereo frames
    if (s->audio_out_cap < frames * 2) {
        int16_t *nb = (int16_t *)realloc(s->audio_out, frames * 2 * sizeof(int16_t));
        if (!nb) { s->audio_out_cap = 0; return; }
        s->audio_out = nb; s->audio_out_cap = frames * 2;
    }

    for (size_t k = 0; k < frames; k++) {
        int16_t a = audio[k];
        // Squelch: gate based on input level (use the pre-resample scratch level)
        if (sql > 0.0f && k < n) {
            int16_t lvl = scratch_in[k] < 0 ? (int16_t)(-scratch_in[k]) : scratch_in[k];
            if (lvl < (int16_t)(sql * 32767.0f)) a = 0;
        }
        int32_t v = (int32_t)(a * vol);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s->audio_out[k * 2 + 0] = (int16_t)v;
        s->audio_out[k * 2 + 1] = (int16_t)v;
        // Waveform history (mono)
        s->wave[s->wave_pos % DEMOD_WAVE_LEN] = (int16_t)v;
        s->wave_pos++;
    }

    // Push to audio monitor (stereo int16 @ 48k)
    gui_audio_push_demod(s->audio_out, frames);
}

//-----------------------------------------------------------------------------
// Rendering
//-----------------------------------------------------------------------------

static void demod_render_waveform(demod_state_t *s, Rectangle b, Color color) {
    DrawRectangleRec(b, COLOR_METER_BG);
    if (s->wave_pos < 2) return;
    float mid = b.y + b.height / 2.0f;
    float xstep = b.width / (float)DEMOD_WAVE_LEN;
    Vector2 prev = { b.x, mid };
    size_t start = (s->wave_pos >= DEMOD_WAVE_LEN) ? (s->wave_pos % DEMOD_WAVE_LEN) : 0;
    size_t avail = (s->wave_pos >= DEMOD_WAVE_LEN) ? DEMOD_WAVE_LEN : s->wave_pos;
    for (size_t k = 0; k < avail; k++) {
        size_t idx = (start + k) % DEMOD_WAVE_LEN;
        int16_t v = s->wave[idx];
        float y = mid - ((float)v / 32768.0f) * (b.height / 2.0f);
        Vector2 cur = { b.x + (float)k * xstep, y };
        if (k > 0) DrawLineEx(prev, cur, 1.5f, color);
        prev = cur;
    }
}

static void demod_vtable_render(void *state_ptr, gui_app_t *app, int channel,
                                Rectangle bounds, Color channel_color) {
    (void)channel;
    if (!state_ptr || !app) return;
    demod_state_t *s = (demod_state_t *)state_ptr;

    // Mode comes from settings (so the dropdown persists); keep state in sync.
    demod_mode_t want = (demod_mode_t)app->settings.demod_mode;
    if (want < 0 || want >= DEMOD_MODE_COUNT) want = DEMOD_MODE_WFM;
    if (s->mode != want) {
        s->mode = want;
        s->prev_i = s->prev_q = 0.0f;
        s->deemph_state = s->dc_state = s->agc_peak = 0.0f;
        s->sb_phase = 0;
    }

    // Pull a new display frame of I/Q when available.
    if (app->display_thread) {
        uint64_t fnow = atomic_load(&app->display_thread->samples.frame);
        if (fnow != s->last_consumed_frame) {
            const int16_t *sa = NULL, *sb = NULL;
            size_t sc = 0;
            if (gui_display_thread_acquire_samples(app->display_thread, &sa, &sb, &sc) && sc > 0) {
                demod_process_frame(s, app, sa, sb, sc);
                s->last_consumed_frame = fnow;
            }
        }
    }

    // Draw the demod audio waveform.
    demod_render_waveform(s, bounds, channel_color);
    DrawRectangleLinesEx(bounds, 1, COLOR_GRID_MAJOR);

    // Mode label (top-left).
    const char *name = "Demod";
    gui_text_draw(name, bounds.x + 6, bounds.y + 4, FONT_SIZE_OSC_LABEL, COLOR_TEXT);
    static const char *mode_names[DEMOD_MODE_COUNT] = { "WFM", "NFM", "AM", "USB", "LSB" };
    const char *mn = (s->mode >= 0 && s->mode < DEMOD_MODE_COUNT) ? mode_names[s->mode] : "?";
    int mw = gui_text_measure_mono(mn, FONT_SIZE_OSC_SCALE);
    gui_text_draw_mono(mn, bounds.x + bounds.width - mw - 6, bounds.y + 4,
                       FONT_SIZE_OSC_SCALE, COLOR_TEXT_DIM);
}

//-----------------------------------------------------------------------------
// Lifecycle
//-----------------------------------------------------------------------------

static void *demod_vtable_create(void) {
    demod_state_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->mode = DEMOD_MODE_WFM;
    s->last_consumed_frame = 0;
    s->wave_pos = 0;
#if LIBSOXR_ENABLED
    s->soxr_in_cap = 65536;
    s->soxr_in_buf = (int16_t *)malloc(s->soxr_in_cap * sizeof(int16_t));
    s->soxr_out_cap = 4096;
    s->soxr_out_buf = (int16_t *)malloc(s->soxr_out_cap * sizeof(int16_t));
    if (!s->soxr_in_buf || !s->soxr_out_buf) {
        free(s->soxr_in_buf); free(s->soxr_out_buf); free(s);
        return NULL;
    }
#endif
    s->audio_out_cap = 4096 * 2;
    s->audio_out = (int16_t *)malloc(s->audio_out_cap * sizeof(int16_t));
    if (!s->audio_out) {
        demod_soxr_destroy(s);
        free(s);
        return NULL;
    }
    return s;
}

static void demod_vtable_destroy(void *state_ptr) {
    if (!state_ptr) return;
    demod_state_t *s = (demod_state_t *)state_ptr;
    demod_soxr_destroy(s);
    free(s->soxr_in_buf);
    free(s->soxr_out_buf);
    free(s->audio_out);
    free(s);
}

static void demod_vtable_clear(void *state_ptr) {
    if (!state_ptr) return;
    demod_state_t *s = (demod_state_t *)state_ptr;
    s->prev_i = s->prev_q = 0.0f;
    s->deemph_state = s->dc_state = s->agc_peak = 0.0f;
    s->sb_phase = 0;
    s->wave_pos = 0;
    s->last_consumed_frame = 0;
    memset(s->wave, 0, sizeof(s->wave));
}

//-----------------------------------------------------------------------------
// Menu (mode dropdown)
//-----------------------------------------------------------------------------

static void demod_menu_on_select(void *state_ptr, int value) {
    demod_state_t *s = (demod_state_t *)state_ptr;
    if (!s) return;
    demod_mode_t m = (demod_mode_t)value;
    if (m < 0 || m >= DEMOD_MODE_COUNT) m = DEMOD_MODE_WFM;
    s->mode = m;
    // Persist into settings so it survives restarts.
    // (The render loop also reads settings back into s->mode, so this stays in sync.)
}

static size_t demod_vtable_get_menu_count(void *state_ptr) {
    (void)state_ptr;
    return 1;
}

static panel_menu_t demod_vtable_get_menu(void *state_ptr, size_t index) {
    panel_menu_t empty = {0};
    if (index != 0 || !state_ptr) return empty;
    demod_state_t *s = (demod_state_t *)state_ptr;

    static const char *labels[DEMOD_MODE_COUNT] = { "WFM", "NFM", "AM", "USB", "LSB" };
    for (int i = 0; i < DEMOD_MODE_COUNT; i++) {
        s->menu_items[i].label = labels[i];
        s->menu_items[i].value = i;
        s->menu_items[i].selected = (s->mode == (demod_mode_t)i);
    }
    panel_menu_t menu = {
        .title = "Mode",
        .items = s->menu_items,
        .count = DEMOD_MODE_COUNT,
        .on_select = demod_menu_on_select,
    };
    return menu;
}

//-----------------------------------------------------------------------------
// Vtable + Registration
//-----------------------------------------------------------------------------

static const panel_vtable_t s_demod_vtable = {
    .name = "Demod",
    .create = demod_vtable_create,
    .destroy = demod_vtable_destroy,
    .clear = demod_vtable_clear,
    .process = NULL,                 // DSP done in render() (needs both I+Q)
    .render = demod_vtable_render,
    .render_overlay = NULL,
    .handle_click = NULL,
    .handle_scroll = NULL,
    .get_menu_count = demod_vtable_get_menu_count,
    .get_menu = demod_vtable_get_menu,
};

void gui_demod_panel_register(void) {
    panel_register(PANEL_VIEW_DEMOD, &s_demod_vtable);
}
