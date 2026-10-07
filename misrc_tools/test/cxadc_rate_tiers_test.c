/*
 * Unit test for the CXADC measured-rate tier snapping
 * (common/cxadc_rate_tiers.h).
 *
 * The snap is what turns a jittery measured feed rate (timed from a real
 * blocking read, e.g. 39.985 MHz) into a clean tier (40.000 MHz), and what
 * rejects unknown rates (keeping the sysfs-derived fallback). These checks
 * pin both behaviors, including the deliberately excluded 35.8 MHz
 * upsampled 8-bit feed (presentation keeps the crystal rate for it).
 */

#include "../common/cxadc_rate_tiers.h"

#include <stdio.h>

static int g_checks;
static int g_failures;

static void expect_snap(uint32_t measured_hz, bool should_snap, uint32_t expected_hz)
{
    g_checks++;
    uint32_t snapped = 0;
    bool snapped_ok = cxadc_snap_rate_tier_hz(measured_hz, &snapped);
    if (snapped_ok != should_snap || (should_snap && snapped != expected_hz)) {
        fprintf(stderr,
                "FAIL: snap(%u) expected snap=%d tier=%u, got snap=%d tier=%u\n",
                (unsigned)measured_hz, should_snap, (unsigned)expected_hz,
                snapped_ok, (unsigned)snapped);
        g_failures++;
    }
}

int main(void)
{
    /* Jittery-but-known feeds snap to their tier. */
    expect_snap(39985698U, true, 40000000U);  /* real 40 MSPS measurement */
    expect_snap(28636363U, true, 28636363U);  /* exact stock crystal */
    expect_snap(28700000U, true, 28636363U);  /* 0.22% over stock */
    expect_snap(28550000U, true, 28636363U);  /* 0.30% under stock */
    expect_snap(14300000U, true, 14318181U);  /* 1.27% under stock 10-bit */
    expect_snap(19900000U, true, 20000000U);  /* 0.50% under 20 MSPS */
    expect_snap(20100000U, true, 20000000U);  /* 0.50% over 20 MSPS */
    expect_snap(53800000U, true, 54000000U);  /* 54-mod */
    expect_snap(26800000U, true, 27000000U);  /* 54-mod 10-bit */
    expect_snap(17900000U, true, 17897727U);  /* stock 10-bit tenxfsc=1 */
    expect_snap(40500000U, true, 40000000U);  /* 1.25% over 40 */

    /* Unknown / ambiguous rates do NOT snap (caller keeps sysfs-derived). */
    expect_snap(39000000U, false, 0);        /* 2.5% off 40: no tier */
    expect_snap(31000000U, false, 0);        /* between 28.6 and 40 */
    expect_snap(5000000U, false, 0);         /* way below all tiers */
    expect_snap(70000000U, false, 0);        /* way above all tiers */
    /* 35.8 (28.636 * 10/8, upsampled 8-bit tenxfsc=1) is deliberately NOT a
     * tier: presentation keeps the crystal rate for that mode. */
    expect_snap(35795454U, false, 0);
    expect_snap(35800000U, false, 0);

    printf("PASS: cxadc rate tier snap (%d checks, %d failures)\n",
           g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
