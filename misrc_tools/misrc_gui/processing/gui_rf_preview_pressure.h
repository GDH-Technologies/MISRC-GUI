/* Capture pressure hysteresis. GPL-3.0-or-later. */
#ifndef GUI_RF_PREVIEW_PRESSURE_H
#define GUI_RF_PREVIEW_PRESSURE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    bool paused, tracking_high, tracking_low, have_drops;
    uint64_t high_since_ms, low_since_ms, last_ms, last_drops;
} gui_rf_preview_pressure_t;
/* Use monotonic byte totals for pressure, not independently wrapped ring
 * head/tail. Caller samples bytes_read before bytes_written. Clamps a transient
 * read-before-write accounting publication and reset to zero occupancy. */
unsigned gui_rf_preview_pressure_fill_percent(uint64_t written, uint64_t read,
                                              uint64_t capacity);
/* One producer owns this state. Ratios are percent of allocated capacity.
 * >=85% or new recording/capture drops pauses immediately; >=60% for 150ms
 * pauses. All relevant buffers <=35%, without new drops, for 2s resumes. */
bool gui_rf_preview_pressure_update(gui_rf_preview_pressure_t *state,
                                    uint64_t now_ms, unsigned raw_percent,
                                    unsigned record_percent, uint64_t drops,
                                    bool capturing);
#endif
