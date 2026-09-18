/* SDTV geometry for the USB video preview. See gui_preview_sdtv.h. */

#include "gui_preview_sdtv.h"

#include <stdio.h>

static uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

/* Even, and never zero: a 0-width TRY_FMT is not a question the driver can
 * answer sensibly, and YUYV pairs pixels horizontally. */
static uint32_t even_clamp(uint32_t v)
{
    v &= ~1u;
    return v ? v : 2u;
}

uint32_t preview_sdtv_active_height(uint32_t framelines)
{
    /* 625/50 systems carry 576 active lines, 525/60 carry 480. The threshold
     * sits between them rather than testing for exactly 625 or 525 because
     * ENUMSTD is free to report a nonstandard total, and everything above the
     * 525 family is a 625 family in practice. */
    return (framelines >= 600) ? 576u : 480u;
}

int preview_sdtv_modes_for_std(uint32_t framelines, uint32_t fps_num, uint32_t fps_den,
                               preview_sdtv_mode_t *out, int max)
{
    if (!out || max <= 0) return 0;

    const uint32_t full_w = 720u;                              /* every SDTV bridge's active width */
    const uint32_t full_h = preview_sdtv_active_height(framelines);

    /* Full raster, then three-quarter, then half. Fractions rather than a list
     * of named sizes so this stays standard-agnostic: 640x480 is not a
     * meaningful step on a 625-line standard, and hard-coding it would give PAL
     * a mode that is neither square-pixel nor a clean scaler ratio. */
    static const uint32_t num[PREVIEW_SDTV_MAX_LADDER] = { 1u, 3u, 1u };
    static const uint32_t den[PREVIEW_SDTV_MAX_LADDER] = { 1u, 4u, 2u };

    int n = 0;
    for (int i = 0; i < PREVIEW_SDTV_MAX_LADDER && n < max; i++) {
        uint32_t w = even_clamp(full_w * num[i] / den[i]);
        uint32_t h = even_clamp(full_h * num[i] / den[i]);

        /* A ratio that collapses onto one already emitted is not a mode. */
        int dup = 0;
        for (int k = 0; k < n; k++) {
            if (out[k].w == w && out[k].h == h) { dup = 1; break; }
        }
        if (dup) continue;

        out[n].w       = w;
        out[n].h       = h;
        out[n].fps_num = fps_num;
        out[n].fps_den = fps_den ? fps_den : 1u;
        n++;
    }
    return n;
}

void preview_aspect_ratios(uint32_t w, uint32_t h, int aspect_mode,
                           uint32_t *dar_n, uint32_t *dar_d,
                           uint32_t *sar_n, uint32_t *sar_d)
{
    uint32_t dn, dd, sn, sd;

    if (w == 0 || h == 0) {
        /* Nothing negotiated yet. 1:1 is the only honest answer. */
        dn = dd = sn = sd = 1u;
        goto out;
    }

    switch (aspect_mode) {
    case PREVIEW_ASPECT_16_9:
        dn = 16u; dd = 9u;
        break;
    case PREVIEW_ASPECT_SQUARE: {
        /* Square pixels: the raster IS the shape. */
        uint32_t g = gcd_u32(w, h);
        dn = w / g; dd = h / g;
        sn = sd = 1u;
        goto out;
    }
    case PREVIEW_ASPECT_4_3:
    case PREVIEW_ASPECT_AUTO:
    default:
        dn = 4u; dd = 3u;
        break;
    }

    {
        /* SAR = (dar_n * h) / (dar_d * w). 720x480 at 4:3 -> 8:9;
         * 720x576 at 4:3 -> 16:15; 640x480 at 4:3 -> 1:1. */
        uint32_t a = dn * h;
        uint32_t b = dd * w;
        uint32_t g = gcd_u32(a, b);
        sn = a / g;
        sd = b / g;
    }

out:
    if (dar_n) *dar_n = dn;
    if (dar_d) *dar_d = dd;
    if (sar_n) *sar_n = sn;
    if (sar_d) *sar_d = sd;
}

float preview_display_aspect(uint32_t w, uint32_t h, int aspect_mode)
{
    uint32_t dn = 4u, dd = 3u;
    preview_aspect_ratios(w, h, aspect_mode, &dn, &dd, NULL, NULL);
    return (dd == 0u) ? (4.0f / 3.0f) : (float)dn / (float)dd;
}

void preview_aspect_string(uint32_t w, uint32_t h, int aspect_mode,
                           char *out, size_t cap)
{
    if (!out || cap == 0) return;

    uint32_t dn = 4u, dd = 3u;
    preview_aspect_ratios(w, h, aspect_mode, &dn, &dd, NULL, NULL);
    snprintf(out, cap, "%u:%u", (unsigned)dn, (unsigned)dd);
}
