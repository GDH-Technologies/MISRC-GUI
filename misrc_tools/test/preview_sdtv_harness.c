/* SDTV preview geometry assertions. See misrc_gui/input/gui_preview_sdtv.h.
 *
 * This covers the arithmetic that cannot be checked against hardware here:
 * there is one NTSC dongle on this site and no PAL or SECAM source at all, so
 * the 625-line answers are only ever proved by this harness. */

#include "gui_preview_sdtv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_true(int condition, const char *message)
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s\n", message);
    failures++;
}

static void expect_u32(uint32_t actual, uint32_t expected, const char *message)
{
    if (actual == expected) return;
    fprintf(stderr, "FAIL: %s (actual %u, expected %u)\n",
            message, (unsigned)actual, (unsigned)expected);
    failures++;
}

static void expect_ratio(uint32_t w, uint32_t h, int mode,
                         uint32_t dn, uint32_t dd, uint32_t sn, uint32_t sd,
                         const char *message)
{
    uint32_t adn = 0, add = 0, asn = 0, asd = 0;
    preview_aspect_ratios(w, h, mode, &adn, &add, &asn, &asd);
    if (adn == dn && add == dd && asn == sn && asd == sd) return;
    fprintf(stderr, "FAIL: %s (actual DAR %u:%u SAR %u:%u, expected DAR %u:%u SAR %u:%u)\n",
            message, (unsigned)adn, (unsigned)add, (unsigned)asn, (unsigned)asd,
            (unsigned)dn, (unsigned)dd, (unsigned)sn, (unsigned)sd);
    failures++;
}

static void expect_str(const char *actual, const char *expected, const char *message)
{
    if (strcmp(actual, expected) == 0) return;
    fprintf(stderr, "FAIL: %s (actual \"%s\", expected \"%s\")\n",
            message, actual, expected);
    failures++;
}

int main(void)
{
    /* ---- active height ---------------------------------------------------- */

    expect_u32(preview_sdtv_active_height(525), 480, "525-line standards carry 480 active lines");
    expect_u32(preview_sdtv_active_height(625), 576, "625-line standards carry 576 active lines");
    expect_u32(preview_sdtv_active_height(0), 480,
               "an unreported line count falls back to the 525-line raster");

    /* ---- the derived mode ladder ------------------------------------------ */

    preview_sdtv_mode_t m[PREVIEW_SDTV_MAX_LADDER];

    int n = preview_sdtv_modes_for_std(525, 30000, 1001, m, PREVIEW_SDTV_MAX_LADDER);
    expect_true(n >= 1, "a 525-line standard yields at least the full raster");
    expect_u32(m[0].w, 720, "NTSC full raster is 720 wide");
    expect_u32(m[0].h, 480, "NTSC full raster is 480 high");
    expect_u32(m[0].fps_num, 30000, "NTSC rate numerator is carried through");
    expect_u32(m[0].fps_den, 1001, "NTSC rate denominator is carried through");
    expect_u32((uint32_t)n, 3, "NTSC yields full, three-quarter and half");
    expect_u32(m[1].w, 540, "NTSC three-quarter width");
    expect_u32(m[1].h, 360, "NTSC three-quarter height");
    expect_u32(m[2].w, 360, "NTSC half width");
    expect_u32(m[2].h, 240, "NTSC half height");

    n = preview_sdtv_modes_for_std(625, 25, 1, m, PREVIEW_SDTV_MAX_LADDER);
    expect_u32(m[0].w, 720, "PAL full raster is 720 wide");
    expect_u32(m[0].h, 576, "PAL full raster is 576 high");
    expect_u32(m[0].fps_den, 1, "PAL rate denominator is carried through");
    expect_u32(m[2].h, 288, "PAL half height is a whole field");

    for (int i = 0; i < n; i++) {
        expect_true((m[i].w % 2) == 0 && (m[i].h % 2) == 0,
                    "every derived raster is even in both axes");
    }

    /* A caller with room for one mode must get the full raster, not a scaled
     * one: mode 0 is the default everywhere in the preview module. */
    n = preview_sdtv_modes_for_std(525, 30000, 1001, m, 1);
    expect_u32((uint32_t)n, 1, "the ladder honours a max of one");
    expect_u32(m[0].h, 480, "a truncated ladder still leads with the full raster");

    expect_u32((uint32_t)preview_sdtv_modes_for_std(525, 30000, 1001, m, 0), 0,
               "a max of zero yields nothing");
    expect_u32((uint32_t)preview_sdtv_modes_for_std(525, 30000, 1001, NULL, 4), 0,
               "a NULL destination yields nothing");

    /* ---- aspect ------------------------------------------------------------ */

    /* The two SD rasters are different shapes that must present identically. */
    expect_ratio(720, 480, PREVIEW_ASPECT_AUTO,  4, 3, 8, 9,   "NTSC 720x480 is DAR 4:3, SAR 8:9");
    expect_ratio(720, 576, PREVIEW_ASPECT_AUTO,  4, 3, 16, 15, "PAL 720x576 is DAR 4:3, SAR 16:15");
    expect_ratio(640, 480, PREVIEW_ASPECT_AUTO,  4, 3, 1, 1,   "640x480 is already square-pixel 4:3");
    expect_ratio(360, 240, PREVIEW_ASPECT_AUTO,  4, 3, 8, 9,   "a half-raster keeps the NTSC sample aspect");
    expect_ratio(540, 360, PREVIEW_ASPECT_AUTO,  4, 3, 8, 9,   "a three-quarter raster keeps it too");

    expect_ratio(720, 480, PREVIEW_ASPECT_4_3,   4, 3, 8, 9,   "an explicit 4:3 matches auto on SDTV");
    expect_ratio(720, 480, PREVIEW_ASPECT_16_9, 16, 9, 32, 27, "anamorphic widescreen on an NTSC raster");
    expect_ratio(720, 576, PREVIEW_ASPECT_16_9, 16, 9, 64, 45, "anamorphic widescreen on a PAL raster");

    expect_ratio(720, 480, PREVIEW_ASPECT_SQUARE, 3, 2, 1, 1,  "square pixels make 720x480 a 3:2 shape");
    expect_ratio(1920, 1080, PREVIEW_ASPECT_SQUARE, 16, 9, 1, 1,
                 "square pixels make an HD raster 16:9");

    /* Nothing negotiated yet must not divide by zero or claim 4:3. */
    expect_ratio(0, 0, PREVIEW_ASPECT_AUTO, 1, 1, 1, 1, "an unnegotiated raster is 1:1");
    expect_ratio(720, 0, PREVIEW_ASPECT_AUTO, 1, 1, 1, 1, "a zero height is 1:1");

    expect_true(fabsf(preview_display_aspect(720, 480, PREVIEW_ASPECT_AUTO) - 4.0f / 3.0f) < 0.0001f,
                "NTSC displays at 1.333");
    expect_true(fabsf(preview_display_aspect(720, 576, PREVIEW_ASPECT_AUTO) - 4.0f / 3.0f) < 0.0001f,
                "PAL displays at 1.333 as well");
    expect_true(fabsf(preview_display_aspect(720, 480, PREVIEW_ASPECT_16_9) - 16.0f / 9.0f) < 0.0001f,
                "16:9 displays at 1.778");

    /* ---- the ffmpeg -aspect argument -------------------------------------- */

    char buf[16];
    preview_aspect_string(720, 480, PREVIEW_ASPECT_AUTO, buf, sizeof(buf));
    expect_str(buf, "4:3", "NTSC records with -aspect 4:3");
    preview_aspect_string(720, 576, PREVIEW_ASPECT_AUTO, buf, sizeof(buf));
    expect_str(buf, "4:3", "PAL records with -aspect 4:3");
    preview_aspect_string(720, 480, PREVIEW_ASPECT_16_9, buf, sizeof(buf));
    expect_str(buf, "16:9", "anamorphic records with -aspect 16:9");
    preview_aspect_string(720, 480, PREVIEW_ASPECT_SQUARE, buf, sizeof(buf));
    expect_str(buf, "3:2", "square pixels record with the raster's own shape");

    /* A zero-capacity buffer must be left alone rather than written through. */
    preview_aspect_string(720, 480, PREVIEW_ASPECT_AUTO, buf, 0);
    expect_str(buf, "3:2", "a zero capacity writes nothing");

    if (failures != 0) {
        fprintf(stderr, "%d SDTV preview geometry assertion(s) failed\n", failures);
        return 1;
    }

    puts("SDTV preview geometry assertions passed");
    return 0;
}
