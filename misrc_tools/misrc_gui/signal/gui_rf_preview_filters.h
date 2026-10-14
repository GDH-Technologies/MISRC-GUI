/*
 * Private VHS monitor filter design. GPL-3.0, as the containing project.
 *
 * NTSC parameter/profile and inverse high-shelf construction ported from
 * machcnz/vhs-decode-dotnet, VHSDecode.Core/Dsp/IirFilterDesign.cs:873-933
 * and Formats/format-params.snapshot.json (NTSC VHS SP deemph_gain/mid/q).
 * The upstream Shelf is the parameterized RBJ second-order high shelf;
 * VideoDeEmphasisShelf swaps numerator and denominator before normalization.
 *
 * This helper designs that transfer at the supplied working rate. At 5MHz
 * it preserves DC, the physical midpoint and both shelf plateaus; bilinear
 * warping and phase differ from a design evaluated at 20MHz or 40MHz. There
 * is no extra rate multiplier, brightness gain, or zero-phase processing.
 * PAL keeps its existing filter; do not use this NTSC profile for PAL.
 */
#ifndef GUI_RF_PREVIEW_FILTERS_H
#define GUI_RF_PREVIEW_FILTERS_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Bitwise finite check remains valid with the production -ffast-math flags. */
static inline bool gui_rf_deemph_finite(double value)
{
    uint64_t bits;
    _Static_assert(sizeof(bits) == sizeof(value), "IEEE binary64 required");
    memcpy(&bits, &value, sizeof(bits));
    return (bits & UINT64_C(0x7ff0000000000000)) != UINT64_C(0x7ff0000000000000);
}

/* H(z) = (b0+b1*z^-1+b2*z^-2)/(1+a1*z^-1+a2*z^-2).
 * Outputs are untouched on failure. The monitor uses fs=5000000 Hz.
 * The finite upper bound avoids badly conditioned float-coefficient designs.
 * The unquantized transfer has exactly unity DC gain; float quantization at
 * 5MHz changes it by about -1.40ppm, not an intentional gain adjustment.
 */
static inline bool gui_rf_ntsc_deemph_coefficients(
    double fs, float *b0, float *b1, float *b2, float *a1, float *a2)
{
    const double midpoint = 273755.82;
    const double q = 0.462088186;
    const double tau = 6.283185307179586476925286766559;
    if (!b0 || !b1 || !b2 || !a1 || !a2 || !gui_rf_deemph_finite(fs)
        || fs <= 2.0*midpoint || fs > 80000000.0)
        return false;

    const double gain = pow(10.0, 13.9794/40.0);
    const double omega = tau*midpoint/fs;
    const double cosine = cos(omega);
    const double alpha = sin(omega)/(2.0*q);
    const double root_gain = sqrt(gain);
    /* Original high-shelf numerator and denominator. */
    const double pre_b0 = gain*((gain+1.0)+(gain-1.0)*cosine+2.0*root_gain*alpha);
    const double pre_b1 = -2.0*gain*((gain-1.0)+(gain+1.0)*cosine);
    const double pre_b2 = gain*((gain+1.0)+(gain-1.0)*cosine-2.0*root_gain*alpha);
    const double pre_a0 = (gain+1.0)-(gain-1.0)*cosine+2.0*root_gain*alpha;
    const double pre_a1 = 2.0*((gain-1.0)-(gain+1.0)*cosine);
    const double pre_a2 = (gain+1.0)-(gain-1.0)*cosine-2.0*root_gain*alpha;
    /* Invert, then normalize the NEW denominator to a0=1. */
    const float cb0 = (float)(pre_a0/pre_b0);
    const float cb1 = (float)(pre_a1/pre_b0);
    const float cb2 = (float)(pre_a2/pre_b0);
    const float ca1 = (float)(pre_b1/pre_b0);
    const float ca2 = (float)(pre_b2/pre_b0);
    /* Jury conditions for the quantized real second-order denominator. */
    if (fabs((double)ca2) >= 1.0 || 1.0+(double)ca1+ca2 <= 0.0
        || 1.0-(double)ca1+ca2 <= 0.0)
        return false;
    *b0=cb0; *b1=cb1; *b2=cb2; *a1=ca1; *a2=ca2;
    return true;
}

/* DC group delay of the actual float coefficients, in working-rate samples.
 * At 5MHz it is approximately 5.1486 samples (1.02972us). This is a
 * mathematical diagnostic, not a portable TBC/display lookahead. The shelf
 * is not linear phase; recording emphasis and the RF/FM/sync paths determine
 * effective edge alignment. Validate that full path before changing timing.
 * Caller must first obtain valid coefficients; invalid fs returns zero.
 */
static inline double gui_rf_ntsc_deemph_dc_delay_samples(double fs)
{
    float b0,b1,b2,a1,a2;
    if (!gui_rf_ntsc_deemph_coefficients(fs,&b0,&b1,&b2,&a1,&a2))
        return 0.0;
    return ((double)b1+2.0*b2)/((double)b0+b1+b2)
         - ((double)a1+2.0*a2)/(1.0+(double)a1+a2);
}

/* Steady TDF2 initialization for the existing rf_filter_sample recurrence:
 * input x0 is residual Hz = blank_hz-fm_center (not absolute carrier Hz).
 * y0 = x0*(b0+b1+b2)/(1+a1+a2);
 * z1 = y0-b0*x0; z2 = b2*x0-a2*y0.
 * Keep the legacy first-order filter + 500kHz LP on the sync branch.
 * Legacy sync delay retains its old first-order + low-pass contributions.
 * Do not add this isolated shelf DC delay to display/TBC sample positions.
 * Recording pre-emphasis, RF/FM processing and the selected sync reference
 * jointly determine edge timing; validate their complete path. In particular,
 * replacing a display filter does not prove its DC-delay difference is a
 * corresponding change in effective luma/chroma registration.
 */
#endif
