#ifndef GUI_CXADC_H
#define GUI_CXADC_H

#include <stdbool.h>

typedef struct gui_app gui_app_t;
// Marker serial values used by synthetic CXADC mode entries.
#define CXADC_MARKER_SERIAL_1CARD "CXADC_1CARD"
#define CXADC_MARKER_SERIAL_2CARD "CXADC_2CARD"
#define CXADC_MARKER_SERIAL_2CARD_CX_CLOCKGEN "CXADC_2CARD_CX_CLOCKGEN"
#define CXADC_MARKER_SERIAL_2CARD_MISRC_CLOCKGEN "CXADC_2CARD_MISRC_CLOCKGEN"

// Detect available CXADC RF cards.
// Returns number of cards detected (0..2).
int gui_cxadc_detect_cards(void);
// Read per-card CXADC center offset (driver DC offset).
// card_idx is zero-based (cxadc0, cxadc1, ...).
// Returns 0 on success, -1 on error.
int gui_cxadc_get_center_offset(int card_idx, int *value_out);

// Set per-card CXADC center offset (driver DC offset).
// value is clamped to the driver range [0, 255].
// Returns 0 on success, -1 on error.
int gui_cxadc_set_center_offset(int card_idx, int value);

// Adjust per-card center offset by delta and optionally return new value.
// Returns 0 on success, -1 on error.
int gui_cxadc_adjust_center_offset(int card_idx, int delta, int *new_value_out);
// Read per-card CXADC tenbit mode (0=8-bit, 1=16-bit tenbit mode).
// Returns 0 on success, -1 on error.
int gui_cxadc_get_tenbit(int card_idx, int *value_out);

// Set per-card CXADC tenbit mode (false=8-bit, true=16-bit tenbit mode).
// Returns 0 on success, -1 on error.
int gui_cxadc_set_tenbit(int card_idx, bool enabled);

// Start CXADC capture mode (card_count: 1 or 2).
// misrc_clockgen_mode selects MISRC v1.5 audio-device matching behavior.
// Returns 0 on success, -1 on error.
int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode);

// Stop CXADC capture mode.
void gui_cxadc_stop(gui_app_t *app);

// Check whether CXADC capture mode is currently running.
bool gui_cxadc_is_running(void);

#endif // GUI_CXADC_H
