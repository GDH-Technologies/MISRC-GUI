/* Production monitor queue, device callback and lifecycle without hardware or SOXR. */
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
static bool monitor_test_flag_take(volatile atomic_flag *flag);
static void monitor_test_flag_clear(volatile atomic_flag *flag);
/* Faults run at the real queue-lock boundary, without production test hooks. */
#undef atomic_flag_test_and_set
#undef atomic_flag_clear
#define atomic_flag_test_and_set(flag) monitor_test_flag_take(flag)
#define atomic_flag_clear(flag) monitor_test_flag_clear(flag)
#ifdef LIBSOXR_ENABLED
#undef LIBSOXR_ENABLED
#endif
#define LIBSOXR_ENABLED 0
#include "../misrc_gui/output/gui_audio.c"

static int stream_loads, stream_unloads, stream_updates, callback_installs;
static bool stream_playing, stream_live;
static int buffer_frames, processed_halves;
static AudioCallback stream_callback;
static int16_t last_pcm[4096 * 2];
static gui_app_t app;
static int source_a, source_b;
static int before_pop_fault, after_pop_fault;
static unsigned before_pop_faults, after_pop_faults;
static const float fresh_pcm[] = {0.25f, -0.5f, 0.5f, -0.25f};
atomic_int do_exit;

static bool monitor_test_flag_take(volatile atomic_flag *flag)
{
    if (flag == &s_hifi_q_guard && before_pop_fault) {
        int fault = before_pop_fault;
        before_pop_fault = 0;
        ++before_pop_faults;
        if (fault == 1) gui_audio_set_hifi_source(&source_b);
        else gui_audio_reset_hifi(&source_a);
        gui_audio_push_hifi(fault == 1 ? &source_b : &source_a, fresh_pcm, 2);
    }
    return atomic_flag_test_and_set_explicit(flag, memory_order_seq_cst);
}
static void monitor_test_flag_clear(volatile atomic_flag *flag)
{
    atomic_flag_clear_explicit(flag, memory_order_seq_cst);
    if (flag == &s_hifi_q_guard && after_pop_fault) {
        int fault = after_pop_fault;
        after_pop_fault = 0;
        ++after_pop_faults;
        /* Simulate another thread changing a gate after PCM has been copied. */
        if (fault == 1) atomic_store(&s_playback_enabled, false);
        else atomic_store(&s_monitor_session_active, false);
    }
}

/* Capture/writer paths are outside this monitor-only test. */
void gui_app_set_status(gui_app_t *a, const char *s) { (void)a; (void)s; assert(false); }
FILE *file_open_utf8(const char *p, const char *m) { (void)p; (void)m; assert(false); return NULL; }
/* gui_audio.c taps the capture stream for the M-Server monitor display only
 * while the unified server runs; idle in this harness. */
bool gui_monitor_server_active(void) { return false; }
int bufmgr_ensure_init(buffer_manager_t *m, buffer_id_t id) { (void)m; (void)id; assert(false); return -1; }
void *bufmgr_read_begin(buffer_manager_t *m, buffer_id_t id, size_t n, int ms)
{ (void)m; (void)id; (void)n; (void)ms; assert(false); return NULL; }
void bufmgr_read_end(buffer_manager_t *m, buffer_id_t id, size_t n)
{ (void)m; (void)id; (void)n; assert(false); }
size_t bufmgr_fill_level(buffer_manager_t *m, buffer_id_t id)
{ (void)m; (void)id; assert(false); return 0; }
void extract_audio_2ch_C(uint16_t *in, size_t n, uint16_t *a, uint16_t *b)
{ (void)in; (void)n; (void)a; (void)b; assert(false); }
void extract_audio_1ch_C(uint8_t *in, size_t n, uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d)
{ (void)in; (void)n; (void)a; (void)b; (void)c; (void)d; assert(false); }
void gui_settings_save(const gui_settings_t *settings) { (void)settings; }
void InitAudioDevice(void) {}
bool IsAudioDeviceReady(void) { return true; }
void SetAudioStreamBufferSizeDefault(int frames)
{
    assert(frames == (gui_audio_get_hifi_source() ? 1024 : 4096));
    buffer_frames = frames;
}
AudioStream LoadAudioStream(unsigned int rate, unsigned int bits, unsigned int channels)
{
    assert(!stream_live && rate == 48000 && bits == 16 && channels == 2);
    ++stream_loads;
    stream_live = true;
    stream_callback = NULL;
    processed_halves = 1;
    return (AudioStream){.buffer = (void *)1, .sampleRate = rate,
                         .sampleSize = bits, .channels = channels};
}
bool IsAudioStreamValid(AudioStream stream) { return stream.buffer != NULL; }
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback)
{
    assert(stream_live && stream.buffer && callback && !stream_playing);
    stream_callback = callback;
    ++callback_installs;
}
void PlayAudioStream(AudioStream stream) { assert(stream_live && stream.buffer); stream_playing = true; }
void StopAudioStream(AudioStream stream) { assert(stream_live && stream.buffer); stream_playing = false; }
void UnloadAudioStream(AudioStream stream)
{
    assert(stream_live && stream.buffer && !stream_playing);
    stream_live = false;
    stream_callback = NULL;
    ++stream_unloads;
}
bool IsAudioStreamPlaying(AudioStream stream) { assert(stream_live && stream.buffer); return stream_playing; }
bool IsAudioStreamProcessed(AudioStream stream)
{
    assert(stream_live && stream.buffer && !stream_callback);
    return processed_halves > 0;
}
void UpdateAudioStream(AudioStream stream, const void *pcm, int frames)
{
    /* Only legacy audio may use GUI-driven whole-half writes. */
    assert(stream_live && stream.buffer && !stream_callback);
    assert(frames == 4096 && frames == buffer_frames && processed_halves > 0);
    --processed_halves;
    memcpy(last_pcm, pcm, (size_t)frames * 2 * sizeof(last_pcm[0]));
    ++stream_updates;
}

static void request_pcm(unsigned frames)
{
    assert(stream_live && stream_playing && stream_callback && frames <= 4096);
    /* This is raylib's stream input format: 48 kHz, signed 16-bit, stereo.
     * The mixer requests frames; it does not promise a 1024-frame half. */
    for (size_t i = 0; i < sizeof(last_pcm)/sizeof(*last_pcm); ++i) last_pcm[i] = 12345;
    stream_callback(last_pcm, frames);
    for (size_t i = (size_t)frames * 2; i < sizeof(last_pcm)/sizeof(*last_pcm); ++i)
        assert(last_pcm[i] == 12345); /* No write beyond the requested frames. */
}
static void expect_silence(unsigned frames)
{
    request_pcm(frames);
    for (size_t i = 0; i < (size_t)frames * 2; ++i) assert(last_pcm[i] == 0);
}
static void expect_fresh(unsigned frames)
{
    request_pcm(frames);
    assert(last_pcm[0] == 8192 && last_pcm[1] == -16384);
    assert(last_pcm[2] == 16384 && last_pcm[3] == -8192);
    for (size_t i = 4; i < (size_t)frames * 2; ++i) assert(last_pcm[i] == 0);
}
static size_t pop_hifi(int16_t *out, size_t count)
{
    return hifi_q_pop_samples(out, count, atomic_load(&s_monitor_epoch));
}
static atomic_bool stress_done;
static int stress_push(void *unused)
{
    (void)unused;
    const float pcm[] = {0.25f, -0.25f, 0.5f, -0.5f};
    for (int i = 0; i < 40000; ++i) gui_audio_push_hifi(&source_a, pcm, 2);
    atomic_store(&stress_done, true);
    return 0;
}

static void test_callback_frames(void)
{
    const unsigned requests[] = {480, 1920, 2560, 512, 4096};
    float pcm[HIFI_Q_FRAMES * 2];
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) { pcm[i * 2] = .25f; pcm[i * 2 + 1] = -.5f; }
    int loads = stream_loads, updates = stream_updates;
    for (unsigned repeat = 0; repeat < 4; ++repeat) for (size_t n = 0; n < sizeof(requests)/sizeof(*requests); ++n) {
        unsigned frames = requests[n];
        gui_audio_push_hifi(&source_a, pcm, frames);
        request_pcm(frames); /* No GUI pump between producer/device callbacks. */
        for (unsigned i = 0; i < frames; ++i)
            assert(last_pcm[i * 2] == 8192 && last_pcm[i * 2 + 1] == -16384);
        assert(s_hifi_q_count == 0);
    }
    processed_halves = 2;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads && stream_updates == updates);
    assert(processed_halves == 2); /* Hi-Fi never polls/updates the stream halves. */
    stream_callback(NULL, 1920);
    request_pcm(0);
}

static void test_callback_control_boundaries(void)
{
    for (int fault = 1; fault <= 2; ++fault) {
        gui_audio_set_hifi_source(&source_a);
        gui_audio_update_playback(&app);
        gui_audio_push_hifi(&source_a, fresh_pcm, 2);
        int loads = stream_loads;
        /* Precheck passes for the old stream. Before taking the queue lock,
         * a source change or gap publishes a fresh epoch and fresh PCM. */
        before_pop_fault = fault;
        expect_silence(1920);
        assert(!before_pop_fault && s_hifi_q_count == 2);
        assert(stream_loads == loads); /* Old callback is still installed. */
        gui_audio_update_playback(&app);
        assert(stream_loads == loads + 1);
        expect_fresh(2560); /* The stale callback must not swallow fresh PCM. */
    }
    assert(before_pop_faults == 2);
    for (int fault = 1; fault <= 2; ++fault) {
        gui_audio_set_hifi_source(&source_a);
        gui_audio_update_playback(&app);
        gui_audio_push_hifi(&source_a, fresh_pcm, 2);
        after_pop_fault = fault;
        expect_silence(1920); /* A concurrent mute/session end discards copied PCM. */
        assert(!after_pop_fault);
        if (fault == 1) app.settings.audio_monitor_playback = false;
        else app.is_capturing = false;
        gui_audio_update_playback(&app);
        assert(!s_play_stream_inited && !stream_live);
        app.is_capturing = true;
        gui_audio_set_playback_enabled(&app, true);
        gui_audio_update_playback(&app);
        expect_silence(1920);
    }
    assert(after_pop_faults == 2);
}

static void test_playback_sidecar_integration(void)
{
    app.is_capturing = true;
    app.settings.audio_monitor_playback = true;
    gui_audio_set_hifi_source(NULL);
    gui_audio_update_playback(&app);
    const int16_t old_pcm[] = {1234, -2345};
    const int16_t new_pcm[] = {3456, -4567};
    gui_audio_push_playback(old_pcm, 1);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 1234 && last_pcm[1] == -2345 && buffer_frames == 4096);
    gui_audio_push_playback(old_pcm, 1);
    assert(!atomic_flag_test_and_set(&s_play_q_guard));
    gui_audio_reset_playback_queue();
    atomic_flag_clear(&s_play_q_guard);
    gui_audio_push_playback(new_pcm, 1);
    int loads = stream_loads, unloads = stream_unloads;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1 && stream_unloads == unloads + 1);
    assert(last_pcm[0] == 3456 && last_pcm[1] == -4567);
    for (size_t i = 2; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);
    gui_audio_push_playback(old_pcm, 1);
    gui_audio_reset_playback_queue();
    int16_t output[8];
    assert(play_q_pop_samples(output, 8) == 0);
    gui_audio_push_playback(old_pcm, SIZE_MAX);
    assert(play_q_pop_samples(output, 8) == 0);
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    size_t epoch = atomic_load(&s_monitor_epoch);
    size_t generation = atomic_load(&s_play_q_generation);
    loads = stream_loads; unloads = stream_unloads;
    gui_audio_push_playback(old_pcm, 1);
    gui_audio_reset_playback_queue();
    assert(atomic_load(&s_monitor_epoch) == epoch && atomic_load(&s_play_q_generation) == generation);
    assert(play_q_pop_samples(output, 8) == 0);
    atomic_fetch_add(&s_play_q_generation, 1); /* Delayed legacy seek must not flush Hi-Fi. */
    gui_audio_update_playback(&app);
    assert(stream_loads == loads && stream_unloads == unloads);
    expect_fresh(1920);
    assert(atomic_load(&s_monitor_epoch) == epoch);
    gui_audio_set_hifi_source(NULL);
    gui_audio_push_playback(new_pcm, 1);
    /* The old, still-live Hi-Fi stream must not consume new legacy input. */
    expect_silence(1920);
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 3456 && last_pcm[1] == -4567 && buffer_frames == 4096);
    assert(!stream_callback);
    gui_audio_stop(&app);
    assert(stream_loads == stream_unloads);
}

int main(void)
{
    app.is_capturing = true;
    assert(!atomic_load(&s_playback_enabled));
    app.settings.audio_monitor_playback = true;
    gui_audio_update_playback(&app); /* Persisted setting without audio writer start. */
    assert(atomic_load(&s_playback_enabled) && !s_audio_running && !stream_callback);
    const int16_t restored_pcm[] = {321, -654};
    gui_audio_push_demod(restored_pcm, 1);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 321 && last_pcm[1] == -654 && buffer_frames == 4096);
    gui_audio_stop(&app);
    assert(stream_loads == stream_unloads);
    stream_loads = stream_unloads = stream_updates = callback_installs = 0;
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    assert(atomic_load(&s_monitor_session_active) && callback_installs == 1);
    expect_silence(1920);
    const float pcm[] = {-1.0f, 1.0f, NAN, INFINITY, -0.25f, 0.5f};
    const int16_t legacy[] = {123, 456};
    gui_audio_push_hifi(&source_b, pcm, 3);
    gui_audio_push_demod(legacy, 1);
    gui_audio_push_hifi(&source_a, pcm, 3);
    request_pcm(1920);
    assert(stream_loads == 1 && stream_updates == 0);
    assert(last_pcm[0] == INT16_MIN && last_pcm[1] == INT16_MAX);
    assert(last_pcm[2] == 0 && last_pcm[3] == 0);
    assert(last_pcm[4] == -8192 && last_pcm[5] == 16384);
    for (size_t i = 6; i < 1920 * 2; ++i) assert(last_pcm[i] == 0);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    gui_audio_reset_hifi(&source_b); /* Wrong owner cannot invalidate selected PCM. */
    expect_fresh(2560);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_reset_hifi(&source_a);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    int loads = stream_loads;
    expect_silence(1920);
    assert(s_hifi_q_count == 2);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1);
    expect_fresh(1920);

    int16_t out[HIFI_Q_FRAMES * 2];
    const size_t big_frames = HIFI_Q_FRAMES + 97;
    float *big = malloc(big_frames * 2 * sizeof(*big));
    assert(big);
    for (size_t i = 0; i < big_frames; ++i) {
        big[i * 2] = i < 97 ? -0.9f : 0.25f;
        big[i * 2 + 1] = i < 97 ? 0.9f : -0.5f;
    }
    gui_audio_push_hifi(&source_a, big, big_frames);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == HIFI_Q_FRAMES * 2);
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) assert(out[i * 2] == 8192 && out[i * 2 + 1] == -16384);
    gui_audio_push_hifi(&source_a, big + 97 * 2, HIFI_Q_FRAMES);
    for (size_t i = 0; i < 128; ++i) { big[i * 2] = 0.75f; big[i * 2 + 1] = -0.75f; }
    gui_audio_push_hifi(&source_a, big, 128);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == HIFI_Q_FRAMES * 2);
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) {
        assert(out[i * 2] == (i < HIFI_Q_FRAMES - 128 ? 8192 : 24575));
        assert(out[i * 2 + 1] == (i < HIFI_Q_FRAMES - 128 ? -16384 : -24575));
    }
    free(big);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    assert(!atomic_flag_test_and_set(&s_hifi_q_guard));
    gui_audio_push_hifi(&source_a, pcm, 3); /* Contention is bounded and does not wait. */
    expect_silence(1920);
    atomic_flag_clear(&s_hifi_q_guard);
    expect_fresh(1920); /* Consumer contention must not consume queued data. */
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_set_hifi_source(&source_b);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_push_hifi(&source_b, fresh_pcm, 2);
    loads = stream_loads;
    expect_silence(2560);
    assert(s_hifi_q_count == 2);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1);
    expect_fresh(1920);
    gui_audio_set_hifi_source(NULL);
    gui_audio_push_demod(legacy, 1);
    expect_silence(1920);
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 123 && last_pcm[1] == 456 && buffer_frames == 4096);
    assert(stream_updates == 1 && !stream_callback);
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_set_playback_enabled(&app, false);
    assert(!stream_live);
    gui_audio_push_hifi(&source_a, pcm, 3);
    int updates = stream_updates;
    gui_audio_update_playback(&app);
    assert(stream_updates == updates);
    loads = stream_loads;
    gui_audio_set_playback_enabled(&app, true);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads + 1);
    expect_silence(1920);
    loads = stream_loads;
    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads);
    test_callback_frames();
    test_callback_control_boundaries();

    gui_audio_push_hifi(&source_a, pcm, 3);
    app.is_capturing = false;
    int unloads = stream_unloads;
    gui_audio_update_playback(&app);
    assert(!atomic_load(&s_monitor_session_active) && !s_play_stream_inited && !stream_live);
    assert(stream_unloads == unloads + 1);
    gui_audio_push_hifi(&source_a, pcm, 3);
    thrd_t late_producer;
    assert(thrd_create(&late_producer, stress_push, NULL) == thrd_success);
    thrd_join(late_producer, NULL);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == 0);
    atomic_store(&stress_done, false);
    app.is_capturing = true;
    gui_audio_push_hifi(&source_a, pcm, 3); /* Reconnect before first pump is not a session. */
    gui_audio_update_playback(&app);
    assert(atomic_load(&s_monitor_session_active));
    expect_silence(1920);
    gui_audio_push_hifi(&source_a, fresh_pcm, 2);
    expect_fresh(2560);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_stop(&app);
    assert(!atomic_load(&s_monitor_session_active));
    gui_audio_push_hifi(&source_a, pcm, 3);
    assert(pop_hifi(out, sizeof(out)/sizeof(*out)) == 0);
    gui_audio_update_playback(&app);
    expect_silence(1920);
    thrd_t producer;
    assert(thrd_create(&producer, stress_push, NULL) == thrd_success);
    while (!atomic_load(&stress_done)) {
        gui_audio_reset_hifi(&source_a);
        gui_audio_set_hifi_source(&source_b);
        gui_audio_set_hifi_source(&source_a);
        pop_hifi(out, sizeof(out)/sizeof(*out));
    }
    thrd_join(producer, NULL);
    assert(s_hifi_q_count <= HIFI_Q_FRAMES);
    gui_audio_stop(&app);
    assert(!s_play_stream_inited && stream_loads == stream_unloads);
    test_playback_sidecar_integration();
    puts("gui_audio_hifi_monitor: callback frame counts, queue bounds, control races, mute/session restart and legacy sidecar passed");
    return 0;
}