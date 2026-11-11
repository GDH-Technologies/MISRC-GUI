/*
 * CPU VHS SP / NTSC Betamax / SuperBeta RF monitor. GPL-3.0, as the containing MISRC GUI project.
 *
 * Algorithm references: machcnz/vhs-decode-dotnet, current fast preview:
 * Formats/format-params.snapshot.json, Dsp/SyncAnalysis.cs,
 * Dsp/VBlankSyncResolver.cs, Decode/VhsChromaDecoder.cs and
 * Preview/PreviewFieldRenderer.cs. Their upstream is oyvindln/vhs-decode.
 * This is a streaming, reduced-bandwidth implementation, not an Exact decoder
 * port. Color-under is demodulated directly into complex baseband; per-line
 * time-base/carrier correction and measured burst replace materializing a
 * second 4fSC carrier. Expensive comb/repair/export processing is omitted.
 */
#include "gui_rf_preview.h"
#include "gui_rf_preview_filters.h"
#include "gui_rf_preview_geometry.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE2__) && !defined(GUI_RF_FORCE_SCALAR_IQ)
#include <emmintrin.h>
#define GUI_RF_PACKED_IQ 1
#else
#define GUI_RF_PACKED_IQ 0
#endif

#define RF_PI 3.14159265358979323846
#define RF_VIDEO_RATE 5000000.0
#define RF_RING_SIZE (1u << 19)
#define RF_RING_MASK (RF_RING_SIZE - 1u)
#define RF_PULSE_SIZE 2048u
#define RF_MAX_LINES 320
#define RF_LEVEL_GRACE 250000u /* 50 ms at the 5 MHz working rate. */
#define RF_LEVEL_POINTS 2048
#define RF_LEVEL_PULSES 256

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } rf_biquad_t;
#if GUI_RF_PACKED_IQ
/* Four independent IIR streams, never four adjacent samples or serial stages.
 * Unaligned loads keep decoder allocation portable on both x86 and x64. */
typedef struct {
    float b0[4],b1[4],b2[4],a1[4],a2[4],z1[4],z2[4];
} rf_biquad4_t;
#endif
typedef struct { double re, im; } rf_complex_t;
typedef enum { RF_PULSE_NONE, RF_PULSE_EQ, RF_PULSE_H, RF_PULSE_V } rf_pulse_kind_t;

/* Hybrid Full-mode background levels: the IRE-domain constants live in
 * gui_rf_preview.h (RF_HYBRID_*) - shared with the CVBS full-frame decode
 * so the two decoders can never drift apart. The derivation is documented
 * there (translated from tbc-tools OutputWriter's fixed 16-bit TBC offsets,
 * commit 063d83e: NTSC 0x0d48, PAL 0x0708). */
typedef struct {
    int width, height, active_first, eq_count;
    double line_us, blank_hz, sync_hz, hz_ire, carrier_hz;
    double burst_start_us, burst_end_us, active_start_us, active_end_us;
    double deemph_zero, deemph_pole;
    double fm_center, rf_low, rf_high, chroma_lowpass;
    int rf_highpass_sections, chroma_sections;
    double shelf_gain, shelf_mid, shelf_q;
    bool beta_rotation;
    double hybrid_black_ire, hybrid_span_ire;
} rf_profile_t;

/* SP carrier/IRE/burst/geometry parameters from the referenced format snapshot. */
static const rf_profile_t rf_profiles[] = {
    {768, 576, 22, 5, GUI_RF_PAL_LINE_US, 4100000.0, 3800000.0, 7000.0, 626953.0,
     5.6, 7.85, GUI_RF_PAL_ACTIVE_START_US, GUI_RF_PAL_ACTIVE_END_US, 100e-9, 400e-9,
     4300000.0,1300000.0,6500000.0,400000.0,1,2,0,0,0,false,RF_HYBRID_BLACK_IRE_PAL,RF_HYBRID_SPAN_IRE_PAL},
    {640, 480, 20, 6, GUI_RF_NTSC_LINE_US, 3685714.2857142854, 3400000.0,
     7142.857142857143, 629370.6293706294,
     5.3, 7.8, GUI_RF_NTSC_ACTIVE_START_US, GUI_RF_NTSC_ACTIVE_END_US, 120e-9, 320e-9,
     3900000.0,1300000.0,6500000.0,400000.0,1,2,13.9794,273755.82,.462088186,false,RF_HYBRID_BLACK_IRE_NTSC,RF_HYBRID_SPAN_IRE_NTSC},
    /* .NET format-params.snapshot.json NTSC BETAMAX / BETAMAX_HIFI:
     * IRE0/Hz-per-IRE, 688.374kHz color-under, [2,0] quarter-turn rotation,
     * RF cutoffs and 8dB/100kHz shelf. Preview remains bandwidth-reduced;
     * nonlinear sub-deemphasis, RF equalization and dropout repair are omitted. */
    {640,480,20,6,GUI_RF_NTSC_LINE_US,3942857.1428571427,3600000.0,
     8571.42857142857,688374.125874126,
     5.3,7.8,GUI_RF_NTSC_ACTIVE_START_US,GUI_RF_NTSC_ACTIVE_END_US,120e-9,320e-9,
     4200000.0,1600000.0,5380000.0,600000.0,1,2,8.0,100000.0,.5,true,RF_HYBRID_BLACK_IRE_NTSC,RF_HYBRID_SPAN_IRE_NTSC},
    {640,480,20,6,GUI_RF_NTSC_LINE_US,4200000.0,3800000.0,
     10000.0,688374.125874126,
     5.3,7.8,GUI_RF_NTSC_ACTIVE_START_US,GUI_RF_NTSC_ACTIVE_END_US,120e-9,320e-9,
     4500000.0,2000000.0,5880000.0,350000.0,6,4,8.0,100000.0,.5,true,RF_HYBRID_BLACK_IRE_NTSC,RF_HYBRID_SPAN_IRE_NTSC},
    /* Ordinary NTSC SuperBeta: 4.4MHz sync tip / 5.6MHz peak white
     * (Sencore TT135 service reference), with -40/100 IRE endpoints.
     * Both audio choices share this video modulation; Hi-Fi is routed by the
     * worker. The lightweight RF/chroma cutoffs are preview engineering
     * choices, retaining 20MSPS input / 5MSPS processing, not tape standards. */
    {640,480,20,6,GUI_RF_NTSC_LINE_US,4742857.142857143,4400000.0,
     8571.42857142857,688374.125874126,
     5.3,7.8,GUI_RF_NTSC_ACTIVE_START_US,GUI_RF_NTSC_ACTIVE_END_US,120e-9,320e-9,
     5000000.0,2000000.0,6880000.0,350000.0,6,4,8.0,100000.0,.5,true,RF_HYBRID_BLACK_IRE_NTSC,RF_HYBRID_SPAN_IRE_NTSC},
    {640,480,20,6,GUI_RF_NTSC_LINE_US,4742857.142857143,4400000.0,
     8571.42857142857,688374.125874126,
     5.3,7.8,GUI_RF_NTSC_ACTIVE_START_US,GUI_RF_NTSC_ACTIVE_END_US,120e-9,320e-9,
     5000000.0,2000000.0,6880000.0,350000.0,6,4,8.0,100000.0,.5,true,RF_HYBRID_BLACK_IRE_NTSC,RF_HYBRID_SPAN_IRE_NTSC},
    /* Native SECAM VHS divides the FM chroma carrier/deviation by four.
     * This is not PAL color-under and not heterodyne MESECAM. The luma
     * modulation and 625/50 geometry are shared with PAL VHS SP. A steeper
     * RF high-pass rejects its higher 1.06/1.10MHz chroma band. */
    {768,576,22,5,GUI_RF_PAL_LINE_US,4100000.0,3800000.0,7000.0,1082031.25,
     5.6,7.85,GUI_RF_PAL_ACTIVE_START_US,GUI_RF_PAL_ACTIVE_END_US,100e-9,400e-9,
     4300000.0,1600000.0,6500000.0,350000.0,2,4,0,0,0,false,RF_HYBRID_BLACK_IRE_PAL,RF_HYBRID_SPAN_IRE_PAL},
    /* MESECAM preserves studio FM deviation and inverts the chroma band
     * through the PAL converter LO, 5.060571875MHz. Rest Dr/Db therefore
     * land at 654321.875/810571.875Hz, without PAL head/line rotation.
     * The wider sideband filter is a preview choice, not a recording spec. */
    {768,576,22,5,GUI_RF_PAL_LINE_US,4100000.0,3800000.0,7000.0,732446.875,
     5.6,7.85,GUI_RF_PAL_ACTIVE_START_US,GUI_RF_PAL_ACTIVE_END_US,100e-9,400e-9,
     4300000.0,1300000.0,6500000.0,600000.0,2,4,0,0,0,false,RF_HYBRID_BLACK_IRE_PAL,RF_HYBRID_SPAN_IRE_PAL},
    /* Sony EDV-9500: 6.8MHz sync / 9.3MHz 100IRE white, 688kHz C.
     * Use the reduced 20MSPS preview band; upper FM sidebands are omitted.
     * Flat preview de-emphasis deliberately omits Sony's nonlinear LCR
     * expansion. RF/IQ bandwidths are streaming-preview engineering choices. */
    {640,480,20,6,GUI_RF_NTSC_LINE_US,7514285.714285714,6800000.0,
     17857.14285714286,688374.125874126,
     5.3,7.8,GUI_RF_NTSC_ACTIVE_START_US,GUI_RF_NTSC_ACTIVE_END_US,0,0,
     8050000.0,4200000.0,9500000.0,350000.0,2,4,0,1000000.0,.70710678,true,RF_HYBRID_BLACK_IRE_NTSC,RF_HYBRID_SPAN_IRE_NTSC}
};

_Static_assert(sizeof(rf_profiles)/sizeof(rf_profiles[0])==GUI_RF_PROFILE_COUNT,
               "RF profile table must match public profile IDs");

static bool rf_profile_valid(gui_rf_profile_t profile) {
    return profile>=GUI_RF_PROFILE_PAL_VHS && profile<GUI_RF_PROFILE_COUNT;
}

uint32_t gui_rf_profile_video_rate_hz(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_ED_BETA ? GUI_RF_ED_BETA_RATE_HZ : GUI_RF_DECODE_RATE_HZ;
}

gui_rf_format_t gui_rf_profile_format(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_PAL_VHS || profile==GUI_RF_PROFILE_SECAM_VHS
        || profile==GUI_RF_PROFILE_MESECAM_VHS
        ? GUI_RF_PAL : GUI_RF_NTSC;
}

static bool rf_native_secam(const gui_rf_decoder_t *d);
static bool rf_secam_fm(const gui_rf_decoder_t *d);

const char *gui_rf_profile_name(gui_rf_profile_t profile) {
    static const char *names[]={"PAL VHS SP","NTSC VHS SP","NTSC Betamax","NTSC Beta Hi-Fi",
        "NTSC SuperBeta","NTSC SuperBeta Hi-Fi","SECAM VHS SP","MESECAM VHS SP","NTSC ED Beta"};
    return rf_profile_valid(profile) ? names[profile] : "Unknown RF profile";
}

bool gui_rf_profile_has_hifi(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI ||
           profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI || profile==GUI_RF_PROFILE_NTSC_ED_BETA;
}

struct gui_rf_decoder {
    gui_rf_format_t format;
    gui_rf_profile_t profile;
    rf_profile_t p;
    double raw_rate;
    unsigned raw_decimation;
    bool full_frame;   /* Active/Full display mode (see set_frame_mode). */
    float *storage, *luma, *sync, *ci, *cq, *envelope, *sync_luma, *secam_fm;
    uint8_t *rgba;
    uint64_t next_raw, low_first, low_last;
    bool have_raw, have_low;
    float *mesecam_i,*mesecam_q;
    rf_biquad_t rf_hp[6], rf_lp, yi[4], yq[4], ui[4], uq[4], luma_deemph;
#if GUI_RF_PACKED_IQ
    rf_biquad4_t beta_iq[4];
#endif
    double y_cos, y_sin, y_step_cos, y_step_sin;
    double c_cos, c_sin, c_step_cos, c_step_sin, fm_center;
    float prev_i, prev_q, deemph_x, deemph_y, sync_y;
    float secam_prev_i, secam_prev_q, secam_deemph_x, secam_deemph_y;
    float secam_b0, secam_b1, secam_a1;
    rf_biquad_t secam_video_lp[2];
    double mesecam_reference_ratio;
    uint8_t secam_pixel_valid[RF_MAX_LINES][GUI_RF_FRAME_MAX_WIDTH];
    bool secam_have_ident, secam_dr_even;
    int secam_ident_streak, secam_ident_age, secam_last_parity;
    double secam_last_field;
    float deemph_b0, deemph_b1, deemph_a1, sync_alpha;
    double luma_delay, chroma_delay, sync_delay, sync_luma_delay, secam_fm_delay;
    double luma_wow;
    bool have_luma_wow, use_ntsc_deemph;
    float field_rows[2][3][GUI_RF_FRAME_MAX_WIDTH];
    /* One field of native line-sequential C, never the opposite field. */
    float secam_rows[RF_MAX_LINES][GUI_RF_FRAME_MAX_WIDTH];
    double sync_est, blank_est, period;
    double sync_anchor, blank_anchor;
    uint64_t level_search_after, last_level_field;
    double display_blank_hz, display_hz_ire;
    uint64_t display_level_search_after;
    bool display_levels_valid;
    bool in_pulse;
    double pulse_start, previous_sync;
    double last_h, previous_h, last_h_width, previous_h_width;
    double first_eq, last_pulse, last_pulse_end, first_v;
    int eq_run, v_run, candidate_v_run, good_h;
    bool have_short_broad, have_split_broad, split_broad_weak;
    double short_broad_start, short_broad_end, split_broad_start;
    double split_tail_start, split_tail_end;
    double split_follow_start[3], split_follow_end[3];
    int split_follow_count;
    rf_pulse_kind_t last_kind;
    double h_positions[RF_PULSE_SIZE];
    unsigned h_head, h_count;
    double field_start;
    int field_parity;
    bool have_field;
    unsigned ready_fields;
    bool ready_color[2];
    gui_rf_decode_status_t status;
};

static bool rf_native_secam(const gui_rf_decoder_t *d) {
    return d->profile==GUI_RF_PROFILE_SECAM_VHS;
}

static bool rf_secam_fm(const gui_rf_decoder_t *d) {
    return rf_native_secam(d) || d->profile==GUI_RF_PROFILE_MESECAM_VHS;
}

static double rf_clamp(double x, double lo, double hi) {
    return x < lo ? lo : x > hi ? hi : x;
}

static rf_biquad_t rf_filter(double fs, double fc, double q, bool highpass) {
    double w = 2.0 * RF_PI * fc / fs, c = cos(w), a = sin(w) / (2.0 * q);
    double s = 1.0 / (1.0 + a), b = (1.0 + (highpass ? c : -c)) * .5;
    rf_biquad_t f = {(float)(b*s), (float)((highpass ? -2.0 : 2.0)*b*s),
                    (float)(b*s), (float)(-2.0*c*s), (float)((1.0-a)*s), 0, 0};
    return f;
}

static inline float rf_filter_sample(rf_biquad_t *f, float x) {
    float y = f->b0*x + f->z1;
    f->z1 = f->b1*x - f->a1*y + f->z2;
    f->z2 = f->b2*x - f->a2*y;
    return y;
}

#if GUI_RF_PACKED_IQ
static inline __m128 rf_filter4_sample(rf_biquad4_t *f,__m128 x) {
    __m128 y=_mm_add_ps(_mm_mul_ps(_mm_loadu_ps(f->b0),x),_mm_loadu_ps(f->z1));
    __m128 z1=_mm_add_ps(_mm_sub_ps(_mm_mul_ps(_mm_loadu_ps(f->b1),x),
                                    _mm_mul_ps(_mm_loadu_ps(f->a1),y)),_mm_loadu_ps(f->z2));
    __m128 z2=_mm_sub_ps(_mm_mul_ps(_mm_loadu_ps(f->b2),x),
                         _mm_mul_ps(_mm_loadu_ps(f->a2),y));
    _mm_storeu_ps(f->z1,z1);_mm_storeu_ps(f->z2,z2);
    return y;
}
#endif

static double rf_filter_delay(const rf_biquad_t *f) {
    return (f->b1 + 2.0*f->b2)/(f->b0+f->b1+f->b2)
         - (f->a1 + 2.0*f->a2)/(1.0+f->a1+f->a2);
}

static float rf_read(const gui_rf_decoder_t *d, const float *plane, double pos) {
    if (!d->have_low || pos < (double)d->low_first || pos + 1.0 > (double)d->low_last)
        return 0.0f;
    uint64_t at = (uint64_t)floor(pos);
    float f = (float)(pos - (double)at);
    float a = plane[at & RF_RING_MASK], b = plane[(at+1u) & RF_RING_MASK];
    return a + (b-a)*f;
}

static rf_complex_t rf_rotate(rf_complex_t z, double phase) {
    double c = cos(phase), s = sin(phase);
    rf_complex_t r = {z.re*c-z.im*s, z.re*s+z.im*c};
    return r;
}

/* Correct the residual color-under carrier produced by local time-base stretch.
 * Both Y and color use the same measured line map. A burst-only rotation would
 * leave a hue ramp across each line when tape speed varies. */
static rf_complex_t rf_color_at(const gui_rf_decoder_t *d, double line_start,
                               double line_period, double field_start,
                               int line, double offset) {
    double pos = line_start + offset + d->chroma_delay - d->sync_delay;
    rf_complex_t z = {rf_read(d, d->ci, pos), rf_read(d, d->cq, pos)};
    double nominal = d->p.line_us * 5.0;
    double timing = line_start-field_start-line*nominal
                  + offset*(1.0-nominal/line_period);
    return rf_rotate(z, 2.0*RF_PI*d->p.carrier_hz/RF_VIDEO_RATE*timing);
}

static double rf_nearest_h(const gui_rf_decoder_t *d, double target, double radius,
                           bool *measured) {
    double best = radius, result = target;
    *measured = false;
    for (unsigned n=0; n<d->h_count; ++n) {
        unsigned idx = (d->h_head + RF_PULSE_SIZE - 1u - n) % RF_PULSE_SIZE;
        double pos = d->h_positions[idx];
        if (pos < target-radius) break;
        double distance = fabs(pos-target);
        if (distance < best) { best=distance; result=pos; *measured=true; }
    }
    return result;
}

static uint8_t rf_byte(double value) {
    return (uint8_t)lrint(rf_clamp(value*255.0, 0.0, 255.0));
}

static int rf_compare_double(const void *a, const void *b) {
    double x=*(const double *)a, y=*(const double *)b;
    return x<y ? -1 : x>y;
}

static double rf_median(double *values, int count) {
    qsort(values,(size_t)count,sizeof(*values),rf_compare_double);
    return count&1 ? values[count/2] : (values[count/2-1]+values[count/2])*.5;
}

/* FM frequencies change with physical tape/clock speed, not ADC amplitude.
 * Estimate only from neighboring measured H anchors; missing or grossly
 * stretched lines cannot modulate display levels. Median/MAD rejection and
 * approximately one-field smoothing keep this independent of noisy porch
 * pixels. This deliberately corrects only a small trustworthy speed range. */
static void rf_prepare_luma_wow(gui_rf_decoder_t *d, const double *periods,
                                const bool *anchors, int rows, double nominal,
                                double *factors) {
    double measured[RF_MAX_LINES], deviations[RF_MAX_LINES];
    int count=0;
    for (int y=0; y<rows; ++y) {
        double factor=periods[y]/nominal;
        factors[y]=factor;
        if (anchors[y] && anchors[y+1] && factor>=.98 && factor<=1.02)
            measured[count++]=factor;
    }
    if (count<rows/2) {
        for (int y=0; y<rows; ++y) factors[y]=d->luma_wow;
        return;
    }
    double median=rf_median(measured,count);
    for (int y=0; y<count; ++y) deviations[y]=fabs(measured[y]-median);
    double threshold=fmax(.001,15.0*rf_median(deviations,count));
    if (!d->have_luma_wow) {
        d->luma_wow=median;
        d->have_luma_wow=true;
    }
    double alpha=1.0/(d->format==GUI_RF_PAL ? 312.5 : 262.5);
    for (int y=0; y<rows; ++y) {
        double target=factors[y];
        if (!anchors[y] || !anchors[y+1] || target<.98 || target>1.02
            || fabs(target-median)>threshold) target=median;
        d->luma_wow=rf_clamp(d->luma_wow+(target-d->luma_wow)*alpha,.98,1.02);
        factors[y]=d->luma_wow;
    }
}

/* Chroma-only 2x2 averaging matches field-local display sampling. Both rows
 * belong to this actual field; luma and the other field are never averaged.
 * A weak burst supplies no color samples, so a valid neighbor is reused
 * without importing the weak line's noise. */
static void rf_render_rows(gui_rf_decoder_t *d, int first, int count, int parity,
                           bool first_valid, bool second_valid) {
    for (int x=0; x<d->p.width; x+=2) {
        int columns=x+1<d->p.width ? 2 : 1;
        double u=0.0,v=0.0; int samples=0;
        for (int r=0; r<count; ++r) if (r ? second_valid : first_valid) {
            for (int c=0; c<columns; ++c) {
                u+=d->field_rows[r][1][x+c]; v+=d->field_rows[r][2][x+c];
                samples++;
            }
        }
        if (samples) { u/=samples; v/=samples; }
        for (int r=0; r<count; ++r) {
            size_t row=(size_t)((first+r)*2+parity)*d->p.width*4u;
            for (int c=0; c<columns; ++c) {
                double luminance=d->field_rows[r][0][x+c];
                d->rgba[row+(x+c)*4u]=rf_byte(luminance+1.140*v);
                d->rgba[row+(x+c)*4u+1u]=rf_byte(luminance-.395*u-.581*v);
                d->rgba[row+(x+c)*4u+2u]=rf_byte(luminance+2.032*u);
                d->rgba[row+(x+c)*4u+3u]=255;
            }
        }
    }
}

static void rf_color_loss(gui_rf_decoder_t *d, const char *reason) {
    if (d->status.color_locked) {
        d->status.color_losses++;
        snprintf(d->status.last_color_loss_reason,
                 sizeof(d->status.last_color_loss_reason),"%s",reason);
    }
    d->status.color_locked=false;
}

typedef enum { RF_REJECT_CADENCE, RF_REJECT_PARITY, RF_REJECT_LINE_MAP } rf_reject_t;

static void rf_prepare_display_levels(gui_rf_decoder_t *d);

/* A stopped/unstable tape can leave the learned threshold outside a healthy
 * signal's sync-to-blank range. H edges cannot then update it back into range.
 * Discard acquisition history once on H loss, keeping source cursors, filters
 * and diagnostic counters continuous. The next frame needs two fresh fields. */
static void rf_reacquire_sync(gui_rf_decoder_t *d) {
    rf_color_loss(d,"RF horizontal sync lost");
    d->status.h_locked=d->status.v_locked=false;
    d->sync_est=d->sync_anchor=d->p.sync_hz;
    d->blank_est=d->blank_anchor=d->p.blank_hz;
    d->period=d->p.line_us*5.0;
    d->good_h=0;d->last_h=d->previous_h=0.0;
    d->last_h_width=d->previous_h_width=0.0;
    d->h_head=d->h_count=0;
    d->in_pulse=false;d->last_kind=RF_PULSE_NONE;
    d->eq_run=d->v_run=d->candidate_v_run=0;
    d->have_short_broad=d->have_split_broad=false;
    d->split_follow_count=0;
    d->ready_fields=0;d->have_field=false;
    d->have_luma_wow=false;d->luma_wow=1.0;
    d->display_levels_valid=false;
    d->secam_have_ident=false;d->secam_ident_streak=d->secam_ident_age=0;
    d->display_level_search_after=0;
    snprintf(d->status.reason,sizeof(d->status.reason),"RF sync lost");
}

static void rf_reject_field(gui_rf_decoder_t *d, rf_reject_t cause, const char *reason) {
    d->status.fields_rejected++;
    if (cause==RF_REJECT_CADENCE) d->status.reject_cadence++;
    else if (cause==RF_REJECT_PARITY) d->status.reject_parity++;
    else d->status.reject_line_map++;
    snprintf(d->status.last_reject_reason,sizeof(d->status.last_reject_reason),"%s",reason);
    snprintf(d->status.reason,sizeof(d->status.reason),"%s",reason);
    rf_color_loss(d,reason);
    d->status.v_locked=false;
    d->display_levels_valid=false;
    d->display_level_search_after=0;
}

/* SECAM VHS: ITU-R BT.470-6 Table 2 and JVC VTG82063 section 3.14.
 * MESECAM instead mixes below LO=5.060571875MHz, with inverted spectrum
 * and full studio deviation. Its H references are separately usable.
 * In the native method:
 * The tape divider records fOB/4=1.0625MHz and fOR/4=1.1015625MHz, with
 * deviations 230kHz/4 and 280kHz/4. No PAL burst, phase rotation, or cloche
 * restoration belongs in this direct FM path. ITU-R BT.470-6 Table 2.18
 * identifies components from references, never active picture frequency:
 * saturated legal magenta reverses the two active medians. Native divider
 * settling makes the H porch unreliable on this tape, so identify the VBI
 * bottles by their opposite ramps AND known plateau frequencies. A gap
 * discards the four-field confirmation and bounded identification flywheel. */
/* Take colour-under from the already buffered MESECAM IQ. The 600kHz
 * sideband filter alone admits low RF near its negative-frequency edge;
 * its phase discriminator can then count spurious cycles even with a strong
 * carrier. Remodulate at 5MSPS and reject that ingress in both directions.
 * This adds no highpass group delay to the legal full-deviation FM. The
 * 128-sample padding bounds each IIR transient outside the committed chunk;
 * both pads must already be present, including after gaps and reacquisition.
 * The 200kHz preview cutoff is below the legal 304kHz instantaneous rail:
 * RF sidebands extend beyond those rails and may not be hard-clipped there. */
/* Additional octant reduction covers legal full-deviation phase without
 * a libm call. For .25<|x/z|<=.7, rotation by pi/8 leaves |r|<=.2217;
 * the same r^7 alternating series then errs below.114Hz at5MSPS, less
 * than the existing .35Hz bound on the unchanged small-ratio branch. */
static inline double rf_mesecam_phase(double x,double z) {
    if(z>0 && fabs(x)<=.25*z) {
        double r=x/z,r2=r*r;
        return r*(1+r2*(-1.0/3+r2*(1.0/5-r2/7)));
    }
    if(z>0 && fabs(x)<=.7*z) {
        const double tangent=.4142135623730950488;
        double magnitude=fabs(x),r=(magnitude-tangent*z)/(z+tangent*magnitude),r2=r*r;
        double residual=r*(1+r2*(-1.0/3+r2*(1.0/5-r2/7)));
        return copysign(RF_PI/8.0+residual,x);
    }
    return atan2f((float)x,(float)z);
}

static bool rf_level_finite(float value);

/* A finite float's square and their sum fit in double without underflow.
 * Keep libm's Inf/NaN rules through the exponent-bit finite check, which
 * also survives finite-math-only. Normal RF needs only hardware sqrt. */
static inline float rf_mesecam_magnitude(float i,float q) {
    if(!rf_level_finite(i) || !rf_level_finite(q))return hypotf(i,q);
    return (float)sqrt((double)i*i+(double)q*q);
}

static void rf_prepare_mesecam_field(gui_rf_decoder_t *d,double field_start) {
    /* 2048 samples commit 409.6us at 5MSPS. Each 25.6us pad is over
     * twelve decay constants of the slowest 200kHz fourth-order pole.
     * Full mode has at most313 rows; cover an extra measured H so its last
     * row's demodulator/post-LP lookahead is still inside prepared data. */
    enum { PAD=128, STEP=2048, N=STEP+2*PAD };
    const double w=2.0*RF_PI*d->p.carrier_hz/RF_VIDEO_RATE;
    const double step_c=cos(w),step_s=sin(w);
    int64_t first=(int64_t)floor(field_start+d->secam_fm_delay-d->sync_delay)-16;
    int64_t last=(int64_t)ceil(field_start+314*d->period+d->secam_fm_delay-d->sync_delay)+32;
    if(first<(int64_t)d->low_first+PAD)first=(int64_t)d->low_first+PAD;
    if(last>(int64_t)d->low_last-PAD)last=(int64_t)d->low_last-PAD;
#if GUI_RF_PACKED_IQ
    /* Lanes0/1 and2/3 are separate chunks' I/Q.
     * Each oscillator seed, pad, filter state, pass order and commit span
     * remains independent. No adjacent samples share recursive state. */
    for(int64_t start=first;start<last;start+=2*STEP) {
        const int blocks=start+STEP<last?2:1;
        int take[2]={0,0},span[2]={0,0};
        int64_t chunk_start[2]={start,start+STEP};
        float re[2][N],im[2][N];
        for(int block=0;block<blocks;block++) {
            int64_t at_start=chunk_start[block];
            take[block]=last-at_start<STEP?(int)(last-at_start):STEP;
            span[block]=take[block]+2*PAD;
            double c=cos(w*(at_start-PAD)),s=sin(w*(at_start-PAD));
            for(int n=0;n<span[block];n++) {
                unsigned at=(unsigned)((at_start-PAD+n)&RF_RING_MASK);
                double i=d->mesecam_i[at],q=d->mesecam_q[at];
                re[block][n]=(float)(i*c-q*s);im[block][n]=(float)(i*s+q*c);
                double next_c=c*step_c-s*step_s;s=s*step_c+c*step_s;c=next_c;
            }
        }
        const double qs[2]={.5411961,1.3065630};
        for(int direction=0;direction<2;direction++) {
            rf_biquad4_t hp[2]={0};
            for(int j=0;j<2;j++) {
                rf_biquad_t f=rf_filter(RF_VIDEO_RATE,200000.0,qs[j],true);
                for(int lane=0;lane<4;lane++) {
                    hp[j].b0[lane]=f.b0;hp[j].b1[lane]=f.b1;hp[j].b2[lane]=f.b2;
                    hp[j].a1[lane]=f.a1;hp[j].a2[lane]=f.a2;
                }
            }
            if(blocks==2 && span[0]==span[1]) {
                /* The common full pair has no per-sample tail branch. */
                for(int k=0;k<span[0];k++) {
                    int n=direction?span[0]-1-k:k;
                    __m128 value=_mm_setr_ps(re[0][n],im[0][n],re[1][n],im[1][n]);
                    value=rf_filter4_sample(&hp[0],value);
                    value=rf_filter4_sample(&hp[1],value);
                    float out[4];_mm_storeu_ps(out,value);
                    re[0][n]=out[0];im[0][n]=out[1];re[1][n]=out[2];im[1][n]=out[3];
                }
            } else {
                /* Backward sample indices are relative to EACH own span.
                 * A short second chunk stops writing at its actual end;
                 * later dummy-lane state is never committed or reused. */
                for(int k=0;k<span[0];k++) {
                    int n0=direction?span[0]-1-k:k;
                    int n1=direction?span[1]-1-k:k;
                    bool second=k<span[1];
                    /* Finished dummy lanes must not run their obsolete state
                     * into subnormals when the host leaves FTZ disabled.
                     * They have no remaining input/output in either pass. */
                    if(k==span[1])for(int j=0;j<2;j++)for(int lane=2;lane<4;lane++)
                        hp[j].z1[lane]=hp[j].z2[lane]=0.0f;
                    __m128 value=_mm_setr_ps(re[0][n0],im[0][n0],
                        second?re[1][n1]:0.0f,second?im[1][n1]:0.0f);
                    value=rf_filter4_sample(&hp[0],value);
                    value=rf_filter4_sample(&hp[1],value);
                    float out[4];_mm_storeu_ps(out,value);re[0][n0]=out[0];im[0][n0]=out[1];
                    if(second){re[1][n1]=out[2];im[1][n1]=out[3];}
                }
            }
        }
        for(int block=0;block<blocks;block++)for(int n=PAD;n<PAD+take[block];n++) {
            double cross=(double)re[block][n-1]*im[block][n]-(double)im[block][n-1]*re[block][n];
            double dot=(double)re[block][n-1]*re[block][n]+(double)im[block][n-1]*im[block][n];
            double x=cross*step_c-dot*step_s,z=dot*step_c+cross*step_s;
            double phase;
            phase=rf_mesecam_phase(x,z);
            unsigned at=(unsigned)((chunk_start[block]+n-PAD)&RF_RING_MASK);
            d->secam_fm[at]=(float)(d->p.carrier_hz+phase*(RF_VIDEO_RATE/(2.0*RF_PI)));
            d->cq[at]=rf_mesecam_magnitude(re[block][n],im[block][n]);
        }
    }
#else
    for(int64_t start=first;start<last;start+=STEP) {
        int take=last-start<STEP?(int)(last-start):STEP,span=take+2*PAD;
        float re[N],im[N];
        double c=cos(w*(start-PAD)),s=sin(w*(start-PAD));
        for(int n=0;n<span;n++) {
            unsigned at=(unsigned)((start-PAD+n)&RF_RING_MASK);
            double i=d->mesecam_i[at],q=d->mesecam_q[at];
            re[n]=(float)(i*c-q*s);im[n]=(float)(i*s+q*c);
            double next_c=c*step_c-s*step_s;s=s*step_c+c*step_s;c=next_c;
        }
        const double qs[2]={.5411961,1.3065630};
        for(int direction=0;direction<2;direction++) {
            rf_biquad_t hi[2],hq[2];
            for(int k=0;k<2;k++)hi[k]=hq[k]=rf_filter(RF_VIDEO_RATE,200000.0,qs[k],true);
            for(int k=0;k<span;k++) {
                int n=direction?span-1-k:k;
                for(int j=0;j<2;j++){re[n]=rf_filter_sample(&hi[j],re[n]);im[n]=rf_filter_sample(&hq[j],im[n]);}
            }
        }
        for(int n=PAD;n<PAD+take;n++) {
            double cross=(double)re[n-1]*im[n]-(double)im[n-1]*re[n];
            double dot=(double)re[n-1]*re[n]+(double)im[n-1]*im[n];
            double x=cross*step_c-dot*step_s,z=dot*step_c+cross*step_s;
            double phase;
            phase=rf_mesecam_phase(x,z);
            unsigned at=(unsigned)((start+n-PAD)&RF_RING_MASK);
            d->secam_fm[at]=(float)(d->p.carrier_hz+phase*(RF_VIDEO_RATE/(2.0*RF_PI)));
            d->cq[at]=rf_mesecam_magnitude(re[n],im[n]);
        }
    }
#endif
}

static float rf_level_quantile(float *values,int count,int target);

/* Identification survives short missing references; carrier presence does
 * not. Measure the current line's amplitude and unclipped FM before the LF
 * inverse can spread a phase error into the picture. Keep a pixel mask so a
 * local dropout never turns a healthy remainder into one-component colour. */
static int rf_build_mesecam_rows(gui_rf_decoder_t *d,const double *lines,
                                 const double *periods,const double *wow,
                                 int rows,int line_first,bool *valid) {
    /* A PAL line is320 working samples. The64-sample (12.8us) tail covers
     * the250kHz post-LP delay, three-sample display registration and linear
     * interpolation. Confidence covers THIS line only. A17-sample (3.4us)
     * envelope window rejects a brief high noise peak; a bounded24us bridge
     * prevents LF memory from extending a local hole across the picture. */
    enum { N=384, AM_PAD=8, CLICK_PAD=8, MAX_REPAIR=120 };
    const double nominal=d->p.line_us*5.0;
    const double low=304321.875,high=1160571.875;
    const double video_delay=rf_filter_delay(&d->secam_video_lp[0])
                            +rf_filter_delay(&d->secam_video_lp[1]);
    const double win_start=d->full_frame?0.0:d->p.active_start_us;
    const double win_end=d->full_frame?d->p.line_us:d->p.active_end_us;
    int valid_rows=0;
    for(int y=0;y<rows;y++) {
        const int line=line_first+y;
        bool dr=(((line&1)==0)==d->secam_dr_even);
        double rest=dr?654321.875:810571.875;
        double deviation=dr?-280000.0:-230000.0;
        double gain=dr?-1.0/(1.902*1.139883):1.0/(1.505*2.032062);
        double scale=periods[y]/nominal;
        float f[N],decoded[N],relative[N],levels[N];int level_count=0;
        uint8_t amplitude[N],click[N],outside[N],good[N],display[N];
        for(int n=0;n<N;n++) {
            double p=lines[y]+n*scale+d->secam_fm_delay-d->sync_delay;
            f[n]=(float)(rf_read(d,d->secam_fm,p)*wow[y]);
            double env=rf_read(d,d->cq,p);
            double yenv=rf_read(d,d->envelope,lines[y]+n*scale+d->sync_luma_delay-d->sync_delay);
            relative[n]=(float)(env/fmax(yenv,1e-12));
            if(n>=ceil(d->p.active_start_us*5.0) && n<d->p.active_end_us*5.0)levels[level_count++]=relative[n];
            outside[n]=(uint8_t)(f[n]<low-100000.0 || f[n]>high+100000.0);
            click[n]=(uint8_t)(env>1e-7 && (f[n]<0 || f[n]>1500000.0));
        }
        /* A low amplitude quantile follows a genuine picture-only AM fade
         * and SECAM's legal Bell envelope. Cap it by the quiet reference so
         * highly modulated colour cannot raise its own admission threshold.
         * The reference floor and raw-FM rail density reject band noise;
         * no active hue, saturation, or single-tone coherence is assumed. */
        double local=rf_level_quantile(levels,level_count,level_count/10);
        double floor_ratio=fmax(.012,fmax(.25*d->mesecam_reference_ratio,
            .75*fmin(d->mesecam_reference_ratio,local)));
        int amplitude_sum[N+1],click_sum[N+1],outside_sum[N+1];
        amplitude_sum[0]=click_sum[0]=outside_sum[0]=0;
        for(int n=0;n<N;n++) {
            amplitude[n]=(uint8_t)(n<320 && relative[n]>floor_ratio);
            amplitude_sum[n+1]=amplitude_sum[n]+amplitude[n];
            click_sum[n+1]=click_sum[n]+click[n];
            outside_sum[n+1]=outside_sum[n]+outside[n];
        }
        for(int n=0;n<N;n++) {
            int a=n-AM_PAD,b=n+AM_PAD;
            if(a<0)a=0;
            if(b>=N)b=N-1;
            int count=b-a+1,am=amplitude_sum[b+1]-amplitude_sum[a];
            display[n]=(uint8_t)(am*5>=count*4 && (outside_sum[b+1]-outside_sum[a])*5<count);
            int ca=n-CLICK_PAD,cb=n+CLICK_PAD;
            if(ca<0)ca=0;
            if(cb>=N)cb=N-1;
            good[n]=(uint8_t)(display[n] && click_sum[cb+1]==click_sum[ca]);
        }
        /* Interpolate only bounded, witnessed phase/AM holes before LF
         * recovery. Missing-carrier pixels stay muted even when frequency
         * is bridged, and a long unusable run cannot invent colour. */
        for(int n=0;n<N;) {
            if(good[n]) {n++;continue;}
            int first=n;while(n<N && !good[n])n++;
            int last=n-1,left=first-1,right=n;
            bool repair=last-first+1<=MAX_REPAIR && left>=0 && right<N;
            double a=left>=0?f[left]:rest,b=right<N?f[right]:a;
            for(int k=first;k<=last;k++) {
                f[k]=(float)(repair?a+(b-a)*(k-left)/(right-left):rest);
                if(!repair)display[k]=0;
            }
        }
        float dx=0,dy=0;rf_biquad_t lp[2]={d->secam_video_lp[0],d->secam_video_lp[1]};
        lp[0].z1=lp[0].z2=lp[1].z1=lp[1].z2=0;
        for(int n=0;n<N;n++) {
            float x=(float)(rf_clamp(f[n],low,high)-rest);
            float z=d->secam_b0*x+d->secam_b1*dx-d->secam_a1*dy;dx=x;dy=z;
            for(int k=0;k<2;k++)z=rf_filter_sample(&lp[k],z);
            decoded[n]=z;
        }
        int visible=0,carrier=0;
        for(int x=0;x<d->p.width;x++) {
            double physical=(win_start+(x+.5)*(win_end-win_start)/d->p.width)*5.0;
            int raw_index=(int)floor(physical+.5);
            double at=physical+3.0+video_delay;int a=(int)floor(at);double frac=at-a;
            if(a<0)a=0;
            if(a>=N-1)a=N-2;
            bool q=raw_index>=0 && raw_index<N && display[raw_index];
            d->secam_pixel_valid[y][x]=(uint8_t)q;
            d->secam_rows[y][x]=q?(float)((decoded[a]+(decoded[a+1]-decoded[a])*frac)/deviation*gain):0.0f;
            if(physical>=d->p.active_start_us*5.0 && physical<d->p.active_end_us*5.0) {
                visible++;carrier+=q;
            }
        }
        valid[y]=carrier>visible/2;if(valid[y])valid_rows++;
    }
    return valid_rows;
}

static bool rf_render_secam_field(gui_rf_decoder_t *d, const double *lines,
                                  const double *periods, const double *luma_wow,
                                  int rows, int line_first, int parity,
                                  double field_start) {
    const double nominal=d->p.line_us*5.0;
    const bool native=rf_native_secam(d);
    if(!native)rf_prepare_mesecam_field(d,field_start);
    bool valid[RF_MAX_LINES];
    int votes[2]={0,0},components[2]={0,0},observations=0,valid_rows=0;
    int previous_line=-2,previous_ident=-1,ident_run=0,longest_ident_run=0;
    double reference_ratios[RF_MAX_LINES],bottle_ratios[RF_MAX_LINES];
    int reference_count=0,bottle_count=0;
    /* Bottles occupy early VBI lines. Use measured H anchors rather than
     * guessing a row phase from a raw block boundary. Require three late
     * windows at the legal Dr/Db endpoint plus the corresponding ramp;
     * picture colour and a single chance FM click cannot vote here. */
    for(int line=7;line<d->p.active_first;++line) {
        bool measured,next_measured;
        double start=rf_nearest_h(d,field_start+line*d->period,nominal*.30,&measured);
        double next=rf_nearest_h(d,start+d->period,nominal*.30,&next_measured);
        if(!measured || !next_measured) continue;
        double scale=(next-start)/nominal;
        if(scale<.98 || scale>1.02) continue;
        double windows[4]={0},power=0,luma_power=0,plateau_power=0,plateau_luma=0;
        const double begin_us[4]={12,28,36,44};
        for(int w=0;w<4;++w) for(int n=0;n<16;++n) {
            double offset=(begin_us[w]+(n+.5)*.5)*5.0*scale;
            double at=start+offset+(native?d->chroma_delay:d->secam_fm_delay)-d->sync_delay;
            windows[w]+=rf_read(d,native?d->ci:d->secam_fm,at)*scale/16.0;
            power+=rf_read(d,d->cq,at)/64.0;
            double ypower=rf_read(d,d->envelope,start+offset+d->sync_luma_delay-d->sync_delay);
            luma_power+=ypower/64.0;
            if(!native && w) {plateau_power+=rf_read(d,d->cq,at)/48.0;plateau_luma+=ypower/48.0;}
        }
        if(power<=1e-7 || power<=luma_power*.012) continue;
        double plateau=(windows[1]+windows[2]+windows[3])/3.0;
        bool dr=native ? plateau>1082031.25 : plateau<732446.875;
        double rest=native ? (dr ? 1189062.5 : 975000.0)
            : (dr ? 304321.875 : 1160571.875);
        bool plateau_valid=true;
        for(int w=1;w<4;++w) if(fabs(windows[w]-rest)>(native?25000.0:45000.0)) plateau_valid=false;
        double ramp=(plateau-windows[0])*(dr?1.0:-1.0)*(native?1.0:-1.0);
        if(!plateau_valid || ramp<(native?20000.0:80000.0)) continue;
        /* A bottles-only recording may have no quiet H carrier. Its known
         * steady plateaus still measure current chroma strength. Divide by
         * four, conservatively allowing the legal HF Bell boost (below3.5)
         * without treating an amplified identification bottle as picture C. */
        if(!native && bottle_count<RF_MAX_LINES)
            bottle_ratios[bottle_count++]=plateau_power/fmax(4.0*plateau_luma,1e-12);
        int ident=dr==((line&1)==0);
        votes[ident]++;components[dr]++;observations++;
        ident_run=line==previous_line+1 && ident==previous_ident ? ident_run+1 : 1;
        if(ident_run>longest_ident_run) longest_ident_run=ident_run;
        previous_line=line;previous_ident=ident;
    }
    bool bottles=observations>=4 && longest_ident_run>=4 && components[0]>=2 && components[1]>=2
        && (votes[0]>votes[1]?votes[0]:votes[1])>=observations*.85;
    if(!native) {
        /* MESECAM's mixer preserves quiet undeviated H references. Read
         * pre-LF discriminator samples: LF/post-LP memory and picture
         * content may not teach line identity. Both known rest carriers,
         * a long alternating sequence and four agreeing fields are needed.
         * Native VHS keeps its separately validated bottles-only ID. */
        if(!bottles) {
            votes[0]=votes[1]=components[0]=components[1]=observations=0;
            previous_line=-2;previous_ident=-1;ident_run=longest_ident_run=0;
        }
        for(int line=7;line<300;++line) {
            bool measured,next_measured;
            double start=rf_nearest_h(d,field_start+line*d->period,nominal*.30,&measured);
            double next=rf_nearest_h(d,start+d->period,nominal*.30,&next_measured);
            if(!measured || !next_measured) continue;
            double scale=(next-start)/nominal;
            if(scale<.98 || scale>1.02) continue;
            double frequency=0,begin=0,end=0,power=0,luma_power=0;
            for(int n=0;n<20;++n) {
                double offset=(7.3+(n+.5)*.1)*5.0*scale;
                double at=start+offset+d->secam_fm_delay-d->sync_delay;
                double f=rf_read(d,d->secam_fm,at)*d->luma_wow;
                frequency+=f/20.0;
                if(n<10) begin+=f/10.0;else end+=f/10.0;
                power+=rf_read(d,d->cq,at)/20.0;
                luma_power+=rf_read(d,d->envelope,start+offset+d->sync_luma_delay-d->sync_delay)/20.0;
            }
            bool dr=frequency<732446.875;
            double rest=dr?654321.875:810571.875;
            if(power<=1e-7 || power<=luma_power*.012 || fabs(frequency-rest)>45000.0 ||
                fabs(begin-end)>60000.0) continue;
            if(reference_count<RF_MAX_LINES)reference_ratios[reference_count++]=(float)(power/fmax(luma_power,1e-12));
            if(bottles)continue;
            int ident=dr==((line&1)==0);
            votes[ident]++;components[dr]++;observations++;
            ident_run=line==previous_line+1 && ident==previous_ident ? ident_run+1 : 1;
            if(ident_run>longest_ident_run)longest_ident_run=ident_run;
            previous_line=line;previous_ident=ident;
        }
    }
    for(int y=0;native && y<rows;++y) {
        float frequencies[48],powers[48];
        double scale=periods[y]/nominal,luma_power=0.0;
        for(int n=0;n<48;++n) {
            double offset=(12.0+n*(48.0/47.0))*5.0*scale;
            double at=lines[y]+offset+d->chroma_delay-d->sync_delay;
            frequencies[n]=(float)(rf_read(d,d->ci,at)*scale);
            powers[n]=rf_read(d,d->cq,at);
            luma_power+=rf_read(d,d->envelope,lines[y]+offset+d->sync_luma_delay-d->sync_delay);
        }
        double frequency;
        /* This small fixed-size sort is field work, never per RF sample. */
        for(int n=1;n<48;++n) {
            float f=frequencies[n],p=powers[n];int a=n,b=n;
            while(a>0 && frequencies[a-1]>f) {frequencies[a]=frequencies[a-1];--a;}
            frequencies[a]=f;
            while(b>0 && powers[b-1]>p) {powers[b]=powers[b-1];--b;}
            powers[b]=p;
        }
        frequency=.5*(frequencies[23]+frequencies[24]);
        double power=.5*(powers[23]+powers[24]);
        valid[y]=power>1e-7 && power>luma_power/48.0*.012
            && frequency>(native?975000.0:304000.0) && frequency<(native?1190000.0:1161000.0);
        if(valid[y]) valid_rows++;
    }
    bool adjacent=d->secam_have_ident &&
        fabs(field_start-d->secam_last_field-312.5*nominal*d->luma_wow)<2.0*nominal;
    if(adjacent) d->secam_dr_even^=(d->secam_last_parity!=0);
    else {d->secam_have_ident=false;d->secam_ident_streak=0;}
    int selected=votes[1]>votes[0] ? 1 : 0;
    bool decisive=observations>=(native||bottles?4:64) && longest_ident_run>=(native||bottles?4:8)
        && components[0]>=(native||bottles?2:24) && components[1]>=(native||bottles?2:24)
        && votes[selected]>=observations*.85;
    if(decisive) {
        if(!native) {
            double reference=reference_count>=4?rf_median(reference_ratios,reference_count):0.0;
            if(bottles && bottle_count>=4)reference=fmax(reference,rf_median(bottle_ratios,bottle_count));
            if(reference>0.0)d->mesecam_reference_ratio=reference;
        }
        if(!d->secam_have_ident || d->secam_dr_even!=(selected!=0))
            d->secam_ident_streak=1;
        else if(d->secam_ident_streak<4) d->secam_ident_streak++;
        d->secam_dr_even=selected!=0;d->secam_have_ident=true;
        d->secam_ident_age=0;
    } else if(d->secam_have_ident && ++d->secam_ident_age>=32) {
        d->secam_have_ident=false;d->secam_ident_streak=0;
    }
    d->secam_last_field=field_start;d->secam_last_parity=parity;
    if(!native)valid_rows=rf_build_mesecam_rows(d,lines,periods,luma_wow,rows,line_first,valid);
    bool identified=d->secam_have_ident && d->secam_ident_streak>=4;
    /* Field lock is a coverage statistic. A locally healthy colour region
     * still uses its pixel confidence when much of the field has dropped. */
    bool color=identified && valid_rows>rows*3/4;
    /* Historical line order alone cannot authorize sparse noise after a
     * carrier failure. Fresh references permit a healthy local remainder;
     * absent references need broadly usable current carrier for flywheel. */
    bool usable_ident=identified && (decisive || valid_rows>rows*3/4);
    if(!native && !usable_ident)memset(d->secam_rows,0,sizeof(d->secam_rows));
    if(!color) rf_color_loss(d,valid_rows<=rows*3/4
        ? "weak SECAM FM chroma" : "acquiring SECAM line identification");
    const double win_start=d->full_frame ? 0.0 : d->p.active_start_us;
    const double win_end=d->full_frame ? d->p.line_us : d->p.active_end_us;
    const int active_x0=(int)ceil(d->p.active_start_us*d->p.width/d->p.line_us-.5);
    const int active_x1=(int)ceil(d->p.active_end_us*d->p.width/d->p.line_us-.5);
    for(int y=0;native && y<rows;++y) {
        int line=line_first+y;
        bool dr=(((line&1)==0)==d->secam_dr_even);
        double rest=native ? (dr ? 1101562.5 : 1062500.0) : (dr ? 654321.875 : 810571.875);
        double deviation=native ? (dr ? 70000.0 : 57500.0) : (dr ? -280000.0 : -230000.0);
        double gain=dr ? -1.0/(1.902*1.139883) : 1.0/(1.505*2.032062);
        double scale=periods[y]/nominal;
        double step=(win_end-win_start)*5.0*scale/d->p.width;
        for(int x=0;x<d->p.width;++x) {
            double offset=win_start*5.0*scale+(x+.5)*step;
            /* Pixel geometry follows measured H spacing. Frequency scaling
             * uses the robust tape clock, so slicer jitter cannot shift the
             * entire line's colour-difference DC level. */
            double frequency=rf_read(d,d->ci,lines[y]+offset+d->chroma_delay-d->sync_delay)*luma_wow[y];
            d->secam_rows[y][x]=color && valid[y] ? (float)((frequency-rest)/deviation*gain) : 0.0f;
        }
    }
    /* Work on like components two measured lines apart in THIS field.
     * Preserve the original previous rows while filtering in place. The
     * range weight rejects a colour boundary instead of spreading it into
     * its neighbour; VBI and picture never contribute to each other. */
    float previous[2][GUI_RF_FRAME_MAX_WIDTH]={{0}};
    for(int y=0;y<rows;++y) for(int x=0;x<d->p.width;++x) {
        float centre=d->secam_rows[y][x];
        double total=2.0*centre,weight=2.0;
        for(int direction=-2;direction<=2;direction+=4) {
            int neighbour=y+direction;
            if(neighbour<0 || neighbour>=rows ||
                (native ? (!valid[y] || !valid[neighbour])
                    : (!usable_ident || !d->secam_pixel_valid[y][x] || !d->secam_pixel_valid[neighbour][x])) ||
                ((line_first+y>=d->p.active_first)!=(line_first+neighbour>=d->p.active_first))) continue;
            double value=direction<0 ? previous[y&1][x] : d->secam_rows[neighbour][x];
            double w=fmax(0.0,1.0-fabs(value-centre)/.12);
            total+=w*value;weight+=w;
        }
        previous[y&1][x]=centre;
        d->secam_rows[y][x]=(float)(total/weight);
    }
    for(int y=0;y<rows;++y) {
        int line=line_first+y;
        bool dr=(((line&1)==0)==d->secam_dr_even);
        double scale=periods[y]/nominal;
        double step=(win_end-win_start)*5.0*scale/d->p.width;
        size_t row=(size_t)(y*2+parity)*d->p.width*4u;
        for(int x=0;x<d->p.width;++x) {
            double offset=win_start*5.0*scale+(x+.5)*step;
            double hz=rf_read(d,d->luma,lines[y]+offset+d->luma_delay-d->sync_delay);
            double ire=(hz*luma_wow[y]-d->display_blank_hz)/d->display_hz_ire;
            bool leveled=!d->full_frame ||
                (line>=d->p.active_first && x>=active_x0 && x<active_x1);
            double luminance=leveled ? rf_display_level(ire/100.0)
                : rf_clamp((ire-d->p.hybrid_black_ire)/d->p.hybrid_span_ire,0.0,1.0);
            double other=0.0;int count=0;
            for(int direction=-1;direction<=1;direction+=2) {
                int neighbour=y+direction;
                if(neighbour>=0 && neighbour<rows &&
                    (native ? (color && valid[y] && valid[neighbour])
                        : (usable_ident && d->secam_pixel_valid[y][x] && d->secam_pixel_valid[neighbour][x])) &&
                    ((line>=d->p.active_first)==(line_first+neighbour>=d->p.active_first))) {
                    other+=d->secam_rows[neighbour][x];count++;
                }
            }
            if(count) other/=count;
            double own=d->secam_rows[y][x];
            double u=dr ? other : own,v=dr ? own : other;
            d->rgba[row+x*4u]=rf_byte(luminance+1.140*v);
            d->rgba[row+x*4u+1u]=rf_byte(luminance-.395*u-.581*v);
            d->rgba[row+x*4u+2u]=rf_byte(luminance+2.032*u);
            d->rgba[row+x*4u+3u]=255;
        }
    }
    return color;
}

static bool rf_render_field(gui_rf_decoder_t *d, double field_start, int parity,
                            uint64_t end_sample, gui_rf_frame_callback_t callback,
                            void *context) {
    double lines[RF_MAX_LINES+1], periods[RF_MAX_LINES];
    rf_complex_t burst[RF_MAX_LINES], corrected[RF_MAX_LINES];
    double magnitudes[RF_MAX_LINES], gain[RF_MAX_LINES], luma_wow[RF_MAX_LINES];
    bool valid[RF_MAX_LINES], anchors[RF_MAX_LINES+1];
    int measured_count=0, burst_count=0;
    double nominal = d->p.line_us*5.0;
    /* Full mode renders from the field boundary (line 0) so the VBI shows;
     * Active mode crops to the profile's active lines. Unmeasured VBI rows
     * (EQ/broad pulses carry no H anchors) fall back to nominal positions. */
    const int line_first = d->full_frame ? 0 : d->p.active_first;
    /* Full mode widens the horizontal window to the whole line so the sync
     * tip and the color burst render (with their decoded chroma). */
    const double win_start_us = d->full_frame ? 0.0 : d->p.active_start_us;
    const double win_end_us = d->full_frame ? d->p.line_us : d->p.active_end_us;
    /* Hybrid level split (tbc-tools OutputWriter hybrid mode): only the
     * active horizontal window of active lines keeps the leveled mapping.
     * The split is positional, like tbc's activeVideoStart/End check: the
     * rendered window and the active area stay the same fraction of the
     * line at any per-line tape-speed scale. */
    const int active_x0 = (int)ceil(d->p.active_start_us*d->p.width/d->p.line_us-.5);
    const int active_x1 = (int)ceil(d->p.active_end_us*d->p.width/d->p.line_us-.5);
    int field_lines=d->format==GUI_RF_PAL ? (parity==0 ? 312 : 313)
                                         : (parity==0 ? 263 : 262);
    /* Full mode renders every field line, so the woven frame is twice the
     * taller parity's line count (PAL 2x313=626, NTSC 2x263=526); Active
     * mode keeps the profile's cropped active rows and 576/480 height. */
    const int rows = d->full_frame ? field_lines : d->p.height/2;
    const int frame_height = d->full_frame
        ? (d->format==GUI_RF_PAL ? 313 : 263)*2 : d->p.height;
    double average=((double)end_sample*.25-field_start)/field_lines;
    average=rf_clamp(average,nominal*.98,nominal*1.02);
    for (int y=0; y<=rows; ++y) {
        bool measured;
        int line=line_first+y;
        double target=y ? lines[y-1]+average : field_start+line*average;
        lines[y]=rf_nearest_h(d,target,nominal*.30,&measured);
        anchors[y]=measured;
        if (measured && y<rows) measured_count++;
    }
    if (measured_count < rows*3/4) {
        d->ready_fields=0;
        rf_reject_field(d,RF_REJECT_LINE_MAP,"insufficient measured line sync");
        return false;
    }
    /* Fill missing H pulses between real neighboring anchors. Never replace
     * every line's measured timing by a nominal constant line length. */
    for (int y=0; y<=rows; ++y) if (!anchors[y]) {
        int left=y-1, right=y+1;
        while (left>=0 && !anchors[left]) left--;
        while (right<=rows && !anchors[right]) right++;
        if (left>=0 && right<=rows)
            lines[y]=lines[left]+(lines[right]-lines[left])*(y-left)/(right-left);
    }
    for (int y=0; y<rows; ++y)
        periods[y]=rf_clamp(lines[y+1]-lines[y], nominal*.8, nominal*1.2);
    rf_prepare_luma_wow(d,periods,anchors,rows,nominal,luma_wow);
    rf_prepare_display_levels(d);
    bool color;
    if(rf_secam_fm(d)) {
        color=rf_render_secam_field(d,lines,periods,luma_wow,rows,line_first,parity,field_start);
    } else {
    for (int y=0; y<rows; ++y) {
        int line=line_first+y;
        double start=d->p.burst_start_us*5.0*periods[y]/nominal;
        double end=d->p.burst_end_us*5.0*periods[y]/nominal;
        /* Trim filter settling and transitions at both ends of the burst. */
        start+=1.0; end-=1.0;
        rf_complex_t total={0,0}; double envelope=0.0; int n=0;
        for (double at=start; at<end; at+=1.0) {
            rf_complex_t z=rf_color_at(d, lines[y], periods[y], field_start, line, at);
            envelope+=rf_read(d,d->envelope,lines[y]+at+d->sync_luma_delay-d->sync_delay);
            total.re+=z.re; total.im+=z.im; n++;
        }
        if (n) { total.re/=n; total.im/=n; }
        burst[y]=total;
        magnitudes[y]=hypot(total.re,total.im);
        envelope=n ? envelope/n : 0.0;
        /* Capture sources have different ADC scaling (12-bit MISRC versus
         * left-aligned DdD). Judge burst strength relative to RF amplitude,
         * so identical signals do not change hue/saturation with input gain. */
        valid[y]=envelope>1e-7 && magnitudes[y]>envelope*.005;
        if (valid[y]) burst_count++;
    }
    /* VHS PAL heads use 0/-90 degrees per line; NTSC heads use -90/+90.
     * Identify the actual track from burst cadence, not from an assumed first
     * playback head. The final measured line burst absorbs remaining phase. */
    double dot_sum=0.0, cross_sum=0.0, abs_dot=0.0, abs_cross=0.0;
    for (int y=1; y<rows; ++y) if (valid[y] && valid[y-1]) {
        double scale=magnitudes[y]*magnitudes[y-1];
        double dot=(burst[y-1].re*burst[y].re+burst[y-1].im*burst[y].im)/scale;
        double cross=(burst[y-1].re*burst[y].im-burst[y-1].im*burst[y].re)/scale;
        dot_sum+=dot; cross_sum+=cross; abs_dot+=fabs(dot); abs_cross+=fabs(cross);
    }
    /* Beta uses 180/0 degrees per line rather than VHS NTSC +/-90.
     * Infer the recorded track from measured burst cadence, never parity. */
    double track_step = d->p.beta_rotation ? (dot_sum<0.0 ? RF_PI : 0.0)
        : d->format==GUI_RF_PAL ? (abs_dot>abs_cross ? -RF_PI*.5 : 0.0)
        : (cross_sum>=0.0 ? RF_PI*.5 : -RF_PI*.5);
    rf_complex_t common={0,0};
    for (int y=0; y<rows; ++y) {
        corrected[y]=rf_rotate(burst[y], -track_step*y);
        if (valid[y]) { common.re+=corrected[y].re/magnitudes[y];
                       common.im+=corrected[y].im/magnitudes[y]; }
    }
    double common_phase=atan2(common.im,common.re)-RF_PI;
    double coherence=burst_count ? hypot(common.re,common.im)/burst_count : 0.0;
    color = burst_count>rows*3/4 && coherence>(d->format==GUI_RF_PAL ? .40 : .65);
    if (!color) rf_color_loss(d,burst_count<=rows*3/4
        ? "insufficient valid color bursts" : "incoherent tape track/burst phase");
    /* PAL swings its burst about the -U axis. Use neighboring measured burst
     * vectors after the tape rotation is removed, rather than field number. */
    for (int y=0; y<rows; ++y) {
        int line=line_first+y;
        rf_complex_t aligned=rf_rotate(corrected[y], -common_phase);
        int vswitch = d->format==GUI_RF_PAL ? (aligned.im>=0.0 ? 1 : -1) : 1;
        double reference_phase = d->format==GUI_RF_PAL
            ? (vswitch>0 ? 3.0*RF_PI/4.0 : -3.0*RF_PI/4.0) : RF_PI;
        double phase = reference_phase-atan2(burst[y].im,burst[y].re);
        /* Burst normalization supplies the chroma AGC. Reject weak bursts;
         * suppress color on invalid lines instead of amplifying noise. */
        gain[y]=valid[y] ? (150.0/700.0)/magnitudes[y] : 0.0;
        if (y>0 && valid[y] && valid[y-1]) gain[y]=gain[y]*.75+gain[y-1]*.25;
        double scale=periods[y]/nominal;
        /* Display levels stay fixed while locked. SuperBeta may calibrate
         * once from a validated multi-line H grid, never individual rows. */
        double pixel_step=(win_end_us-win_start_us)*5.0*scale/d->p.width;
        double first_offset=win_start_us*5.0*scale+pixel_step*.5;
        double carrier_scale=2.0*RF_PI*d->p.carrier_hz/RF_VIDEO_RATE;
        /* Chroma timing follows the actual demodulated line number: Full
         * mode starts at line 0, so VBI rows keep their true burst phase
         * instead of an active-line-shifted one (wrong hue on VBI rows). */
        double timing0=lines[y]-field_start-(line_first+y)*nominal
                      +first_offset*(1.0-nominal/periods[y]);
        double angle=phase+carrier_scale*timing0;
        double rotate_re=cos(angle), rotate_im=sin(angle);
        double step_angle=carrier_scale*pixel_step*(1.0-nominal/periods[y]);
        double step_re=cos(step_angle), step_im=sin(step_angle);
        const bool active_line=line>=d->p.active_first;
        for (int x=0; x<d->p.width; ++x) {
            double offset=first_offset+x*pixel_step;
            double hz=rf_read(d,d->luma,lines[y]+offset+d->luma_delay-d->sync_delay);
            bool leveled=!d->full_frame
                       || (active_line && x>=active_x0 && x<active_x1);
            double luminance;
            if (d->p.beta_rotation && leveled) {
                /* Retain the validated Beta active-picture transfer exactly.
                 * VHS keeps the upstream monitor range below; Full-mode VBI
                 * and porches use the shared hybrid mapping for every tape. */
                luminance=rf_clamp((hz*luma_wow[y]-d->display_blank_hz)
                                   /(100.0*d->display_hz_ire),0.0,1.0);
            } else {
                double ire=(hz*luma_wow[y]-d->display_blank_hz)/d->display_hz_ire;
                luminance=leveled ? rf_display_level(ire/100.0)
                    : rf_clamp((ire-d->p.hybrid_black_ire)/d->p.hybrid_span_ire,0.0,1.0);
            }
            double u=0.0,v=0.0;
            if (color && valid[y]) {
                double pos=lines[y]+offset+d->chroma_delay-d->sync_delay;
                double re=rf_read(d,d->ci,pos), im=rf_read(d,d->cq,pos);
                u=(re*rotate_re-im*rotate_im)*gain[y];
                /* With e^-i(w*t), NTSC color-under U*cos+V*sin yields
                 * U-iV. The .NET path instead multiplies by -cos((fsc+fcc)t),
                 * conjugating it before its negative-U/negative-V renderer.
                 * We skip that upconversion, so recover V from -Im here.
                 * PAL uses its separately resolved swinging-burst basis. */
                double imaginary=re*rotate_im+im*rotate_re;
                v=imaginary*gain[y]*vswitch*(d->format==GUI_RF_NTSC ? -1.0 : 1.0);
            }
            double next_re=rotate_re*step_re-rotate_im*step_im;
            rotate_im=rotate_im*step_re+rotate_re*step_im; rotate_re=next_re;
            d->field_rows[y&1][0][x]=(float)luminance;
            d->field_rows[y&1][1][x]=(float)u;
            d->field_rows[y&1][2][x]=(float)v;
        }
        if (y&1) rf_render_rows(d,y-1,2,parity,color&&valid[y-1],color&&valid[y]);
        else if (y+1==rows) rf_render_rows(d,y,1,parity,color&&valid[y],false);
    }
    }
    /* PAL parity-0 (312 lines) can never write row 624 and NTSC parity-1
     * never writes row 525; each field clears its own beyond-last row so no
     * stale content survives from earlier geometry or mode switches. */
    if (rows*2+parity<frame_height)
        memset(&d->rgba[(size_t)(rows*2+parity)*d->p.width*4u],0,
               (size_t)d->p.width*4u);
    d->status.fields_decoded++;
    d->status.fields_accepted++;
    d->status.h_locked=true;
    d->status.v_locked=true;
    d->last_level_field=end_sample/4u;
    d->status.line_period_us=average/5.0;
    d->ready_color[parity]=color;
    d->ready_fields|=1u<<parity;
    if (d->ready_fields==3u) {
        d->status.frames_decoded++;
        d->status.color_locked=d->ready_color[0] && d->ready_color[1];
        snprintf(d->status.reason,sizeof(d->status.reason),"%s %s",
                 gui_rf_profile_name(d->profile),
                 d->status.color_locked ? "color locked" :
                    rf_secam_fm(d) ? "luma locked; acquiring FM color" : "luma locked; weak burst");
        if (callback) callback(context,d->rgba,d->p.width,frame_height,end_sample,&d->status);
        d->ready_fields=0;
    }
    return true;
}

static void rf_vertical_start(gui_rf_decoder_t *d, double first_eq,
                              gui_rf_frame_callback_t callback, void *context) {
    double nominal=d->p.line_us*5.0;
    bool recovered_first_eq=false;
    if(d->format==GUI_RF_PAL && d->p.eq_count==5 && d->eq_run==d->p.eq_count-1 &&
        d->last_h_width>=3.5 && d->last_h_width<=3.8 &&
        d->previous_h>0.0 && d->previous_h_width>=3.8 &&
        fabs(first_eq-d->last_h-.5*d->period)<.08*d->period &&
        fabs(d->first_v-d->last_h-.5*d->p.eq_count*d->period)<.10*d->period) {
        /* A band-limited first equalizer can straddle the 3.5us H/EQ
         * threshold. Four following measured half-line equalizers and
         * three measured broad pulses place it in the missing fifth slot.
         * Reclassify only that borderline pulse, then retain the earlier
         * clear H for parity: using the equalizer itself would falsely
         * force integer phase if the half-line field is also broadened.
         * This is PAL-family luminance timing, independent of the selected
         * chroma decoder: a wrong colour system must still show stable Y.
         * No global pulse-width threshold or field cadence is relaxed. */
        first_eq=d->last_h;recovered_first_eq=true;
        d->last_h=d->previous_h;d->last_h_width=d->previous_h_width;
        if(d->h_count) {
            d->h_head=(d->h_head+RF_PULSE_SIZE-1u)%RF_PULSE_SIZE;
            d->h_count--;
            /* Classification events may already have reached the worker.
             * Keep published counters monotonic across this correction. */
        }
    }
    if(d->profile==GUI_RF_PROFILE_MESECAM_VHS &&
        d->eq_run!=d->p.eq_count && !recovered_first_eq) {
        /* A genuinely absent equalizer is not the witnessed borderline
         * pulse above. Do not invent its phase from the expected cadence. */
        rf_reject_field(d,RF_REJECT_PARITY,"MESECAM vertical EQ train incomplete");
        return;
    }
    if (d->last_h<=0.0 || first_eq-d->last_h>nominal*2.1) {
        rf_reject_field(d,RF_REJECT_PARITY,"vertical EQ lacks recent H reference");
        return;
    }
    double relative=(first_eq-d->last_h)/d->period;
    double half_distance=fabs(relative-(floor(relative)+.5));
    double integer_distance=fabs(relative-floor(relative+.5));
    bool half=half_distance<integer_distance;
    if (fmin(half_distance,integer_distance)>.18) {
        rf_reject_field(d,RF_REJECT_PARITY,"vertical EQ/H parity phase uncertain");
        return;
    }
    int parity=d->format==GUI_RF_PAL ? (half ? 0 : 1) : (half ? 1 : 0);
    double boundary=d->format==GUI_RF_PAL ? (parity==0 ? .5 : 1.0)
                                         : (parity==0 ? 1.0 : .5);
    double start=first_eq-boundary*d->period;
    if (d->have_field) {
        /* Last-line H jitter near a tape head switch must not move a field's
         * cadence reference. SECAM uses the robust preceding line-grid clock;
         * allow its initial +/-1% acquisition before that clock is learned. */
        double cadence_period=nominal;
        double cadence_tolerance=2.0;
        if(rf_secam_fm(d)) {
            cadence_period*=d->luma_wow;
            if(!d->have_luma_wow) cadence_tolerance=4.0;
        }
        double field_lines=(start-d->field_start)/cadence_period;
        double expected=d->format==GUI_RF_PAL ? 312.5 : 262.5;
        if (fabs(field_lines-expected)<cadence_tolerance && parity!=d->field_parity) {
            rf_render_field(d,d->field_start,d->field_parity,
                            (uint64_t)llround(start*4.0),callback,context);
        } else {
            d->ready_fields=0;
            if (fabs(field_lines-expected)>=cadence_tolerance)
                rf_reject_field(d,RF_REJECT_CADENCE,"vertical field cadence outside profile");
            else rf_reject_field(d,RF_REJECT_PARITY,"vertical fields repeat parity");
        }
    }
    d->field_start=start; d->field_parity=parity; d->have_field=true;
    d->status.vsync_count++;
}

static void rf_commit_pulse_mode(gui_rf_decoder_t *d, double start, double end,
                                gui_rf_frame_callback_t callback, void *context,
                                bool allow_recovery) {
    double us=(end-start)/5.0, spacing=start-d->last_pulse;
    double nominal=d->p.line_us*5.0;
    rf_pulse_kind_t kind=RF_PULSE_NONE;
    if (us>=1.0 && us<3.5) kind=RF_PULSE_EQ;
    else if (us>=3.5 && us<=8.0) kind=RF_PULSE_H;
    else if (us>=19.0 && us<=36.0) kind=RF_PULSE_V;
    /* Only the first broad slot after a real EQ train may be reconstructed.
     * Some tapes split it into 19..20 us + an EQ tail, or 13..18 us + an H
     * tail. Both low pieces must fit one half-line and their total span must
     * still meet the ordinary broad width. No global pulse class changes.
     * Defer subsequent broad pulses until their cadence confirms the split;
     * an interruption replays the original tail and all deferred pulses. */
    if (allow_recovery && d->have_short_broad) {
        d->have_short_broad=false;
        double span=(end-d->short_broad_start)/5.0;
        if ((kind==RF_PULSE_EQ || kind==RF_PULSE_H)
            && start-d->short_broad_end<=nominal*.12
            && end-d->short_broad_start<=nominal*.5 && span>=19.0 && span<=36.0) {
            d->have_split_broad=true; d->split_broad_weak=true;
            d->split_broad_start=d->short_broad_start;
            d->split_tail_start=start; d->split_tail_end=end; d->split_follow_count=0;
            return;
        }
    }
    if (kind==RF_PULSE_NONE) {
        if (allow_recovery && !d->have_split_broad && d->last_kind==RF_PULSE_EQ
            && d->eq_run>=3 && d->last_h>0.0 && d->first_eq-d->last_h<=nominal*2.1
            && fabs(spacing-nominal*.5)<nominal*.12 && us>=13.0 && us<19.0) {
            d->have_short_broad=true;d->short_broad_start=start;d->short_broad_end=end;
        }
        return;
    }
    if (allow_recovery && d->have_split_broad) {
        int count=d->split_follow_count;
        double previous=count ? d->split_follow_start[count-1] : d->split_broad_start;
        if (kind==RF_PULSE_V && fabs(start-previous-nominal*.5)<nominal*.12) {
            d->split_follow_start[count]=start;d->split_follow_end[count]=end;
            d->split_follow_count=++count;
            int needed=d->split_broad_weak ? 3 : 2;
            if (count<needed) return;
            d->have_split_broad=false;
            if (d->split_broad_weak) {
                /* The uncertain first piece never counts as a broad pulse:
                 * three following complete broad pulses prove the EQ origin. */
                d->last_kind=RF_PULSE_V;d->last_pulse=d->split_broad_start;
                d->v_run=0;d->candidate_v_run=0;
            }
            for (int i=0; i<count; ++i)
                rf_commit_pulse_mode(d,d->split_follow_start[i],d->split_follow_end[i],callback,context,false);
            return;
        }
        d->have_split_broad=false;
        rf_commit_pulse_mode(d,d->split_tail_start,d->split_tail_end,callback,context,false);
        for (int i=0; i<count; ++i)
            rf_commit_pulse_mode(d,d->split_follow_start[i],d->split_follow_end[i],callback,context,false);
        spacing=start-d->last_pulse;
    }
    double combined=(end-d->last_pulse)/5.0;
    if (allow_recovery && (kind==RF_PULSE_EQ || kind==RF_PULSE_H)
        && d->last_kind==RF_PULSE_V && d->v_run==1 && d->eq_run>=3
        && d->last_pulse==d->first_v
        && start-d->last_pulse_end<=nominal*.12 && end-d->last_pulse<=nominal*.5
        && combined>=19.0 && combined<=36.0) {
        d->have_split_broad=true;d->split_broad_weak=false;d->split_broad_start=d->last_pulse;
        d->split_tail_start=start;d->split_tail_end=end;d->split_follow_count=0;
        return;
    }
    /* Threshold ripple during a real EQ train can insert a narrow pulse a
     * few microseconds after a valid EQ. Keep the measured half-line cadence;
     * otherwise one noisy extra edge makes every second field disappear. */
    if (kind==RF_PULSE_EQ && d->last_kind==RF_PULSE_EQ && d->eq_run>=3
        && spacing<nominal*.35) return;
    if (kind==RF_PULSE_V) {
        d->candidate_v_run=d->last_kind==RF_PULSE_V
            && fabs(spacing-nominal*.5)<nominal*.12 ? d->candidate_v_run+1 : 1;
        if (d->candidate_v_run==3) d->status.vsync_candidates++;
    } else d->candidate_v_run=0;
    if (kind==RF_PULSE_H) {
        if (d->last_h>0.0) {
            double step=start-d->last_h;
            if (fabs(step-nominal)<nominal*.12) {
                d->period=d->period*.95+step*.05;
                if (d->good_h<20) d->good_h++;
            }
            /* A preceding complete H line supplies blanking independently of
             * picture extrema; sync tips are measured inside the pulse. */
            double blank=0.0; int n=0;
            for (double at=8.3*5; at<9.0*5; at+=1) {
                blank+=rf_read(d,d->sync_luma,d->last_h+at+d->sync_luma_delay-d->sync_delay); n++;
            }
            blank/=n;
            if (fabs(blank-d->blank_anchor)<400000.0)
                d->blank_est=d->blank_est*.99+blank*.01;
        }
        double tip=0.0; int n=0;
        for (double at=1.7*5; at<3.5*5; at+=1) {
            tip+=rf_read(d,d->sync_luma,start+at+d->sync_luma_delay-d->sync_delay); n++;
        }
        tip/=n;
        if (fabs(tip-d->sync_anchor)<400000.0) d->sync_est=d->sync_est*.99+tip*.01;
        d->previous_h=d->last_h;d->previous_h_width=d->last_h_width;
        d->last_h=start;d->last_h_width=us;
        /* Native SECAM's higher colour-under band can disturb the falling
         * sync threshold while the H trailing edge remains clean. Use the
         * measured end of THIS pulse and the physical 4.7us H duration for
         * rendering; retain its original start for pulse/cadence tracking.
         * No neighbouring line position is smoothed into the tape timing. */
        d->h_positions[d->h_head]=rf_secam_fm(d)
            ? end-4.7*5.0*d->luma_wow : start;
        d->h_head=(d->h_head+1u)%RF_PULSE_SIZE;
        if (d->h_count<RF_PULSE_SIZE) d->h_count++;
        d->status.hsync_count++;
        d->status.h_locked=d->good_h>=20;
        d->eq_run=0; d->v_run=0;
    } else if (kind==RF_PULSE_EQ) {
        if (d->last_kind==RF_PULSE_EQ && fabs(spacing-nominal*.5)<nominal*.12)
            d->eq_run++;
        else { d->eq_run=1; d->first_eq=start; }
        d->v_run=0;
    } else if (kind==RF_PULSE_V) {
        /* Broad pulses may confirm an intact preceding EQ train after a
         * fragmented first broad pulse. H or an isolated EQ invalidates that
         * origin: later broad pulses must not revive its stale first_eq. */
        if (d->eq_run>=3 && d->last_kind==RF_PULSE_V
            && fabs(spacing-nominal*.5)<nominal*.12)
            d->v_run++;
        else if (d->last_kind==RF_PULSE_EQ && d->eq_run>=3
                 && fabs(spacing-nominal*.5)<nominal*.12) {
            d->v_run=1; d->first_v=start;
        } else d->v_run=0;
        if (d->v_run==3) rf_vertical_start(d,d->first_eq,callback,context);
        else if (d->candidate_v_run==3 && d->v_run==0)
            rf_reject_field(d,RF_REJECT_CADENCE,"vertical broad pulses lack EQ cadence");
    } else { d->eq_run=0; d->v_run=0; }
    d->last_kind=kind; d->last_pulse=start;d->last_pulse_end=end;
}

static void rf_commit_pulse(gui_rf_decoder_t *d, double start, double end,
                            gui_rf_frame_callback_t callback, void *context) {
    rf_commit_pulse_mode(d,start,end,callback,context,true);
}

/* Beta's two video modulations need different nominal FM levels. A mismatched
 * profile can put the initial slicer below every sync tip, before ordinary
 * H-based level tracking has any chance to run. This bounded acquisition path
 * only reads the existing working-rate rings after field progress stops. It
 * never changes RF filters, profile, pulse classes or field acceptance rules. */
static bool rf_level_finite(float value) {
    uint32_t bits;
    memcpy(&bits,&value,sizeof(bits));
    return (bits&UINT32_C(0x7f800000))!=UINT32_C(0x7f800000);
}

static float rf_level_quantile(float *values,int count,int target) {
    int left=0,right=count-1;
    while(left<right) {
        float pivot=values[left+(right-left)/2];
        int i=left,j=right;
        do {
            while(i<=right && values[i]<pivot) i++;
            while(j>=left && values[j]>pivot) j--;
            if(i<=j) {
                float swap=values[i];values[i]=values[j];values[j]=swap;
                i++;j--;
            }
        } while(i<=j);
        if(target<=j) right=j;
        else if(target>=i) left=i;
        else break;
    }
    return values[target];
}

static bool rf_level_measure(const gui_rf_decoder_t *d,uint64_t first,
                             uint64_t last,double threshold,
                             double *sync_level,double *blank_level) {
    float tips[RF_LEVEL_PULSES],porches[RF_LEVEL_PULSES],powers[RF_LEVEL_PULSES];
    double nominal=d->p.line_us*5.0;
    uint64_t fall=0,previous_fall=0;
    int count=0,regular=0,run=0,longest=0;
    bool low=d->sync[first&RF_RING_MASK]<threshold,have_fall=false;
    /* Leave the whole back porch available for each candidate H pulse. */
    for(uint64_t at=first+1;at+48<=last;++at) {
        float value=d->sync[at&RF_RING_MASK];
        if(!rf_level_finite(value)) return false;
        bool next_low=value<threshold;
        if(next_low && !low) {fall=at;have_fall=true;}
        else if(!next_low && low && have_fall) {
            double width=(at-fall)/5.0;
            if(width>=3.5 && width<=8.0) {
                if(count==RF_LEVEL_PULSES) return false;
                if(count && fabs((double)(fall-previous_fall)-nominal)<nominal*.12) {
                    regular++;run++;
                } else run=1;
                if(run>longest) longest=run;
                previous_fall=fall;
                double tip=0,porch=0,power=0;
                double origin=(double)fall+d->sync_luma_delay-d->sync_delay;
                for(int n=0;n<9;++n) {
                    tip+=rf_read(d,d->sync_luma,origin+8.5+n);
                    power+=rf_read(d,d->envelope,origin+8.5+n);
                }
                for(int n=0;n<4;++n) porch+=rf_read(d,d->sync_luma,origin+41.5+n);
                tips[count]=(float)(tip/9);porches[count]=(float)(porch/4);
                powers[count]=(float)(power/9);
                if(!rf_level_finite(tips[count]) || !rf_level_finite(porches[count])
                    || !rf_level_finite(powers[count])) return false;
                count++;
            }
            have_fall=false;
        }
        low=next_low;
    }
    /* A few chance noise crossings must not accumulate into a healthy H flag.
     * Require a dense, locally continuous line grid within this single window. */
    if(count<64 || longest<24 || regular*10<(count-1)*7) return false;
    double tip=rf_level_quantile(tips,count,count/2);
    double porch=rf_level_quantile(porches,count,count/2);
    double span=porch-tip,nominal_span=d->p.blank_hz-d->p.sync_hz;
    if(tip<=d->fm_center-1750000.0 || porch>=d->fm_center+1750000.0
        || span<nominal_span*.35 || span>nominal_span*2.5
        || tip>=threshold || porch<=threshold
        || rf_level_quantile(powers,count,count/2)<=1e-8f) return false;
    double tip_spread=rf_level_quantile(tips,count,count*9/10)
                      -rf_level_quantile(tips,count,count/10);
    double porch_spread=rf_level_quantile(porches,count,count*9/10)
                        -rf_level_quantile(porches,count,count/10);
    if(tip_spread>span*.8 || porch_spread>span*.8) return false;
    *sync_level=tip;*blank_level=porch;
    return true;
}

/* SuperBeta recordings do not necessarily reach the nominal FM endpoints.
 * A correct H slicer alone cannot fix the display's independent black/IRE
 * mapping. Calibrate only when an actual field has passed its geometry check;
 * robust measured H tips and porches provide -40/0 IRE without picture extrema.
 * Keep the resulting affine mapping until loss/reacquisition, so fades, noisy
 * individual porches and ordinary picture changes cannot pump brightness. */
static void rf_prepare_display_levels(gui_rf_decoder_t *d) {
    if((d->profile!=GUI_RF_PROFILE_NTSC_SUPERBETA &&
        d->profile!=GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI &&
        d->profile!=GUI_RF_PROFILE_NTSC_ED_BETA && !rf_secam_fm(d)) ||
        d->display_levels_valid || !d->have_low ||
        d->low_last<d->display_level_search_after) return;
    uint64_t window=(uint64_t)(d->p.line_us*5.0*192.0);
    if(d->low_last-d->low_first<window) return;
    d->display_level_search_after=d->low_last+RF_LEVEL_GRACE;
    double tip,blank;
    if(!rf_level_measure(d,d->low_last-window,d->low_last,
                         (d->sync_est+d->blank_est)*.5,&tip,&blank)) return;
    double sync_ire=rf_secam_fm(d) ? (d->p.blank_hz-d->p.sync_hz)/d->p.hz_ire : 40.0;
    double hz_ire=(blank-tip)/sync_ire;
    /* Acquisition permits a wider slicer range than a trustworthy display
     * gain. Reject improbable scale changes instead of magnifying noise. */
    if(hz_ire<d->p.hz_ire*.5 || hz_ire>d->p.hz_ire*1.75) return;
    d->display_blank_hz=blank*d->luma_wow;
    d->display_hz_ire=hz_ire*d->luma_wow;
    d->display_levels_valid=true;
    d->display_level_search_after=0;
}

static void rf_acquire_beta_levels(gui_rf_decoder_t *d) {
    if((!d->p.beta_rotation && !rf_secam_fm(d)) || !d->have_low || d->low_last<d->level_search_after) return;
    /* Healthy fields take the original path, including its exact level updates.
     * A stale H lock alone is insufficient: H-only input has no field progress. */
    if(d->status.fields_decoded && d->low_last>=d->last_level_field
        && d->low_last-d->last_level_field<RF_LEVEL_GRACE) return;
    uint64_t window=(uint64_t)(d->p.line_us*5.0*192.0);
    if(d->low_last-d->low_first<window) return;
    d->level_search_after=d->low_last+RF_LEVEL_GRACE;
    uint64_t first=d->low_last-window;
    float samples[RF_LEVEL_POINTS];
    for(int n=0;n<RF_LEVEL_POINTS;++n) {
        uint64_t at=first+window*(uint64_t)n/RF_LEVEL_POINTS;
        samples[n]=d->sync[at&RF_RING_MASK];
        if(!rf_level_finite(samples[n])) return;
    }
    double low=rf_level_quantile(samples,RF_LEVEL_POINTS,RF_LEVEL_POINTS/20);
    double high=rf_level_quantile(samples,RF_LEVEL_POINTS,RF_LEVEL_POINTS/4);
    if(high-low<(d->p.blank_hz-d->p.sync_hz)*.2) return;
    /* Bright pictures can raise the upper quantile above the actual porch.
     * Treat quantiles only as brackets; measured H tip/porch must validate it. */
    const double fractions[]={.5,.25,.125,.75};
    for(size_t n=0;n<sizeof(fractions)/sizeof(fractions[0]);++n) {
        double sync_level,blank_level;
        if(!rf_level_measure(d,first,d->low_last,low+(high-low)*fractions[n],
                              &sync_level,&blank_level)) continue;
        if(!rf_level_measure(d,first,d->low_last,(sync_level+blank_level)*.5,
                              &sync_level,&blank_level)) continue;
        /* No old pulse, partial field or line anchor survives a slicer change.
         * The ordinary state machine must validate future alternating fields. */
        rf_reacquire_sync(d);
        d->sync_est=d->sync_anchor=sync_level;
        d->blank_est=d->blank_anchor=blank_level;
        d->previous_sync=d->sync_y;
        snprintf(d->status.reason,sizeof(d->status.reason),"%s RF levels acquired; waiting for field sync",
                 rf_secam_fm(d) ? "SECAM" : "Beta");
        return;
    }
}

void gui_rf_decoder_set_frame_mode(gui_rf_decoder_t *d, bool full_frame) {
    if (!d) return;
    d->full_frame = full_frame;
    /* Assemble the next frame with the new geometry: drop any half-built
     * interlace pair so a mode switch cannot mix modes inside one frame. */
    d->ready_fields = 0;
}

void gui_rf_decoder_reset_profile(gui_rf_decoder_t *d, gui_rf_profile_t profile) {
    if (!d || !rf_profile_valid(profile)) return;
    float *storage=d->storage;
    uint8_t *rgba=d->rgba;
    bool full_frame=d->full_frame;
    memset(d,0,sizeof(*d));
    d->storage=storage; d->rgba=rgba;
    d->full_frame=full_frame;   /* Display mode survives resets/rebinds. */
    d->luma=storage; d->sync=storage+RF_RING_SIZE;
    d->ci=storage+2u*RF_RING_SIZE; d->cq=storage+3u*RF_RING_SIZE;
    d->envelope=storage+4u*RF_RING_SIZE;
    d->sync_luma=storage+5u*RF_RING_SIZE;
    d->secam_fm=storage+6u*RF_RING_SIZE;
    d->mesecam_i=storage+7u*RF_RING_SIZE;d->mesecam_q=storage+8u*RF_RING_SIZE;
    d->profile=profile;
    d->format=gui_rf_profile_format(profile);
    d->p=rf_profiles[profile];
    d->raw_rate=gui_rf_profile_video_rate_hz(profile);
    d->raw_decimation=(unsigned)(d->raw_rate/RF_VIDEO_RATE);
    d->period=d->p.line_us*5.0;
    d->sync_est=d->p.sync_hz; d->blank_est=d->p.blank_hz;
    d->sync_anchor=d->p.sync_hz;d->blank_anchor=d->p.blank_hz;
    d->display_blank_hz=d->p.blank_hz;d->display_hz_ire=d->p.hz_ire;
    d->luma_wow=1.0;
    d->fm_center=d->p.fm_center;
    d->y_cos=d->c_cos=1.0;
    d->y_step_cos=cos(2.0*RF_PI*d->fm_center/d->raw_rate);
    d->y_step_sin=sin(2.0*RF_PI*d->fm_center/d->raw_rate);
    d->c_step_cos=cos(2.0*RF_PI*d->p.carrier_hz/d->raw_rate);
    d->c_step_sin=sin(2.0*RF_PI*d->p.carrier_hz/d->raw_rate);
    for (int n=0;n<d->p.rf_highpass_sections;++n) {
        double q=d->p.rf_highpass_sections==1 ? .70710678
            : 1.0/(2.0*cos((2.0*n+1.0)*RF_PI/(4.0*d->p.rf_highpass_sections)));
        d->rf_hp[n]=rf_filter(d->raw_rate,d->p.rf_low,q,true);
    }
    d->rf_lp=rf_filter(d->raw_rate,d->p.rf_high,.70710678,false);
    const double qs[2]={.5411961,1.3065630};
    /* ED filters before each reduction: Y at 20/20M, C at 20/20/5/5M.
     * Keep absolute raw-sample phases in process(), and account for each
     * section's actual rate when registering the common 5M picture. */
    /* Elliptic order 4: 1.75 MHz edge, 0.2 dB ripple, 40 dB stopband at 20M. */
    static const float ed_pre[2][5]={
        {0.019659819678039651, 0.0077996131655782711, 0.019659819678039651, -1.3876127529628812, 0.52659077574587287},
        {1, -1.0966208836416169, 0.99999999999999978, -1.5179442038870754, 0.83136103141623829}
    };
    for (int n=0; n<2; ++n) {
        double stage_rate=d->raw_rate;
        if(profile==GUI_RF_PROFILE_NTSC_ED_BETA)
            d->yi[n]=d->yq[n]=(rf_biquad_t){ed_pre[n][0],ed_pre[n][1],ed_pre[n][2],ed_pre[n][3],ed_pre[n][4],0,0};
        else d->yi[n]=d->yq[n]=rf_filter(stage_rate,1750000.0,qs[n],false);
        d->luma_delay+=rf_filter_delay(&d->yi[n])/(stage_rate/RF_VIDEO_RATE);
    }
    if(profile==GUI_RF_PROFILE_NTSC_ED_BETA) for(int n=2;n<4;++n) {
        double rate=RF_VIDEO_RATE;
        double cutoff=2000000.0;
        double q=qs[n-2];
        d->yi[n]=d->yq[n]=rf_filter(rate,cutoff,q,false);
        d->luma_delay+=rf_filter_delay(&d->yi[n])/(rate/RF_VIDEO_RATE);
    }
    for (int n=0;n<d->p.chroma_sections;++n) {
        double q=d->p.chroma_sections==2 ? qs[n]
            : 1.0/(2.0*cos((2.0*n+1.0)*RF_PI/(4.0*d->p.chroma_sections)));
        double stage_rate=profile==GUI_RF_PROFILE_NTSC_ED_BETA ?
            (n>=2 ? RF_VIDEO_RATE : d->raw_rate) : d->raw_rate;
        d->ui[n]=d->uq[n]=rf_filter(stage_rate,d->p.chroma_lowpass,q,false);
        d->chroma_delay+=rf_filter_delay(&d->ui[n])/(stage_rate/RF_VIDEO_RATE);
    }
#if GUI_RF_PACKED_IQ
    if(d->p.beta_rotation || rf_secam_fm(d)) for(int stage=0;stage<d->p.chroma_sections;++stage) {
        for(int lane=0;lane<4;++lane) {
            const rf_biquad_t *f=lane>=2 ? (lane==2 ? &d->ui[stage] : &d->uq[stage])
                : stage<(profile==GUI_RF_PROFILE_NTSC_ED_BETA ? 4 : 2) ? (lane==0 ? &d->yi[stage] : &d->yq[stage]) : NULL;
            if(!f) continue; // Extra Hi-Fi chroma stages have no luma stream.
            rf_biquad4_t *packed=&d->beta_iq[stage];
            packed->b0[lane]=f->b0;packed->b1[lane]=f->b1;packed->b2[lane]=f->b2;
            packed->a1[lane]=f->a1;packed->a2[lane]=f->a2;
        }
    }
#endif
    d->secam_fm_delay=d->chroma_delay+.5;
    double k=2.0*RF_VIDEO_RATE, denominator=1.0+k*d->p.deemph_pole;
    d->deemph_b0=(float)((1.0+k*d->p.deemph_zero)/denominator);
    d->deemph_b1=(float)((1.0-k*d->p.deemph_zero)/denominator);
    d->deemph_a1=(float)((1.0-k*d->p.deemph_pole)/denominator);
    d->luma_delay+=.5+(d->p.deemph_pole-d->p.deemph_zero)*RF_VIDEO_RATE;
    /* The higher SuperBeta FM band changes the reduced-bandwidth Y/C path
     * registration. Physical midtone RF edges across clock/jitter cases
     * retain a small chroma lag with DC-only filter delay compensation.
     * Advance sampled chroma by one working sample (200ns), cached here;
     * this is preview calibration, not a normative tape timing parameter. */
    if(profile==GUI_RF_PROFILE_NTSC_SUPERBETA || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI)
        d->chroma_delay+=1.0;
    d->sync_luma_delay=d->luma_delay;
    d->sync_alpha=(float)exp(-2.0*RF_PI*500000.0/RF_VIDEO_RATE);
    d->sync_delay=d->sync_luma_delay+d->sync_alpha/(1.0-d->sync_alpha);
    if (d->format==GUI_RF_NTSC && gui_rf_deemph_coefficients(RF_VIDEO_RATE,
            d->p.shelf_gain,d->p.shelf_mid,d->p.shelf_q,&d->luma_deemph.b0,&d->luma_deemph.b1,&d->luma_deemph.b2,
            &d->luma_deemph.a1,&d->luma_deemph.a2)) {
        d->use_ntsc_deemph=true;
        /* Retain measured display edge registration against legacy sync.
         * The recorded FM is already pre-emphasized: a bare shelf's DC group
         * delay is not the complete recording/decoder/sync path delay. Adding
         * it here advances actual Y edges about ten output pixels. This fixed
         * edge alignment is intentionally not a linear-phase/full-path model. */
        d->luma_delay=d->sync_luma_delay;
        double x=d->p.blank_hz-d->fm_center;
        double dc=(d->luma_deemph.b0+d->luma_deemph.b1+d->luma_deemph.b2)
                 /(1.0+d->luma_deemph.a1+d->luma_deemph.a2);
        double y=x*dc;
        d->luma_deemph.z1=(float)(y-d->luma_deemph.b0*x);
        d->luma_deemph.z2=(float)(d->luma_deemph.b2*x-d->luma_deemph.a2*y);
    }
    d->deemph_x=d->deemph_y=(float)(d->p.blank_hz-d->fm_center);
    if(rf_secam_fm(d)) {
        /* Inverse of SECAM's 85/255kHz LF pre-correction, with phase and
         * continuous state. The divider scales deviation, not video time. */
        double pole=1.0/(2.0*RF_PI*85000.0),zero=pole/3.0;
        double lf_denominator=1.0+2.0*RF_VIDEO_RATE*pole;
        d->secam_b0=(float)((1.0+2.0*RF_VIDEO_RATE*zero)/lf_denominator);
        d->secam_b1=(float)((1.0-2.0*RF_VIDEO_RATE*zero)/lf_denominator);
        d->secam_a1=(float)((1.0-2.0*RF_VIDEO_RATE*pole)/lf_denominator);
        /* The recording's LF pre-correction cancels this inverse network's
         * delay. Compensating its DC delay again advances C about 18 pixels
         * relative to Y. Retain the discriminator's half-sample, plus the
         * three working samples measured on physical midtone RF edges for
         * these reduced-bandwidth filter pairs (physical native/ME RF tests).
         * This 600ns registration trim is not a tape-standard parameter. */
        d->chroma_delay+=3.5;
        /* LF inverse alone retains one third of discriminator HF noise.
         * Limit decoded C separately from the pre-FM sideband filter and
         * register its causal delay against the original luma samples. */
        for(int n=0;n<2;++n) {
            d->secam_video_lp[n]=rf_filter(RF_VIDEO_RATE,250000.0,qs[n],false);
            d->chroma_delay+=rf_filter_delay(&d->secam_video_lp[n]);
        }
    }
    d->sync_y=(float)d->p.blank_hz;
    memset(d->rgba,0,(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    snprintf(d->status.reason,sizeof(d->status.reason),"waiting for %s RF sync",gui_rf_profile_name(profile));
}

void gui_rf_decoder_reset(gui_rf_decoder_t *d, gui_rf_format_t format) {
    gui_rf_decoder_reset_profile(d,format==GUI_RF_NTSC ? GUI_RF_PROFILE_NTSC_VHS : GUI_RF_PROFILE_PAL_VHS);
}

gui_rf_decoder_t *gui_rf_decoder_create_profile(gui_rf_profile_t profile) {
    if (!rf_profile_valid(profile)) return NULL;
    gui_rf_decoder_t *d=calloc(1,sizeof(*d));
    if (!d) return NULL;
    d->storage=calloc(RF_RING_SIZE*9u,sizeof(float));
    d->rgba=calloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT,4u);
    if (!d->storage || !d->rgba) { gui_rf_decoder_destroy(d); return NULL; }
    gui_rf_decoder_reset_profile(d,profile);
    return d;
}

gui_rf_decoder_t *gui_rf_decoder_create(gui_rf_format_t format) {
    return gui_rf_decoder_create_profile(format==GUI_RF_NTSC ? GUI_RF_PROFILE_NTSC_VHS : GUI_RF_PROFILE_PAL_VHS);
}

void gui_rf_decoder_destroy(gui_rf_decoder_t *d) {
    if (!d) return;
    free(d->storage); free(d->rgba); free(d);
}

void gui_rf_decoder_get_status(const gui_rf_decoder_t *d, gui_rf_decode_status_t *status) {
    if (status) { if (d) *status=d->status; else memset(status,0,sizeof(*status)); }
}

void gui_rf_decoder_process(gui_rf_decoder_t *d, const float *samples, size_t count,
                            uint64_t first_sample, gui_rf_frame_callback_t callback,
                            void *context) {
    if (!d || !count || !samples) return;
    if (d->have_raw && first_sample!=d->next_raw) {
        gui_rf_decoder_reset_profile(d,d->profile);
        snprintf(d->status.reason,sizeof(d->status.reason),"RF gap; reacquiring sync");
    }
    d->have_raw=true; d->next_raw=first_sample+count;
    const bool ed=d->profile==GUI_RF_PROFILE_NTSC_ED_BETA;
    /* 20M -> 5M uses an absolute four-phase schedule.
     * Derive it once per call so arbitrary block splits retain the same
     * absolute phase without a 64-bit division on every input sample. */
    unsigned ed_phase=ed ? (unsigned)(first_sample%4u) : 0;
    uint64_t ed_position=ed ? first_sample/4u+(ed_phase!=0u) : 0;
#if GUI_RF_PACKED_IQ
    /* ED's two independent oscillators share SSE2 lanes. Keep their state
     * local across the block; the field renderer never changes RF state. */
    __m128d ed_cos=_mm_set_pd(d->c_cos,d->y_cos),ed_sin=_mm_set_pd(d->c_sin,d->y_sin);
    const __m128d ed_step_cos=_mm_set_pd(d->c_step_cos,d->y_step_cos);
    const __m128d ed_step_sin=_mm_set_pd(d->c_step_sin,d->y_step_sin);
#endif
    for (size_t n=0; n<count; ++n) {
        uint64_t raw_pos=first_sample+n;
        bool picture_tick=ed && ed_phase==0u;
        if(ed && ++ed_phase==4u) ed_phase=0;
        float raw=isfinite(samples[n]) ? samples[n] : 0.0f;
        float band=raw;
        for (int s=0;s<d->p.rf_highpass_sections;++s) band=rf_filter_sample(&d->rf_hp[s],band);
        band=rf_filter_sample(&d->rf_lp,band);
        float yi=0,yq=0,ci=0,cq=0;
#if GUI_RF_PACKED_IQ
        __m128 mixed;
        if(ed) {
            __m128d amplitude=_mm_set_pd(raw,band);
            mixed=_mm_unpacklo_ps(_mm_cvtpd_ps(_mm_mul_pd(amplitude,ed_cos)),
                _mm_cvtpd_ps(_mm_sub_pd(_mm_setzero_pd(),_mm_mul_pd(amplitude,ed_sin))));
        } else
#endif
        {
            yi=(float)(band*d->y_cos); yq=(float)(-band*d->y_sin);
            ci=(float)(raw*d->c_cos); cq=(float)(-raw*d->c_sin);
        }
#if GUI_RF_PACKED_IQ
        if(d->p.beta_rotation || rf_secam_fm(d)) {
            __m128 iq=ed ? mixed : _mm_setr_ps(yi,yq,ci,cq);
            iq=rf_filter4_sample(&d->beta_iq[0],iq);
            iq=rf_filter4_sample(&d->beta_iq[1],iq);
            {
                __m128 luma_iq=iq;
                for(int s=2;s<d->p.chroma_sections;++s)
                    if(!ed || picture_tick) iq=rf_filter4_sample(&d->beta_iq[s],iq);
                // Ordinary Beta preserves Y through its additional C-only stages.
                if(!ed) iq=_mm_movelh_ps(luma_iq,_mm_movehl_ps(iq,iq));
            }
            if(!ed || picture_tick) {
                float filtered[4];_mm_storeu_ps(filtered,iq);
                yi=filtered[0];yq=filtered[1];ci=filtered[2];cq=filtered[3];
            }
        } else
#endif
        {
            for (int s=0; s<2; ++s) {
                yi=rf_filter_sample(&d->yi[s],yi); yq=rf_filter_sample(&d->yq[s],yq);
            }
            for (int s=0;s<d->p.chroma_sections;++s) {
                if(!ed || s<2 || picture_tick) {
                    ci=rf_filter_sample(&d->ui[s],ci); cq=rf_filter_sample(&d->uq[s],cq);
                }
            }
            if(ed) for(int s=2;s<4;++s) if(picture_tick) {
                yi=rf_filter_sample(&d->yi[s],yi); yq=rf_filter_sample(&d->yq[s],yq);
            }
        }
#if GUI_RF_PACKED_IQ
        if(ed) {
            __m128d next_cos=_mm_sub_pd(_mm_mul_pd(ed_cos,ed_step_cos),_mm_mul_pd(ed_sin,ed_step_sin));
            ed_sin=_mm_add_pd(_mm_mul_pd(ed_sin,ed_step_cos),_mm_mul_pd(ed_cos,ed_step_sin));
            ed_cos=next_cos;
            if(!(raw_pos&1023u)) {
                __m128d magnitude=_mm_sqrt_pd(_mm_add_pd(_mm_mul_pd(ed_cos,ed_cos),_mm_mul_pd(ed_sin,ed_sin)));
                ed_cos=_mm_div_pd(ed_cos,magnitude);ed_sin=_mm_div_pd(ed_sin,magnitude);
            }
        } else
#endif
        {
            double yc=d->y_cos*d->y_step_cos-d->y_sin*d->y_step_sin;
            d->y_sin=d->y_sin*d->y_step_cos+d->y_cos*d->y_step_sin; d->y_cos=yc;
            double cc=d->c_cos*d->c_step_cos-d->c_sin*d->c_step_sin;
            d->c_sin=d->c_sin*d->c_step_cos+d->c_cos*d->c_step_sin; d->c_cos=cc;
            if ((raw_pos&1023u)==0u) {
                double ym=hypot(d->y_cos,d->y_sin), cm=hypot(d->c_cos,d->c_sin);
                if (ym>0.0) { d->y_cos/=ym; d->y_sin/=ym; }
                if (cm>0.0) { d->c_cos/=cm; d->c_sin/=cm; }
            }
        }
        uint64_t pos;
        if(ed) {
            if(!picture_tick) continue;
            pos=ed_position++;
        } else {
            if ((raw_pos&(d->raw_decimation-1u))!=0u) continue;
            pos=raw_pos/d->raw_decimation;
        }
        double cross=(double)d->prev_i*yq-(double)d->prev_q*yi;
        double dot=(double)d->prev_i*yi+(double)d->prev_q*yq;
        /* ED uses a full-quadrant bounded approximation; MESECAM keeps
         * its small-angle path. Other profiles retain atan2f. */
        double yphase=d->profile==GUI_RF_PROFILE_MESECAM_VHS
            ? rf_mesecam_phase(cross,dot) : ed ? gui_rf_preview_phase(cross,dot)
                : atan2f((float)cross,(float)dot);
        double frequency=d->fm_center+yphase*(RF_VIDEO_RATE/(2.0*RF_PI));
        d->prev_i=yi; d->prev_q=yq;
        if (!isfinite(frequency)) frequency=d->p.blank_hz;
        float x=(float)(frequency-d->fm_center);
        float y=d->deemph_b0*x+d->deemph_b1*d->deemph_x-d->deemph_a1*d->deemph_y;
        d->deemph_x=x; d->deemph_y=y;
        float luma=d->use_ntsc_deemph ? rf_filter_sample(&d->luma_deemph,x) : y;
        y+=(float)d->fm_center;
        /* Beta record emphasis requires its matched inverse shelf for timing too.
         * The VHS legacy plane fragments real Beta EQ/broad pulses, losing
         * fields despite a valid display luma signal. Preserve VHS timing. */
        float sync_input=d->p.beta_rotation ? luma+(float)d->fm_center : y;
        d->sync_y=d->sync_alpha*d->sync_y+(1.0f-d->sync_alpha)*sync_input;
        unsigned at=(unsigned)(pos&RF_RING_MASK);
        d->luma[at]=luma+(float)d->fm_center; d->sync[at]=d->sync_y;
        d->sync_luma[at]=sync_input;
        if(d->profile==GUI_RF_PROFILE_MESECAM_VHS) {
            d->mesecam_i[at]=ci;d->mesecam_q[at]=cq;
            /* Field preparation fills only the buffered, bounded interval.
             * Any unavailable margin remains explicitly without carrier. */
            d->secam_fm[at]=(float)d->p.carrier_hz;
            d->ci[at]=(float)d->p.carrier_hz;d->cq[at]=0.0f;
        } else if(rf_secam_fm(d)) {
            double ccross=(double)d->secam_prev_i*cq-(double)d->secam_prev_q*ci;
            double cdot=(double)d->secam_prev_i*ci+(double)d->secam_prev_q*cq;
            /* Most deviations are small around the mixed-down centre.
             * atan(r) through r^7 has error <= 4.3e-7 rad for |r|<=.25
             * (under .35Hz at 5MSPS). Keep atan2 for fades/clicks/quadrants. */
            double phase;
            if(cdot>0.0 && fabs(ccross)<=.25*cdot) {
                double r=ccross/cdot,r2=r*r;
                phase=r*(1.0+r2*(-1.0/3.0+r2*(1.0/5.0-r2/7.0)));
            } else phase=atan2f((float)ccross,(float)cdot);
            double cf=d->p.carrier_hz+phase*(RF_VIDEO_RATE/(2.0*RF_PI));
            d->secam_prev_i=ci;d->secam_prev_q=cq;
            /* Clamp in the recorded carrier domain before LF recovery.
             * MESECAM inverts the studio rails, native divides by four. */
            double low=rf_native_secam(d) ? 975000.0 : 304321.875;
            double high=rf_native_secam(d) ? 1189062.5 : 1160571.875;
            /* Unclipped frequency makes a quiet reference independent of
             * the limiter, LF network, post-filter and active content. */
            if(!rf_native_secam(d)) d->secam_fm[at]=(float)cf;
            float cx=(float)(rf_clamp(cf,low,high)-d->p.carrier_hz);
            float cy=d->secam_b0*cx+d->secam_b1*d->secam_deemph_x
                -d->secam_a1*d->secam_deemph_y;
            d->secam_deemph_x=cx;d->secam_deemph_y=cy;
            for(int stage=0;stage<2;++stage) cy=rf_filter_sample(&d->secam_video_lp[stage],cy);
            d->ci[at]=cy+(float)d->p.carrier_hz;
            d->cq[at]=hypotf(ci,cq);
        } else {d->ci[at]=ci;d->cq[at]=cq;}
        d->envelope[at]=(ed || d->profile==GUI_RF_PROFILE_MESECAM_VHS)
            ? rf_mesecam_magnitude(yi,yq) : hypotf(yi,yq);
        if (!d->have_low) {
            d->low_first=pos; d->have_low=true;
            d->level_search_after=pos+RF_LEVEL_GRACE;
        }
        d->low_last=pos;
        if (pos-d->low_first>=RF_RING_SIZE) d->low_first=pos-RF_RING_SIZE+1u;
        if (pos-d->low_first<128u) { d->previous_sync=d->sync_y; continue; }
        double threshold=(d->sync_est+d->blank_est)*.5;
        bool low=d->sync_y<threshold;
        if (low && !d->in_pulse) {
            double delta=d->sync_y-d->previous_sync;
            double f=fabs(delta)>1e-9 ? (threshold-d->previous_sync)/delta : 1.0;
            d->pulse_start=(double)pos-1.0+rf_clamp(f,0,1);
            d->in_pulse=true;
        } else if (!low && d->in_pulse) {
            double delta=d->sync_y-d->previous_sync;
            double f=fabs(delta)>1e-9 ? (threshold-d->previous_sync)/delta : 1.0;
            rf_commit_pulse(d,d->pulse_start,(double)pos-1.0+rf_clamp(f,0,1),callback,context);
            d->in_pulse=false;
        }
        if (d->last_h>0.0 && (double)pos-d->last_h>d->period*40.0)
            rf_reacquire_sync(d);
        d->previous_sync=d->sync_y;
    }
#if GUI_RF_PACKED_IQ
    if(ed) {
        double cs[2],sn[2];_mm_storeu_pd(cs,ed_cos);_mm_storeu_pd(sn,ed_sin);
        d->y_cos=cs[0];d->c_cos=cs[1];d->y_sin=sn[0];d->c_sin=sn[1];
    }
#endif
    rf_acquire_beta_levels(d);
}
