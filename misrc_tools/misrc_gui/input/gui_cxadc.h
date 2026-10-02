#ifndef GUI_CXADC_H
#define GUI_CXADC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct gui_app gui_app_t;

// Maximum CXADC cards the GUI drives (card 0 -> channel A, card 1 -> B).
#define CXADC_MAX_CARDS 2
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
// Read per-card CXADC tenbit mode (0=8-bit, 1=16-bit tenbit mode).
// Returns 0 on success, -1 on error.
int gui_cxadc_get_tenbit(int card_idx, int *value_out);

// Set per-card CXADC tenbit mode (false=8-bit, true=16-bit tenbit mode).
// Returns 0 on success, -1 on error.
int gui_cxadc_set_tenbit(int card_idx, bool enabled);

// Resolve a card's real hardware sample rate (Hz) for the given tenbit mode.
// Reads crystal + tenxfsc from sysfs (Linux) and applies the cxadc rate rule:
//   8-bit + tenxfsc=1 (upsampled 35.8) is presented as the crystal rate;
//   10-bit + tenxfsc=1 (17.9 on stock) is exposed as a real tier;
//   tenxfsc=2/3 -> 40 MSPS (20 in 10-bit); tenxfsc=0 -> crystal (halved in 10-bit).
// Supports stock 28.6, 40 MHz mod/clockgen, and 54 MHz crystal-mod cards.
// Returns false when unreadable (Windows, no cxadcN node, missing sysfs) so
// callers fall back to the 40/20 MSPS clockgen baseline.
bool gui_cxadc_get_sample_rate_hz(int card_idx, bool tenbit, uint32_t *rate_hz_out);

// Measured (empirical) CXADC card feed rate. sysfs crystal/tenxfsc can be
// stale on hardware-modded cards (e.g. a 40 MHz-modded card still reporting
// the stock 28.636 MHz crystal), which caps the rate readout and the
// resample options at 28.6 while the card actually feeds 40 MSPS. The
// measured rate is timed from a real blocking read of /dev/cxadcN and
// cached per (card, tenbit mode) for the session; refreshed at startup
// enumeration (gui_cxadc_probe_card_rates) and at every capture start.
// Falls back to the sysfs-derived rate when nothing was measured.
bool gui_cxadc_get_effective_rate_hz(int card_idx, bool tenbit, uint32_t *rate_hz_out);

// Probe the present cards now (startup/enumeration hook): time a short
// blocking-read window per card in the given tenbit modes, snap to the
// known rate tiers (common/cxadc_rate_tiers.h), cache the results, and log
// any sysfs-vs-measured disagreement. No-op when a CXADC capture is already
// running (the capture-start probe owns the cards then) or when
// MISRC_GUI_NO_CXADC_RATE_PROBE=1 is set. Note: the probe briefly reads the
// cards, so it must not run while another process is capturing on them.
// Returns the number of newly measured (card, mode) entries.
int gui_cxadc_probe_card_rates(int card_count, const bool tenbit_modes[CXADC_MAX_CARDS]);

// Start CXADC capture mode (card_count: 1 or 2).
// misrc_clockgen_mode selects MISRC v1.5 audio-device matching behavior.
// Returns 0 on success, -1 on error.
int gui_cxadc_start(gui_app_t *app, int card_count, bool misrc_clockgen_mode);

// Stop CXADC capture mode.
void gui_cxadc_stop(gui_app_t *app);

// Check whether CXADC capture mode is currently running.
bool gui_cxadc_is_running(void);

// Direct native RAW recording support (FLAC off + no resampling).
// True when the running CXADC capture can feed channel `channel` (card
// index == channel) byte-exact native samples of the requested RAW bit
// width: rf_bits 8 requires the card in 8-bit mode (1 byte/sample),
// rf_bits 16 requires tenbit mode (2 bytes/sample). bytes_per_sample_out
// may be NULL. False for anything else (capture not running, no such
// card, width mismatch).
bool gui_cxadc_direct_record_available(int channel, uint8_t rf_bits, size_t *bytes_per_sample_out);

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
