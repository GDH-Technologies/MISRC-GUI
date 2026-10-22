/*
 * MISRC hsdaoh-rp2350 GUI - Sample Extraction and Display Processing
 *
 * Continuous extraction thread that runs from capture start to capture stop.
 * - Always reads from capture ringbuffer
 * - Always updates display buffers for GUI
 * - When recording enabled, also writes to record ringbuffers
 */

#include "gui_extract.h"
#include "gui_rf_stream_marks.h"
#include "../core/gui_app.h"
#include "../output/gui_record.h"
#include "../visualization/gui_oscilloscope.h"
#include "../signal/gui_cvbs.h"
#include "../../common/extract.h"
#include "../../common/ringbuffer.h"
#include "../../common/rb_event.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"
#include "../../common/buffer.h"
#include "../../common/misrc_debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

// External do_exit flag from ringbuffer.h
extern atomic_int do_exit;

// Buffer sizes
#define BUFFER_READ_SIZE 65536

// Extraction buffers (page-aligned for SSE/AVX)
static int16_t *s_buf_a = NULL;
static int16_t *s_buf_b = NULL;
static uint8_t *s_buf_aux = NULL;
static conv_function_t s_extract_fn = NULL;
static bool s_extract_fn_ab = false;  // s_extract_fn also unpacks channel B
static bool s_initialized = false;

static bool s_b_present = true; // Runtime capability set at capture start

// Scratch buffers for recording conversions
static int8_t *s_tmp8_a = NULL;
static int8_t *s_tmp8_b = NULL;
static int32_t *s_tmp32_a = NULL;
static int32_t *s_tmp32_b = NULL;

// Note: Record ringbuffers now managed by buffer manager (BUF_RECORD_A, BUF_RECORD_B)

// Extraction thread state
static thrd_t s_extract_thread;
static bool s_extract_thread_running = false;
static atomic_bool s_extract_stop_requested = false;
static gui_app_t *s_extract_app = NULL;
// Note: s_capture_rb removed - now using app->buffers (buffer_manager)

// Recording state (atomic for thread-safe access)
static atomic_bool s_recording_enabled = false;
static atomic_bool s_use_flac = false;
static atomic_uchar s_rf_bits_a = 16;
static atomic_uchar s_rf_bits_b = 16;
// Direct native RAW channels: their record ringbuffer is fed by the
// capture-side tap (gui_record_direct_tap_push), so the extraction thread
// must not write those rings (single producer per ring).
static atomic_bool s_direct_channel_a = false;
static atomic_bool s_direct_channel_b = false;

// Extraction-thread-only: whether the previous iteration was inside an open
// record gate, and the capture-buffer drop count it had seen (so only drops
// that happen while recording are reported against the recording).
static bool s_record_gate_seen = false;
static uint32_t s_capture_drops_seen = 0;

static conv_16to8_t s_conv_16to8 = NULL;
static conv_16to32_t s_conv_16to8to32 = NULL;

// Event signaling for producer/consumer synchronization
static rb_event_t s_data_event;       // Signaled when new data is available in ringbuffer
static rb_event_t s_space_event;      // Signaled when space becomes available in ringbuffer
static bool s_events_initialized = false;

// Note: Record buffer events now managed by buffer manager

// Periodic buffer stats logging interval (in frames, ~1 frame = 65536 samples at 40MHz = 1.64ms)
// Log every ~2 seconds = 1220 frames
#define BUFMGR_LOG_INTERVAL 1220

// Sideband stamps stay aligned with the playback RF FIFO across seek, loop,
// padding and queue drops. 8192 blocks exceed the maximum 1 GiB RF FIFO.
#define RF_STAMP_CAPACITY 8192u
typedef struct { gui_rf_block_info_t info[2]; size_t counts[2]; } rf_input_stamp_t;
static rf_input_stamp_t s_rf_stamps[RF_STAMP_CAPACITY];
static atomic_uint_fast64_t s_stamp_write, s_stamp_read;
static bool s_tagged_input;
bool gui_extract_stamp_playback(const gui_rf_block_info_t info[2], const size_t counts[2]) {
    uint64_t write = atomic_load_explicit(&s_stamp_write, memory_order_relaxed);
    uint64_t read = atomic_load_explicit(&s_stamp_read, memory_order_acquire);
    if (!s_tagged_input || write-read >= RF_STAMP_CAPACITY) return false;
    rf_input_stamp_t *stamp = &s_rf_stamps[write % RF_STAMP_CAPACITY];
    memcpy(stamp->info, info, sizeof(stamp->info));
    memcpy(stamp->counts, counts, sizeof(stamp->counts));
    atomic_store_explicit(&s_stamp_write, write+1, memory_order_release);
    return true;
}
static bool read_rf_stamp(rf_input_stamp_t *stamp) {
    uint64_t read = atomic_load_explicit(&s_stamp_read, memory_order_relaxed);
    if (read == atomic_load_explicit(&s_stamp_write, memory_order_acquire)) return false;
    *stamp = s_rf_stamps[read % RF_STAMP_CAPACITY];
    atomic_store_explicit(&s_stamp_read, read+1, memory_order_release);
    return true;
}
static gui_rf_stream_marks_t *s_rf_marks;
static bool s_rf_observer_seen, s_rf_seen_b;
static uint32_t s_rf_seen_parser, s_rf_seen_missed, s_rf_seen_drops;
static uint64_t s_rf_seen_epoch;
static void observe_rf_write(void *context, uint64_t byte_position, size_t bytes) {
    gui_app_t *app = context;
    if (!app || !s_rf_marks) return;
    uint32_t parser = atomic_load(&app->parser_error_count);
    uint32_t missed = atomic_load(&app->missed_frame_count);
    uint32_t drops = atomic_load(&app->buffers.stats[BUF_CAPTURE_RF].write_drops);
    uint64_t epoch = atomic_load(&app->rf_stream_epoch);
    bool has_b = app->capture_has_channel_b;
    bool gap = !s_rf_observer_seen || (has_b != s_rf_seen_b || parser != s_rf_seen_parser || missed != s_rf_seen_missed ||
                                    drops != s_rf_seen_drops || epoch != s_rf_seen_epoch);
    if (gap && epoch == s_rf_seen_epoch) epoch = atomic_fetch_add(&app->rf_stream_epoch,1)+1;
    gui_rf_block_info_t info[2] = {{0}};
    uint32_t shared_rate = atomic_load(&app->sample_rate);
    for (int ch=0; ch<2; ++ch) {
        info[ch].sample_rate_hz = atomic_load(&app->rf_source_rate_hz[ch]);
        if (!info[ch].sample_rate_hz) info[ch].sample_rate_hz = shared_rate;
        info[ch].epoch = epoch;
    }
    if (!has_b) info[1].sample_rate_hz = 0;
    // A parser may compact valid payload around an unknown position in this
    // write. Exclude that entire write from preview; recording is untouched.
    gui_rf_stream_marks_observe(s_rf_marks,byte_position,bytes,info,gap);
    s_rf_observer_seen = true; s_rf_seen_b = has_b;
    s_rf_seen_parser = parser; s_rf_seen_missed = missed; s_rf_seen_drops = drops; s_rf_seen_epoch = epoch;
}
void gui_extract_prepare_stream(gui_app_t *app) {
    if (!app || app->is_capturing || s_extract_thread_running) return;
    bufmgr_reset(&app->buffers,BUF_CAPTURE_RF);
    bufmgr_reset(&app->buffers,BUF_DISPLAY);
    atomic_store(&s_stamp_write,0); atomic_store(&s_stamp_read,0);
    if (!s_rf_marks) s_rf_marks = gui_rf_stream_marks_create();
    gui_rf_stream_marks_reset(s_rf_marks);
    s_rf_observer_seen = false;
    atomic_fetch_add(&app->rf_stream_epoch,1);
    atomic_store(&app->rf_source_rate_hz[0],0); atomic_store(&app->rf_source_rate_hz[1],0);
    atomic_store(&app->rf_preview_input_supported,true);
    // No extraction producer is running here. Clear pressure from the prior
    // session so a new capture does not inherit its frozen-preview state.
    const gui_rf_block_info_t empty_info[2] = {{0}};
    gui_video_submit_rf(app,NULL,NULL,0,0,empty_info,true);
    app->buffers.rf_write_observer = observe_rf_write;
    app->buffers.rf_write_observer_context = app;
}
// Extraction thread - runs continuously from capture start to stop
// Always updates display/stats, conditionally writes to record ringbuffers
static int extraction_thread(void *ctx) {
    (void)ctx;
    size_t read_size = BUFFER_READ_SIZE * 4;  // 4 bytes per sample pair
    size_t clip[2] = {0, 0};
    uint16_t peak[2] = {0, 0};
    uint32_t frame_count = 0;
    uint64_t source_offset = 0;
    bool have_mapping = false, previous_swap = false, previous_b = false;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);
    /* Best-effort I/O level 0: this thread writes the record spill file when
     * the writers fall behind. */
    thrd_set_io_priority(THRD_IOPRIO(THRD_IOPRIO_CLASS_BE, 0));

    fprintf(stderr, "[EXTRACT] Continuous extraction thread started\n");

    while (1) {
        // Check for exit
        if (atomic_load(&s_extract_stop_requested) || atomic_load(&do_exit)) {
            break;
        }
        const int16_t *mapped_a = NULL;
        const int16_t *mapped_b = NULL;
        bool recording_enabled = false;
        uint32_t ui_stride = 1;

        // Try to read from capture ringbuffer via buffer manager
        void *buf = bufmgr_read_begin(&s_extract_app->buffers, BUF_CAPTURE_RF, read_size, 20);
        if (!buf) {
            // No data available yet - check if we should exit
            if (!s_extract_app || !s_extract_app->is_capturing) {
                break;
            }
            // Buffer manager already waits with timeout, just continue loop
            continue;
        }

        rf_input_stamp_t stamp = {0};
        bool preview_safe;
        bool block_has_b = s_extract_app->capture_has_channel_b;
        if (s_tagged_input) {
            preview_safe = read_rf_stamp(&stamp);
            if (preview_safe) block_has_b = stamp.counts[1] != 0;
        } else {
            preview_safe = gui_rf_stream_marks_read(s_rf_marks,source_offset*4,read_size,stamp.info);
            // An unsafe RF range can still carry known channel mapping. With
            // no metadata, preserve capture capability for raw recording.
            if (stamp.info[0].sample_rate_hz || stamp.info[1].sample_rate_hz)
                block_has_b = stamp.info[1].sample_rate_hz != 0;
            stamp.counts[0] = stamp.counts[1] = BUFFER_READ_SIZE;
        }
        if (!preview_safe) {
            stamp.counts[0] = stamp.counts[1] = 0;
            stamp.info[0].discontinuity = stamp.info[1].discontinuity = true;
        }
        // Extract samples (always - this is the core work)
        // Channel B can join or leave mid-run (RF B toggled on a connected
        // two-card CXADC rig, upstream dual-ADC auto-detection on the first
        // stream_id=1 callback), so follow the flag with the unpack function.
        // The upstream backend always unpacks A+B. An A-only function never
        // fills s_buf_b, so B is present only when the function unpacks it.
        // Prefer known per-block presence over the producer's newest flag, but
        // preview-only metadata failures must not silence the recorded B data.
        bool want_ab = s_extract_app &&
                       (block_has_b || s_extract_app->capture_has_channel_b ||
                        s_extract_app->capture_backend_upstream);
        if (want_ab != s_extract_fn_ab) {
            s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, want_ab ? (void*)1 : NULL);
            s_extract_fn_ab = want_ab;
        }
        s_b_present = s_extract_fn_ab && block_has_b;
        s_extract_fn((uint32_t*)buf, BUFFER_READ_SIZE, clip, s_buf_aux, s_buf_a, s_buf_b, peak);
        if (!s_b_present && s_extract_fn_ab) {
            // Prevent stale/uninitialized data from driving CH-B meters/display.
            // An A-only function never writes s_buf_b, and mapped_b stays NULL.
            memset(s_buf_b, 0, BUFFER_READ_SIZE * sizeof(int16_t));
        }
        bool swap_channels = false;
        if (s_extract_app) {
            swap_channels = s_extract_app->is_recording
                            ? s_extract_app->capture_mode_runtime_misrc
                            : s_extract_app->user_capture_mode_misrc;
            // Hardware-variant override: some MISRC V1.5/V2.5 setups have the
            // analog channels wired opposite to the default MISRC mapping.
            // In that case, invert MISRC swap behavior so GUI/record channels
            // match the physical A/B inputs. Read from the main-thread flag,
            // not the settings field: on a net client the settings struct is
            // swapped for the server's copy during the UI pass, and the flag
            // is what the server's snapshot says either way.
            if (swap_channels && s_extract_app->capture_ab_swap_invert) {
                swap_channels = false;
            }
        }
        bool mapping_gap = have_mapping && (previous_swap != swap_channels || previous_b != s_b_present);
        have_mapping = true; previous_swap = swap_channels; previous_b = s_b_present;
        if (s_b_present && swap_channels) {
            mapped_a = s_buf_b;
            mapped_b = s_buf_a;
        } else {
            mapped_a = s_buf_a;
            mapped_b = s_b_present ? s_buf_b : NULL;
        }

        // Mark capture buffer as consumed via buffer manager
        bufmgr_read_end(&s_extract_app->buffers, BUF_CAPTURE_RF, read_size);
        frame_count++;
        recording_enabled = atomic_load(&s_recording_enabled);
        // Record: int16 blocks into each channel's ordered record FIFO
        // (record ring first, local spill for overflow; gui_record_fifo.h).
        // The writer threads handle RF bit depth conversion and optional
        // soxr resampling. Nothing in here may wait or touch the output
        // filesystem: this thread must keep pace with the card.
        if (recording_enabled) {
            bool dropped = false;
            if (gui_record_producer_enter()) {
                // Capture-buffer drops since the last iteration are real
                // gaps in the RF: report them against the recording.
                uint32_t cap_drops = atomic_load(&s_extract_app->buffers.stats[BUF_CAPTURE_RF].write_drops);
                if (!s_record_gate_seen) {
                    s_record_gate_seen = true;
                    s_capture_drops_seen = cap_drops;
                } else if (cap_drops != s_capture_drops_seen) {
                    if (cap_drops > s_capture_drops_seen) {
                        gui_record_note_capture_drops(s_extract_app,
                                                      cap_drops - s_capture_drops_seen, frame_count);
                    }
                    s_capture_drops_seen = cap_drops;
                }

                char disk_guard_msg[640] = {0};
                if (gui_record_check_disk_space_guard(s_extract_app, frame_count,
                                                      disk_guard_msg, sizeof(disk_guard_msg))) {
                    atomic_store(&s_recording_enabled, false);
                    recording_enabled = false;
                    if (!atomic_load(&s_extract_app->dropout_stop_requested)) {
                        atomic_store(&s_extract_app->dropout_stop_reason, GUI_DROPOUT_DISK_SPACE);
                        atomic_store(&s_extract_app->dropout_stop_requested, true);
                    }
                    if (misrc_debug_enabled() && disk_guard_msg[0]) {
                        fprintf(stderr, "[EXTRACT] %s", disk_guard_msg);
                        fputc('\n', stderr);
                    }
                } else {
                    size_t block_bytes = BUFFER_READ_SIZE * sizeof(int16_t);
                    // A channel the recording does not include has no FIFO
                    // (push returns IDLE), so the session's latched channel
                    // choice decides, never the live settings. Direct native
                    // channels are recorded by the CXADC capture-side tap;
                    // skipping them keeps one producer per record FIFO.
                    if (!atomic_load(&s_direct_channel_a) &&
                        gui_record_push(0, mapped_a, block_bytes, frame_count) == GUI_RECORD_PUSH_DROPPED) {
                        dropped = true;
                    }
                    if (s_b_present && mapped_b && !atomic_load(&s_direct_channel_b) &&
                        gui_record_push(1, mapped_b, block_bytes, frame_count) == GUI_RECORD_PUSH_DROPPED) {
                        dropped = true;
                    }
                }
            } else {
                s_record_gate_seen = false;
            }
            // Always leave, before any exit path: gui_record_stop waits for
            // the gate to go idle.
            gui_record_producer_leave();

            // The FIFO has already logged and counted the loss.
            if (dropped &&
                s_extract_app->settings.stop_on_dropout &&
                !atomic_load(&s_extract_app->dropout_stop_requested)) {
                atomic_store(&s_extract_app->dropout_stop_reason, GUI_DROPOUT_BACKPRESSURE);
                atomic_store(&s_extract_app->dropout_stop_requested, true);
            }
            if (dropped && atomic_load(&do_exit)) {
                goto exit_thread;
            }
        } else {
            s_record_gate_seen = false;
        }

        if (recording_enabled) {
            bool record_a = s_extract_app->settings.capture_a;
            bool record_b = s_b_present && s_extract_app->settings.capture_b;
            unsigned int max_fill_pct = 0;
            if (record_a) {
                size_t cap_a = s_extract_app->buffers.buffers[BUF_RECORD_A].buffer_size;
                if (cap_a > 0) {
                    size_t fill_a = bufmgr_fill_level(&s_extract_app->buffers, BUF_RECORD_A);
                    unsigned int pct_a = (unsigned int)((fill_a * 100) / cap_a);
                    if (pct_a > max_fill_pct) max_fill_pct = pct_a;
                }
            }
            if (record_b) {
                size_t cap_b = s_extract_app->buffers.buffers[BUF_RECORD_B].buffer_size;
                if (cap_b > 0) {
                    size_t fill_b = bufmgr_fill_level(&s_extract_app->buffers, BUF_RECORD_B);
                    unsigned int pct_b = (unsigned int)((fill_b * 100) / cap_b);
                    if (pct_b > max_fill_pct) max_fill_pct = pct_b;
                }
            }

            if (max_fill_pct >= 85) {
                ui_stride = 8;
            } else if (max_fill_pct >= 70) {
                ui_stride = 4;
            } else if (max_fill_pct >= 50) {
                ui_stride = 2;
            } else {
                ui_stride = 1;
            }
        }

        // Signal that space is now available (buffer manager handles events internally)

        // Write extracted samples to display buffer (lossy - OK to drop for display)
        // Display thread handles CVBS decode + oscilloscope processing
        // Frame layout: [samples_a: BUFFER_READ_SIZE * 2 bytes] [samples_b: BUFFER_READ_SIZE * 2 bytes]
        if ((frame_count % ui_stride) == 0) {
            size_t display_frame_size = BUFFER_READ_SIZE * sizeof(int16_t) * 2;
            uint8_t *display_buf = bufmgr_write_begin(&s_extract_app->buffers, BUF_DISPLAY,
                                                    display_frame_size, NULL);
            if (display_buf) {
                    memcpy(display_buf, mapped_a, BUFFER_READ_SIZE * sizeof(int16_t));

                if (mapped_b) {
                    memcpy(display_buf + BUFFER_READ_SIZE * sizeof(int16_t),
                    mapped_b, BUFFER_READ_SIZE * sizeof(int16_t));
                } else {
                    memset(display_buf + BUFFER_READ_SIZE * sizeof(int16_t),
                    0,
                    BUFFER_READ_SIZE * sizeof(int16_t));
                }

                bufmgr_write_end(&s_extract_app->buffers, BUF_DISPLAY, display_frame_size);
            }
            // If display buffer full, frame is silently dropped (lossy buffer policy)
        }

        // RF bypasses the lossy display stride. Only bounded copies happen here;
        // a full preview queue causes a source-position gap.
        if (s_b_present && swap_channels) {
            gui_rf_block_info_t info_tmp = stamp.info[0];
            stamp.info[0] = stamp.info[1]; stamp.info[1] = info_tmp;
            size_t count_tmp = stamp.counts[0];
            stamp.counts[0] = stamp.counts[1]; stamp.counts[1] = count_tmp;
        }
        stamp.info[0].discontinuity |= mapping_gap;
        stamp.info[1].discontinuity |= mapping_gap;
        gui_video_submit_rf(s_extract_app, mapped_a, mapped_b, stamp.counts[0], stamp.counts[1],
                              stamp.info, atomic_load(&s_extract_app->rf_preview_input_supported));
        source_offset += BUFFER_READ_SIZE;
        // Update expensive UI stats at the same dynamic stride used for display.
        if ((frame_count % ui_stride) == 0) {
            //gui_extract_update_stats(s_extract_app, s_buf_a, s_buf_b, BUFFER_READ_SIZE);
            gui_extract_update_stats(s_extract_app, mapped_a, mapped_b, BUFFER_READ_SIZE);
        }

        // Sample counters always updated (cheap atomic ops)
        atomic_fetch_add(&s_extract_app->total_samples, BUFFER_READ_SIZE);
        atomic_fetch_add(&s_extract_app->samples_a, BUFFER_READ_SIZE);
        if (s_b_present) {
        atomic_fetch_add(&s_extract_app->samples_b, BUFFER_READ_SIZE);
        }

        // Periodic buffer stats logging
        if (frame_count % BUFMGR_LOG_INTERVAL == 0) {
            bufmgr_log_periodic(&s_extract_app->buffers);
        }

        // Note: FFT is now processed from display samples in the render thread
        // (see gui_oscilloscope.c render_oscilloscope_channel split mode)
    }

exit_thread:
    fprintf(stderr, "[EXTRACT] Continuous extraction thread exiting\n"); 
    return 0;
}

void gui_extract_init(void) {
    if (s_initialized) return;

    // Allocate 32-byte aligned buffers for SSE/AVX
    s_buf_a = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_b = (int16_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int16_t));
    s_buf_aux = aligned_alloc(16, BUFFER_READ_SIZE);

    // Scratch buffers for conversions
    s_tmp8_a = (int8_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int8_t));
    s_tmp8_b = (int8_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int8_t));
    s_tmp32_a = (int32_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int32_t));
    s_tmp32_b = (int32_t *)aligned_alloc(32, BUFFER_READ_SIZE * sizeof(int32_t));

    s_conv_16to8 = get_16to8_function();
    s_conv_16to8to32 = get_16to8to32_function();

    // Startup default only. gui_extract_start() always overwrites this from
    // runtime capability; this does not imply Channel B exists in upstream mode.
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, (void*)1);
    s_b_present = true;



    // Initialize synchronization events
    if (!s_events_initialized) {
        if (rb_event_init(&s_data_event) == 0 && rb_event_init(&s_space_event) == 0) {
            s_events_initialized = true;
            fprintf(stderr, "[EXTRACT] Event signaling initialized\n");
        } else {
            fprintf(stderr, "[EXTRACT] Warning: Failed to initialize events, falling back to polling\n");
        }
    }

    // Note: Record buffer events now managed by buffer manager

    s_initialized = true;
}

void gui_extract_cleanup(void) {
    // Stop extraction thread if running
    gui_extract_stop();
    gui_rf_stream_marks_destroy(s_rf_marks);
    s_rf_marks = NULL;

    // Note: Record buffers now managed by buffer manager (cleanup via bufmgr_cleanup)

    // Destroy synchronization events
    if (s_events_initialized) {
        rb_event_destroy(&s_data_event);
        rb_event_destroy(&s_space_event);
        s_events_initialized = false;
    }

    // Free extraction buffers
    if (s_initialized) {
        if (s_buf_a) {
            aligned_free(s_buf_a);
            s_buf_a = NULL;
        }
        if (s_buf_b) {
            aligned_free(s_buf_b);
            s_buf_b = NULL;
        }
        if (s_buf_aux) {
            aligned_free(s_buf_aux);
            s_buf_aux = NULL;
        }
        if (s_tmp8_a) {
            aligned_free(s_tmp8_a);
            s_tmp8_a = NULL;
        }
        if (s_tmp8_b) {
            aligned_free(s_tmp8_b);
            s_tmp8_b = NULL;
        }
        if (s_tmp32_a) {
            aligned_free(s_tmp32_a);
            s_tmp32_a = NULL;
        }
        if (s_tmp32_b) {
            aligned_free(s_tmp32_b);
            s_tmp32_b = NULL;
        }
        s_initialized = false;
    }
}

static int extract_start_internal(gui_app_t *app, bool tagged) {
    if (s_extract_thread_running) {
        return 0;  // Already running
    }

    // Initialize extraction if needed
    gui_extract_init();

    // Configure extraction path from runtime capture capability.
    // For upstream backend, always use the AB extraction function so that
    // dual-ADC hardware (auto-detected via stream_id=1 mid-capture) has its
    // Channel B data unpacked correctly. When B is not present (single ADC),
    // bits[31:20] are zero and the extraction loop memsets s_buf_b to zero,
    // so display/recording are unaffected.
    bool upstream = (app && app->capture_backend_upstream);
    s_b_present = (app && app->capture_has_channel_b);
    bool use_ab_fn = s_b_present || upstream;
    s_extract_fn = get_conv_function(0, 0, 0, 0, (void*)1, use_ab_fn ? (void*)1 : NULL);
    s_extract_fn_ab = use_ab_fn;
    if (!s_b_present && misrc_debug_enabled()) {
        fprintf(stderr, "[EXTRACT] Runtime A-only extraction enabled (upstream=%d)\n", upstream);
    }

    // Store context (uses app->buffers for capture ringbuffer via buffer manager)
    s_extract_app = app;
    s_tagged_input = tagged;
    atomic_store(&s_stamp_write, 0);
    atomic_store(&s_stamp_read, 0);

    atomic_store(&s_recording_enabled, false);
    atomic_store(&s_use_flac, false);
    atomic_store(&s_extract_stop_requested, false);

    // Ensure buffers are initialized in buffer manager
    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) < 0) {
        fprintf(stderr, "[EXTRACT] Failed to initialize capture buffer\n");
        return -1;
    }

    // Start extraction thread
    if (thrd_create_with_priority(&s_extract_thread,
                                  extraction_thread,
                                  NULL,
                                  THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[EXTRACT] Failed to create extraction thread\n");
        return -1;
    }

    s_extract_thread_running = true;
    fprintf(stderr, "[EXTRACT] Started continuous extraction thread\n");
    return 0;
}

int gui_extract_start(gui_app_t *app) { return extract_start_internal(app, false); }
int gui_extract_start_tagged(gui_app_t *app) { return extract_start_internal(app, true); }

void gui_extract_stop(void) {
    if (!s_extract_thread_running) {
        return;
    }

    // Disable recording first
    atomic_store(&s_recording_enabled, false);
    // Stop also works during startup failure, when is_capturing is still true,
    // and with a producer that continuously keeps the input FIFO nonempty.
    atomic_store(&s_extract_stop_requested, true);

    // Wait for thread to exit
    thrd_join(s_extract_thread, NULL);
    s_extract_thread_running = false;
    s_extract_app = NULL;

    fprintf(stderr, "[EXTRACT] Stopped continuous extraction thread\n");
}

bool gui_extract_is_running(void) {
    return s_extract_thread_running;
}

// Note: Record ringbuffers now accessed via app->buffers (buffer_manager)
// Use BUF_RECORD_A and BUF_RECORD_B with bufmgr_read_begin/bufmgr_read_end

void gui_extract_set_recording(bool enabled, bool use_flac, uint8_t rf_bits_a, uint8_t rf_bits_b,
                               bool direct_a, bool direct_b) {
    atomic_store(&s_use_flac, use_flac);
    atomic_store(&s_rf_bits_a, rf_bits_a);
    atomic_store(&s_rf_bits_b, rf_bits_b);
    atomic_store(&s_direct_channel_a, direct_a);
    atomic_store(&s_direct_channel_b, direct_b);
    atomic_store(&s_recording_enabled, enabled);
    fprintf(stderr, "[EXTRACT] Recording %s (FLAC: %s, bits A:%u B:%u, direct A:%s B:%s)\n",
            enabled ? "enabled" : "disabled",
            use_flac ? "yes" : "no",
            (unsigned)rf_bits_a, (unsigned)rf_bits_b,
            direct_a ? "yes" : "no", direct_b ? "yes" : "no");
}

void gui_extract_reset_record_rbs(gui_app_t *app) {
    // Reset record buffers via buffer manager
    if (app) {
        bufmgr_reset(&app->buffers, BUF_RECORD_A);
        bufmgr_reset(&app->buffers, BUF_RECORD_B);
    }
}

void gui_extract_init_record_rbs(gui_app_t *app) {
    // Initialize record buffers via buffer manager (for simulated capture that
    // doesn't go through gui_extract_start)
    if (app) {
        if (app->settings.capture_a) {
            bufmgr_ensure_init(&app->buffers, BUF_RECORD_A);
        }
        if (s_b_present && app->settings.capture_b) {
            bufmgr_ensure_init(&app->buffers, BUF_RECORD_B);
        }
        fprintf(stderr, "[EXTRACT] Record buffers initialized (for simulated capture)\n");
    }
}

bool gui_extract_is_recording(bool *use_flac) {
    if (use_flac) {
        *use_flac = atomic_load(&s_use_flac);
    }
    return atomic_load(&s_recording_enabled);
}

extract_fn_t gui_extract_get_function(void) {
    if (!s_initialized) gui_extract_init();
    return (extract_fn_t)s_extract_fn;
}

int16_t *gui_extract_get_buf_a(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_a;
}

int16_t *gui_extract_get_buf_b(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_b;
}

uint8_t *gui_extract_get_buf_aux(void) {
    if (!s_initialized) gui_extract_init();
    return s_buf_aux;
}

void gui_extract_update_stats(gui_app_t *app, const int16_t *buf_a,
                              const int16_t *buf_b, size_t num_samples) {
    size_t clip_a_pos = 0, clip_a_neg = 0;
    size_t clip_b_pos = 0, clip_b_neg = 0;
    uint16_t peak_a_pos = 0, peak_a_neg = 0;
    uint16_t peak_b_pos = 0, peak_b_neg = 0;
     bool have_b = (buf_b != NULL); // buf_b may be NULL in upstream mode, so check before processing B channel stats                            
    // Peak detection uses first 1000 samples
    size_t peak_samples = (num_samples < 1000) ? num_samples : 1000;

     for (size_t i = 0; i < num_samples; i++) {
        int16_t sa = buf_a[i];

        // Clipping detection (12-bit ADC: +2047 is positive clip, -2048 is negative clip)
        if (sa >= 2047) clip_a_pos++;
        else if (sa <= -2048) clip_a_neg++;

        // Peak detection (first N samples only) - A
        if (i < peak_samples) {
            if (sa > 0 && (uint16_t)sa > peak_a_pos) peak_a_pos = (uint16_t)sa;
            if (sa < 0 && (uint16_t)(-sa) > peak_a_neg) peak_a_neg = (uint16_t)(-sa);
        }

        // B only if present
        if (have_b) {
            int16_t sb = buf_b[i];

            if (sb >= 2047) clip_b_pos++;
            else if (sb <= -2048) clip_b_neg++;

            if (i < peak_samples) {
                if (sb > 0 && (uint16_t)sb > peak_b_pos) peak_b_pos = (uint16_t)sb;
                if (sb < 0 && (uint16_t)(-sb) > peak_b_neg) peak_b_neg = (uint16_t)(-sb);
            }
        }
    }


    // Update atomic counters
    atomic_fetch_add(&app->clip_count_a_pos, clip_a_pos);
    atomic_fetch_add(&app->clip_count_a_neg, clip_a_neg);
    atomic_store(&app->peak_a_pos, peak_a_pos);
    atomic_store(&app->peak_a_neg, peak_a_neg);

    if (have_b) {
        atomic_fetch_add(&app->clip_count_b_pos, clip_b_pos);
        atomic_fetch_add(&app->clip_count_b_neg, clip_b_neg);
        atomic_store(&app->peak_b_pos, peak_b_pos);
        atomic_store(&app->peak_b_neg, peak_b_neg);
    } else {
        atomic_store(&app->peak_b_pos, 0);
        atomic_store(&app->peak_b_neg, 0);
        atomic_store(&app->clip_count_b_pos, 0);
        atomic_store(&app->clip_count_b_neg, 0);
    }

}

rb_event_t *gui_extract_get_data_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_data_event;
}

rb_event_t *gui_extract_get_space_event(void) {
    if (!s_events_initialized) return NULL;
    return &s_space_event;
}

