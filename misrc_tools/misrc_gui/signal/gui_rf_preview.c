/*
 * CPU VHS SP RF monitor. GPL-3.0, as the containing MISRC GUI project.
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RF_PI 3.14159265358979323846
#define RF_RAW_RATE 20000000.0
#define RF_VIDEO_RATE 5000000.0
#define RF_RING_SIZE (1u << 19)
#define RF_RING_MASK (RF_RING_SIZE - 1u)
#define RF_PULSE_SIZE 2048u
#define RF_MAX_LINES 320

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } rf_biquad_t;
typedef struct { double re, im; } rf_complex_t;
typedef enum { RF_PULSE_NONE, RF_PULSE_EQ, RF_PULSE_H, RF_PULSE_V } rf_pulse_kind_t;

typedef struct {
    int width, height, active_first, eq_count;
    double line_us, blank_hz, sync_hz, hz_ire, carrier_hz;
    double burst_start_us, burst_end_us, active_start_us, active_end_us;
    double deemph_zero, deemph_pole;
} rf_profile_t;

/* SP carrier/IRE/burst/geometry parameters from the referenced format snapshot. */
static const rf_profile_t rf_profiles[] = {
    {768, 576, 22, 5, 64.0, 4100000.0, 3800000.0, 7000.0, 626953.0,
     5.6, 7.85, 10.5, 62.5, 100e-9, 400e-9},
    {640, 480, 20, 6, 63.55555555555556, 3685714.2857142854, 3400000.0,
     7142.857142857143, 629370.6293706294,
     5.3, 7.8, 9.45, 62.55555555555556, 120e-9, 320e-9}
};

struct gui_rf_decoder {
    gui_rf_format_t format;
    rf_profile_t p;
    bool full_frame;   /* Active/Full display mode (see set_frame_mode). */
    float *storage, *luma, *sync, *ci, *cq, *envelope, *sync_luma;
    uint8_t *rgba;
    uint64_t next_raw, low_first, low_last;
    bool have_raw, have_low;
    rf_biquad_t rf_hp, rf_lp, yi[2], yq[2], ui[2], uq[2], luma_deemph;
    double y_cos, y_sin, y_step_cos, y_step_sin;
    double c_cos, c_sin, c_step_cos, c_step_sin, fm_center;
    float prev_i, prev_q, deemph_x, deemph_y, sync_y;
    float deemph_b0, deemph_b1, deemph_a1, sync_alpha;
    double luma_delay, chroma_delay, sync_delay, sync_luma_delay;
    double luma_wow;
    bool have_luma_wow, use_ntsc_deemph;
    float field_rows[2][3][GUI_RF_FRAME_MAX_WIDTH];
    double sync_est, blank_est, period;
    bool in_pulse;
    double pulse_start, previous_sync;
    double last_h, first_eq, last_pulse, last_pulse_end, first_v;
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

/* A stopped/unstable tape can leave the learned threshold outside a healthy
 * signal's sync-to-blank range. H edges cannot then update it back into range.
 * Discard acquisition history once on H loss, keeping source cursors, filters
 * and diagnostic counters continuous. The next frame needs two fresh fields. */
static void rf_reacquire_sync(gui_rf_decoder_t *d) {
    rf_color_loss(d,"RF horizontal sync lost");
    d->status.h_locked=d->status.v_locked=false;
    d->sync_est=d->p.sync_hz;d->blank_est=d->p.blank_hz;
    d->period=d->p.line_us*5.0;
    d->good_h=0;d->last_h=0.0;
    d->h_head=d->h_count=0;
    d->in_pulse=false;d->last_kind=RF_PULSE_NONE;
    d->eq_run=d->v_run=d->candidate_v_run=0;
    d->have_short_broad=d->have_split_broad=false;
    d->split_follow_count=0;
    d->ready_fields=0;d->have_field=false;
    d->have_luma_wow=false;d->luma_wow=1.0;
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
}

static bool rf_render_field(gui_rf_decoder_t *d, double field_start, int parity,
                            uint64_t end_sample, gui_rf_frame_callback_t callback,
                            void *context) {
    int rows = d->p.height/2;
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
    int field_lines=d->format==GUI_RF_PAL ? (parity==0 ? 312 : 313)
                                         : (parity==0 ? 263 : 262);
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
    (void)dot_sum;
    double track_step = d->format==GUI_RF_PAL
        ? (abs_dot>abs_cross ? -RF_PI*.5 : 0.0)
        : (cross_sum>=0.0 ? RF_PI*.5 : -RF_PI*.5);
    rf_complex_t common={0,0};
    for (int y=0; y<rows; ++y) {
        corrected[y]=rf_rotate(burst[y], -track_step*y);
        if (valid[y]) { common.re+=corrected[y].re/magnitudes[y];
                       common.im+=corrected[y].im/magnitudes[y]; }
    }
    double common_phase=atan2(common.im,common.re)-RF_PI;
    double coherence=burst_count ? hypot(common.re,common.im)/burst_count : 0.0;
    bool color = burst_count>rows*3/4 && coherence>(d->format==GUI_RF_PAL ? .40 : .65);
    if (!color) rf_color_loss(d,burst_count<=rows*3/4
        ? "insufficient valid color bursts" : "incoherent VHS track/burst phase");
    /* PAL swings its burst about the -U axis. Use neighboring measured burst
     * vectors after the tape rotation is removed, rather than field number. */
    for (int y=0; y<rows; ++y) {
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
        /* Display black/IRE are format parameters. Four noisy porch samples
         * and individual sync tips must not offset or amplify entire rows. */
        double pixel_step=(win_end_us-win_start_us)*5.0*scale/d->p.width;
        double first_offset=win_start_us*5.0*scale+pixel_step*.5;
        double carrier_scale=2.0*RF_PI*d->p.carrier_hz/RF_VIDEO_RATE;
        double timing0=lines[y]-field_start-(d->p.active_first+y)*nominal
                      +first_offset*(1.0-nominal/periods[y]);
        double angle=phase+carrier_scale*timing0;
        double rotate_re=cos(angle), rotate_im=sin(angle);
        double step_angle=carrier_scale*pixel_step*(1.0-nominal/periods[y]);
        double step_re=cos(step_angle), step_im=sin(step_angle);
        for (int x=0; x<d->p.width; ++x) {
            double offset=first_offset+x*pixel_step;
            double hz=rf_read(d,d->luma,lines[y]+offset+d->luma_delay-d->sync_delay);
            double luminance=rf_clamp((hz*luma_wow[y]-d->p.blank_hz)
                                      /(100.0*d->p.hz_ire),0.0,1.0);
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
    d->status.fields_decoded++;
    d->status.fields_accepted++;
    d->status.h_locked=true;
    d->status.v_locked=true;
    d->status.line_period_us=average/5.0;
    d->ready_color[parity]=color;
    d->ready_fields|=1u<<parity;
    if (d->ready_fields==3u) {
        d->status.frames_decoded++;
        d->status.color_locked=d->ready_color[0] && d->ready_color[1];
        snprintf(d->status.reason,sizeof(d->status.reason),"%s VHS SP %s",
                 d->format==GUI_RF_PAL ? "PAL" : "NTSC",
                 d->status.color_locked ? "color locked" : "luma locked; weak burst");
        if (callback) callback(context,d->rgba,d->p.width,d->p.height,end_sample,&d->status);
        d->ready_fields=0;
    }
    return true;
}

static void rf_vertical_start(gui_rf_decoder_t *d, double first_eq,
                              gui_rf_frame_callback_t callback, void *context) {
    double nominal=d->p.line_us*5.0;
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
        double field_lines=(start-d->field_start)/nominal;
        double expected=d->format==GUI_RF_PAL ? 312.5 : 262.5;
        if (fabs(field_lines-expected)<2.0 && parity!=d->field_parity) {
            rf_render_field(d,d->field_start,d->field_parity,
                            (uint64_t)llround(start*4.0),callback,context);
        } else {
            d->ready_fields=0;
            if (fabs(field_lines-expected)>=2.0)
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
            if (fabs(blank-d->p.blank_hz)<400000.0)
                d->blank_est=d->blank_est*.99+blank*.01;
        }
        double tip=0.0; int n=0;
        for (double at=1.7*5; at<3.5*5; at+=1) {
            tip+=rf_read(d,d->sync_luma,start+at+d->sync_luma_delay-d->sync_delay); n++;
        }
        tip/=n;
        if (fabs(tip-d->p.sync_hz)<400000.0) d->sync_est=d->sync_est*.99+tip*.01;
        d->last_h=start;
        d->h_positions[d->h_head]=start;
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

void gui_rf_decoder_set_frame_mode(gui_rf_decoder_t *d, bool full_frame) {
    if (!d) return;
    d->full_frame = full_frame;
    /* Assemble the next frame with the new geometry: drop any half-built
     * interlace pair so a mode switch cannot mix modes inside one frame. */
    d->ready_fields = 0;
}

void gui_rf_decoder_reset(gui_rf_decoder_t *d, gui_rf_format_t format) {
    if (!d) return;
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
    d->format=format==GUI_RF_NTSC ? GUI_RF_NTSC : GUI_RF_PAL;
    d->p=rf_profiles[d->format];
    d->period=d->p.line_us*5.0;
    d->sync_est=d->p.sync_hz; d->blank_est=d->p.blank_hz;
    d->luma_wow=1.0;
    d->fm_center=d->format==GUI_RF_PAL ? 4300000.0 : 3900000.0;
    d->y_cos=d->c_cos=1.0;
    d->y_step_cos=cos(2.0*RF_PI*d->fm_center/RF_RAW_RATE);
    d->y_step_sin=sin(2.0*RF_PI*d->fm_center/RF_RAW_RATE);
    d->c_step_cos=cos(2.0*RF_PI*d->p.carrier_hz/RF_RAW_RATE);
    d->c_step_sin=sin(2.0*RF_PI*d->p.carrier_hz/RF_RAW_RATE);
    d->rf_hp=rf_filter(RF_RAW_RATE,1300000.0,.70710678,true);
    d->rf_lp=rf_filter(RF_RAW_RATE,6500000.0,.70710678,false);
    const double qs[2]={.5411961,1.3065630};
    for (int n=0; n<2; ++n) {
        d->yi[n]=d->yq[n]=rf_filter(RF_RAW_RATE,1750000.0,qs[n],false);
        d->ui[n]=d->uq[n]=rf_filter(RF_RAW_RATE,400000.0,qs[n],false);
        d->luma_delay+=rf_filter_delay(&d->yi[n])*.25;
        d->chroma_delay+=rf_filter_delay(&d->ui[n])*.25;
    }
    double k=2.0*RF_VIDEO_RATE, denominator=1.0+k*d->p.deemph_pole;
    d->deemph_b0=(float)((1.0+k*d->p.deemph_zero)/denominator);
    d->deemph_b1=(float)((1.0-k*d->p.deemph_zero)/denominator);
    d->deemph_a1=(float)((1.0-k*d->p.deemph_pole)/denominator);
    d->luma_delay+=.5+(d->p.deemph_pole-d->p.deemph_zero)*RF_VIDEO_RATE;
    d->sync_luma_delay=d->luma_delay;
    d->sync_alpha=(float)exp(-2.0*RF_PI*500000.0/RF_VIDEO_RATE);
    d->sync_delay=d->sync_luma_delay+d->sync_alpha/(1.0-d->sync_alpha);
    if (d->format==GUI_RF_NTSC && gui_rf_ntsc_deemph_coefficients(RF_VIDEO_RATE,
            &d->luma_deemph.b0,&d->luma_deemph.b1,&d->luma_deemph.b2,
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
    d->sync_y=(float)d->p.blank_hz;
    memset(d->rgba,0,(size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    snprintf(d->status.reason,sizeof(d->status.reason),"waiting for VHS SP RF sync");
}

gui_rf_decoder_t *gui_rf_decoder_create(gui_rf_format_t format) {
    gui_rf_decoder_t *d=calloc(1,sizeof(*d));
    if (!d) return NULL;
    d->storage=calloc(RF_RING_SIZE*6u,sizeof(float));
    d->rgba=calloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT,4u);
    if (!d->storage || !d->rgba) { gui_rf_decoder_destroy(d); return NULL; }
    gui_rf_decoder_reset(d,format);
    return d;
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
        gui_rf_decoder_reset(d,d->format);
        snprintf(d->status.reason,sizeof(d->status.reason),"RF gap; reacquiring sync");
    }
    d->have_raw=true; d->next_raw=first_sample+count;
    for (size_t n=0; n<count; ++n) {
        uint64_t raw_pos=first_sample+n;
        float raw=isfinite(samples[n]) ? samples[n] : 0.0f;
        float band=rf_filter_sample(&d->rf_lp,rf_filter_sample(&d->rf_hp,raw));
        float yi=(float)(band*d->y_cos), yq=(float)(-band*d->y_sin);
        float ci=(float)(raw*d->c_cos), cq=(float)(-raw*d->c_sin);
        for (int s=0; s<2; ++s) {
            yi=rf_filter_sample(&d->yi[s],yi); yq=rf_filter_sample(&d->yq[s],yq);
            ci=rf_filter_sample(&d->ui[s],ci); cq=rf_filter_sample(&d->uq[s],cq);
        }
        double yc=d->y_cos*d->y_step_cos-d->y_sin*d->y_step_sin;
        d->y_sin=d->y_sin*d->y_step_cos+d->y_cos*d->y_step_sin; d->y_cos=yc;
        double cc=d->c_cos*d->c_step_cos-d->c_sin*d->c_step_sin;
        d->c_sin=d->c_sin*d->c_step_cos+d->c_cos*d->c_step_sin; d->c_cos=cc;
        if ((raw_pos&1023u)==0u) {
            double ym=hypot(d->y_cos,d->y_sin), cm=hypot(d->c_cos,d->c_sin);
            if (ym>0.0) { d->y_cos/=ym; d->y_sin/=ym; }
            if (cm>0.0) { d->c_cos/=cm; d->c_sin/=cm; }
        }
        if ((raw_pos&3u)!=0u) continue;
        uint64_t pos=raw_pos/4u;
        double cross=(double)d->prev_i*yq-(double)d->prev_q*yi;
        double dot=(double)d->prev_i*yi+(double)d->prev_q*yq;
        double frequency=d->fm_center+atan2f((float)cross,(float)dot)*(RF_VIDEO_RATE/(2.0*RF_PI));
        d->prev_i=yi; d->prev_q=yq;
        if (!isfinite(frequency)) frequency=d->p.blank_hz;
        float x=(float)(frequency-d->fm_center);
        float y=d->deemph_b0*x+d->deemph_b1*d->deemph_x-d->deemph_a1*d->deemph_y;
        d->deemph_x=x; d->deemph_y=y;
        float luma=d->use_ntsc_deemph ? rf_filter_sample(&d->luma_deemph,x) : y;
        y+=(float)d->fm_center;
        d->sync_y=d->sync_alpha*d->sync_y+(1.0f-d->sync_alpha)*y;
        unsigned at=(unsigned)(pos&RF_RING_MASK);
        d->luma[at]=luma+(float)d->fm_center; d->sync[at]=d->sync_y;
        d->sync_luma[at]=y;
        d->ci[at]=ci; d->cq[at]=cq;
        d->envelope[at]=hypotf(yi,yq);
        if (!d->have_low) { d->low_first=pos; d->have_low=true; }
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
}
