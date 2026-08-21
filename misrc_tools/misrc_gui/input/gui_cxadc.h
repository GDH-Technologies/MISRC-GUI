#ifndef GUI_CXADC_H
#define GUI_CXADC_H

#include <stdbool.h>

typedef struct gui_app gui_app_t;
// Marker serial values used by synthetic CXADC mode entries.
#define CXADC_MARKER_SERIAL_1CARD "CXADC_1CARD"
#define CXADC_MARKER_SERIAL_2CARD "CXADC_2CARD"
#define CXADC_MARKER_SERIAL_2CARD_CX_CLOCKGEN "CXADC_2CARD_CX_CLOCKGEN"
#define CXADC_MARKER_SERIAL_2CARD_MISRC_CLOCKGEN "CXADC_2CARD_MISRC_CLOCKGEN"
// MISRC Clockgen is a first-class device type (DEVICE_TYPE_MISRC_CLOCKGEN),
// not a CXADC variant. This marker serial identifies its synthetic entry so
// reconnect/selection can match it by name/serial.
#define MISRC_CLOCKGEN_MARKER_SERIAL "MISRC_CLOCKGEN"

// Detect available CXADC RF cards.
// Returns number of cards detected (0..2).
int gui_cxadc_detect_cards(void);

// Detect whether a MISRC Clockgen USB-audio capture endpoint is present on the
// host (WASAPI on Windows, ALSA on Linux), independent of CXADC RF cards. Used
// to surface the "[CXADC] MISRC Clockgen" entry on rigs that have the clockgen
// audio device but no cxadcN card nodes (the MISRC v1.5 presents purely as a
// USB audio-class device).
// Returns true if a matching endpoint is found.
bool gui_cxadc_detect_misrc_clockgen_audio(void);
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

// CXADC capture gain ("level"), the driver's 0-31 input attenuator setting.
// Linux/sysfs only: cxadc-win exposes no equivalent, so these fail with
// errno=ENOTSUP on Windows and callers should not offer the control there.
#define CXADC_SYSFS_LEVEL_MIN 0
#define CXADC_SYSFS_LEVEL_MAX 31

// Read per-card CXADC capture gain.
// card_idx is zero-based (cxadc0, cxadc1, ...).
// Returns 0 on success, -1 on error.
int gui_cxadc_get_level(int card_idx, int *value_out);

// Set per-card CXADC capture gain.
// value is clamped to the driver range [0, 31]. The write is read back and
// verified; a mismatch fails with errno=ERANGE.
// Returns 0 on success, -1 on error.
int gui_cxadc_set_level(int card_idx, int value);

// Adjust per-card capture gain by delta and optionally return new value.
// Returns 0 on success, -1 on error.
int gui_cxadc_adjust_level(int card_idx, int delta, int *new_value_out);
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

// Start ONLY the clockgen audio capture thread (WASAPI/ALSA) feeding
// BUF_CAPTURE_AUDIO + headswitch ingest, with no CXADC RF cards and no
// extraction/display/audio-monitor thread ownership. Used by MISRC Clockgen
// mode, which owns its RF feed separately (hsdaoh raw-parser) and shares one
// set of extraction/display/audio-monitor threads. Returns 0 on success.
int gui_cxadc_start_clockgen_audio(gui_app_t *app, bool misrc_clockgen_mode);

// Stop/join the clockgen audio thread and close the audio device. Does NOT
// touch extraction/display/audio-monitor/cards (owned by the caller's RF path).
void gui_cxadc_stop_clockgen_audio(void);

#endif // GUI_CXADC_H
