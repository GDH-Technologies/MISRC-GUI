/*
 * MISRC GUI - RTL-SDR (RTL2832) Device Support Implementation
 *
 * Local USB via librtlsdr. Mirrors the FX3 backend lifecycle
 * (enumerate/open/start/stop/is_running) and the playback/CXADC capture-feed
 * pattern: pack 8-bit I/Q into the hsdaoh 32-bit capture format and write
 * BUF_CAPTURE_RF, letting the shared extraction thread produce display +
 * record + stats. Demodulation is handled by the separate Demod panel.
 */

#ifdef ENABLE_RTLSDR

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

// rtl-sdr.h only pulls stdint.h + rtl-sdr_export.h (no libusb.h, no Windows
// header conflict), so it is safe to include before raylib/gui headers.
#include <rtl-sdr.h>

#include "gui_rtlsdr.h"
#include "../core/gui_app.h"
#include "../processing/gui_extract.h"
#include "../processing/gui_display_thread.h"
#include "../../common/buffer_manager.h"
#include "../../common/threading.h"

/* do_exit is defined in the core capture module; declared extern here like gui_fx3.c. */
extern atomic_int do_exit;

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------

// One rtlsdr_read_sync call reads this many bytes (must be a multiple of 512).
// 131072 bytes = 65536 interleaved I/Q pairs = 65536 packed uint32 samples.
#define RTL_READ_BYTES      (131072)
#define RTL_PAIRS_PER_READ  (RTL_READ_BYTES / 2)        // 65536 I/Q pairs
#define RTL_SAMPLES_PER_BATCH (RTL_PAIRS_PER_READ)     // 65536 packed samples
#define RTL_WRITE_BYTES    (RTL_SAMPLES_PER_BATCH * 4) // 262144 bytes (matches playback)

#define RTL_DEFAULT_RATE_HZ 2400000U

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------

static rtlsdr_dev_t *s_rtlsdr_dev = NULL;
static atomic_bool s_rtlsdr_running = false;
static void *s_rtlsdr_thread = NULL;

//-----------------------------------------------------------------------------
// Raw format encoding (mirrors gui_playback.c / extract.c decoding)
//   Bits 0-11:  Channel A (12-bit, stored as 2047 - sample)
//   Bits 12-19: AUX data (8 bits, 0)
//   Bits 20-31: Channel B (12-bit, stored as 2047 - sample)
//-----------------------------------------------------------------------------

static inline uint32_t rtl_encode_raw_sample(int16_t sample_a, int16_t sample_b) {
    if (sample_a > 2047) sample_a = 2047;
    if (sample_a < -2048) sample_a = -2048;
    if (sample_b > 2047) sample_b = 2047;
    if (sample_b < -2048) sample_b = -2048;
    uint32_t ch_a = (uint32_t)((2047 - sample_a) & 0xFFF);
    uint32_t ch_b = (uint32_t)((2047 - sample_b) & 0xFFF);
    uint32_t aux = 0;
    return ch_a | (aux << 12) | (ch_b << 20);
}

//-----------------------------------------------------------------------------
// Enumeration
//-----------------------------------------------------------------------------

int gui_rtlsdr_enumerate(rtlsdr_device_info_t *devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;

    uint32_t count = rtlsdr_get_device_count();
    int added = 0;
    for (uint32_t i = 0; i < count && added < max_devices; i++) {
        rtlsdr_device_info_t *dst = &devices[added];
        dst->index = (int)i;

        const char *name = rtlsdr_get_device_name(i);
        snprintf(dst->name, sizeof(dst->name), "%s", name ? name : "RTL-SDR");

        dst->serial[0] = '\0';
        char manufact[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        if (rtlsdr_get_device_usb_strings(i, manufact, product, serial) == 0) {
            snprintf(dst->serial, sizeof(dst->serial), "%s", serial);
        }
        added++;
    }
    return added;
}

//-----------------------------------------------------------------------------
// Open / Close
//-----------------------------------------------------------------------------

int gui_rtlsdr_open(gui_app_t *app, int device_index) {
    if (!app) return -1;
    if (s_rtlsdr_dev) {
        rtlsdr_close(s_rtlsdr_dev);
        s_rtlsdr_dev = NULL;
    }

    int r = rtlsdr_open(&s_rtlsdr_dev, (uint32_t)device_index);
    if (r < 0 || !s_rtlsdr_dev) {
        fprintf(stderr, "[RTL-SDR] Failed to open device %d (err %d)\n", device_index, r);
        s_rtlsdr_dev = NULL;
        return -1;
    }

    uint32_t rate = app->settings.rtlsdr_sample_rate_hz;
    if (rate == 0) rate = RTL_DEFAULT_RATE_HZ;
    if (rtlsdr_set_sample_rate(s_rtlsdr_dev, rate) < 0) {
        fprintf(stderr, "[RTL-SDR] Warning: set_sample_rate(%u) failed\n", rate);
    }

    if (rtlsdr_set_center_freq(s_rtlsdr_dev, (uint32_t)app->settings.rtlsdr_freq_hz) < 0) {
        fprintf(stderr, "[RTL-SDR] Warning: set_center_freq(%llu) failed\n",
                (unsigned long long)app->settings.rtlsdr_freq_hz);
    }

    // AGC: rtlsdr_set_agc_mode(1=on,0=off). Gain mode: 0=auto,1=manual.
    rtlsdr_set_agc_mode(s_rtlsdr_dev, app->settings.rtlsdr_agc ? 1 : 0);
    if (app->settings.rtlsdr_gain_mode == 1) {
        rtlsdr_set_tuner_gain_mode(s_rtlsdr_dev, 1);  // manual
        rtlsdr_set_tuner_gain(s_rtlsdr_dev, app->settings.rtlsdr_gain_tenths_db);
    } else {
        rtlsdr_set_tuner_gain_mode(s_rtlsdr_dev, 0);  // auto
    }

    rtlsdr_set_offset_tuning(s_rtlsdr_dev, app->settings.rtlsdr_offset_corr ? 1 : 0);
    rtlsdr_reset_buffer(s_rtlsdr_dev);

    fprintf(stderr, "[RTL-SDR] Opened device %d: %u Hz, freq %llu Hz, agc=%d, gain_mode=%d\n",
            device_index, rate, (unsigned long long)app->settings.rtlsdr_freq_hz,
            app->settings.rtlsdr_agc ? 1 : 0, app->settings.rtlsdr_gain_mode);
    return 0;
}

void gui_rtlsdr_close(gui_app_t *app) {
    (void)app;
    if (s_rtlsdr_dev) {
        rtlsdr_close(s_rtlsdr_dev);
        s_rtlsdr_dev = NULL;
    }
}

//-----------------------------------------------------------------------------
// Capture thread
//-----------------------------------------------------------------------------

static int rtlsdr_capture_thread(void *ctx) {
    gui_app_t *app = (gui_app_t *)ctx;
    thrd_set_priority(THRD_PRIORITY_CRITICAL);

    uint8_t *in = (uint8_t *)malloc(RTL_READ_BYTES);
    uint32_t *packed = (uint32_t *)malloc(RTL_WRITE_BYTES);
    if (!in || !packed) {
        fprintf(stderr, "[RTL-SDR] Failed to allocate capture buffers\n");
        free(in); free(packed);
        atomic_store(&s_rtlsdr_running, false);
        return -1;
    }

    fprintf(stderr, "[RTL-SDR] Capture thread started\n");
    atomic_store(&app->stream_synced, true);
    atomic_store(&app->last_callback_time_ms, get_time_ms());

    uint64_t batch_count = 0;
    while (atomic_load(&s_rtlsdr_running) && !atomic_load(&do_exit)) {
        int n_read = 0;
        int r = rtlsdr_read_sync(s_rtlsdr_dev, in, RTL_READ_BYTES, &n_read);
        if (r < 0) {
            fprintf(stderr, "[RTL-SDR] read_sync error %d\n", r);
            thrd_sleep_ms(10);
            continue;
        }
        if (n_read <= 0) {
            thrd_sleep_ms(1);
            continue;
        }

        size_t pairs = (size_t)n_read / 2;
        if (pairs > RTL_PAIRS_PER_READ) pairs = RTL_PAIRS_PER_READ;

        for (size_t i = 0; i < pairs; i++) {
            int8_t i_s = (int8_t)in[i * 2] - 128;      // I: centered to signed
            int8_t q_s = (int8_t)in[i * 2 + 1] - 128;  // Q: centered to signed
            int16_t a16 = (int16_t)((int)i_s << 4);    // shift into 12-bit field
            int16_t b16 = (int16_t)((int)q_s << 4);
            packed[i] = rtl_encode_raw_sample(a16, b16);
        }
        // Zero-pad the remainder of the batch so the write is a fixed size
        // (matches what the extraction thread expects to read).
        for (size_t i = pairs; i < RTL_SAMPLES_PER_BATCH; i++) {
            packed[i] = rtl_encode_raw_sample(0, 0);
        }

        uint8_t *out = bufmgr_write_begin(&app->buffers, BUF_CAPTURE_RF,
                                           RTL_WRITE_BYTES, NULL);
        if (out) {
            memcpy(out, packed, RTL_WRITE_BYTES);
            bufmgr_write_end(&app->buffers, BUF_CAPTURE_RF, RTL_WRITE_BYTES);
            bufmgr_signal_data(&app->buffers, BUF_CAPTURE_RF);
        } else {
            atomic_fetch_add(&app->rb_drop_count, 1);
            if (atomic_load(&app->rb_drop_count) <= 5) {
                fprintf(stderr, "[RTL-SDR] Warning: BUF_CAPTURE_RF full, data dropped\n");
            }
        }

        atomic_fetch_add(&app->total_samples, (uint64_t)pairs);
        atomic_fetch_add(&app->samples_a, (uint64_t)pairs);
        atomic_fetch_add(&app->samples_b, (uint64_t)pairs);
        atomic_store(&app->last_callback_time_ms, get_time_ms());
        batch_count++;
    }

    fprintf(stderr, "[RTL-SDR] Capture thread exiting after %llu batches\n",
            (unsigned long long)batch_count);
    free(in);
    free(packed);
    return 0;
}

//-----------------------------------------------------------------------------
// Start / Stop
//-----------------------------------------------------------------------------

int gui_rtlsdr_start(gui_app_t *app) {
    if (!app) return -1;
    fprintf(stderr, "[RTL-SDR] Starting capture\n");
    if (!s_rtlsdr_dev) {
        fprintf(stderr, "[RTL-SDR] No device open\n");
        return -1;
    }

    bufmgr_reset_stats(&app->buffers, BUF_COUNT);

    atomic_store(&app->total_samples, 0);
    atomic_store(&app->samples_a, 0);
    atomic_store(&app->samples_b, 0);
    atomic_store(&app->frame_count, 0);
    atomic_store(&app->missed_frame_count, 0);
    atomic_store(&app->error_count, 0);
    atomic_store(&app->parser_error_count, 0);
    atomic_store(&app->system_error_count, 0);
    atomic_store(&app->error_count_a, 0);
    atomic_store(&app->error_count_b, 0);
    atomic_store(&app->clip_count_a_pos, 0);
    atomic_store(&app->clip_count_a_neg, 0);
    atomic_store(&app->clip_count_b_pos, 0);
    atomic_store(&app->clip_count_b_neg, 0);
    atomic_store(&app->rb_wait_count, 0);
    atomic_store(&app->rb_drop_count, 0);
    atomic_store(&app->stream_synced, false);

    uint32_t rate = app->settings.rtlsdr_sample_rate_hz;
    if (rate == 0) rate = RTL_DEFAULT_RATE_HZ;
    atomic_store(&app->sample_rate, rate);

    app->display_samples_available_a = 0;
    app->display_samples_available_b = 0;

    if (bufmgr_ensure_init(&app->buffers, BUF_CAPTURE_RF) != 0) {
        fprintf(stderr, "[RTL-SDR] Failed to initialize capture ringbuffer\n");
        gui_app_set_status(app, "Failed to initialize capture buffer");
        return -1;
    }

    atomic_store(&s_rtlsdr_running, true);
    app->is_capturing = true;

    // Extraction thread: reads BUF_CAPTURE_RF -> BUF_DISPLAY + BUF_RECORD_* + stats.
    int r = gui_extract_start(app);
    if (r < 0) {
        fprintf(stderr, "[RTL-SDR] Failed to start extraction thread\n");
        gui_app_set_status(app, "Failed to start extraction");
        atomic_store(&s_rtlsdr_running, false);
        app->is_capturing = false;
        return -1;
    }

    // Display thread: processes BUF_DISPLAY for oscilloscope/FFT/waterfall/demod.
    if (app->display_thread) {
        r = gui_display_thread_start(app->display_thread, app, &app->buffers);
        if (r < 0) {
            fprintf(stderr, "[RTL-SDR] Failed to start display thread (non-fatal)\n");
        }
    }

    thrd_t thread;
    if (thrd_create_with_priority(&thread,
                                   rtlsdr_capture_thread,
                                   app,
                                   THRD_PRIORITY_CRITICAL) != thrd_success) {
        fprintf(stderr, "[RTL-SDR] Failed to create capture thread\n");
        gui_extract_stop();
        if (app->display_thread) gui_display_thread_stop(app->display_thread);
        atomic_store(&s_rtlsdr_running, false);
        app->is_capturing = false;
        return -1;
    }
    s_rtlsdr_thread = (void *)(uintptr_t)thread;

    gui_app_set_status(app, "RTL-SDR capture running");
    return 0;
}

void gui_rtlsdr_stop(gui_app_t *app) {
    if (!atomic_load(&s_rtlsdr_running)) return;
    fprintf(stderr, "[RTL-SDR] Stopping capture\n");

    app->is_capturing = false;
    atomic_store(&s_rtlsdr_running, false);

    if (s_rtlsdr_thread) {
        thrd_t thread = (thrd_t)(uintptr_t)s_rtlsdr_thread;
        thrd_join(thread, NULL);
        s_rtlsdr_thread = NULL;
    }

    if (app->display_thread) gui_display_thread_stop(app->display_thread);
    gui_extract_stop();

    gui_rtlsdr_close(app);
    atomic_store(&app->stream_synced, false);
    gui_app_set_status(app, "RTL-SDR capture stopped");
}

bool gui_rtlsdr_is_running(gui_app_t *app) {
    (void)app;
    return atomic_load(&s_rtlsdr_running);
}

int gui_rtlsdr_set_frequency(gui_app_t *app, uint64_t hz) {
    if (!app) return -1;
    // Persist so the next capture start uses the new frequency even if not live.
    app->settings.rtlsdr_freq_hz = hz;
    gui_settings_save(&app->settings);
    // Live retune only if the device is open and capture is running.
    if (s_rtlsdr_dev && atomic_load(&s_rtlsdr_running)) {
        int r = rtlsdr_set_center_freq(s_rtlsdr_dev, (uint32_t)hz);
        if (r < 0) {
            fprintf(stderr, "[RTL-SDR] live retune to %llu Hz failed (err %d)\n",
                    (unsigned long long)hz, r);
            return -1;
        }
        fprintf(stderr, "[RTL-SDR] live retune to %llu Hz\n", (unsigned long long)hz);
    }
    return 0;
}

#endif // ENABLE_RTLSDR
