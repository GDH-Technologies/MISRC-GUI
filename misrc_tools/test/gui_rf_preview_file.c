/* Offline end-to-end RF preview probe. Links the production worker, resampler
 * and decoder, without raylib or capture devices. Input is signed s16le RF. */
#include "../misrc_gui/processing/gui_rf_preview_worker.h"
#include "../common/threading.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define FILE_BLOCK_SAMPLES 65536u
#define FILE_FRAME_BYTES ((size_t)GUI_RF_FRAME_MAX_WIDTH * GUI_RF_FRAME_MAX_HEIGHT * 4)

static uint64_t file_now_us(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, counter;
    if (QueryPerformanceFrequency(&frequency) && QueryPerformanceCounter(&counter)) {
        uint64_t hz = (uint64_t)frequency.QuadPart;
        uint64_t ticks = (uint64_t)counter.QuadPart;
        return (ticks / hz) * 1000000u + (ticks % hz) * 1000000u / hz;
    }
#endif
    return get_time_us();
}

static void print_json_string(const char *text) {
    putchar('"');
    while (*text) {
        unsigned char ch = (unsigned char)*text++;
        if (ch == '"' || ch == '\\') { putchar('\\'); putchar(ch); }
        else if (ch < 32) printf("\\u%04x", (unsigned int)ch);
        else putchar(ch);
    }
    putchar('"');
}

static bool save_ppm(const char *path, const uint8_t *rgba, int width, int height) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    uint8_t row[GUI_RF_FRAME_MAX_WIDTH * 3];
    bool ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
    for (int y = 0; y < height && ok; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t *pixel = rgba + ((size_t)y * width + x) * 4;
            memcpy(row + x * 3, pixel, 3);
        }
        ok = fwrite(row, 3, (size_t)width, file) == (size_t)width;
    }
    if (fclose(file) != 0) ok = false;
    return ok;
}

static void pace_realtime(uint64_t start_us, uint64_t samples, uint32_t rate) {
    uint64_t target = start_us + (samples / rate) * 1000000u +
                      (samples % rate) * 1000000u / rate;
    uint64_t now;
    while ((now = file_now_us()) < target) {
        uint64_t remaining = target - now;
        thrd_sleep_ms(remaining >= 2000 ? (int)(remaining / 1000) - 1 : 1);
    }
}

int main(int argc, char **argv) {
    if (argc < 5 || argc > 6 ||
        (strcmp(argv[3], "pal") != 0 && strcmp(argv[3], "ntsc") != 0) ||
        (argc == 6 && strcmp(argv[5], "--max") != 0 && strcmp(argv[5], "--realtime") != 0)) {
        fprintf(stderr, "Usage: %s input.s16le sample_rate_hz pal|ntsc output.ppm [--max|--realtime]\n", argv[0]);
        return 1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long parsed_rate = strtoul(argv[2], &end, 10);
    if (errno || !end || *end || parsed_rate < 16000000u || parsed_rate > 160000000u) {
        fprintf(stderr, "RF sample rate must be 16000000..160000000 Hz\n");
        return 1;
    }
    uint32_t rate = (uint32_t)parsed_rate;
    gui_rf_format_t format = strcmp(argv[3], "pal") == 0 ? GUI_RF_PAL : GUI_RF_NTSC;
    bool realtime = argc == 6 && strcmp(argv[5], "--realtime") == 0;
    if (!gui_rf_preview_worker_set_budget(realtime ? 1.0 : 0.0)) return 1;
    uint64_t wall_start = file_now_us();
    FILE *input = fopen(argv[1], "rb");
    if (!input) { perror("Open RF input"); return 1; }
    uint8_t *bytes = malloc(FILE_BLOCK_SAMPLES * 2);
    int16_t *samples = malloc(FILE_BLOCK_SAMPLES * sizeof(*samples));
    uint8_t *frame = malloc(FILE_FRAME_BYTES);
    gui_rf_preview_worker_t *worker = gui_rf_preview_worker_create(format);
    if (!bytes || !samples || !frame || !worker) {
        fprintf(stderr, "RF probe allocation failed\n");
        gui_rf_preview_worker_destroy(worker);
        free(bytes); free(samples); free(frame); fclose(input);
        return 1;
    }
    uint64_t pace_start = file_now_us();
    uint64_t total_samples = 0, generation = 0, submitted = 0, rejected = 0;
    int width = 0, height = 0, exit_status = 0;
    gui_rf_preview_stats_t stats = {0};
    for (;;) {
        size_t nbytes = fread(bytes, 1, FILE_BLOCK_SAMPLES * 2, input);
        if (!nbytes) {
            if (ferror(input)) { perror("Read RF input"); exit_status = 1; }
            break;
        }
        if (nbytes & 1u) { fprintf(stderr, "Input ends with an incomplete s16le sample\n"); exit_status = 1; break; }
        size_t count = nbytes / 2;
        for (size_t i = 0; i < count; i++)
            samples[i] = (int16_t)((uint16_t)bytes[i * 2] | ((uint16_t)bytes[i * 2 + 1] << 8));
        gui_rf_block_info_t info = { rate, total_samples, 1, false };
        if (gui_rf_preview_worker_submit(worker, samples, count, &info)) submitted++;
        else {
            rejected++;
            if (!realtime) {
                fprintf(stderr, "Offline queue rejected RF despite bounded batches\n");
                exit_status = 1;
                break;
            }
        }
        total_samples += count;
        /* Offline maximum speed still preserves the entire continuous source:
         * eight blocks are below the input-duration cap even at 16 MSPS. */
        if (!realtime && (submitted % 8u) == 0 &&
            !gui_rf_preview_worker_wait_idle(worker, 30000)) {
            fprintf(stderr, "RF worker failed to drain\n"); exit_status = 1; break;
        }
        if (realtime) pace_realtime(pace_start, total_samples, rate);
        (void)gui_rf_preview_worker_take_frame(worker, frame, FILE_FRAME_BYTES,
                                               &generation, &width, &height, &stats);
    }
    fclose(input);
    if (!gui_rf_preview_worker_wait_idle(worker, 30000)) {
        fprintf(stderr, "RF worker did not finish final input\n"); exit_status = 1;
    }
    (void)gui_rf_preview_worker_take_frame(worker, frame, FILE_FRAME_BYTES,
                                           &generation, &width, &height, &stats);
    gui_rf_preview_worker_get_stats(worker, &stats);
    if (!realtime && stats.blocks_dropped && !exit_status) {
        fprintf(stderr, "Offline RF input was discarded; this run is not a continuous-file validation\n");
        exit_status = 1;
    }
    uint64_t processing_end = file_now_us();
    bool saved = width > 0 && height > 0 && save_ppm(argv[4], frame, width, height);
    if (!saved && !exit_status) exit_status = 2;
    gui_rf_preview_worker_destroy(worker);
    free(bytes); free(samples); free(frame);
    uint64_t wall_end = file_now_us();
    double processing_ms = (double)(processing_end - wall_start) / 1000.0;
    printf("{\"sample_rate_hz\":%u,\"format\":\"%s\",\"mode\":\"%s\","
           "\"samples\":%llu,\"input_seconds\":%.9f,\"blocks_submitted\":%llu,"
           "\"blocks_rejected\":%llu,\"blocks_dropped\":%llu,\"frames\":%llu,"
           "\"width\":%d,\"height\":%d,\"frame_saved\":%s,\"resets\":%llu,"
           "\"processing_wall_ms\":%.3f,\"wall_ms\":%.3f,\"throughput_msps\":%.3f,"
           "\"fps\":%.3f,\"decode_ms\":%.3f,\"latency_ms\":%.3f,"
           "\"h_locked\":%s,\"v_locked\":%s,\"color_locked\":%s,\"reason\":",
           rate, argv[3], realtime ? "realtime" : "max", (unsigned long long)total_samples,
           (double)total_samples / rate, (unsigned long long)submitted,
           (unsigned long long)rejected, (unsigned long long)stats.blocks_dropped,
           (unsigned long long)stats.total_frames, width, height, saved ? "true" : "false",
           (unsigned long long)stats.resets, processing_ms,
           (double)(wall_end - wall_start) / 1000.0,
           processing_ms > 0.0 ? (double)total_samples / (processing_ms * 1000.0) : 0.0,
           stats.fps, stats.decode_ms, stats.latency_ms,
           stats.decode.h_locked ? "true" : "false", stats.decode.v_locked ? "true" : "false",
           stats.decode.color_locked ? "true" : "false");
    print_json_string(stats.decode.reason);

    printf(",\"fields\":%llu,\"vsync_candidates\":%llu,\"fields_accepted\":%llu,"
           "\"fields_rejected\":%llu,\"reject_cadence\":%llu,\"reject_parity\":%llu,"
           "\"reject_line_map\":%llu,\"color_losses\":%llu,\"pause_drops\":%llu,"
           "\"worker_cpu_ms\":%.3f,\"worker_processing_wall_ms\":%.3f,\"budget_wait_ms\":%.3f,"
           "\"cpu_percent_one_core\":%.3f,\"priority_lowered\":%s,\"cpu_time_is_wall\":%s,\"last_reject_reason\":",
           (unsigned long long)stats.total_fields,(unsigned long long)stats.total_vsync_candidates,
           (unsigned long long)stats.total_fields_accepted,(unsigned long long)stats.total_fields_rejected,
           (unsigned long long)stats.total_reject_cadence,(unsigned long long)stats.total_reject_parity,
           (unsigned long long)stats.total_reject_line_map,(unsigned long long)stats.total_color_losses,
           (unsigned long long)stats.pause_drops,stats.processing_cpu_ms,stats.processing_wall_ms,
           stats.budget_wait_ms,stats.cpu_percent,stats.priority_lowered ? "true" : "false",
           stats.cpu_time_is_wall ? "true" : "false");
    print_json_string(stats.decode.last_reject_reason);
    puts("}");
    return exit_status;
}
