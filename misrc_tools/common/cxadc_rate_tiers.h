/*
 * MISRC Common - CXADC feed-rate tiers
 *
 * Known CXADC card feed rates (Hz), used to sanity-snap a measured rate.
 * The measured rate is timed from a real blocking read of /dev/cxadcN, so
 * system jitter can make it differ slightly from the exact tier (e.g.
 * 39.985 MHz instead of 40.000); snapping within a 2% tolerance keeps the
 * readout, labels and resample presets clean.
 *
 * 35.8 MHz (35795454 Hz = 28636363 * 10/8, the upsampled 8-bit tenxfsc=1
 * feed) is deliberately NOT a tier: the sysfs presentation rule reports the
 * crystal rate for that mode, and an unsnapped measurement falls back to
 * the sysfs-derived rate, preserving that presentation.
 *
 * Dependency-free (header-only) so unit tests compile it standalone.
 */

#ifndef MISRC_CXADC_RATE_TIERS_H
#define MISRC_CXADC_RATE_TIERS_H

#include <stdbool.h>
#include <stdint.h>

static const uint32_t CXADC_RATE_TIER_HZ[] = {
    14318181U,  /* 14.3: stock 10-bit (28.636 / 2) */
    17897727U,  /* 17.9: stock 10-bit tenxfsc=1 (28.636 * 5/8) */
    20000000U,  /* 20: clockgen/40-mod 10-bit */
    27000000U,  /* 27: 54-mod 10-bit */
    28636363U,  /* 28.636: stock 8-bit crystal */
    40000000U,  /* 40: 40 MHz mod / clockgen 8-bit */
    54000000U,  /* 54: 54 MHz mod 8-bit */
};
#define CXADC_RATE_TIER_COUNT ((int)(sizeof(CXADC_RATE_TIER_HZ) / sizeof(CXADC_RATE_TIER_HZ[0])))

/* Tolerance for snapping a measured rate to a tier: 2% (the stale-sysfs
 * cases are 28.6 vs 40 vs 54 — a 1.4x ratio — so 2% is unambiguous). */
#define CXADC_RATE_TIER_TOLERANCE_PCT 2ULL

/* Snap a measured feed rate to the nearest known tier.
 * Returns true and writes the tier when within the tolerance; false when
 * the measurement matches no known tier (caller keeps its sysfs-derived
 * rate). */
static inline bool cxadc_snap_rate_tier_hz(uint32_t measured_hz, uint32_t *snapped_hz_out)
{
    for (int i = 0; i < CXADC_RATE_TIER_COUNT; i++) {
        uint32_t tier = CXADC_RATE_TIER_HZ[i];
        uint64_t diff = (measured_hz > tier) ? ((uint64_t)measured_hz - tier)
                                             : ((uint64_t)tier - measured_hz);
        if (diff * 100ULL <= (uint64_t)tier * CXADC_RATE_TIER_TOLERANCE_PCT) {
            if (snapped_hz_out) {
                *snapped_hz_out = tier;
            }
            return true;
        }
    }
    return false;
}

#endif /* MISRC_CXADC_RATE_TIERS_H */
