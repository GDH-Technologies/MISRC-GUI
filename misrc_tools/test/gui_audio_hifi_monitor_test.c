/* Production monitor queue and device lifecycle, without SOXR or hardware. */
#ifdef LIBSOXR_ENABLED
#undef LIBSOXR_ENABLED
#endif
#define LIBSOXR_ENABLED 0
#include "../misrc_gui/output/gui_audio.c"
#include <assert.h>

static int stream_loads, stream_unloads, stream_updates;
static bool stream_playing;
static int buffer_frames, processed_halves, updates_1024, updates_4096;
static int16_t last_pcm[4096 * 2];
atomic_int do_exit;
/* Capture/writer paths are deliberately outside this monitor-only test. */
void gui_app_set_status(gui_app_t *a, const char *s) { (void)a; (void)s; assert(false); }
FILE *file_open_utf8(const char *p, const char *m) { (void)p; (void)m; assert(false); return NULL; }
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
    assert(rate == 48000 && bits == 16 && channels == 2);
    ++stream_loads;
    processed_halves = 1;
    return (AudioStream){.buffer = (void *)1, .sampleRate = rate,
                         .sampleSize = bits, .channels = channels};
}
bool IsAudioStreamValid(AudioStream stream) { return stream.buffer != NULL; }
void PlayAudioStream(AudioStream stream) { assert(stream.buffer); stream_playing = true; }
void StopAudioStream(AudioStream stream) { assert(stream.buffer); stream_playing = false; }
void UnloadAudioStream(AudioStream stream) { assert(stream.buffer); ++stream_unloads; }
bool IsAudioStreamPlaying(AudioStream stream) { assert(stream.buffer); return stream_playing; }
bool IsAudioStreamProcessed(AudioStream stream) { assert(stream.buffer); return processed_halves > 0; }
void UpdateAudioStream(AudioStream stream, const void *pcm, int frames)
{
    assert(stream.buffer && frames == buffer_frames && processed_halves > 0);
    --processed_halves;
    memset(last_pcm, 0, sizeof(last_pcm));
    memcpy(last_pcm, pcm, (size_t)frames * 2 * sizeof(last_pcm[0]));
    if (frames == 1024) ++updates_1024;
    if (frames == 4096) ++updates_4096;
    ++stream_updates;
}

static gui_app_t app;
static int source_a, source_b;
static atomic_bool stress_done;
static int stress_push(void *unused)
{
    (void)unused;
    const float pcm[] = {0.25f, -0.25f, 0.5f, -0.5f};
    for (int i = 0; i < 40000; ++i) gui_audio_push_hifi(&source_a, pcm, 2);
    atomic_store(&stress_done, true);
    return 0;
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

    // A seek can arrive while the queue is busy: it must not wait, and the
    // next producer must retain post-seek PCM rather than clear it later.
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
    gui_audio_push_playback(old_pcm, SIZE_MAX); // Rejected before frames * 2.
    assert(play_q_pop_samples(output, 8) == 0);

    gui_audio_set_hifi_source(&source_a);
    gui_audio_update_playback(&app);
    const float hifi_pcm[] = {0.25f, -0.5f};
    gui_audio_push_hifi(&source_a, hifi_pcm, 1);
    size_t epoch = atomic_load(&s_monitor_epoch);
    size_t generation = atomic_load(&s_play_q_generation);
    loads = stream_loads; unloads = stream_unloads;
    gui_audio_push_playback(old_pcm, 1);
    gui_audio_reset_playback_queue();
    assert(atomic_load(&s_monitor_epoch) == epoch);
    assert(atomic_load(&s_play_q_generation) == generation);
    assert(play_q_pop_samples(output, 8) == 0);

    // Control the check/use boundary of a seek that observed legacy input
    // just before the GUI selected HiFi: its delayed legacy-only increment
    // must not invalidate HiFi PCM or its already opened device buffers.
    atomic_fetch_add(&s_play_q_generation, 1);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads && stream_unloads == unloads);
    assert(last_pcm[0] == 8192 && last_pcm[1] == -16384);
    for (size_t i = 2; i < 1024 * 2; ++i) assert(last_pcm[i] == 0);
    assert(atomic_load(&s_monitor_epoch) == epoch);

    gui_audio_set_hifi_source(NULL);
    gui_audio_push_playback(new_pcm, 1);
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 3456 && last_pcm[1] == -4567 && buffer_frames == 4096);
    gui_audio_stop(&app);
    assert(stream_loads == stream_unloads);
}

int main(void)
{
    app.is_capturing = true;
    // Persisted Audio Mon must work for file/DDD Demod without an audio
    // writer start or a user toggle after launch.
    assert(!atomic_load(&s_playback_enabled));
    app.settings.audio_monitor_playback = true;
    gui_audio_update_playback(&app);
    assert(atomic_load(&s_playback_enabled) && !s_audio_running);
    const int16_t restored_pcm[] = {321, -654};
    gui_audio_push_demod(restored_pcm, 1);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 321 && last_pcm[1] == -654 && buffer_frames == 4096);
    gui_audio_stop(&app);
    assert(stream_loads == stream_unloads);
    stream_loads = stream_unloads = stream_updates = updates_1024 = updates_4096 = 0;
    gui_audio_set_hifi_source(&source_a);
    assert(gui_audio_get_hifi_source() == &source_a);
    gui_audio_update_playback(&app); // Open the session before accepting RF PCM.
    assert(atomic_load(&s_monitor_session_active));
    for (size_t i = 0; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);
    processed_halves = 1;
    const float pcm[] = {-1.0f, 1.0f, NAN, INFINITY, -0.25f, 0.5f};
    const int16_t legacy[] = {123, 456};
    gui_audio_push_hifi(&source_b, pcm, 3);
    gui_audio_push_demod(legacy, 1);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_update_playback(&app);
    assert(stream_loads == 1 && stream_updates == 2 && updates_1024 == 2);
    assert(last_pcm[0] == INT16_MIN && last_pcm[1] == INT16_MAX);
    assert(last_pcm[2] == 0 && last_pcm[3] == 0);
    assert(last_pcm[4] == -8192 && last_pcm[5] == 16384);
    for (size_t i = 6; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);

    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_reset_hifi(&source_b);
    int16_t out[HIFI_Q_FRAMES * 2];
    assert(hifi_q_pop_samples(out, 16) == 6);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_reset_hifi(&source_a);
    gui_audio_update_playback(&app);
    assert(stream_unloads == 1 && stream_loads == 2);
    for (size_t i = 0; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);

    const size_t big_frames = HIFI_Q_FRAMES + 97;
    float *big = malloc(big_frames * 2 * sizeof(*big));
    assert(big);
    for (size_t i = 0; i < big_frames; ++i) {
        big[i * 2] = i < 97 ? -0.9f : 0.25f;
        big[i * 2 + 1] = i < 97 ? 0.9f : -0.5f;
    }
    gui_audio_push_hifi(&source_a, big, big_frames);
    assert(hifi_q_pop_samples(out, sizeof(out) / sizeof(*out)) == HIFI_Q_FRAMES * 2);
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) {
        assert(out[i * 2] == 8192 && out[i * 2 + 1] == -16384);
    }
    // Ordinary block overflow drops the oldest frames, including ring wrap.
    gui_audio_push_hifi(&source_a, big + 97 * 2, HIFI_Q_FRAMES);
    for (size_t i = 0; i < 128; ++i) {
        big[i * 2] = 0.75f;
        big[i * 2 + 1] = -0.75f;
    }
    gui_audio_push_hifi(&source_a, big, 128);
    assert(hifi_q_pop_samples(out, sizeof(out) / sizeof(*out)) == HIFI_Q_FRAMES * 2);
    for (size_t i = 0; i < HIFI_Q_FRAMES; ++i) {
        assert(out[i * 2] == (i < HIFI_Q_FRAMES - 128 ? 8192 : 24575));
        assert(out[i * 2 + 1] == (i < HIFI_Q_FRAMES - 128 ? -16384 : -24575));
    }
    free(big);

    assert(!atomic_flag_test_and_set(&s_hifi_q_guard));
    gui_audio_push_hifi(&source_a, pcm, 3);
    assert(hifi_q_pop_samples(out, 16) == 0);
    atomic_flag_clear(&s_hifi_q_guard);
    assert(hifi_q_pop_samples(out, 16) == 0);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_set_hifi_source(&source_b);
    gui_audio_push_hifi(&source_a, pcm, 3);
    assert(hifi_q_pop_samples(out, 16) == 0);
    int loads_before_switch = stream_loads;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads_before_switch + 1 && buffer_frames == 1024);
    for (size_t i = 0; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);
    gui_audio_set_hifi_source(NULL);
    gui_audio_push_demod(legacy, 1);
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == 123 && last_pcm[1] == 456);
    assert(buffer_frames == 4096 && updates_4096 == 1);
    for (size_t i = 2; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);

    gui_audio_set_hifi_source(&source_a);
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_set_playback_enabled(&app, false);
    gui_audio_push_hifi(&source_a, pcm, 3);
    int updates = stream_updates;
    gui_audio_update_playback(&app);
    assert(stream_updates == updates);
    loads_before_switch = stream_loads;
    gui_audio_set_playback_enabled(&app, true);
    gui_audio_update_playback(&app);
    assert(stream_loads == loads_before_switch + 1 && buffer_frames == 1024);
    for (size_t i = 0; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);
    loads_before_switch = stream_loads;
    gui_audio_set_hifi_source(&source_a); // Re-selecting the same token is stable.
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads_before_switch && buffer_frames == 1024);

    // Both processed HiFi halves must be refilled after a delayed GUI frame.
    float two_halves[2048 * 2];
    for (size_t i = 0; i < 2048; ++i) {
        two_halves[i * 2] = 0.25f;
        two_halves[i * 2 + 1] = -0.5f;
    }
    gui_audio_push_hifi(&source_a, two_halves, 2048);
    processed_halves = 2;
    updates = stream_updates;
    gui_audio_update_playback(&app);
    assert(stream_updates == updates + 2 && processed_halves == 0 && buffer_frames == 1024);
    for (size_t i = 0; i < 1024; ++i) {
        assert(last_pcm[i * 2] == 8192 && last_pcm[i * 2 + 1] == -16384);
    }
    assert(hifi_q_pop_samples(out, sizeof(out) / sizeof(*out)) == 0);

    // A DDD/playback-style stop only changes capture state. The pump must
    // stop the stream and reject late callbacks even though the source lives.
    gui_audio_push_hifi(&source_a, pcm, 3);
    app.is_capturing = false;
    int unloads_before_stop = stream_unloads;
    gui_audio_update_playback(&app);
    assert(!atomic_load(&s_monitor_session_active) && !s_play_stream_inited);
    assert(stream_unloads == unloads_before_stop + 1);
    gui_audio_push_hifi(&source_a, pcm, 3);
    thrd_t late_producer;
    assert(thrd_create(&late_producer, stress_push, NULL) == thrd_success);
    thrd_join(late_producer, NULL);
    assert(hifi_q_pop_samples(out, sizeof(out) / sizeof(*out)) == 0);
    atomic_store(&stress_done, false);
    app.is_capturing = true;
    gui_audio_push_hifi(&source_a, pcm, 3); // Reconnect before its first pump.
    loads_before_switch = stream_loads;
    gui_audio_update_playback(&app);
    assert(stream_loads == loads_before_switch + 1 && buffer_frames == 1024);
    assert(atomic_load(&s_monitor_session_active));
    for (size_t i = 0; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);
    gui_audio_push_hifi(&source_a, pcm, 3);
    processed_halves = 1;
    gui_audio_update_playback(&app);
    assert(last_pcm[0] == INT16_MIN && last_pcm[1] == INT16_MAX);

    // Explicit audio_stop closes the session even if capture is still true;
    // asynchronous pushes remain rejected until a fresh main-thread pump.
    gui_audio_push_hifi(&source_a, pcm, 3);
    gui_audio_stop(&app);
    assert(!atomic_load(&s_monitor_session_active));
    gui_audio_push_hifi(&source_a, pcm, 3);
    assert(hifi_q_pop_samples(out, sizeof(out) / sizeof(*out)) == 0);
    gui_audio_update_playback(&app);
    assert(atomic_load(&s_monitor_session_active) && buffer_frames == 1024);
    for (size_t i = 0; i < 4096 * 2; ++i) assert(last_pcm[i] == 0);

    thrd_t producer;
    assert(thrd_create(&producer, stress_push, NULL) == thrd_success);
    while (!atomic_load(&stress_done)) {
        gui_audio_reset_hifi(&source_a);
        gui_audio_set_hifi_source(&source_b);
        gui_audio_set_hifi_source(&source_a);
        hifi_q_pop_samples(out, sizeof(out) / sizeof(*out));
    }
    thrd_join(producer, NULL);
    assert(s_hifi_q_count <= HIFI_Q_FRAMES);
    gui_audio_stop(&app);
    assert(!s_play_stream_inited && stream_loads == stream_unloads);
    test_playback_sidecar_integration();
    puts("gui_audio_hifi_monitor: routing, finite PCM, bound, reset, mute, session restart, contention, sidecar seek passed");
    return 0;
}
