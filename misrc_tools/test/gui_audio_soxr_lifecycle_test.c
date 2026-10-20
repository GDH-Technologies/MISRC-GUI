/* Drive the production capture-audio thread through controlled SOXR calls.
 * Fake SOXR state keeps history so a missing epoch reset is audible/testable;
 * an in-process barrier makes UI/control interleavings deterministic. */
#ifdef LIBSOXR_ENABLED
#undef LIBSOXR_ENABLED
#endif
#define LIBSOXR_ENABLED 1
#include "../misrc_gui/output/gui_audio.c"
#include <assert.h>

atomic_int do_exit;
static _Thread_local bool main_thread;
static atomic_bool in_soxr, hold_next_process, process_held, release_process;
static atomic_bool fail_create;
static atomic_int supplied_blocks, completed_blocks, marker;
static atomic_int creates, clears, producer_deletes, stopped_deletes;
static uint8_t captured_pcm[BUFFER_AUDIO_READ_SIZE];
static int device_attempts, stream_loads, stream_unloads, processed_halves;
static bool fail_device, fail_stream, stream_playing;
static int stream_buffer_frames;
static gui_app_t app;
struct soxr { int history; };

static void enter_soxr(bool producer_only)
{
    if (producer_only) assert(!main_thread);
    assert(!atomic_exchange(&in_soxr, true));
}
static void leave_soxr(void) { atomic_store(&in_soxr, false); }
soxr_io_spec_t soxr_io_spec(soxr_datatype_t in, soxr_datatype_t out)
{ assert(!main_thread); return (soxr_io_spec_t){.itype=in,.otype=out}; }
soxr_quality_spec_t soxr_quality_spec(unsigned long quality, unsigned long flags)
{ assert(!main_thread && quality==SOXR_HQ); return (soxr_quality_spec_t){.flags=flags}; }
soxr_t soxr_create(double in, double out, unsigned channels, soxr_error_t *error,
                    const soxr_io_spec_t *io, const soxr_quality_spec_t *quality,
                    const soxr_runtime_spec_t *runtime)
{
    (void)quality; (void)runtime;
    enter_soxr(true);
    assert(in>=8000 && out==48000 && channels==2);
    assert(io->itype==SOXR_INT16_I && io->otype==SOXR_INT16_I);
    atomic_fetch_add(&creates,1);
    if (atomic_load(&fail_create)) {
        *error="controlled creation failure";
        leave_soxr();
        return NULL;
    }
    *error=NULL;
    soxr_t result=calloc(1,sizeof(*result)); assert(result);
    leave_soxr();
    return result;
}
soxr_error_t soxr_clear(soxr_t resampler)
{
    enter_soxr(true); assert(resampler);
    resampler->history=0; atomic_fetch_add(&clears,1);
    leave_soxr(); return NULL;
}
void soxr_delete(soxr_t resampler)
{
    enter_soxr(false); assert(resampler);
    if (main_thread) {
        assert(!s_audio_running && !atomic_load(&s_monitor_session_active));
        atomic_fetch_add(&stopped_deletes,1);
    } else atomic_fetch_add(&producer_deletes,1);
    free(resampler); leave_soxr();
}
soxr_error_t soxr_process(soxr_t resampler, soxr_in_t in, size_t ilen,
                          size_t *idone, soxr_out_t out, size_t olen, size_t *odone)
{
    enter_soxr(true); assert(resampler && in && olen>=4);
    int current=atomic_load(&marker);
    if (atomic_exchange(&hold_next_process,false)) {
        atomic_store(&process_held,true);
        while (!atomic_load(&release_process)) thrd_sleep_ms(1);
    }
    int16_t *pcm=out;
    int value=resampler->history ? resampler->history : current;
    for (int i=0;i<8;++i) pcm[i]=(int16_t)value;
    resampler->history=current;
    *idone=ilen; *odone=4;
    leave_soxr(); return NULL;
}

void gui_settings_save(const gui_settings_t *settings) { (void)settings; }
void gui_app_set_status(gui_app_t *a,const char *s) { (void)a;(void)s; assert(false); }
FILE *file_open_utf8(const char *p,const char *m) { (void)p;(void)m; assert(false);return NULL; }
int bufmgr_ensure_init(buffer_manager_t *m,buffer_id_t id)
{ (void)m; assert(id==BUF_CAPTURE_AUDIO); return 0; }
void *bufmgr_read_begin(buffer_manager_t *m,buffer_id_t id,size_t n,int timeout)
{
    (void)m;(void)timeout; assert(id==BUF_CAPTURE_AUDIO && n==BUFFER_AUDIO_READ_SIZE);
    int available=1;
    if (atomic_compare_exchange_strong(&supplied_blocks,&available,0)) return captured_pcm;
    thrd_sleep_ms(1); return NULL;
}
void bufmgr_read_end(buffer_manager_t *m,buffer_id_t id,size_t n)
{ (void)m; assert(id==BUF_CAPTURE_AUDIO && n==BUFFER_AUDIO_READ_SIZE); atomic_fetch_add(&completed_blocks,1); }
size_t bufmgr_fill_level(buffer_manager_t *m,buffer_id_t id)
{ (void)m;(void)id; return 0; }
void extract_audio_2ch_C(uint16_t *in,size_t n,uint16_t *a,uint16_t *b)
{ (void)in;(void)n;(void)a;(void)b; assert(false); }
void extract_audio_1ch_C(uint8_t *in,size_t n,uint8_t *a,uint8_t *b,uint8_t *c,uint8_t *d)
{ (void)in;(void)n;(void)a;(void)b;(void)c;(void)d; assert(false); }
void InitAudioDevice(void) { assert(main_thread); ++device_attempts; }
bool IsAudioDeviceReady(void) { return !fail_device; }
void SetAudioStreamBufferSizeDefault(int frames)
{ assert(main_thread && (frames==1024 || frames==4096)); stream_buffer_frames=frames; }
AudioStream LoadAudioStream(unsigned rate,unsigned bits,unsigned channels)
{
    assert(main_thread && rate==48000 && bits==16 && channels==2);
    if (fail_stream) return (AudioStream){0};
    ++stream_loads; processed_halves=1;
    return (AudioStream){.buffer=(void*)1,.sampleRate=rate,.sampleSize=bits,.channels=channels};
}
bool IsAudioStreamValid(AudioStream stream) { return stream.buffer!=NULL; }
void PlayAudioStream(AudioStream stream) { assert(main_thread && stream.buffer); stream_playing=true; }
void StopAudioStream(AudioStream stream) { assert(main_thread && stream.buffer); stream_playing=false; }
void UnloadAudioStream(AudioStream stream) { assert(main_thread && stream.buffer); ++stream_unloads; }
bool IsAudioStreamPlaying(AudioStream stream) { assert(stream.buffer); return stream_playing; }
bool IsAudioStreamProcessed(AudioStream stream) { assert(stream.buffer); return processed_halves>0; }
void UpdateAudioStream(AudioStream stream,const void *pcm,int frames)
{ assert(main_thread && stream.buffer && pcm && frames==stream_buffer_frames); --processed_halves; }

static void wait_flag(atomic_bool *flag)
{
    uint64_t end=get_time_ms()+3000;
    while (!atomic_load(flag)) { assert(get_time_ms()<end); thrd_sleep_ms(1); }
}
static void wait_blocks(int count)
{
    uint64_t end=get_time_ms()+3000;
    while (atomic_load(&completed_blocks)<count) { assert(get_time_ms()<end); thrd_sleep_ms(1); }
}
static void supply(int value,bool hold)
{
    assert(!atomic_load(&supplied_blocks));
    atomic_store(&marker,value); atomic_store(&hold_next_process,hold);
    atomic_store(&process_held,false); atomic_store(&release_process,!hold);
    atomic_store(&supplied_blocks,1);
}
static void expect_pcm(int value)
{
    int16_t pcm[16]; size_t n=play_q_pop_samples(pcm,16);
    assert(n==(value ? 8u : 0u));
    for (size_t i=0;i<n;++i) assert(pcm[i]==value);
}
static int release_after_stop(void *unused)
{
    (void)unused;
    wait_flag(&s_audio_stop);
    atomic_store(&release_process,true);
    return 0;
}
int main(void)
{
    main_thread=true;
    app.is_capturing=true; app.settings.audio_monitor_playback=true;
    app.device_count=1; app.selected_device=0; app.devices[0].type=DEVICE_TYPE_CXADC;
    atomic_store(&app.audio_sample_rate,46875);
    // The main pump can fail/retry either device or stream creation without
    // creating/touching a SOXR object or preventing later captured playback.
    fail_device=true; gui_audio_update_playback(&app);
    assert(device_attempts==1 && !s_play_stream_inited && !atomic_load(&creates));
    fail_device=false; fail_stream=true; gui_audio_update_playback(&app);
    assert(device_attempts==2 && !s_play_stream_inited && !atomic_load(&creates));
    fail_stream=false; gui_audio_update_playback(&app);
    assert(s_play_stream_inited && !atomic_load(&creates));
    assert(gui_audio_start(&app,&app.buffers)==0);

    supply(1111,true); wait_flag(&process_held);
    // Exercise both redundant true and false->true while the owner is inside
    // process. Also select HiFi and restore normal PCM before it returns.
    gui_audio_set_playback_enabled(&app,true);
    gui_audio_set_playback_enabled(&app,false);
    gui_audio_set_playback_enabled(&app,true);
    int hifi_source;
    gui_audio_set_hifi_source(&hifi_source); gui_audio_update_playback(&app);
    gui_audio_set_hifi_source(NULL); gui_audio_update_playback(&app);
    assert(!atomic_load(&clears) && !atomic_load(&producer_deletes));
    atomic_store(&release_process,true); wait_blocks(1); expect_pcm(0);
    supply(2222,false); wait_blocks(2); expect_pcm(2222);
    assert(atomic_load(&clears)==1);

    supply(3333,true); wait_flag(&process_held);
    gui_audio_set_hifi_source(&hifi_source); gui_audio_update_playback(&app);
    gui_audio_set_hifi_source(NULL); gui_audio_update_playback(&app);
    atomic_store(&release_process,true); wait_blocks(3); expect_pcm(0);
    supply(4444,false); wait_blocks(4); expect_pcm(4444);
    assert(atomic_load(&clears)==2);

    atomic_store(&app.audio_sample_rate,96000);
    supply(5555,false); wait_blocks(5); expect_pcm(5555);
    assert(atomic_load(&creates)==2 && atomic_load(&producer_deletes)==1);
    atomic_store(&app.audio_sample_rate,48000); atomic_store(&fail_create,true);
    supply(6666,false); wait_blocks(6); expect_pcm(0);
    assert(atomic_load(&creates)==3 && atomic_load(&producer_deletes)==2);
    atomic_store(&fail_create,false);
    supply(7777,false); wait_blocks(7); expect_pcm(7777);
    assert(atomic_load(&creates)==4);

    supply(8888,true); wait_flag(&process_held);
    thrd_t releaser; assert(thrd_create(&releaser,release_after_stop,NULL)==thrd_success);
    gui_audio_stop(&app); // Must join the held process before deleting SOXR.
    thrd_join(releaser,NULL);
    expect_pcm(0);
    assert(!s_resampler && !s_audio_running && !atomic_load(&in_soxr));
    assert(atomic_load(&stopped_deletes)==1 && stream_loads==stream_unloads);
    puts("gui_audio_soxr_lifecycle: owner, controlled UI overlap, stale epoch, rate/failure, stop join passed");
    return 0;
}
