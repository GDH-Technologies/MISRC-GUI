/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CXADC_SAMPLES_H
#define CXADC_SAMPLES_H
#include <stdint.h>
/* Visualization only; the native RAW recording bypasses this conversion. */
static inline int16_t cxadc_decode_sample_8bit(uint8_t sample) {
    return (int16_t)(((int)sample - 128) * 16);
}
#endif
