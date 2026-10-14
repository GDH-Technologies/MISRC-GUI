/* Synthetic RF contract tests, not a claim of tape/hardware qualification.
 * The fixture is FM luma plus VHS color-under, including sync/equalizing
 * cadence, PAL V-switch, alternating recording-head line rotation, and a
 * warped tape time axis. It never feeds decoded RGB/YUV to the decoder. */
#include "../misrc_gui/signal/gui_rf_preview.h"
#include "../misrc_gui/signal/gui_rf_preview_filters.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PI 3.14159265358979323846
#define TEST_RATE 20000000.0

typedef struct {
    uint8_t *rgba;
    int width, height, frames;
    uint64_t end_sample;
    gui_rf_decode_status_t status;
} test_frames_t;

static void receive_frame(void *context, const uint8_t *rgba, int width, int height,
                          uint64_t end_sample, const gui_rf_decode_status_t *status) {
    test_frames_t *f=context;
    memcpy(f->rgba,rgba,(size_t)width*height*4u);
    f->width=width; f->height=height; f->frames++;
    assert(end_sample>=f->end_sample);
    f->end_sample=end_sample; f->status=*status;
}

static const double bars[8][3] = {
    {1,1,1},{1,1,0},{0,1,1},{0,1,0},{1,0,1},{1,0,0},{0,0,1},{0,0,0}
};

static double clamp_value(double value, double minimum, double maximum) {
    return value<minimum ? minimum : value>maximum ? maximum : value;
}

typedef enum {
    TEST_V_NORMAL, TEST_V_SPLIT_FIRST, TEST_V_NEW_EQ, TEST_V_ISOLATED_FRAGMENT,
    TEST_V_NO_PRE_EQ, TEST_V_ONE_PRE_EQ, TEST_V_BAD_SPACING, TEST_V_PAST_HALF,
    TEST_V_SPLIT_SECOND_H, TEST_V_SPLIT_SHORT_FIRST, TEST_V_SHORT_INTERRUPTED,
    TEST_V_SPLIT_15P5_FIRST, TEST_V_SHORT_BELOW_MIN, TEST_V_SHORT_NO_PRE_EQ,
    TEST_V_SHORT_SECOND_15P5, TEST_V_SPLIT_13P5_FIRST, TEST_V_SHORT_SECOND_13P5
} test_vertical_t;

typedef enum { TEST_Q_GRAY, TEST_Q_COLOR_BARS, TEST_Q_FADE, TEST_Q_MID_BARS } test_quality_pattern_t;
typedef struct {
    test_quality_pattern_t pattern;
    int fields;
    double window_noise_hz, carrier_offset_hz, speed_wander;
    uint64_t field_start_sample[17];
} test_quality_t;

/* Video values are a physical FM oracle: blank + 100*Hz/IRE*gray, not a
 * reconstruction of the decoder's porch estimator or render algorithm. */
static double quality_gray(int field,test_quality_pattern_t pattern) {
    return pattern==TEST_Q_FADE ? fmax(0.0,.70-.10*field) : field&1 ? .25 : .65;
}

static float *make_rf_signal(gui_rf_format_t format, double jitter_us, bool with_chroma,
                            double clock_scale, int damaged_field,
                            test_vertical_t vertical_case, test_quality_t *quality,
                            size_t *length) {
    bool pal=format==GUI_RF_PAL;
    double line_us=pal ? 64.0 : 63.55555555555556;
    double carrier=pal ? 626953.0 : 629370.6293706294;
    double blank=pal ? 4100000.0 : 3685714.2857142854;
    double sync=pal ? 3800000.0 : 3400000.0;
    double hz_ire=pal ? 7000.0 : 7142.857142857143;
    double burst_start=pal ? 5.6 : 5.3, burst_end=pal ? 7.85 : 7.8;
    double active_start=pal ? 10.5 : 9.45, active_end=pal ? 62.5 : 62.55555555555556;
    int eq_count=pal ? 5 : 6;
    int field_count=quality ? quality->fields : 6;
    assert(field_count>=6 && field_count<=16);
    int starts[17]; starts[0]=0;
    for (int f=0; f<field_count; ++f) starts[f+1]=starts[f]+(pal ? (f&1 ? 313 : 312) : (f&1 ? 262 : 263));
    int half_lines=starts[field_count]*2;
    double *timeline=malloc((size_t)(half_lines+1)*sizeof(double));
    assert(timeline);
    for (int h=0; h<=half_lines; ++h) {
        timeline[h]=h*line_us*.5*clock_scale+jitter_us*sin(h*2.0*TEST_PI/19.0);
        if (quality && quality->speed_wander!=0.0) {
            /* Integrate a slow physical speed variation. Both FM carrier and
             * actual H timing move together; active video values stay fixed. */
            double duration=half_lines*line_us*.5;
            timeline[h]+=quality->speed_wander*duration/(2.0*TEST_PI)
                *(1.0-cos(h*2.0*TEST_PI/half_lines));
        }
    }
    if (quality) for (int f=0; f<=field_count; ++f)
        quality->field_start_sample[f]=(uint64_t)floor(timeline[starts[f]*2]*20.0);
    *length=(size_t)floor(timeline[half_lines]*20.0);
    float *rf=malloc(*length*sizeof(float)); assert(rf);
    int half=0, field=0;
    double fm_phase=0.0, previous_ideal_us=0.0;
    /* Inverse of the decoder's linear tape de-emphasis shelf. */
    double tau_zero=pal ? 100e-9 : 120e-9, tau_pole=pal ? 400e-9 : 320e-9;
    double k=2.0*TEST_RATE, denom=1.0+k*tau_zero;
    double b0=(1.0+k*tau_pole)/denom, b1=(1.0-k*tau_pole)/denom;
    double a1=(1.0-k*tau_zero)/denom, previous_base=0.0, previous_pre=0.0;
    for (size_t n=0; n<*length; ++n) {
        double real_us=n/20.0;
        while (half+1<half_lines && timeline[half+1]<=real_us) half++;
        double fraction=(real_us-timeline[half])/(timeline[half+1]-timeline[half]);
        double ideal_us=(half+fraction)*line_us*.5;
        while (field<field_count-1 && ideal_us>=starts[field+1]*line_us) field++;
        double local_us=ideal_us-starts[field]*line_us;
        int line=(int)floor(local_us/line_us);
        double within=local_us-line*line_us;
        int parity=field&1;
        double boundary=pal ? (parity ? 1.0 : .5) : (parity ? .5 : 1.0);
        double eq_begin=boundary*line_us;
        double broad_begin=eq_begin+eq_count*line_us*.5;
        double eq2_begin=broad_begin+eq_count*line_us*.5;
        double vertical_end=eq2_begin+eq_count*line_us*.5;
        bool sync_pulse=false;
        if (local_us<eq_begin) sync_pulse=within<4.7;
        else if (local_us<vertical_end) {
            double section=local_us<broad_begin ? eq_begin : local_us<eq2_begin ? broad_begin : eq2_begin;
            double in_half=fmod(local_us-section,line_us*.5);
            double pulse_width=local_us>=broad_begin && local_us<eq2_begin
                ? (pal ? 27.3 : 27.1) : (pal ? 2.35 : 2.3);
            sync_pulse=in_half<pulse_width;
            if (field==damaged_field && local_us>=broad_begin && local_us<eq2_begin) {
                int broad=(int)((local_us-broad_begin)/(line_us*.5));
                /* Missing first broad pulse and a split second pulse leave
                 * four real broad pulses, but only one spurious preceding EQ.
                 * A valid earlier field must not supply this train's origin. */
                if (broad==0) sync_pulse=false;
                else if (broad==1)
                    sync_pulse=in_half<4.7 || (in_half>=23.0 && in_half<25.3);
            }
            if (jitter_us>0.0 && local_us<broad_begin) {
                double glitch=local_us-(broad_begin-line_us*.5+6.0);
                if (glitch>=0.0 && glitch<1.6) sync_pulse=true;
            }
        } else sync_pulse=within<4.7;
        if (vertical_case!=TEST_V_NORMAL && local_us>=eq_begin && local_us<broad_begin) {
            if (vertical_case==TEST_V_NO_PRE_EQ || vertical_case==TEST_V_SHORT_NO_PRE_EQ) sync_pulse=false;
            if (vertical_case==TEST_V_ONE_PRE_EQ)
                sync_pulse=local_us>=broad_begin-line_us*.5 && sync_pulse;
        }
        if (vertical_case!=TEST_V_NORMAL && local_us>=broad_begin && local_us<eq2_begin) {
            int broad=(int)((local_us-broad_begin)/(line_us*.5));
            double in_half=fmod(local_us-broad_begin,line_us*.5);
            if (broad==0) {
                bool short_first=vertical_case==TEST_V_SPLIT_SHORT_FIRST || vertical_case==TEST_V_SHORT_INTERRUPTED;
                double leading=short_first ? 17.5 : 20.0;
                double tail_start=short_first ? 23.5 : 25.0;
                double tail_end=27.3;
                if (vertical_case>=TEST_V_SPLIT_15P5_FIRST) {
                    bool shorter=vertical_case==TEST_V_SHORT_BELOW_MIN
                        || vertical_case==TEST_V_SHORT_NO_PRE_EQ || vertical_case>=TEST_V_SPLIT_13P5_FIRST;
                    leading=vertical_case==TEST_V_SHORT_BELOW_MIN ? 12.5 : shorter ? 13.5 : 15.5;
                    tail_start=leading+6.0;tail_end=tail_start+(shorter ? 7.8 : 6.0);
                }
                if (vertical_case==TEST_V_PAST_HALF) {
                    tail_start=line_us*.5-1.0; tail_end=line_us*.5+1.3;
                }
                sync_pulse=in_half<leading || (in_half>=tail_start && in_half<tail_end);
            }
            if (vertical_case==TEST_V_ISOLATED_FRAGMENT && broad>0) sync_pulse=false;
            if (vertical_case==TEST_V_BAD_SPACING && broad==1) sync_pulse=false;
            if (vertical_case==TEST_V_SHORT_INTERRUPTED && broad==2) sync_pulse=in_half<4.7;
            if (vertical_case==TEST_V_PAST_HALF && broad==1)
                sync_pulse=in_half>=4.5 && in_half<(pal ? 27.3 : 27.1);
            if (vertical_case==TEST_V_PAST_HALF && local_us>=broad_begin+line_us*.5-1.0
                && local_us<broad_begin+line_us*.5+1.3) sync_pulse=true;
            if (vertical_case==TEST_V_SPLIT_SECOND_H) {
                if (broad==0) sync_pulse=in_half<(pal ? 27.3 : 27.1);
                if (broad==1) sync_pulse=in_half<17.5 || (in_half>=23.5 && in_half<27.3);
            }
            if (vertical_case==TEST_V_SHORT_SECOND_15P5) {
                if (broad==0) sync_pulse=in_half<(pal ? 27.3 : 27.1);
                if (broad==1) sync_pulse=in_half<15.5 || (in_half>=21.5 && in_half<27.5);
            }
            if (vertical_case==TEST_V_SHORT_SECOND_13P5) {
                if (broad==0) sync_pulse=in_half<(pal ? 27.3 : 27.1);
                if (broad==1) sync_pulse=in_half<13.5 || (in_half>=19.5 && in_half<27.3);
            }
        }
        if (vertical_case==TEST_V_NEW_EQ && local_us>=broad_begin
            && local_us<broad_begin+25.0+eq_count*line_us) {
            double new_eq=broad_begin+25.0;
            if (local_us<new_eq) sync_pulse=local_us-broad_begin<20.0;
            else {
                double at=local_us-new_eq;
                double in_half=fmod(at,line_us*.5);
                sync_pulse=in_half<(at<eq_count*line_us*.5 ? (pal ? 2.35 : 2.3)
                                                                        : (pal ? 27.3 : 27.1));
            }
        }
        double r=0,g=0,b=0,u=0,v=0,luminance=0;
        bool active=local_us>=vertical_end && within>=active_start && within<active_end;
        if (active) {
            int bar=(int)((within-active_start)/(active_end-active_start)*8.0);
            if (bar>7) bar=7;
            r=bars[bar][0]; g=bars[bar][1]; b=bars[bar][2];
            /* Distinct genuine field content. If the monitor bobs/duplicates a
             * single field, adjacent rows in the first bar become equal. */
            if (bar==0) r=g=b=parity ? .22 : .82;
            if (quality && (quality->pattern==TEST_Q_GRAY || quality->pattern==TEST_Q_FADE))
                r=g=b=quality_gray(field,quality->pattern);
            if (quality && quality->pattern==TEST_Q_MID_BARS) {
                /* Stay inside RGB gamut even while Y and C are misaligned.
                 * Otherwise RGB clipping can make recovered Y follow C. */
                r=.3+.4*r;g=.3+.4*g;b=.3+.4*b;
            }
            luminance=.299*r+.587*g+.114*b;
            u=.492*(b-luminance); v=.877*(r-luminance);
        }
        double frequency=sync_pulse ? sync : blank+100.0*hz_ire*luminance;
        if (quality) {
            /* Only ordinary-line sync-tip/back-porch windows are corrupted.
             * Active gray/bars, burst and the entire V cadence remain intact.
             * All disturbances modulate real RF; no decoded plane is patched. */
            if (local_us>=vertical_end && line>=20 && quality->window_noise_hz>0.0) {
                int source_line=starts[field]+line;
                double porch_error=quality->window_noise_hz*(sin(source_line*2.0*TEST_PI/7.0)
                    +.4*cos(source_line*2.0*TEST_PI/17.0));
                double tip_error=-.25*quality->window_noise_hz*cos(source_line*2.0*TEST_PI/9.0);
                if (source_line%23==7) {
                    porch_error+=1.6*quality->window_noise_hz;
                    tip_error-=.7*quality->window_noise_hz;
                }
                if (!sync_pulse && within>=8.0 && within<9.1) frequency+=porch_error;
                if (sync_pulse && within>=1.7 && within<3.5) frequency+=tip_error;
            }
            /* Carrier-only offset is intentionally distinct from speed drift:
             * with unchanged H timing a fixed FM profile must not erase it. */
            frequency+=quality->carrier_offset_hz;
        }
        double base=frequency-blank;
        double pre=b0*base+b1*previous_base-a1*previous_pre;
        previous_base=base; previous_pre=pre;
        frequency=clamp_value(blank+pre,2400000.0,6800000.0);
        fm_phase+=2.0*TEST_PI*frequency*(ideal_us-previous_ideal_us)*1e-6;
        previous_ideal_us=ideal_us;
        if (fm_phase>2.0*TEST_PI) fm_phase=fmod(fm_phase,2.0*TEST_PI);
        int vswitch=pal && (line&1) ? -1 : 1;
        if (local_us>=vertical_end && within>=burst_start && within<burst_end) {
            u=pal ? -(150.0/700.0)/sqrt(2.0) : -(150.0/700.0);
            v=pal ? (150.0/700.0)/sqrt(2.0) : 0.0;
        }
        double track_step=pal ? (parity ? -TEST_PI*.5 : 0.0)
                              : (parity ? TEST_PI*.5 : -TEST_PI*.5);
        double chroma_phase=2.0*TEST_PI*carrier*ideal_us*1e-6+track_step*line+.37;
        /* NTSC color-under records U*cos+V*sin: the sum-frequency VHS
         * playback heterodyne conjugates this quadrature. PAL uses a swinging
         * burst with its V switch; do not copy the decoder IQ sign as an oracle. */
        double color=u*cos(chroma_phase)+(pal ? -vswitch : 1)*v*sin(chroma_phase);
        rf[n]=(float)(.72*cos(fm_phase)+(with_chroma ? .20*color : 0.0));
    }
    free(timeline);
    return rf;
}

static float *make_rf_vertical(gui_rf_format_t format, double jitter_us, bool with_chroma,
                              double clock_scale, int damaged_field,
                              test_vertical_t vertical_case, size_t *length) {
    return make_rf_signal(format,jitter_us,with_chroma,clock_scale,damaged_field,
                          vertical_case,NULL,length);
}

static float *make_rf(gui_rf_format_t format, double jitter_us, bool with_chroma,
                      double clock_scale, int damaged_field, size_t *length) {
    return make_rf_vertical(format,jitter_us,with_chroma,clock_scale,damaged_field,TEST_V_NORMAL,length);
}

static void process_fixture(gui_rf_decoder_t *decoder, const float *rf, size_t length,
                            uint64_t offset, test_frames_t *frames) {
    size_t position=0;
    while (position<length) {
        size_t chunk=position&1 ? 32771u : 4093u;
        if (chunk>length-position) chunk=length-position;
        gui_rf_decoder_process(decoder,rf+position,chunk,offset+position,receive_frame,frames);
        position+=chunk;
    }
}

static double plane_mean(const test_frames_t *f, int bar, int parity, int channel) {
    double total=0; int count=0;
    int center=(int)((bar+.5)*f->width/8.0);
    for (int y=120+parity; y<160; y+=2)
        for (int x=center-4; x<=center+4; ++x) {
            total+=f->rgba[((size_t)y*f->width+x)*4u+channel]; count++;
        }
    return total/count/255.0;
}

static void check_bars(const test_frames_t *f, double maximum_error) {
    assert(f->frames>=2);
    assert(f->status.h_locked && f->status.v_locked && f->status.color_locked);
    assert(f->status.fields_accepted==(uint64_t)f->status.fields_decoded);
    assert(f->status.vsync_candidates>=f->status.fields_accepted);
    assert(f->status.fields_rejected==f->status.reject_cadence+
        f->status.reject_parity+f->status.reject_line_map);
    double largest_error=0.0;
    for (int bar=1; bar<8; ++bar) for (int parity=0; parity<2; ++parity)
        for (int c=0; c<3; ++c) {
            double actual=plane_mean(f,bar,parity,c);
            double error=fabs(actual-bars[bar][c]);
            if (error>largest_error) largest_error=error;
            if (error>maximum_error) {
                fprintf(stderr,"bar %d field %d channel %d: %.3f expected %.3f\n",
                        bar,parity,c,actual,bars[bar][c]);
                assert(error<=maximum_error);
            }
        }
    double first=plane_mean(f,0,0,1), second=plane_mean(f,0,1,1);
    fprintf(stdout,"%dx%d: %d frames, bars max error %.3f, woven fields %.3f / %.3f\n",
            f->width,f->height,f->frames,largest_error,first,second);
    assert(first>.65 && second<.38 && first-second>.40);
}

static void check_bar_registration(const test_frames_t *f) {
    /* The independent RF modulator changes Y and color-under together at the
     * red/blue boundary. Their nonzero Y and UV steps make this a physical
     * registration oracle, unlike equal-luminance boundaries or gray alone.
     * Average only rows of one original field; never mix its opposite field. */
    int center=f->width*6/8,radius=f->width/32;
    for (int parity=0; parity<2; ++parity) {
        double previous_y=0.0,previous_u=0.0,previous_v=0.0;
        double y_gradient=0.0,c_gradient=0.0;int y_edge=0,c_edge=0;
        for (int x=center-radius; x<=center+radius; ++x) {
            double rgb[3]={0};int count=0;
            for (int row=120+parity; row<160; row+=2) {
                for (int channel=0; channel<3; ++channel)
                    rgb[channel]+=f->rgba[((size_t)row*f->width+x)*4u+channel]/255.0;
                count++;
            }
            for (int channel=0; channel<3; ++channel) rgb[channel]/=count;
            double y=.299*rgb[0]+.587*rgb[1]+.114*rgb[2];
            double u=.492*(rgb[2]-y),v=.877*(rgb[0]-y);
            if (x>center-radius) {
                double dy=fabs(y-previous_y),dc=hypot(u-previous_u,v-previous_v);
                if (dy>y_gradient) { y_gradient=dy;y_edge=x; }
                if (dc>c_gradient) { c_gradient=dc;c_edge=x; }
            }
            previous_y=y;previous_u=u;previous_v=v;
        }
        printf("%dx%d field %d red/blue registration: Y %d C %d, separation %d pixels (steps %.5f/%.5f)\n",
               f->width,f->height,parity,y_edge,c_edge,abs(y_edge-c_edge),y_gradient,c_gradient);
        fflush(stdout);
        /* PAL's existing first-order/chroma path has a <=7px footprint at
         * 768px width. NTSC's 640px path must remain within <=5px. This tests
         * functional registration, not universal tape pre-emphasis phase. */
        int maximum_separation=f->height==576 ? 7 : 5;
        assert(y_gradient>.005 && c_gradient>.01 && abs(y_edge-c_edge)<=maximum_separation);
    }
}

static void run_registration_contract(void) {
    for (int format=GUI_RF_PAL; format<=GUI_RF_NTSC; ++format) {
        gui_rf_decoder_t *decoder=gui_rf_decoder_create((gui_rf_format_t)format);assert(decoder);
        test_frames_t frames={0};
        frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
        test_quality_t signal={.pattern=TEST_Q_MID_BARS,.fields=6};
        size_t length;float *rf=make_rf_signal((gui_rf_format_t)format,0.0,true,1.0,-1,
                                               TEST_V_NORMAL,&signal,&length);
        process_fixture(decoder,rf,length,0,&frames);
        assert(frames.frames>=2 && frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
        check_bar_registration(&frames);
        free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
    }
}

/* Evaluate the returned transfer, without repeating its coefficient design.
 * These targets are the published physical shelf plateaus and midpoint. */
static double transfer_magnitude(const float c[5],double omega) {
    double nr=c[0]+c[1]*cos(omega)+c[2]*cos(2.0*omega);
    double ni=-c[1]*sin(omega)-c[2]*sin(2.0*omega);
    double dr=1.0+c[3]*cos(omega)+c[4]*cos(2.0*omega);
    double di=-c[3]*sin(omega)-c[4]*sin(2.0*omega);
    return sqrt((nr*nr+ni*ni)/(dr*dr+di*di));
}

static void run_deemphasis_contract(void) {
    float c[5];
    assert(gui_rf_ntsc_deemph_coefficients(5000000.0,c,c+1,c+2,c+3,c+4));
    double dc=transfer_magnitude(c,0.0);
    double mid=transfer_magnitude(c,2.0*TEST_PI*273755.82/5000000.0);
    double high=transfer_magnitude(c,TEST_PI);
    printf("NTSC shelf: DC %.9f, midpoint %.9f, Nyquist %.9f\n",dc,mid,high);
    assert(fabs(dc-1.0)<3e-6);
    assert(fabs(mid-pow(10.0,-13.9794/40.0))<3e-6);
    assert(fabs(high-pow(10.0,-13.9794/20.0))<3e-6);
    const double invalid[]={0.0,-1.0,547511.64,80000001.0,INFINITY,NAN};
    const float sentinel[5]={17.0f,-23.0f,31.0f,-43.0f,59.0f};
    for (size_t n=0; n<sizeof(invalid)/sizeof(invalid[0]); ++n) {
        memcpy(c,sentinel,sizeof(c));
        assert(!gui_rf_ntsc_deemph_coefficients(invalid[n],c,c+1,c+2,c+3,c+4));
        assert(memcmp(c,sentinel,sizeof(c))==0);
    }
    for (int missing=0; missing<5; ++missing) {
        float *out[5]={c,c+1,c+2,c+3,c+4};out[missing]=NULL;
        memcpy(c,sentinel,sizeof(c));
        assert(!gui_rf_ntsc_deemph_coefficients(5000000.0,out[0],out[1],out[2],out[3],out[4]));
        assert(memcmp(c,sentinel,sizeof(c))==0);
    }
}

typedef struct { double mean,row_rms,maximum_error; } quality_rows_t;

static quality_rows_t quality_rows(const test_frames_t *f,int parity,int bar,
                                   int channel,double expected) {
    double sum=0.0,sum_square=0.0,maximum_error=0.0;int count=0;
    int x0=f->width*2/5,x1=f->width*3/5;
    if (bar>=0) { int center=(int)((bar+.5)*f->width/8.0);x0=center-4;x1=center+5; }
    for (int y=40+parity; y<f->height-40; y+=2) {
        double row=0.0;
        for (int x=x0; x<x1; ++x) row+=f->rgba[((size_t)y*f->width+x)*4u+channel];
        row/=(x1-x0)*255.0;
        sum+=row;sum_square+=row*row;count++;
        maximum_error=fmax(maximum_error,fabs(row-expected));
    }
    assert(count>100);
    double mean=sum/count;
    quality_rows_t rows={mean,sqrt(fmax(0.0,sum_square/count-mean*mean)),maximum_error};
    return rows;
}

static void check_quality_gray(const test_frames_t *f,const char *name,
                               double offset,double mean_limit,double row_limit,
                               double outlier_limit) {
    assert(f->frames>=2 && f->status.h_locked && f->status.v_locked);
    printf("Quality cadence: %d frames, %llu accepted, %llu rejected (cad %llu/par %llu/map %llu)\n",
           f->frames,(unsigned long long)f->status.fields_accepted,
           (unsigned long long)f->status.fields_rejected,(unsigned long long)f->status.reject_cadence,
           (unsigned long long)f->status.reject_parity,(unsigned long long)f->status.reject_line_map);
    fflush(stdout);
    for (int parity=0; parity<2; ++parity) {
        double expected=quality_gray(parity,TEST_Q_GRAY)+offset;
        quality_rows_t rows=quality_rows(f,parity,-1,1,expected);
        printf("%s field %d: gray %.6f expected %.6f, row RMS %.6f, max error %.6f\n",
               name,parity,rows.mean,expected,rows.row_rms,rows.maximum_error);
        fflush(stdout);
        assert(fabs(rows.mean-expected)<=mean_limit);
        assert(rows.row_rms<=row_limit && rows.maximum_error<=outlier_limit);
    }
    /* Adjacent rows still contain two different original fields. */
    double first=quality_rows(f,0,-1,1,.65+offset).mean;
    double second=quality_rows(f,1,-1,1,.25+offset).mean;
    assert(first-second>.35);
}

typedef struct {
    test_frames_t frame;
    const test_quality_t *signal;
    int checked,black_planes;
    double maximum_mean_error,maximum_row_rms,maximum_outlier,maximum_black;
} test_fade_frames_t;

static void receive_fade_frame(void *context,const uint8_t *rgba,int width,int height,
                               uint64_t end_sample,const gui_rf_decode_status_t *status) {
    test_fade_frames_t *f=context;
    receive_frame(&f->frame,rgba,width,height,end_sample,status);
    if (f->frame.frames<2) return;
    int next=0;
    while (next<f->signal->fields && f->signal->field_start_sample[next+1]<=end_sample) next++;
    assert(next>=3 && next<f->signal->fields);
    for (int parity=0; parity<2; ++parity) {
        int source_field=next-1;
        if ((source_field&1)!=parity) source_field--;
        double expected=quality_gray(source_field,TEST_Q_FADE);
        quality_rows_t rows=quality_rows(&f->frame,parity,-1,1,expected);
        f->maximum_mean_error=fmax(f->maximum_mean_error,fabs(rows.mean-expected));
        f->maximum_row_rms=fmax(f->maximum_row_rms,rows.row_rms);
        f->maximum_outlier=fmax(f->maximum_outlier,rows.maximum_error);
        if (expected<.001) {
            f->maximum_black=fmax(f->maximum_black,rows.mean);f->black_planes++;
        }
        f->checked++;
    }
}

static void run_quality_format(gui_rf_format_t format) {
    const char *system=format==GUI_RF_PAL ? "PAL" : "NTSC";
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format);assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    test_quality_t signal={.pattern=TEST_Q_GRAY,.fields=8,.window_noise_hz=60000.0};
    size_t length;float *rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    process_fixture(decoder,rf,length,0,&frames);
    printf("%s noisy porch/sync RF\n",system);
    check_quality_gray(&frames,"noisy windows",0.0,.03,.012,.045);
    free(rf);

    signal.pattern=TEST_Q_COLOR_BARS;signal.window_noise_hz=0.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);check_bars(&frames,.08);
    size_t pixels=(size_t)frames.width*frames.height*4u;
    uint8_t *clean_bars=malloc(pixels);assert(clean_bars);memcpy(clean_bars,frames.rgba,pixels);
    free(rf);signal.window_noise_hz=60000.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);check_bars(&frames,.08);
    double largest_row_rms=0.0,largest_window_rms=0.0,largest_window_error=0.0;
    for (int bar=1; bar<8; ++bar) for (int parity=0; parity<2; ++parity)
        for (int c=0; c<3; ++c) {
            quality_rows_t rows=quality_rows(&frames,parity,bar,c,bars[bar][c]);
            largest_row_rms=fmax(largest_row_rms,rows.row_rms);
            int center=(int)((bar+.5)*frames.width/8.0),count=0;double sum_square=0.0;
            for (int y=40+parity; y<frames.height-40; y+=2) {
                double delta=0.0;
                for (int x=center-4; x<=center+4; ++x) {
                    size_t n=((size_t)y*frames.width+x)*4u+c;
                    delta+=(int)frames.rgba[n]-(int)clean_bars[n];
                }
                delta/=9.0*255.0;sum_square+=delta*delta;count++;
                largest_window_error=fmax(largest_window_error,fabs(delta));
            }
            largest_window_rms=fmax(largest_window_rms,sqrt(sum_square/count));
        }
    /* Color-under already has line/track texture. The independent paired
     * control changes only porch/tip RF, so its effect on active colors is
     * bounded separately from the clean fixture's chroma reconstruction. */
    printf("%s noisy color bars: row RMS %.6f, window-only RMS %.6f, max delta %.6f\n",
           system,largest_row_rms,largest_window_rms,largest_window_error);fflush(stdout);
    assert(largest_window_rms<=.012 && largest_window_error<=.045);
    free(clean_bars);free(rf);

    signal.pattern=TEST_Q_GRAY;signal.window_noise_hz=0.0;signal.carrier_offset_hz=35000.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);
    double hz_ire=format==GUI_RF_PAL ? 7000.0 : 7142.857142857143;
    check_quality_gray(&frames,"carrier-only +35kHz",35000.0/(100.0*hz_ire),.03,.012,.045);
    free(rf);

    /* Keep the fixture inside the existing nominal +/-2-line V admission:
     * PAL's 313-line field needs a smaller margin than NTSC's 263-line field.
     * Larger speed excursions are not a supported-preview claim here. */
    signal.carrier_offset_hz=0.0;signal.speed_wander=format==GUI_RF_PAL ? .004 : .006;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);frames.end_sample=0;frames.frames=0;
    process_fixture(decoder,rf,length,0,&frames);
    check_quality_gray(&frames,format==GUI_RF_PAL ? "physical slow speed +/-0.4%"
                                                : "physical slow speed +/-0.6%",0.0,.06,.020,.075);
    assert(frames.status.reject_cadence==0 && frames.status.reject_line_map==0);
    free(rf);

    signal.pattern=TEST_Q_FADE;signal.fields=10;signal.speed_wander=0.0;signal.window_noise_hz=60000.0;
    rf=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,&signal,&length);
    gui_rf_decoder_reset(decoder,format);
    test_fade_frames_t fade={.frame={.rgba=frames.rgba},.signal=&signal};
    size_t position=0;
    while (position<length) {
        size_t chunk=4093u;if (chunk>length-position) chunk=length-position;
        gui_rf_decoder_process(decoder,rf+position,chunk,position,receive_fade_frame,&fade);
        position+=chunk;
    }
    printf("%s physical fade: %d field planes, max gray error %.6f, row RMS %.6f, outlier %.6f, black %.6f\n",
           system,fade.checked,fade.maximum_mean_error,fade.maximum_row_rms,
           fade.maximum_outlier,fade.maximum_black);fflush(stdout);
    assert(fade.checked>=6 && fade.black_planes>=2);
    assert(fade.maximum_mean_error<=.03 && fade.maximum_row_rms<=.012);
    assert(fade.maximum_outlier<=.045 && fade.maximum_black<=.015);
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_quality_contracts(void) {
    run_quality_format(GUI_RF_PAL);run_quality_format(GUI_RF_NTSC);
}

/* Reuse actual FM ordinary lines, without calling private pulse/parser APIs.
 * A long carrier-only interval removes H while preserving source continuity.
 * One isolated H after that loss must not inherit the previous lock's trust. */
static void check_h_reacquisition(gui_rf_decoder_t *decoder, gui_rf_format_t format,
                                  const float *rf, size_t length, test_frames_t *frames) {
    double line_us=format==GUI_RF_PAL ? 64.0 : 63.55555555555556;
    double blank_hz=format==GUI_RF_PAL ? 4100000.0 : 3685714.2857142854;
    size_t blank_count=(size_t)ceil(60.0*line_us*20.0);
    float *blank_rf=malloc(blank_count*sizeof(*blank_rf));assert(blank_rf);
    for (size_t n=0; n<blank_count; ++n)
        blank_rf[n]=(float)(.72*cos(2.0*TEST_PI*blank_hz*n/TEST_RATE));
    uint64_t offset=length;
    process_fixture(decoder,blank_rf,blank_count,offset,frames);offset+=blank_count;
    gui_rf_decode_status_t lost, isolated, recovered;
    gui_rf_decoder_get_status(decoder,&lost);
    assert(!lost.h_locked && !lost.v_locked && !lost.color_locked);

    size_t ordinary=(size_t)floor(50.0*line_us*20.0);
    size_t one_line=(size_t)ceil(line_us*20.0);
    size_t settling=(size_t)ceil(5.0*line_us*20.0);
    assert(ordinary+one_line<length && settling<blank_count);
    process_fixture(decoder,rf+ordinary,one_line,offset,frames);offset+=one_line;
    process_fixture(decoder,blank_rf,settling,offset,frames);offset+=settling;
    gui_rf_decoder_get_status(decoder,&isolated);
    assert(isolated.hsync_count==lost.hsync_count+1);
    assert(!isolated.h_locked && !isolated.v_locked);
    assert(isolated.frames_decoded==lost.frames_decoded);

    size_t train=(size_t)floor(30.0*line_us*20.0);
    assert(ordinary+train<length);
    process_fixture(decoder,rf+ordinary,train,offset,frames);
    gui_rf_decoder_get_status(decoder,&recovered);
    assert(recovered.hsync_count>=isolated.hsync_count+20);
    assert(recovered.h_locked && !recovered.v_locked);
    assert(recovered.frames_decoded==lost.frames_decoded);
    printf("%s H loss discarded trust; isolated H stayed unlocked; regular H reacquired\n",
           format==GUI_RF_PAL ? "PAL" : "NTSC");
    free(blank_rf);
}

/* A lock can learn a carrier offset during unstable playback. Its old sync
 * threshold must not prevent a later healthy recording from ever supplying H.
 * Exercise physical RF only, with continuous sample positions and no reset. */
static void run_stop_resume_format(gui_rf_format_t format, int direction) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format);assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    size_t length;float *rf=make_rf(format,0.0,true,1.0,-1,&length);
    uint64_t offset=0;
    process_fixture(decoder,rf,length,offset,&frames);offset+=length;
    check_bars(&frames,.08);
    for (int step=1; step<=10; ++step) {
        test_quality_t quality={.pattern=TEST_Q_COLOR_BARS,.fields=6,
            .carrier_offset_hz=direction*step*25000.0};
        size_t shifted_length;
        float *shifted=make_rf_signal(format,0.0,true,1.0,-1,TEST_V_NORMAL,
                                      &quality,&shifted_length);
        process_fixture(decoder,shifted,shifted_length,offset,&frames);
        offset+=shifted_length;free(shifted);
    }
    gui_rf_decode_status_t before;
    gui_rf_decoder_get_status(decoder,&before);
    assert(before.h_locked && before.v_locked);
    float silence[8192]={0};
    for (int cycle=0; cycle<2; ++cycle) {
        /* The second stop follows a successful automatic reacquisition. */
        size_t gap=(size_t)(TEST_RATE*(cycle ? .5 : .1));
        while (gap) {
            size_t chunk=gap<8192u ? gap : 8192u;
            gui_rf_decoder_process(decoder,silence,chunk,offset,receive_frame,&frames);
            offset+=chunk;gap-=chunk;
        }
        gui_rf_decode_status_t lost,recovered;
        gui_rf_decoder_get_status(decoder,&lost);
        assert(!lost.h_locked && !lost.v_locked && !lost.color_locked);
        assert(lost.frames_decoded>=before.frames_decoded);
        assert(lost.fields_accepted>=before.fields_accepted);
        assert(lost.vsync_candidates>=before.vsync_candidates);
        assert(lost.color_losses==before.color_losses+1u);
        int old_frames=frames.frames;
        uint64_t resume_start=offset;
        process_fixture(decoder,rf,length,offset,&frames);offset+=length;
        gui_rf_decoder_get_status(decoder,&recovered);
        printf("%s carrier offset %+d Hz, stop/resume %d: new frames %d, H/V/color %d/%d/%d\n",
               format==GUI_RF_PAL ? "PAL" : "NTSC",direction*250000,cycle+1,
               frames.frames-old_frames,recovered.h_locked,recovered.v_locked,
               recovered.color_locked);fflush(stdout);
        assert(frames.frames>=old_frames+2);
        assert(recovered.h_locked && recovered.v_locked && recovered.color_locked);
        assert(recovered.hsync_count>lost.hsync_count && recovered.vsync_count>lost.vsync_count);
        assert(recovered.fields_accepted>lost.fields_accepted);
        assert(recovered.color_losses==lost.color_losses);
        assert(frames.end_sample>=resume_start && frames.end_sample<=offset);
        check_bars(&frames,.08);
        before=recovered;
    }
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_stop_resume(void) {
    run_stop_resume_format(GUI_RF_PAL,-1);run_stop_resume_format(GUI_RF_PAL,1);
    run_stop_resume_format(GUI_RF_NTSC,-1);run_stop_resume_format(GUI_RF_NTSC,1);
}

static void run_format(gui_rf_format_t format) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u); assert(frames.rgba);
    size_t length;
    float *rf=make_rf(format,0.0,true,1.0,-1,&length);
    process_fixture(decoder,rf,length,0,&frames);
    check_bars(&frames,.08);
    /* The same RF at the MISRC 12-bit ADC amplitude must have the same AGC,
     * color gating, and rendered colors as left-aligned 16-bit DdD RF. */
    size_t pixels=(size_t)frames.width*frames.height*4u;
    uint8_t *reference=malloc(pixels); assert(reference);
    memcpy(reference,frames.rgba,pixels);
    check_h_reacquisition(decoder,format,rf,length,&frames);
    for (size_t n=0; n<length; ++n) rf[n]*=1.0f/16.0f;
    /* Process the other tape system between reference and replay. Returning
     * through reset must discard both its filter state and its field map. */
    gui_rf_format_t other=format==GUI_RF_PAL ? GUI_RF_NTSC : GUI_RF_PAL;
    size_t other_length;float *other_rf=make_rf(other,0.0,true,1.0,-1,&other_length);
    gui_rf_decoder_reset(decoder,other);frames.end_sample=0;
    process_fixture(decoder,other_rf,other_length,0,&frames);check_bars(&frames,.08);
    free(other_rf);
    gui_rf_decoder_reset(decoder,format); frames.end_sample=0;
    process_fixture(decoder,rf,length,0,&frames);
    check_bars(&frames,.08);
    int maximum_difference=0; double total_difference=0.0;
    for (size_t n=0; n<pixels; ++n) {
        int difference=abs((int)frames.rgba[n]-(int)reference[n]);
        if (difference>maximum_difference) maximum_difference=difference;
        total_difference+=difference;
    }
    printf("ADC amplitude invariance: max %d, mean %.5f / 255\n",
           maximum_difference,total_difference/pixels);
    assert(maximum_difference<=2 && total_difference/pixels<.1);
    free(reference);
    free(rf);
    /* A discontinuity must discard both partial fields and all DSP history. */
    float silence[4096]={0};
    gui_rf_decoder_process(decoder,silence,4096,UINT64_C(100000003),receive_frame,&frames);
    gui_rf_decode_status_t status;
    gui_rf_decoder_get_status(decoder,&status);
    assert(!status.h_locked && !status.v_locked && !status.color_locked);
    assert(status.frames_decoded==0);
    assert(status.vsync_candidates==0 && status.fields_accepted==0 && status.fields_rejected==0);
    assert(status.color_losses==0 && status.last_reject_reason[0]=='\0');
    int before=frames.frames;
    rf=make_rf(format,.18,true,1.002,-1,&length);
    process_fixture(decoder,rf,length,UINT64_C(100004099),&frames);
    assert(frames.frames>=before+2);
    check_bars(&frames,.10);
    assert(frames.end_sample>UINT64_C(100004099));
    uint64_t continuous_offset=UINT64_C(100004099)+length;
    free(rf);
    rf=make_rf(format,0.0,false,1.0,-1,&length);
    int color_frames=frames.frames;
    process_fixture(decoder,rf,length,continuous_offset,&frames);
    assert(frames.frames>=color_frames+2);
    assert(frames.status.h_locked && frames.status.v_locked && !frames.status.color_locked);
    assert(frames.status.color_losses>0 && frames.status.last_color_loss_reason[0]!='\0');
    assert(frames.status.fields_accepted==(uint64_t)frames.status.fields_decoded);
    assert(frames.status.fields_rejected==frames.status.reject_cadence+
        frames.status.reject_parity+frames.status.reject_line_map);
    for (int bar=0; bar<8; ++bar) {
        double r=plane_mean(&frames,bar,0,0), g=plane_mean(&frames,bar,0,1), b=plane_mean(&frames,bar,0,2);
        assert(fabs(r-g)<.01 && fabs(r-b)<.01);
    }
    free(rf); free(frames.rgba); gui_rf_decoder_destroy(decoder);
}

static void run_damaged_vertical(void) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(GUI_RF_NTSC); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    size_t length;
    float *rf=make_rf(GUI_RF_NTSC,0.0,true,1.0,2,&length);
    /* Stop after field 2's malformed vertical interval, before field 3 can
     * reacquire. The broad train remains observable as a source candidate. */
    size_t cutoff=(size_t)((525.0+12.0)*63.55555555555556*20.0);
    assert(cutoff<length);
    process_fixture(decoder,rf,cutoff,0,&frames);
    gui_rf_decode_status_t status;
    gui_rf_decoder_get_status(decoder,&status);
    printf("Damaged NTSC vertical: %llu validated boundaries, %llu source candidates\n",
           (unsigned long long)status.vsync_count,(unsigned long long)status.vsync_candidates);
    /* Acquisition starts at field 1: field 0 lacks a settled preceding H. */
    assert(status.vsync_count==1 && status.vsync_candidates==3);
    assert(status.reject_cadence==1 && status.reject_parity==1
           && status.fields_rejected==2);
    assert(status.last_reject_reason[0]!='\0');
    free(rf); free(frames.rgba); gui_rf_decoder_destroy(decoder);
}

static void run_split_vertical(gui_rf_format_t format, test_vertical_t vertical_case,
                               bool should_decode) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);assert(frames.rgba);
    size_t length;
    float *rf=make_rf_vertical(format,0.0,true,1.0,-1,vertical_case,&length);
    process_fixture(decoder,rf,length,UINT64_C(6600000000),&frames);
    gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
    printf("%s vertical case %d: %d frames, %llu fields, %llu candidates, %llu rejected (cad %llu/par %llu/map %llu), H %llu\n",
           format==GUI_RF_PAL ? "PAL" : "NTSC",(int)vertical_case,frames.frames,
           (unsigned long long)status.fields_accepted,(unsigned long long)status.vsync_candidates,
           (unsigned long long)status.fields_rejected,(unsigned long long)status.reject_cadence,
           (unsigned long long)status.reject_parity,(unsigned long long)status.reject_line_map,
           (unsigned long long)status.hsync_count);
    fflush(stdout);
    if (should_decode) check_bars(&frames,.10);
    else assert(frames.frames==0 && status.fields_accepted==0 && status.vsync_count==0);
    /* The immutable pre-fix core counts these same physical H pulses. The
     * deferred H tail and intervening H must both be replayed on failure. */
    if (vertical_case==TEST_V_SHORT_INTERRUPTED)
        assert(status.hsync_count==(format==GUI_RF_PAL ? 1841 : 1532));
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

static void run_split_vertical_cases(void) {
    for (int format=GUI_RF_PAL; format<=GUI_RF_NTSC; ++format) {
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_FIRST,true);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_SHORT_FIRST,true);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_15P5_FIRST,true);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SPLIT_13P5_FIRST,true);
        for (int shape=TEST_V_NEW_EQ; shape<=TEST_V_SPLIT_SECOND_H; ++shape)
            run_split_vertical((gui_rf_format_t)format,(test_vertical_t)shape,false);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SHORT_INTERRUPTED,false);
        for (int shape=TEST_V_SHORT_BELOW_MIN; shape<=TEST_V_SHORT_SECOND_15P5; ++shape)
            run_split_vertical((gui_rf_format_t)format,(test_vertical_t)shape,false);
        run_split_vertical((gui_rf_format_t)format,TEST_V_SHORT_SECOND_13P5,false);
    }
}

static void run_split_gap(gui_rf_format_t format) {
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(format);assert(decoder);
    test_frames_t frames={0};frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);size_t length;
    float *rf=make_rf_vertical(format,0.0,true,1.0,-1,TEST_V_SPLIT_SHORT_FIRST,&length);
    bool pal=format==GUI_RF_PAL;double line_us=pal ? 64.0 : 63.55555555555556;
    double broad_us=((pal ? 312.0 : 263.0)+(pal ? 1.0 : .5)+(pal ? 5.0 : 6.0)*.5)*line_us;
    size_t first=(size_t)((broad_us+31.0)*20.0);
    size_t last=(size_t)((broad_us+(pal ? 5.0 : 6.0)*line_us*.5+10.0)*20.0);
    assert(first<last && last<length);uint64_t origin=UINT64_C(6600000000);
    process_fixture(decoder,rf,first,origin,&frames);
    gui_rf_decode_status_t status;gui_rf_decoder_get_status(decoder,&status);
    assert(status.fields_accepted==0);
    /* The discontinuity occurs after both first-slot fragments, before the
     * complete confirming broad pulses. Those pulses cannot reuse its EQ. */
    process_fixture(decoder,rf+first,last-first,origin+first+200000,&frames);
    gui_rf_decoder_get_status(decoder,&status);
    assert(status.fields_accepted==0 && status.vsync_count==0 && frames.frames==0);
    process_fixture(decoder,rf,length,origin+last+200000,&frames);
    check_bars(&frames,.10);
    printf("%s split-pending discontinuity discarded origin and reacquired\n",pal ? "PAL":"NTSC");
    free(rf);free(frames.rgba);gui_rf_decoder_destroy(decoder);
}

/* Optional external-media regression for the exact 20 MHz mono s16le excerpt
 * of DanDanYouQing-DHX2.ddd.flac, source seconds 5..7. Its SMPTE bars are an
 * independent polarity oracle: an inverted V can pass a synthetic fixture
 * whose modulator shares that sign error, but turns cyan/magenta/red/green
 * into the wrong hues here. This is a hue/cadence check, not archive parity.
 * SHA256: A054AC393B3CCDD69BE1B3A6E3ED8A32A390DEBD186D40A4FB89C36FA53283D2 */
static void run_real_ntsc_bars(const char *path) {
    FILE *input=fopen(path,"rb"); assert(input);
    gui_rf_decoder_t *decoder=gui_rf_decoder_create(GUI_RF_NTSC); assert(decoder);
    test_frames_t frames={0};
    frames.rgba=malloc((size_t)GUI_RF_FRAME_MAX_WIDTH*GUI_RF_FRAME_MAX_HEIGHT*4u);
    assert(frames.rgba);
    uint8_t packed[65536]; float rf[32768];
    uint64_t position=0; size_t bytes;
    while ((bytes=fread(packed,1,sizeof(packed),input))!=0) {
        assert((bytes&1u)==0);
        size_t count=bytes/2u;
        for (size_t n=0; n<count; ++n) {
            unsigned word=(unsigned)packed[n*2u]|((unsigned)packed[n*2u+1u]<<8);
            int sample=word<32768u ? (int)word : (int)word-65536;
            rf[n]=sample/32768.0f;
        }
        gui_rf_decoder_process(decoder,rf,count,position,receive_frame,&frames);
        position+=count;
    }
    assert(!ferror(input)); fclose(input);
    assert(position==UINT64_C(40000000));
    assert(frames.width==640 && frames.height==480 && frames.frames>=55);
    assert(frames.status.h_locked && frames.status.v_locked && frames.status.color_locked);
    assert(frames.status.fields_accepted==(uint64_t)frames.status.fields_decoded);
    assert(frames.status.fields_rejected==frames.status.reject_cadence+
        frames.status.reject_parity+frames.status.reject_line_map);
    for (int bar=1; bar<7; ++bar) for (int parity=0; parity<2; ++parity) {
        double minimum_on=1.0, maximum_off=0.0;
        for (int c=0; c<3; ++c) {
            double value=plane_mean(&frames,bar,parity,c);
            if (bars[bar][c]>0.5) minimum_on=fmin(minimum_on,value);
            else maximum_off=fmax(maximum_off,value);
        }
        if (minimum_on-maximum_off<=.20)
            fprintf(stderr,"real NTSC bar %d field %d has wrong hue: %.3f vs %.3f\n",
                    bar,parity,minimum_on,maximum_off);
        assert(minimum_on-maximum_off>.20);
    }
    printf("Real NTSC RF bars: %d woven frames, all sync/color locks, correct hue order\n",
           frames.frames);
    free(frames.rgba); gui_rf_decoder_destroy(decoder);
}

int main(int argc, char **argv) {
    if (argc==2 && strcmp(argv[1],"--stop-resume-only")==0) {
        run_stop_resume();puts("Continuous RF stop/resume contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--registration-only")==0) {
        run_registration_contract();puts("Physical RF luma/chroma registration contract passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--quality-only")==0) {
        run_quality_contracts();puts("Physical RF gray, row stability, frequency and fade contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--deemphasis-only")==0) {
        run_deemphasis_contract();puts("NTSC physical de-emphasis transfer contract passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-vertical-only")==0) {
        run_split_vertical_cases();puts("Split vertical positive/negative contracts passed");return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-first-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-short-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_SHORT_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-gap-negative-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_BAD_SPACING,false);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-h-replay-only")==0) {
        run_split_vertical(GUI_RF_PAL,TEST_V_SHORT_INTERRUPTED,false);
        run_split_vertical(GUI_RF_NTSC,TEST_V_SHORT_INTERRUPTED,false);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-15p5-only")==0) {
        run_split_vertical(GUI_RF_PAL,TEST_V_SPLIT_15P5_FIRST,true);
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_15P5_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-boundary-ntsc-only")==0) {
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_15P5_FIRST,true);return 0;
    }
    if (argc==2 && strcmp(argv[1],"--split-13p5-only")==0) {
        run_split_vertical(GUI_RF_PAL,TEST_V_SPLIT_13P5_FIRST,true);
        run_split_vertical(GUI_RF_NTSC,TEST_V_SPLIT_13P5_FIRST,true);return 0;
    }
    if (argc!=1 && (argc!=3 || strcmp(argv[1],"--real-ntsc-bars")!=0)) {
        fprintf(stderr,"usage: %s [--real-ntsc-bars excerpt.s16le]\n",argv[0]);
        return 2;
    }
    run_deemphasis_contract();
    run_registration_contract();
    run_quality_contracts();
    run_stop_resume();
    run_format(GUI_RF_PAL);
    run_format(GUI_RF_NTSC);
    run_damaged_vertical();
    run_split_vertical_cases();
    run_split_gap(GUI_RF_PAL);
    run_split_gap(GUI_RF_NTSC);
    if (argc==3) run_real_ntsc_bars(argv[2]);
    puts("RF preview PAL/NTSC color, TBC, weave and gap contracts passed");
    return 0;
}
