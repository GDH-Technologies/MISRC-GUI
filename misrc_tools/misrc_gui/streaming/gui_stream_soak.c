/*
 * MISRC GUI - concurrency proof for the RTSP stream
 *
 * The acceptance test for the streaming feature. Everything else is plumbing;
 * this is the part that says the feature is safe to have.
 *
 * The claim under test is narrow and falsifiable: turning the stream on must
 * not cost the RF ingest path throughput, and must not cost the reference
 * recording frames. So the harness measures both with the stream off, turns it
 * on, measures again, and compares the two halves of the same run rather than
 * two different runs -- the same tape, the same thermal state, the same
 * everything except the stream.
 *
 * RF load is real: it reads /dev/cxadc0 exactly the way gui_cxadc.c does, at
 * whatever rate the driver delivers. A synthetic CPU burner would prove
 * nothing about a path whose problem is I/O scheduling.
 *
 * What this harness does NOT do is drive the GUI's own capture pipeline, which
 * needs a window. It measures the RF *device* under load, not gui_capture's
 * bookkeeping, so a full-confidence run still means starting a real capture in
 * the app with a tape playing.
 */

#include "gui_rtsp_stream.h"
#include "gui_mediamtx.h"
#include "../input/gui_preview_v4l2.h"
#include "../output/gui_video_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && !defined(__ANDROID__)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

#define SOAK_READ_BYTES (1u << 20)   /* 1 MiB, the order gui_cxadc.c reads in */

static struct {
    int          fd;
    atomic_bool  run;
    atomic_ullong bytes;
    atomic_ullong read_errors;
    atomic_ullong short_reads;
    pthread_t    thread;
    bool         running;
} rf;

static double soak_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Reads the RF device as fast as it will give data, which is what the real
 * ingest path does. Counts nothing clever: throughput is the signal, because a
 * reader that loses the race reads less. */
static void *rf_reader(void *arg)
{
    (void)arg;
    uint8_t *buf = malloc(SOAK_READ_BYTES);
    if (!buf) return NULL;
    while (atomic_load_explicit(&rf.run, memory_order_acquire)) {
        ssize_t n = read(rf.fd, buf, SOAK_READ_BYTES);
        if (n < 0) {
            if (errno == EINTR) continue;
            atomic_fetch_add(&rf.read_errors, 1);
            break;
        }
        if (n == 0) break;
        if ((size_t)n < SOAK_READ_BYTES) atomic_fetch_add(&rf.short_reads, 1);
        atomic_fetch_add(&rf.bytes, (unsigned long long)n);
    }
    free(buf);
    return NULL;
}

static bool rf_start(const char *dev)
{
    rf.fd = open(dev, O_RDONLY);
    if (rf.fd < 0) return false;
    atomic_store(&rf.run, true);
    atomic_store(&rf.bytes, 0);
    atomic_store(&rf.read_errors, 0);
    atomic_store(&rf.short_reads, 0);
    if (pthread_create(&rf.thread, NULL, rf_reader, NULL) != 0) {
        close(rf.fd);
        rf.fd = -1;
        return false;
    }
    rf.running = true;
    return true;
}

static void rf_stop(void)
{
    if (!rf.running) return;
    atomic_store_explicit(&rf.run, false, memory_order_release);
    pthread_join(rf.thread, NULL);
    if (rf.fd >= 0) close(rf.fd);
    rf.fd = -1;
    rf.running = false;
}

typedef struct {
    double   seconds;
    double   rf_mib_s;
    double   rec_fps;
    uint64_t rec_dropped;
    uint64_t rf_errors;
} soak_window_t;

static void soak_sample(soak_window_t *w, double seconds)
{
    unsigned long long b0 = atomic_load(&rf.bytes);
    unsigned long long e0 = atomic_load(&rf.read_errors);
    gui_video_record_status_t r0 = gui_video_record_get_status();
    double t0 = soak_now();

    double deadline = t0 + seconds;
    while (soak_now() < deadline) {
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        gui_mediamtx_poll();
        gui_rtsp_stream_poll();
    }

    double dt = soak_now() - t0;
    unsigned long long b1 = atomic_load(&rf.bytes);
    unsigned long long e1 = atomic_load(&rf.read_errors);
    gui_video_record_status_t r1 = gui_video_record_get_status();

    w->seconds = dt;
    w->rf_mib_s = dt > 0 ? ((double)(b1 - b0) / (1024.0 * 1024.0)) / dt : 0.0;
    w->rec_fps = dt > 0 ? (double)(r1.frames_written - r0.frames_written) / dt : 0.0;
    w->rec_dropped = r1.frames_dropped - r0.frames_dropped;
    w->rf_errors = e1 - e0;
}

int gui_stream_soak_main(const char *device, const char *rf_device, int seconds)
{
    if (!device || !device[0]) device = "/dev/video0";
    if (!rf_device || !rf_device[0]) rf_device = "/dev/cxadc0";
    if (seconds <= 0) seconds = 20;

    char err[256] = {0};
    int bad = 0;

    /* --- RF ingest load ------------------------------------------------- */
    bool have_rf = rf_start(rf_device);
    if (!have_rf) {
        printf("NOTE: %s could not be opened (%s); running without RF load.\n"
               "      The recorder comparison below still stands, but the RF half\n"
               "      of this proof does not.\n", rf_device, strerror(errno));
    }

    /* --- preview ---------------------------------------------------------- */
    gui_preview_init(NULL);
    gui_preview_refresh_devices();
    size_t n_devs = 0;
    const preview_device_t *devs = gui_preview_devices(&n_devs);
    int idx = -1;
    for (size_t i = 0; i < n_devs; i++) {
        if (strcmp(devs[i].path, device) == 0) { idx = (int)i; break; }
    }
    if (idx < 0) { fprintf(stderr, "no such preview device: %s\n", device); rf_stop(); return 1; }
    gui_preview_select(idx, 0);
    if (gui_preview_connect() != 0) {
        preview_status_t ps = gui_preview_get_status();
        fprintf(stderr, "preview: %s\n", ps.err_text);
        gui_preview_shutdown();
        rf_stop();
        return 1;
    }
    preview_status_t st = gui_preview_get_status();
    uint32_t pitch = gui_preview_negotiated_pitch();
    if (pitch == 0) pitch = st.width * 2;

    /* --- reference recording, running throughout -------------------------- */
    char mkv[256];
    snprintf(mkv, sizeof(mkv), "%s/misrc-soak.mkv",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp");
    if (gui_video_record_start(mkv, VIDEO_CODEC_H264, st.width, st.height, pitch,
                               st.fps_num, st.fps_den, err, sizeof(err)) != 0) {
        fprintf(stderr, "reference recorder: %s\n", err);
        gui_preview_disconnect();
        gui_preview_shutdown();
        rf_stop();
        return 1;
    }

    printf("soak: %ux%u @ %u/%u, rf=%s, %ds per half\n",
           st.width, st.height, st.fps_num, st.fps_den,
           have_rf ? rf_device : "(none)", seconds);

    /* Let everything reach steady state before the baseline is taken. */
    struct timespec settle = { 3, 0 };
    nanosleep(&settle, NULL);

    /* --- A: stream OFF ---------------------------------------------------- */
    soak_window_t before = {0};
    soak_sample(&before, (double)seconds);
    printf("  stream OFF: rf %.1f MiB/s   recorder %.2f fps   dropped %llu\n",
           before.rf_mib_s, before.rec_fps, (unsigned long long)before.rec_dropped);

    /* --- B: stream ON ----------------------------------------------------- */
    gui_mediamtx_config_t ports = gui_mediamtx_default_config();
    gui_rtsp_stream_opts_t opts = {0};
    opts.width = st.width; opts.height = st.height; opts.pitch = pitch;
    opts.fps_num = st.fps_num ? st.fps_num : 25;
    opts.fps_den = st.fps_den ? st.fps_den : 1;
    opts.video_device = device;
    opts.audio_device = "";
    opts.encoder = RTSP_ENCODER_AUTO;
    opts.ports = ports;
    opts.reader_host = "";

    if (gui_rtsp_stream_start(&opts, err, sizeof(err)) != 0) {
        fprintf(stderr, "stream: %s\n", err);
        bad = 1;
    } else {
        double give_up = soak_now() + 8.0;
        while (soak_now() < give_up && gui_rtsp_stream_get_status().starting) {
            struct timespec ts = { 0, 50 * 1000 * 1000 };
            nanosleep(&ts, NULL);
            gui_rtsp_stream_poll();
        }
        gui_rtsp_stream_status_t ss = gui_rtsp_stream_get_status();
        if (ss.error) { fprintf(stderr, "stream: %s\n", ss.err_text); bad = 1; }
        else printf("  streaming to %s, audio %s\n", ss.url_rtsp,
                    ss.audio_active ? ss.audio_device : "off");
    }

    soak_window_t during = {0};
    if (!bad) {
        soak_sample(&during, (double)seconds);
        printf("  stream ON : rf %.1f MiB/s   recorder %.2f fps   dropped %llu\n",
               during.rf_mib_s, during.rec_fps, (unsigned long long)during.rec_dropped);
    }

    gui_rtsp_stream_status_t sfinal = gui_rtsp_stream_get_status();

    /* --- teardown --------------------------------------------------------- */
    gui_rtsp_stream_request_stop();
    gui_rtsp_stream_finish();
    gui_video_record_request_stop();
    gui_video_record_finish();
    gui_preview_disconnect();
    gui_preview_shutdown();
    gui_mediamtx_stop();
    rf_stop();
    remove(mkv);

    if (bad) { fprintf(stderr, "soak FAILED: the stream did not run\n"); return 1; }

    /* --- the assertions --------------------------------------------------- */
    printf("\n");
    int fails = 0;

    if (before.rec_dropped != 0) {
        printf("  NOTE: the recorder was already dropping frames with the stream OFF,\n"
               "        so this run cannot attribute anything to streaming.\n");
        fails++;
    }
    if (during.rec_dropped != 0) {
        printf("  FAIL: recorder dropped %llu frames with the stream on\n",
               (unsigned long long)during.rec_dropped);
        fails++;
    } else {
        printf("  ok:   recorder dropped nothing with the stream on\n");
    }

    /* 2% of 25fps is half a frame per second -- comfortably inside sampling
     * noise, and far below anything an operator would see. */
    double fps_delta = before.rec_fps > 0
                     ? (before.rec_fps - during.rec_fps) / before.rec_fps : 0.0;
    if (fps_delta > 0.02) {
        printf("  FAIL: recorder lost %.1f%% of its frame rate (%.2f -> %.2f fps)\n",
               fps_delta * 100.0, before.rec_fps, during.rec_fps);
        fails++;
    } else {
        printf("  ok:   recorder frame rate held (%.2f -> %.2f fps)\n",
               before.rec_fps, during.rec_fps);
    }

    if (have_rf) {
        if (during.rf_errors != 0 || before.rf_errors != 0) {
            printf("  FAIL: RF read errors (off=%llu on=%llu)\n",
                   (unsigned long long)before.rf_errors,
                   (unsigned long long)during.rf_errors);
            fails++;
        } else {
            printf("  ok:   no RF read errors in either half\n");
        }
        double rf_delta = before.rf_mib_s > 0
                        ? (before.rf_mib_s - during.rf_mib_s) / before.rf_mib_s : 0.0;
        if (rf_delta > 0.05) {
            printf("  FAIL: RF throughput fell %.1f%% (%.1f -> %.1f MiB/s)\n",
                   rf_delta * 100.0, before.rf_mib_s, during.rf_mib_s);
            fails++;
        } else {
            printf("  ok:   RF throughput held (%.1f -> %.1f MiB/s)\n",
                   before.rf_mib_s, during.rf_mib_s);
        }
    }

    if (sfinal.frames_written == 0) {
        printf("  FAIL: the stream sent no frames, so it was not under test\n");
        fails++;
    } else {
        printf("  ok:   the stream sent %llu frames, dropped %llu\n",
               (unsigned long long)sfinal.frames_written,
               (unsigned long long)sfinal.frames_dropped);
    }

    if (fails) { fprintf(stderr, "\nsoak FAILED (%d)\n", fails); return 1; }
    puts("\nconcurrency proof passed");
    return 0;
}

#else

int gui_stream_soak_main(const char *device, const char *rf_device, int seconds)
{
    (void)device; (void)rf_device; (void)seconds;
    fprintf(stderr, "requires Linux\n");
    return 2;
}

#endif /* __linux__ */
