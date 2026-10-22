#ifndef GUI_RF_RESAMPLER_H
#define GUI_RF_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_rf_resampler gui_rf_resampler_t;

/* Converts real, signed ADC PCM to normalized float RF at 20 MSPS.
 * Filter history and fractional sample position persist across calls. */
gui_rf_resampler_t *gui_rf_resampler_create(uint32_t input_rate_hz);
void gui_rf_resampler_destroy(gui_rf_resampler_t *resampler);
void gui_rf_resampler_reset(gui_rf_resampler_t *resampler);
size_t gui_rf_resampler_output_capacity(uint32_t input_rate_hz, size_t input_count);
bool gui_rf_resampler_process(gui_rf_resampler_t *resampler,
                              const int16_t *input, size_t input_count,
                              float *output, size_t output_capacity,
                              size_t *output_count);
/* Signal alignment delay in 20 MHz output samples. This excludes buffered
 * availability latency, which the worker measures from its output cursor. */
double gui_rf_resampler_delay_samples(const gui_rf_resampler_t *resampler);

#endif
