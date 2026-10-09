#include "../misrc_gui/signal/gui_rf_resampler.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LIBSOXR_ENABLED
#define LIBSOXR_ENABLED 0
#endif

#define PI 3.14159265358979323846
#define SAMPLE_COUNT 131072u

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s (%d)\n", message, __LINE__); ++failures; } \
} while (0)

static size_t convert(uint32_t rate, const int16_t *input, size_t count,
                      bool partitioned, float *output) {
    gui_rf_resampler_t *resampler = gui_rf_resampler_create(rate);
    CHECK(resampler != NULL, "create supported converter");
    if (!resampler) return 0;
    size_t consumed = 0;
    size_t produced = 0;
    const size_t partitions[] = {1, 3, 257, 2, 4093, 31, 2048, 17};
    size_t partition_index = 0;
    while (consumed < count) {
        size_t chunk = partitioned ? partitions[partition_index++ % 8u] : count;
        if (chunk > count - consumed) chunk = count - consumed;
        size_t capacity = gui_rf_resampler_output_capacity(rate, chunk);
        float *scratch = calloc(capacity ? capacity : 1u, sizeof(*scratch));
        CHECK(scratch != NULL, "allocate output");
        if (!scratch) break;
        size_t output_count = 0;
        CHECK(gui_rf_resampler_process(resampler, input + consumed, chunk,
              scratch, capacity, &output_count), "consume entire input chunk");
        CHECK(output_count <= capacity, "output capacity bound");
        if (output_count) memcpy(output + produced, scratch, output_count * sizeof(*output));
        produced += output_count;
        consumed += chunk;
        free(scratch);
    }
    CHECK(isfinite(gui_rf_resampler_delay_samples(resampler)), "finite delay");
    gui_rf_resampler_destroy(resampler);
    return produced;
}

static void partition_and_reset(uint32_t rate) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *whole = calloc(SAMPLE_COUNT * 2u, sizeof(*whole));
    float *split = calloc(SAMPLE_COUNT * 2u, sizeof(*split));
    CHECK(input && whole && split, "allocate equivalence fixture");
    if (!input || !whole || !split) goto cleanup;
    uint32_t random = 0x9183abcdu;
    for (size_t index = 0; index < SAMPLE_COUNT; ++index) {
        random = random * 1664525u + 1013904223u;
        input[index] = (int16_t)(random >> 16);
    }
    size_t n_whole = convert(rate, input, SAMPLE_COUNT, false, whole);
    size_t n_split = convert(rate, input, SAMPLE_COUNT, true, split);
    CHECK(n_whole == n_split, "chunk partition preserves output count");
    if (n_whole == n_split) {
        float maximum_error = 0.0f;
        for (size_t index = 0; index < n_whole; ++index) {
            float error = fabsf(whole[index] - split[index]);
            if (error > maximum_error) maximum_error = error;
        }
        CHECK(maximum_error < 2e-6f, "chunk partition preserves samples");
    }

    gui_rf_resampler_t *resampler = gui_rf_resampler_create(rate);
    size_t capacity = gui_rf_resampler_output_capacity(rate, SAMPLE_COUNT);
    float *scratch = calloc(capacity ? capacity : 1u, sizeof(*scratch));
    CHECK(resampler && scratch, "allocate reset fixture");
    if (resampler && scratch) {
        size_t first_count = 0;
        size_t reset_count = 0;
        CHECK(gui_rf_resampler_process(resampler, input, SAMPLE_COUNT,
              scratch, capacity, &first_count), "initial process");
        gui_rf_resampler_reset(resampler);
        CHECK(gui_rf_resampler_process(resampler, input, SAMPLE_COUNT,
              split, capacity, &reset_count), "process after reset");
        CHECK(first_count == reset_count, "reset restores initial output count");
        CHECK(first_count == reset_count &&
              memcmp(scratch, split, first_count * sizeof(*scratch)) == 0,
              "reset restores history and phase");
    }
    free(scratch);
    gui_rf_resampler_destroy(resampler);
cleanup:
    free(input);
    free(whole);
    free(split);
}

static double tone_gain(uint32_t rate, double frequency) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *output = calloc(SAMPLE_COUNT * 2u, sizeof(*output));
    CHECK(input && output, "allocate tone fixture");
    if (!input || !output) { free(input); free(output); return 0.0; }
    for (size_t index = 0; index < SAMPLE_COUNT; ++index)
        input[index] = (int16_t)lrint(16384.0 * sin(2.0 * PI * frequency * index / rate));
    size_t count = convert(rate, input, SAMPLE_COUNT, true, output);
    CHECK(count > 2048u, "enough settled tone samples");
    double sum = 0.0;
    size_t begin = 1024u;
    size_t end = count > 1024u ? count - 1024u : 0;
    for (size_t index = begin; index < end; ++index) sum += output[index] * output[index];
    double gain = end > begin ? sqrt(sum / (end - begin)) / (0.5 / sqrt(2.0)) : 0.0;
    free(input);
    free(output);
    return gain;
}

static void dc_and_frequency(uint32_t rate, double stop_frequency) {
    int16_t *input = malloc(SAMPLE_COUNT * sizeof(*input));
    float *output = calloc(SAMPLE_COUNT * 2u, sizeof(*output));
    CHECK(input && output, "allocate DC fixture");
    if (!input || !output) { free(input); free(output); return; }
    for (size_t index = 0; index < SAMPLE_COUNT; ++index) input[index] = 8192;
    size_t count = convert(rate, input, SAMPLE_COUNT, true, output);
    double maximum_error = 0.0;
    for (size_t index = 2048u; index < count; ++index) {
        double error = fabs(output[index] - 0.25);
        if (error > maximum_error) maximum_error = error;
    }
    CHECK(count > 2048u && maximum_error < 2e-5, "unity DC gain after settling");
    free(input);
    free(output);
    double pass_gain = tone_gain(rate, 7500000.0);
    CHECK(pass_gain > 0.96 && pass_gain < 1.02, "retain 7.5 MHz VHS RF");
    if (stop_frequency > 0.0) {
        double stop_gain = tone_gain(rate, stop_frequency);
        CHECK(stop_gain < 0.025, "reject RF above output Nyquist before decimation");
    }
}

static void bypass_and_contract(void) {
    gui_rf_resampler_t *resampler = gui_rf_resampler_create(20000000u);
    const int16_t input[] = {-32768, -16384, -1, 0, 1, 16384, 32767};
    float output[7];
    size_t count = 99u;
    CHECK(resampler != NULL, "20 MSPS converter");
    CHECK(gui_rf_resampler_process(resampler, input, 7u, output, 6u, &count) == false
          && count == 0u, "small output rejected before consuming input");
    CHECK(gui_rf_resampler_process(resampler, input, 7u, output, 7u, &count)
          && count == 7u, "20 MSPS bypass count");
    for (size_t index = 0; index < 7u; ++index)
        CHECK(output[index] == (float)input[index] / 32768.0f, "exact normalized bypass");
    CHECK(gui_rf_resampler_delay_samples(resampler) == 0.0, "bypass delay");
    CHECK(gui_rf_resampler_process(resampler, NULL, 0, NULL, 0, &count) && count == 0,
          "empty chunk is not end of stream");
    gui_rf_resampler_destroy(resampler);
    CHECK(gui_rf_resampler_create(0) == NULL, "reject zero rate");
    CHECK(gui_rf_resampler_create(15000000u) == NULL, "reject insufficient input band");
    CHECK(gui_rf_resampler_create(161000000u) == NULL, "reject rate outside contract");
    CHECK(gui_rf_resampler_output_capacity(16000000u, SIZE_MAX) == 0,
          "oversized output bound cannot wrap");
#if !LIBSOXR_ENABLED
    CHECK(gui_rf_resampler_create(25000000u) == NULL, "arbitrary rate requires soxr");
    CHECK(gui_rf_resampler_output_capacity(25000000u, 1000) == 0,
          "unsupported rate has no output bound");
#endif
}

static void impulse_delay(uint32_t rate, unsigned factor, double expected_delay) {
    int16_t input[256] = {0};
    input[64] = 16384;
    float output[256] = {0};
    gui_rf_resampler_t *resampler = gui_rf_resampler_create(rate);
    size_t count = 0;
    CHECK(resampler != NULL, "impulse converter");
    CHECK(gui_rf_resampler_delay_samples(resampler) == expected_delay, "reported FIR group delay");
    CHECK(gui_rf_resampler_process(resampler, input, 256, output, 256, &count), "impulse process");
    CHECK(count == 256u / factor, "halfband count follows fixed phase");
    double total = 0.0;
    double moment = 0.0;
    for (size_t index = 0; index < count; ++index) {
        total += output[index];
        moment += index * output[index];
    }
    CHECK(fabs(total - 0.5 / factor) < 2e-5, "impulse gain matches decimation factor");
    CHECK(fabs(moment / total - (64.0 / factor + expected_delay)) < 2e-4,
          "impulse sample position matches causal group delay");
    /* Odd chunk sizes must retain the decimation phase across buffer edges. */
    gui_rf_resampler_reset(resampler);
    size_t first = 0;
    size_t second = 0;
    CHECK(gui_rf_resampler_process(resampler, input, 65, output, 256, &first), "odd first chunk");
    CHECK(gui_rf_resampler_process(resampler, input + 65, 191, output + first,
          256 - first, &second), "remaining chunk");
    CHECK(first + second == count, "phase survives odd chunk boundary");
    gui_rf_resampler_destroy(resampler);
}

int main(void) {
    bypass_and_contract();
    const uint32_t exact_rates[] = {20000000u, 40000000u, 80000000u};
    for (size_t index = 0; index < 3u; ++index) {
        partition_and_reset(exact_rates[index]);
        dc_and_frequency(exact_rates[index], index ? 12500000.0 : 0.0);
    }
    impulse_delay(40000000u, 2u, 7.5);
    impulse_delay(80000000u, 4u, 11.25);
    CHECK(tone_gain(80000000u, 30000000.0) < 0.025, "first 80 MSPS stage rejects high RF");
#if LIBSOXR_ENABLED
    const uint32_t rates[] = {16000000u, 25000000u, 28636363u, 54000000u, 160000000u};
    for (size_t index = 0; index < 5u; ++index) {
        partition_and_reset(rates[index]);
        dc_and_frequency(rates[index], index ? 11000000.0 : 0.0);
    }
#endif
    if (failures) { fprintf(stderr, "%d RF resampler checks failed\n", failures); return 1; }
    puts("RF resampler continuity, gain, anti-aliasing, phase and reset checks passed");
    return 0;
}
