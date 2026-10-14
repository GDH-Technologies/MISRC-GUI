/* Exercise production playback sideband ownership through wrap, overflow,
 * seek epochs, final-block padding and concurrent producer/consumer copies. */
#include "../misrc_gui/processing/gui_extract.c"
#include "../misrc_gui/processing/gui_rf_stream_marks.c"
#include <assert.h>

/* Run the actual extraction start/loop/stop with controlled FIFO readiness.
 * Only its surrounding hardware/record/display services are replaced. */
atomic_int do_exit = 0;
static atomic_bool stop_test_continuous;
static atomic_bool stop_test_done;
static atomic_uint stop_test_reads;
static uint32_t stop_test_raw[BUFFER_READ_SIZE];
static atomic_bool record_test_active;
static atomic_bool record_test_input_ready;
static atomic_bool record_test_input_taken;
static atomic_bool record_test_preview_done;
static atomic_uint_fast64_t record_test_bytes[2];
static int16_t record_test_output[2][BUFFER_READ_SIZE];
static size_t record_test_preview_counts[2];

static void stop_test_convert(void *input, size_t count, size_t *clip, uint8_t *aux,
                               void *output_a, void *output_b, uint16_t *peak) {
    (void)input; (void)clip; (void)aux; (void)peak;
    if (atomic_load(&record_test_active)) {
        for (size_t i = 0; i < count; ++i) ((int16_t *)output_a)[i] = 111;
        if (output_b) for (size_t i = 0; i < count; ++i) ((int16_t *)output_b)[i] = 222;
    } else {
        memset(output_a, 0, count * sizeof(int16_t));
        if (output_b) memset(output_b, 0, count * sizeof(int16_t));
    }
    /* Input remains available throughout: this yields CPU, not FIFO data. */
    thrd_sleep_ms(1);
}
conv_function_t get_conv_function(bool single, bool pad, bool dword, bool peak,
                                  void *out_a, void *out_b) {
    (void)single; (void)pad; (void)dword; (void)peak; (void)out_a; (void)out_b;
    return stop_test_convert;
}
conv_16to8_t get_16to8_function(void) { return NULL; }
conv_16to32_t get_16to8to32_function(void) { return NULL; }
int rb_event_init(rb_event_t *event) { (void)event; return -1; }
void rb_event_destroy(rb_event_t *event) { (void)event; }
int bufmgr_ensure_init(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; return 0; }
void *bufmgr_read_begin(buffer_manager_t *mgr, buffer_id_t id, size_t bytes, int timeout_ms) {
    (void)mgr;
    assert(id == BUF_CAPTURE_RF && bytes == sizeof(stop_test_raw));
    atomic_fetch_add(&stop_test_reads, 1);
    if (atomic_load(&record_test_active)) {
        if (atomic_load(&record_test_input_ready) && !atomic_exchange(&record_test_input_taken, true))
            return stop_test_raw;
        thrd_sleep_ms(timeout_ms);
        return NULL;
    }
    if (atomic_load(&stop_test_continuous)) return stop_test_raw;
    thrd_sleep_ms(timeout_ms);
    return NULL;
}
void bufmgr_read_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    (void)mgr; (void)id; (void)bytes;
}
void *bufmgr_write_begin(buffer_manager_t *mgr, buffer_id_t id, size_t bytes,
                          const backpressure_policy_t *policy) {
    (void)mgr; (void)bytes; (void)policy;
    if (atomic_load(&record_test_active)) {
        if (id == BUF_RECORD_A) return record_test_output[0];
        if (id == BUF_RECORD_B) return record_test_output[1];
    }
    return NULL;
}
void bufmgr_write_end(buffer_manager_t *mgr, buffer_id_t id, size_t bytes) {
    (void)mgr;
    if (id == BUF_RECORD_A) atomic_fetch_add(&record_test_bytes[0], bytes);
    if (id == BUF_RECORD_B) atomic_fetch_add(&record_test_bytes[1], bytes);
}
size_t bufmgr_fill_level(buffer_manager_t *mgr, buffer_id_t id) { (void)mgr; (void)id; return 0; }
void bufmgr_log_periodic(buffer_manager_t *mgr) { (void)mgr; }
void gui_video_submit_rf(gui_app_t *app, const int16_t *a, const int16_t *b,
                          size_t count_a, size_t count_b, const gui_rf_block_info_t info[2], bool supported) {
    (void)app; (void)a; (void)b; (void)count_a; (void)count_b; (void)info; (void)supported;
    if (atomic_load(&record_test_active)) {
        record_test_preview_counts[0] = count_a;
        record_test_preview_counts[1] = count_b;
        atomic_store(&record_test_preview_done, true);
    }
}
bool gui_record_check_disk_space_guard(gui_app_t *app, uint32_t frame, char *msg, size_t capacity) {
    (void)app; (void)frame; (void)msg; (void)capacity; return false;
}
bool gui_record_spill_enqueue(gui_app_t *app, int channel, const void *samples, size_t bytes,
                              uint32_t frame, char *msg, size_t capacity) {
    (void)app; (void)channel; (void)samples; (void)bytes; (void)frame; (void)msg; (void)capacity;
    return false;
}
bool gui_record_spill_is_forced(int channel) { (void)channel; return false; }
void gui_record_spill_clear_forced(int channel) { (void)channel; }
void gui_record_log_capture_event(gui_app_t *app, const char *level, const char *message,
                                  gui_error_class_t error_class, uint32_t count) {
    (void)app; (void)level; (void)message; (void)error_class; (void)count;
}

static int stop_test_call_stop(void *unused) {
    (void)unused;
    gui_extract_stop();
    atomic_store(&stop_test_done, true);
    return 0;
}
static void stop_test_timeout(const char *condition) {
    fprintf(stderr, "Extraction stop regression timed out: %s\n", condition);
    /* Exit the entire test process, rather than joining a potentially stuck
     * regression thread. The test can never turn this failure into a hang. */
    exit(1);
}
static void stop_test_case(bool continuous) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = true; /* Exactly the startup-failure deadlock condition. */
    app->capture_has_channel_b = true;
    atomic_init(&app->rf_preview_input_supported, true);
    atomic_init(&app->total_samples, 0);
    atomic_init(&app->samples_a, 0);
    atomic_init(&app->samples_b, 0);
    atomic_init(&app->clip_count_a_pos, 0); atomic_init(&app->clip_count_a_neg, 0);
    atomic_init(&app->clip_count_b_pos, 0); atomic_init(&app->clip_count_b_neg, 0);
    atomic_init(&app->peak_a_pos, 0); atomic_init(&app->peak_a_neg, 0);
    atomic_init(&app->peak_b_pos, 0); atomic_init(&app->peak_b_neg, 0);
    atomic_store(&stop_test_continuous, continuous);
    atomic_store(&stop_test_done, false);
    atomic_store(&stop_test_reads, 0);
    assert(gui_extract_start_tagged(app) == 0);
    assert(s_buf_a && s_buf_b && s_buf_aux);

    uint64_t begin = get_time_ms();
    unsigned minimum_reads = continuous ? 3u : 1u;
    while (atomic_load(&stop_test_reads) < minimum_reads) {
        if (get_time_ms() - begin > 2000u) stop_test_timeout("reader did not start/restart");
        thrd_sleep_ms(1);
    }
    thrd_t stopper;
    assert(thrd_create_with_priority(&stopper, stop_test_call_stop, NULL, THRD_PRIORITY_NORMAL) == thrd_success);
    begin = get_time_ms();
    while (!atomic_load(&stop_test_done)) {
        if (get_time_ms() - begin > 2000u)
            stop_test_timeout(continuous ? "input continuously available" : "empty FIFO, is_capturing=true");
        thrd_sleep_ms(1);
    }
    thrd_join(stopper, NULL);
    assert(app->is_capturing && !gui_extract_is_running());
    /* A stop/start round trip must clear the prior independent stop request. */
    free(app);
}

/* A preview metadata failure must not silence raw recording or alter its A/B
 * swap. Known per-block channel presence also wins over a newer capability. */
static void record_test_case(bool tagged, bool metadata, bool unsafe,
                              bool block_has_b, bool capability_b, bool swap) {
    gui_app_t *app = calloc(1, sizeof(*app));
    assert(app);
    app->is_capturing = app->is_recording = true;
    app->capture_has_channel_b = capability_b;
    app->capture_backend_upstream = true; /* Keep both conversion outputs. */
    app->capture_mode_runtime_misrc = swap;
    app->settings.capture_a = app->settings.capture_b = true;
    atomic_init(&app->rf_preview_input_supported, true);
    atomic_store(&record_test_active, true);
    atomic_store(&record_test_input_ready, false);
    atomic_store(&record_test_input_taken, false);
    atomic_store(&record_test_preview_done, false);
    for (int ch = 0; ch < 2; ++ch) {
        atomic_store(&record_test_bytes[ch], 0);
        record_test_preview_counts[ch] = 0;
    }
    memset(record_test_output, 0, sizeof(record_test_output));
    gui_rf_block_info_t info[2] = {{20000000u, 0, 1, unsafe}, {20000000u, 0, 1, unsafe}};
    if (!block_has_b) info[1].sample_rate_hz = 0;
    size_t counts[2] = {BUFFER_READ_SIZE, block_has_b ? BUFFER_READ_SIZE : 0};
    assert(s_rf_marks == NULL);
    if (!tagged && metadata) {
        s_rf_marks = gui_rf_stream_marks_create();
        assert(s_rf_marks);
        gui_rf_stream_marks_observe(s_rf_marks, 0, sizeof(stop_test_raw), info, unsafe);
    }
    assert((tagged ? gui_extract_start_tagged(app) : gui_extract_start(app)) == 0);
    if (tagged && metadata) assert(gui_extract_stamp_playback(info, counts));
    gui_extract_set_recording(true, false, 16, 16, false, false);
    atomic_store(&record_test_input_ready, true);
    uint64_t until = get_time_ms() + 2000;
    while (!atomic_load(&record_test_preview_done)) {
        if (get_time_ms() >= until) stop_test_timeout("record/preview isolation block");
        thrd_sleep_ms(1);
    }
    gui_extract_stop();
    bool expected_b = metadata ? block_has_b : capability_b;
    bool expected_swap = expected_b && swap;
    assert(atomic_load(&record_test_bytes[0]) == sizeof(record_test_output[0]));
    assert(atomic_load(&record_test_bytes[1]) == (expected_b ? sizeof(record_test_output[1]) : 0));
    for (size_t i = 0; i < BUFFER_READ_SIZE; ++i) {
        assert(record_test_output[0][i] == (expected_swap ? 222 : 111));
        assert(record_test_output[1][i] == (expected_b ? (expected_swap ? 111 : 222) : 0));
    }
    bool preview_safe = metadata && (tagged || !unsafe);
    assert(record_test_preview_counts[0] == (preview_safe ? (expected_swap ? counts[1] : counts[0]) : 0));
    assert(record_test_preview_counts[1] == (preview_safe ? (expected_swap ? counts[0] : counts[1]) : 0));
    gui_rf_stream_marks_destroy(s_rf_marks);
    s_rf_marks = NULL;
    atomic_store(&record_test_active, false);
    free(app);
}

#define STAMP_TEST_BLOCKS 100000u
static void stamp_test_data(uint64_t n, gui_rf_block_info_t info[2], size_t counts[2]) {
    for (int ch=0; ch<2; ++ch) {
        info[ch] = (gui_rf_block_info_t){
            .sample_rate_hz = (uint32_t[]){20000000,40000000,80000000}[n%3],
            .start_sample = (n%1000)*65536u + ch,
            .epoch = n/1000 + ch*10000u,
            .discontinuity = n%1000 == 0
        };
        counts[ch] = n%1000 == 999 ? 1234u : 65536u;
    }
}
static void stamp_test_check(uint64_t n, const rf_input_stamp_t *stamp) {
    gui_rf_block_info_t info[2]; size_t counts[2];
    stamp_test_data(n,info,counts);
    for (int ch=0;ch<2;++ch) {
        assert(stamp->info[ch].sample_rate_hz == info[ch].sample_rate_hz);
        assert(stamp->info[ch].start_sample == info[ch].start_sample);
        assert(stamp->info[ch].epoch == info[ch].epoch);
        assert(stamp->info[ch].discontinuity == info[ch].discontinuity);
        assert(stamp->counts[ch] == counts[ch]);
    }
}
static int stamp_test_producer(void *unused) {
    (void)unused;
    for(uint64_t n=0;n<STAMP_TEST_BLOCKS;++n) {
        gui_rf_block_info_t info[2]; size_t counts[2]; stamp_test_data(n,info,counts);
        while(!gui_extract_stamp_playback(info,counts)) { }
    }
    return 0;
}
int main(void) {
    s_tagged_input=true;
    atomic_store(&s_stamp_write,0); atomic_store(&s_stamp_read,0);
    for(uint64_t n=0;n<RF_STAMP_CAPACITY;++n) {
        gui_rf_block_info_t info[2]; size_t counts[2]; stamp_test_data(n,info,counts);
        assert(gui_extract_stamp_playback(info,counts));
    }
    gui_rf_block_info_t extra[2]={{0}}; size_t extra_counts[2]={1,1};
    assert(!gui_extract_stamp_playback(extra,extra_counts));
    for(uint64_t n=0;n<RF_STAMP_CAPACITY;++n) {
        rf_input_stamp_t stamp; assert(read_rf_stamp(&stamp)); stamp_test_check(n,&stamp);
    }
    rf_input_stamp_t stamp; assert(!read_rf_stamp(&stamp));
    atomic_store(&s_stamp_write,0); atomic_store(&s_stamp_read,0);
    thrd_t producer;
    assert(thrd_create_with_priority(&producer,stamp_test_producer,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    for(uint64_t n=0;n<STAMP_TEST_BLOCKS;++n) {
        while(!read_rf_stamp(&stamp)) { }
        stamp_test_check(n,&stamp);
    }
    thrd_join(producer,NULL);
    assert(!read_rf_stamp(&stamp));
    s_tagged_input=false;
    assert(!gui_extract_stamp_playback(extra,extra_counts));
    puts("RF playback stamps: wrap, overflow, seek, padding and concurrent ownership passed");
    stop_test_case(false);
    stop_test_case(true);
    record_test_case(false, true, false, true, true, false);
    record_test_case(false, false, false, true, true, false);
    record_test_case(false, false, false, true, true, true);
    record_test_case(false, false, false, false, false, true);
    record_test_case(false, true, true, true, false, true);
    record_test_case(false, true, true, false, true, true);
    record_test_case(true, true, false, true, true, true);
    record_test_case(true, false, false, true, true, false);
    record_test_case(true, false, false, true, true, true);
    record_test_case(true, true, true, false, true, true);
    puts("Extraction recording/preview isolation: missing metadata, known unsafe mapping and A/B swap passed");
    gui_extract_cleanup();
    puts("Extraction actual-thread stop: empty and continuously available input, is_capturing=true passed");
    return 0;
}
