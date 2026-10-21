/* Physical four-carrier FM fixtures; no decoded PCM is fed to the decoder.
 * These establish streaming contracts, not tape/hardware listening quality. */
#include "../misrc_gui/signal/gui_beta_hifi.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef NDEBUG
#error "gui_beta_hifi_test requires enabled assertions; remove NDEBUG"
#endif

#define TEST_PI 3.14159265358979323846
#define TEST_RATE 20000000u
#define TEST_FRAMES 48000u

typedef struct {
    float stereo[TEST_FRAMES*2];
    size_t frames;
    uint64_t end_sample;
    double processing_seconds;
    bool saw_lock;
    gui_beta_hifi_status_t status;
} audio_capture_t;

typedef struct {
    double re[4],im[4],step_re[4],step_im[4];
    uint64_t next;
} rf_source_t;

static gui_beta_hifi_iq_kernel_t requested_kernel=GUI_BETA_HIFI_IQ_AUTO;

static gui_beta_hifi_decoder_t *create_selected_decoder(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();
    assert(d && gui_beta_hifi_set_iq_kernel(d,requested_kernel));
    return d;
}

static void capture_pcm(void *context,const float *stereo,size_t frames,
                         uint64_t end_sample,const gui_beta_hifi_status_t *status) {
    audio_capture_t *audio=context;
    assert(end_sample>=audio->end_sample);
    assert(audio->frames+frames<=TEST_FRAMES);
    for(size_t i=0;i<frames*2;i++) {
        uint32_t bits;memcpy(&bits,stereo+i,sizeof(bits));
        assert((bits&UINT32_C(0x7f800000))!=UINT32_C(0x7f800000));
        assert(fabs(stereo[i])<=.981);
    }
    memcpy(audio->stereo+audio->frames*2,stereo,frames*2*sizeof(float));
    audio->frames+=frames;audio->end_sample=end_sample;audio->status=*status;
    audio->saw_lock |= status->locked;
}

static void source_reset(rf_source_t *source) {
    memset(source,0,sizeof(*source));
    for(int i=0;i<4;i++) source->re[i]=1;
}

/* mode 0: true alternating pairs, 1: no head alternation, 2: no right pair,
 * 3: missing RF for 100 ms after initial lock; 4/5: unequal head response puts
 * both dominance states above/below zero, plus unequal left/right response.
 * Stereo programmes are different
 * 1 kHz/1.7 kHz tones with 20 kHz deviation and no programme DC cancellation. */
static void source_fill(rf_source_t *source,float *rf,size_t count,int mode) {
    const double multiples[4]={87.75,97.25,106.75,116.25};
    for(size_t i=0;i<count;i++,source->next++) {
        uint64_t n=source->next;
        if(n%40==0) {
            double t=(double)n/TEST_RATE;
            for(int branch=0;branch<4;branch++) {
                double programme=branch<2 ? 1000 : 1700;
                double frequency=multiples[branch]*(15750000.0/1001)
                                    +20000*sin(2*TEST_PI*programme*t);
                double phase=2*TEST_PI*frequency/TEST_RATE;
                source->step_re[branch]=cos(phase);
                source->step_im[branch]=sin(phase);
            }
        }
        bool head_a=((n*60000ull/(TEST_RATE*1001ull))&1)==0;
        if(mode==1) head_a=true;
        if(mode==6 && n<TEST_RATE/5) head_a=((n/(TEST_RATE/500))&1)==0;
        double value=0;
        for(int branch=0;branch<4;branch++) {
            bool active=head_a==((branch&1)==0);
            double amplitude=active ? .19 : .0019;
            if(mode==2 && branch>=2) amplitude=0;
            if(mode==4 || mode==5) {
                bool attenuated=mode==4 ? (branch&1)!=0 : (branch&1)==0;
                if(attenuated) amplitude/=150;
                if(branch>=2) amplitude*=.3;
            }
            if(mode==7 && (branch&1)!=0) amplitude=head_a ? .05 : .016;
            value += amplitude*source->re[branch];
            double next_re=source->re[branch]*source->step_re[branch]
                            -source->im[branch]*source->step_im[branch];
            source->im[branch]=source->im[branch]*source->step_re[branch]
                                +source->re[branch]*source->step_im[branch];
            source->re[branch]=next_re;
        }
        if(mode==3 && n>=TEST_RATE*35ull/100 && n<TEST_RATE*45ull/100) value=0;
        if(mode==6 && n<TEST_RATE/5) {
            uint32_t noise=(uint32_t)n*1664525u+1013904223u;
            noise ^= noise>>16;
            value+=.02*((double)(noise&65535)/32768-1);
        }
        rf[i]=(float)value;
    }
}

static void run_fixture_kernel(audio_capture_t *audio,size_t count,size_t chunk,int mode,
                                 gui_beta_hifi_iq_kernel_t kernel) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();
    assert(d && gui_beta_hifi_set_iq_kernel(d,kernel));
    rf_source_t source;source_reset(&source);
    float *buffer=malloc(chunk*sizeof(float));assert(buffer);
    for(size_t position=0;position<count;) {
        size_t length=count-position<chunk ? count-position : chunk;
        source_fill(&source,buffer,length,mode);
        clock_t start=clock();
        gui_beta_hifi_process(d,buffer,length,position,capture_pcm,audio);
        audio->processing_seconds+=(double)(clock()-start)/CLOCKS_PER_SEC;
        position+=length;
    }
    gui_beta_hifi_get_status(d,&audio->status);
    free(buffer);gui_beta_hifi_destroy(d);
}

static void run_fixture(audio_capture_t *audio,size_t count,size_t chunk,int mode) {
    run_fixture_kernel(audio,count,chunk,mode,requested_kernel);
}

static double energy(const audio_capture_t *audio,int channel,size_t first,size_t count) {
    double sum=0;
    for(size_t i=first;i<first+count;i++) {
        double value=audio->stereo[i*2+(size_t)channel];sum+=value*value;
    }
    return sum/count;
}

static double tone_power(const audio_capture_t *audio,int channel,double frequency,
                           size_t first,size_t count) {
    double re=0,im=0;
    for(size_t i=0;i<count;i++) {
        double value=audio->stereo[(first+i)*2+(size_t)channel];
        double phase=2*TEST_PI*frequency*i/48000;
        re+=value*cos(phase);im+=value*sin(phase);
    }
    return (re*re+im*im)/(count*count);
}

static void test_four_carriers_and_chunk_invariance(void) {
    audio_capture_t *whole=calloc(1,sizeof(*whole)),*split=calloc(1,sizeof(*split));
    assert(whole && split);
    const size_t samples=TEST_RATE*3u/4u;
    run_fixture(whole,samples,samples,0);
    run_fixture(split,samples,7919,0);
    assert(whole->saw_lock && whole->status.locked);
    assert(whole->status.head_switches>=35);
    assert(whole->frames>=35999 && whole->frames<=36001);
    assert(split->frames==whole->frames);
    assert(memcmp(split->stereo,whole->stereo,whole->frames*2*sizeof(float))==0);
    assert(split->status.head_switches==whole->status.head_switches);
    for(size_t i=0;i<10000*2;i++) assert(whole->stereo[i]==0);
    assert(energy(whole,0,24000,10000)>1e-5);
    assert(energy(whole,1,24000,10000)>1e-5);
    assert(tone_power(whole,0,1000,24000,9600)>10*tone_power(whole,0,1700,24000,9600));
    assert(tone_power(whole,1,1700,24000,9600)>10*tone_power(whole,1,1000,24000,9600));
    printf("four-carrier/chunk gate: %zu frames, %llu head switches, RMS %.6f/%.6f; "
           "DSP %.3f CPU s / 0.750 RF s\n",
           whole->frames,(unsigned long long)whole->status.head_switches,
           sqrt(energy(whole,0,24000,10000)),sqrt(energy(whole,1,24000,10000)),
           whole->processing_seconds);
    free(whole);free(split);
}

static void test_unlocked_and_missing_pair_stay_silent(void) {
    for(int mode=1;mode<=2;mode++) {
        audio_capture_t *audio=calloc(1,sizeof(*audio));assert(audio);
        run_fixture(audio,TEST_RATE/2,16381,mode);
        assert(!audio->saw_lock && !audio->status.locked);
        for(size_t i=0;i<audio->frames*2;i++) assert(audio->stereo[i]==0);
        free(audio);
    }
}

static void test_capture_response_imbalance_learns_head_midpoint(void) {
    for(int mode=4;mode<=5;mode++) {
        audio_capture_t *audio=calloc(1,sizeof(*audio));assert(audio);
        run_fixture(audio,TEST_RATE*3u/4u,7919,mode);
        if(!audio->status.locked) fprintf(stderr,"imbalance %d failed: %s\n",mode,audio->status.reason);
        assert(audio->saw_lock && audio->status.locked);
        assert(audio->status.head_switches>=20);
        assert(energy(audio,0,24000,10000)>1e-5);
        assert(energy(audio,1,24000,10000)>1e-5);
        assert(tone_power(audio,0,1000,24000,9600)>10*tone_power(audio,0,1700,24000,9600));
        assert(tone_power(audio,1,1700,24000,9600)>10*tone_power(audio,1,1000,24000,9600));
        printf("capture-response imbalance %s: locked, %llu switches\n",
               mode==4 ? "positive" : "negative",(unsigned long long)audio->status.head_switches);
        free(audio);
    }
}

static void test_startup_noise_does_not_poison_recent_cadence(void) {
    audio_capture_t *audio=calloc(1,sizeof(*audio));assert(audio);
    run_fixture(audio,TEST_RATE*19u/20u,7919,6);
    assert(audio->saw_lock && audio->status.locked);
    assert(energy(audio,0,40000,5000)>1e-5);
    assert(energy(audio,1,40000,5000)>1e-5);
    printf("startup-noise recovery: locked after clean current calibration window\n");
    free(audio);
}

static void test_weak_head_with_stronger_other_head_carrier_residual(void) {
    /* From 1986 real RF: the B-frequency residual during head A is stronger
     * than that carrier's actual head B programme. Joint dominance still
     * separates the heads; individual active/inactive power comparison fails. */
    audio_capture_t *audio=calloc(1,sizeof(*audio));assert(audio);
    run_fixture(audio,TEST_RATE*3u/4u,7919,7);
    assert(audio->saw_lock && audio->status.locked);
    assert(energy(audio,0,24000,10000)>1e-5);
    assert(energy(audio,1,24000,10000)>1e-5);
    assert(tone_power(audio,0,1000,24000,9600)>10*tone_power(audio,0,1700,24000,9600));
    assert(tone_power(audio,1,1700,24000,9600)>10*tone_power(audio,1,1000,24000,9600));
    printf("weak-head/strong other-head residual: locked with independent active references\n");
    free(audio);
}

static void test_loss_silence_recovery_and_reset(void) {
    audio_capture_t *audio=calloc(1,sizeof(*audio));assert(audio);
    run_fixture(audio,TEST_RATE,16381,3);
    assert(audio->saw_lock && audio->status.locked);
    for(size_t i=19000;i<21000;i++) {
        assert(audio->stereo[i*2]==0);
        assert(audio->stereo[i*2+1]==0);
    }
    assert(energy(audio,0,40000,6000)>1e-5);
    free(audio);

    gui_beta_hifi_decoder_t *d=create_selected_decoder();
    float zero[1001]={0};
    gui_beta_hifi_process(d,zero,1001,0,NULL,NULL);
    gui_beta_hifi_status_t before,after;
    gui_beta_hifi_get_status(d,&before);
    gui_beta_hifi_process(d,zero,1001,2000,NULL,NULL);
    gui_beta_hifi_get_status(d,&after);
    assert(after.resets==before.resets+1);
    assert(after.input_samples==1001 && !after.locked);
    gui_beta_hifi_reset(d);
    gui_beta_hifi_get_status(d,&after);
    assert(!after.locked && after.input_samples==0 && after.pcm_frames==0);
    zero[0]=NAN;zero[1]=INFINITY;
    gui_beta_hifi_process(d,zero,1001,0,NULL,NULL);
    gui_beta_hifi_get_status(d,&after);
    assert(!after.locked);
    gui_beta_hifi_destroy(d);

    /* Malformed input after a real lock must also close the gate. This is
     * exercised in fast-math builds, where C isfinite may be optimized away. */
    d=create_selected_decoder();
    audio=calloc(1,sizeof(*audio));assert(audio);
    const size_t samples=TEST_RATE/3;
    float *rf=malloc(samples*sizeof(float));assert(rf);
    rf_source_t source;source_reset(&source);source_fill(&source,rf,samples,0);
    gui_beta_hifi_process(d,rf,samples,0,capture_pcm,audio);
    gui_beta_hifi_get_status(d,&before);assert(before.locked);
    zero[0]=NAN;zero[1]=INFINITY;
    size_t previous_frames=audio->frames;
    gui_beta_hifi_process(d,zero,1001,samples,capture_pcm,audio);
    gui_beta_hifi_get_status(d,&after);assert(!after.locked);
    for(size_t i=previous_frames*2;i<audio->frames*2;i++) assert(audio->stereo[i]==0);
    free(rf);free(audio);gui_beta_hifi_destroy(d);
}

static bool supports_kernel(gui_beta_hifi_iq_kernel_t kernel) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    bool supported=gui_beta_hifi_set_iq_kernel(d,kernel);
    gui_beta_hifi_destroy(d);
    return supported;
}

static void test_kernel_dispatch_and_equivalence(void) {
    gui_beta_hifi_decoder_t *d=gui_beta_hifi_create();assert(d);
    gui_beta_hifi_iq_kernel_t automatic=gui_beta_hifi_get_iq_kernel(d);
    assert(automatic!=GUI_BETA_HIFI_IQ_AUTO);
    if(!supports_kernel(GUI_BETA_HIFI_IQ_AVX2)) {
        assert(!gui_beta_hifi_set_iq_kernel(d,GUI_BETA_HIFI_IQ_AVX2));
        assert(gui_beta_hifi_get_iq_kernel(d)==automatic);
    }
    assert(gui_beta_hifi_set_iq_kernel(d,GUI_BETA_HIFI_IQ_SCALAR));
    assert(!gui_beta_hifi_set_iq_kernel(d,(gui_beta_hifi_iq_kernel_t)99));
    assert(gui_beta_hifi_get_iq_kernel(d)==GUI_BETA_HIFI_IQ_SCALAR);
    gui_beta_hifi_reset(d);
    assert(gui_beta_hifi_get_iq_kernel(d)==GUI_BETA_HIFI_IQ_SCALAR);
    float zero=0;
    gui_beta_hifi_process(d,&zero,1,0,NULL,NULL);
    assert(!gui_beta_hifi_set_iq_kernel(d,GUI_BETA_HIFI_IQ_AUTO));
    gui_beta_hifi_destroy(d);

    audio_capture_t *reference=calloc(1,sizeof(*reference));
    audio_capture_t *other=calloc(1,sizeof(*other));
    assert(reference && other);
    gui_beta_hifi_iq_kernel_t baseline=supports_kernel(GUI_BETA_HIFI_IQ_SSE2)
                                          ? GUI_BETA_HIFI_IQ_SSE2 : GUI_BETA_HIFI_IQ_SCALAR;
    run_fixture_kernel(reference,TEST_RATE*3u/4u,7919,0,baseline);
    for(int kernel=GUI_BETA_HIFI_IQ_SCALAR;kernel<=GUI_BETA_HIFI_IQ_AVX2;kernel++) {
        if(!supports_kernel((gui_beta_hifi_iq_kernel_t)kernel)) continue;
        memset(other,0,sizeof(*other));
        run_fixture_kernel(other,TEST_RATE*3u/4u,TEST_RATE*3u/4u,0,(gui_beta_hifi_iq_kernel_t)kernel);
        assert(reference->frames==other->frames && other->status.locked);
        double maximum=0,squared=0;
        size_t different=0;
        for(size_t i=0;i<reference->frames*2;i++) {
            double difference=(double)reference->stereo[i]-other->stereo[i];
            if(difference!=0) different++;
            maximum=fmax(maximum,fabs(difference));squared+=difference*difference;
        }
        double rms=sqrt(squared/(reference->frames*2));
        assert(maximum<2e-5 && rms<1e-6);
        printf("IQ kernel %d vs %d: max %.9g RMS %.9g, %zu different float samples\n",
               kernel,(int)baseline,maximum,rms,different);
    }
    free(reference);free(other);
}

int main(int argc,char **argv) {
    if(argc==3 && strcmp(argv[1],"--kernel")==0) {
        if(strcmp(argv[2],"scalar")==0) requested_kernel=GUI_BETA_HIFI_IQ_SCALAR;
        else if(strcmp(argv[2],"sse2")==0) requested_kernel=GUI_BETA_HIFI_IQ_SSE2;
        else if(strcmp(argv[2],"avx2")==0) requested_kernel=GUI_BETA_HIFI_IQ_AVX2;
        else return 2;
    } else if(argc!=1) return 2;
    if(!supports_kernel(requested_kernel)) {
        printf("Requested IQ kernel is unavailable on this CPU/build\n");return 77;
    }
    clock_t start=clock();
    test_four_carriers_and_chunk_invariance();
    test_unlocked_and_missing_pair_stay_silent();
    test_capture_response_imbalance_learns_head_midpoint();
    test_startup_noise_does_not_poison_recent_cadence();
    test_weak_head_with_stronger_other_head_carrier_residual();
    test_loss_silence_recovery_and_reset();
    test_kernel_dispatch_and_equivalence();
    printf("gui_beta_hifi tests passed (%.3f CPU seconds)\n",(double)(clock()-start)/CLOCKS_PER_SEC);
    return 0;
}
