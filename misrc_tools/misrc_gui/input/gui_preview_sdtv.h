/*
 * MISRC GUI - SDTV geometry for the USB video preview
 *
 * An analog capture dongle (em28xx, cx231xx, bt878 and friends) is not a
 * webcam. It has no discrete mode list to enumerate: it has a video standard,
 * which fixes the active raster and the frame rate, and a scaler that will
 * serve any smaller size you ask for. V4L2 reports that as a *stepwise* frame
 * size range, and such a device frequently implements neither
 * VIDIOC_ENUM_FRAMEINTERVALS nor VIDIOC_CROPCAP.
 *
 * So the mode list has to be derived rather than enumerated, and the aspect
 * ratio has to be computed rather than assumed. That derivation is pure
 * arithmetic with no device and no window attached, which is exactly the part
 * worth testing: the standards nobody here owns (SECAM, PAL-M, PAL-N) are the
 * ones most likely to be got wrong, and they cannot be checked against real
 * hardware on this site.
 *
 * Deliberately free of raylib, V4L2 and globals so test/preview_sdtv_harness.c
 * can compile it on its own.
 */

#ifndef GUI_PREVIEW_SDTV_H
#define GUI_PREVIEW_SDTV_H

#include <stddef.h>
#include <stdint.h>

/* Persisted as gui_settings_t.usbref_aspect; the numbering is on disk,
 * so append new members, never renumber. */
typedef enum {
    PREVIEW_ASPECT_AUTO   = 0,  /* 4:3 for SDTV -- what a tape actually is */
    PREVIEW_ASPECT_4_3    = 1,
    PREVIEW_ASPECT_16_9   = 2,  /* anamorphic widescreen on an SD raster */
    PREVIEW_ASPECT_SQUARE = 3   /* trust the pixels; SAR 1:1 */
} preview_aspect_mode_t;

#define PREVIEW_SDTV_MAX_LADDER 3

typedef struct {
    uint32_t w, h;
    uint32_t fps_num, fps_den;
} preview_sdtv_mode_t;

/* Active picture height for a standard's *total* line count, as ENUMSTD
 * reports it: 525 -> 480, 625 -> 576. The remainder is blanking and is not
 * captured. */
uint32_t preview_sdtv_active_height(uint32_t framelines);

/* The geometry ladder for one standard: the full active raster first, then the
 * scaler sizes worth offering. Widths and heights are kept even because YUYV
 * subsamples horizontally and an odd height would split a field pair.
 *
 * These are *candidates*. The caller must put each one through VIDIOC_TRY_FMT
 * and adopt what the driver answers -- this function cannot know where a given
 * bridge clamps. Returns the number written, never more than `max`. */
int preview_sdtv_modes_for_std(uint32_t framelines, uint32_t fps_num, uint32_t fps_den,
                               preview_sdtv_mode_t *out, int max);

/* Display and sample aspect for a raster, as reduced integer ratios.
 *
 * The display aspect belongs to the *standard*, not to the pixel count: 525/60
 * and 625/50 are both 4:3 pictures, which is why 720x480 and 720x576 -- two
 * different shapes -- must present identically. The sample aspect then falls
 * out as SAR = (dar_n * h) / (dar_d * w), giving the familiar 8:9 for NTSC and
 * 16:15 for PAL, and 1:1 for a square-pixel raster such as 640x480.
 *
 * Any pointer may be NULL. Safe against w or h of 0 (answers 1:1). */
void preview_aspect_ratios(uint32_t w, uint32_t h, int aspect_mode,
                           uint32_t *dar_n, uint32_t *dar_d,
                           uint32_t *sar_n, uint32_t *sar_d);

/* The display aspect as a float, for the render-side aspect fit. */
float preview_display_aspect(uint32_t w, uint32_t h, int aspect_mode);

/* The display aspect formatted for ffmpeg's -aspect, e.g. "4:3". Always
 * NUL-terminates. */
void preview_aspect_string(uint32_t w, uint32_t h, int aspect_mode,
                           char *out, size_t cap);

#endif /* GUI_PREVIEW_SDTV_H */
