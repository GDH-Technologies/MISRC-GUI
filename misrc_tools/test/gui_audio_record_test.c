/* WAV recording through upstream's record API on the production audio thread:
 * gui_audio_begin_recording -> gui_audio_request_record_stop ->
 * gui_audio_wait_recording_idle. A fake BUF_CAPTURE_AUDIO hands the real
 * audio_thread_main a byte stream, and real files are written to a scratch
 * directory. Proves the START discard, the bytes written while recording, the
 * STOP tail flush, the header rewritten at close, exclusive create of an
 * unconfirmed output, and that a thread exit under an active recording is a
 * failed WAV. (Fork, 2026-11-17: the pre-adopted API had no test upstream's
 * record path could not reach.) */
#ifdef LIBSOXR_ENABLED
#undef LIBSOXR_ENABLED
#endif
#define LIBSOXR_ENABLED 0
#include "../misrc_gui/output/gui_audio.c"
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

atomic_int do_exit;
static gui_app_t app;

/* ----- fake capture ring: a byte counter over one patterned block ----- */
static uint8_t s_block[BUFFER_AUDIO_READ_SIZE];
static atomic_size_t s_available;
static atomic_size_t s_consumed;

int bufmgr_ensure_init(buffer_manager_t *m, buffer_id_t id)
{ (void)m; assert(id == BUF_CAPTURE_AUDIO); return 0; }
void *bufmgr_read_begin(buffer_manager_t *m, buffer_id_t id, size_t n, int timeout)
{
    (void)m; assert(id == BUF_CAPTURE_AUDIO && n <= sizeof(s_block));
    if (atomic_load(&s_available) >= n) return s_block;
    if (timeout > 0) thrd_sleep_ms(1);
    return NULL;
}
void bufmgr_read_end(buffer_manager_t *m, buffer_id_t id, size_t n)
{
    (void)m; assert(id == BUF_CAPTURE_AUDIO);
    atomic_fetch_sub(&s_available, n);
    atomic_fetch_add(&s_consumed, n);
}
size_t bufmgr_fill_level(buffer_manager_t *m, buffer_id_t id)
{ (void)m; (void)id; return atomic_load(&s_available); }

/* ----- the rest of gui_audio.c's world ----- */
void gui_settings_save(const gui_settings_t *settings) { (void)settings; }
void gui_app_set_status(gui_app_t *a, const char *s) { (void)a; (void)s; }
bool gui_monitor_server_active(void) { return false; }
void extract_audio_2ch_C(uint16_t *in, size_t n, uint16_t *a, uint16_t *b)
{ (void)in; (void)n; (void)a; (void)b; assert(false); }
void extract_audio_1ch_C(uint8_t *in, size_t n, uint8_t *a, uint8_t *b, uint8_t *c, uint8_t *d)
{ (void)in; (void)n; (void)a; (void)b; (void)c; (void)d; assert(false); }
void InitAudioDevice(void) {}
bool IsAudioDeviceReady(void) { return true; }
void SetAudioStreamBufferSizeDefault(int frames) { (void)frames; }
AudioStream LoadAudioStream(unsigned int rate, unsigned int bits, unsigned int channels)
{ (void)rate; (void)bits; (void)channels; return (AudioStream){0}; }
bool IsAudioStreamValid(AudioStream stream) { return stream.buffer != NULL; }
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback) { (void)stream; (void)callback; }
void PlayAudioStream(AudioStream stream) { (void)stream; }
void StopAudioStream(AudioStream stream) { (void)stream; }
void UnloadAudioStream(AudioStream stream) { (void)stream; }
bool IsAudioStreamPlaying(AudioStream stream) { (void)stream; return false; }
bool IsAudioStreamProcessed(AudioStream stream) { (void)stream; return false; }
void UpdateAudioStream(AudioStream stream, const void *pcm, int frames) { (void)stream; (void)pcm; (void)frames; }

/* gui_audio.c routes stdio through the Local Save proxy; here it is plain
 * stdio (glibc and the MinGW CRT both take "wbx" as an exclusive create). */
#undef fwrite
#undef fclose
#undef fflush
#undef ferror
#undef clearerr
#undef file_open_utf8
FILE *mls_proxy_open(const char *p, const char *m) { return fopen(p, m); }
size_t mls_proxy_write(const void *d, size_t s, size_t n, FILE *f) { return fwrite(d, s, n, f); }
int mls_proxy_close(FILE *f) { return fclose(f); }
int mls_proxy_flush(FILE *f) { return fflush(f); }
int mls_proxy_error(FILE *f) { return ferror(f); }
void mls_proxy_clearerr(FILE *f) { clearerr(f); }

/* ----- helpers ----- */
static char s_dir[512];
static char s_paths[GUI_AUDIO_RECORD_OUTPUT_COUNT][GUI_AUDIO_RECORD_PATH_MAX];

static void wait_for(bool (*cond)(void), const char *what)
{
    uint64_t end = get_time_ms() + 5000;
    while (!cond()) {
        if (get_time_ms() > end) { printf("FAIL: timed out waiting for %s\n", what); exit(1); }
        thrd_sleep_ms(1);
    }
}
static bool ring_empty(void) { return atomic_load(&s_available) == 0; }
static bool quad_open(void) { return s_audio_ctx.f_4ch != NULL; }

static long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void only_quad(const char *name)
{
    for (int i = 0; i < GUI_AUDIO_RECORD_OUTPUT_COUNT; i++) s_paths[i][0] = '\0';
    snprintf(s_paths[0], sizeof(s_paths[0]), "%s/%s", s_dir, name);
}

static void expect(bool ok, const char *what)
{
    if (!ok) { printf("FAIL: %s\n", what); exit(1); }
    printf("  ok: %s\n", what);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(s_dir, sizeof(s_dir), "%s/misrc_audio_record_XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(s_dir)) { printf("FAIL: mkdtemp: %s\n", strerror(errno)); return 1; }
    for (size_t i = 0; i < sizeof(s_block); i++) s_block[i] = (uint8_t)(i * 13 + 7);

    app.is_capturing = true;
    app.device_count = 1; app.selected_device = 0; app.devices[0].type = DEVICE_TYPE_CXADC;
    atomic_store(&app.audio_sample_rate, 46875);
    expect(gui_audio_start(&app, &app.buffers) == 0, "audio thread starts monitor-only (no WAV opened)");
    expect(!quad_open(), "no output is open before gui_audio_begin_recording");

    /* 1. A recording: pre-record audio is discarded, two full blocks and a
     *    576 B-granular tail are written, the header carries the length. */
    const size_t pre = 576 * 10, tail = 576 * 3, body = 2 * (size_t)BUFFER_AUDIO_READ_SIZE;
    atomic_store(&s_available, pre);
    only_quad("quad.wav");
    gui_audio_begin_recording(&app, (const char (*)[GUI_AUDIO_RECORD_PATH_MAX])s_paths, 0);
    wait_for(quad_open, "the START transition to open the WAV");
    wait_for(ring_empty, "the START discard");
    atomic_fetch_add(&s_available, body);
    wait_for(ring_empty, "the recorded blocks");
    atomic_fetch_add(&s_available, tail);
    gui_audio_request_record_stop(&app);
    expect(gui_audio_wait_recording_idle(), "a clean stop reports success");
    long size = file_size(s_paths[0]);
    expect(size == (long)(sizeof(wave_header_t) + body + tail),
           "WAV = header + the recorded blocks + the flushed tail (pre-record audio discarded)");
    FILE *f = fopen(s_paths[0], "rb");
    wave_header_t h; memset(&h, 0, sizeof(h));
    expect(f && fread(&h, 1, sizeof(h), f) == sizeof(h), "header readable");
    uint8_t first[8]; expect(fread(first, 1, sizeof(first), f) == sizeof(first), "payload readable");
    fclose(f);
    expect(h.riff == WAVE_RIFF && h.data == WAVE_data && h.channelCount == 4 &&
           h.bitsPerSample == 24 && h.sampleRate == 46875 && h.dataSize == body + tail,
           "header rewritten at close: RIFF, 4 ch, 24 bit, 46875 Hz, data size = payload");
    expect(memcmp(first, s_block, sizeof(first)) == 0, "payload is the captured bytes");
    expect(!quad_open() && gui_audio_is_running(), "stop closed the file and kept the thread (monitor) running");

    /* 2. An unconfirmed output that already exists is created exclusively:
     *    the open fails, the file is untouched, and the stop reports failure. */
    only_quad("sentinel.wav");
    f = fopen(s_paths[0], "wb"); fputs("KEEP", f); fclose(f);
    atomic_store(&s_available, 576);
    gui_audio_begin_recording(&app, (const char (*)[GUI_AUDIO_RECORD_PATH_MAX])s_paths, 0);
    wait_for(ring_empty, "the START discard (exclusive case)");
    gui_audio_request_record_stop(&app);
    expect(!gui_audio_wait_recording_idle(), "exclusive create refused an existing file: stop reports failure");
    expect(file_size(s_paths[0]) == 4, "the existing file was left alone");

    /* 3. The same file with its overwrite bit set is replaced. */
    gui_audio_begin_recording(&app, (const char (*)[GUI_AUDIO_RECORD_PATH_MAX])s_paths, 1u);
    wait_for(quad_open, "the START transition (overwrite case)");
    gui_audio_request_record_stop(&app);
    expect(gui_audio_wait_recording_idle(), "a confirmed overwrite succeeds");
    expect(file_size(s_paths[0]) == (long)sizeof(wave_header_t), "the confirmed file was replaced");

    /* 4. The thread exits under an ACTIVE recording (capture lost): the WAV
     *    is closed with its header, and the recording counts as failed. */
    only_quad("cut.wav");
    gui_audio_begin_recording(&app, (const char (*)[GUI_AUDIO_RECORD_PATH_MAX])s_paths, 0);
    wait_for(quad_open, "the START transition (cut case)");
    gui_audio_stop(&app);
    expect(!gui_audio_wait_recording_idle(), "a thread exit while ACTIVE is a failed WAV");
    expect(file_size(s_paths[0]) == (long)sizeof(wave_header_t), "the cut WAV still got its header");

    /* 5. No thread at all: a stop completes at once (nobody opened anything). */
    for (int i = 0; i < GUI_AUDIO_RECORD_OUTPUT_COUNT; i++) s_paths[i][0] = '\0';
    gui_audio_begin_recording(&app, (const char (*)[GUI_AUDIO_RECORD_PATH_MAX])s_paths, 0);
    gui_audio_request_record_stop(&app);
    expect(gui_audio_wait_recording_idle(), "with no audio thread the stop completes immediately");

    for (const char *n = "quad.wav\0sentinel.wav\0cut.wav\0"; *n; n += strlen(n) + 1) {
        char p[600]; snprintf(p, sizeof(p), "%s/%s", s_dir, n); remove(p);
    }
    rmdir(s_dir);
    puts("gui_audio_record: begin/stop/wait, discard + tail flush, header, exclusive create, cut recording passed");
    return 0;
}
