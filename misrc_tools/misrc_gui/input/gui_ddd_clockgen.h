/*
 * MISRC GUI - DdD + Clockgen Lite audio sync mode
 *
 * A capture mode that runs the DomesdayDuplicator (DdD) RF sampler in parallel
 * with a Clockgen Lite USB audio device (78125 Hz / 2ch), mirroring the
 * ddd-capture-toolkit's DdD + clockgen sync flow but in-process: the GUI's own
 * RF + audio record pipeline handles encoding instead of shelling out to
 * sox|flac. Both threads are started back-to-back at capture start (same
 * pattern as the existing CXADC Clockgen mode) and feed BUF_CAPTURE_RF /
 * BUF_CAPTURE_AUDIO respectively.
 *
 * The "[DdD] Clockgen" device entry is synthetic: it reuses DEVICE_TYPE_DDD
 * (so the existing DdD RF open/start/stop path drives RF) and is marked by
 * storing DDD_CLOCKGEN_MARKER_SERIAL in device_info_t.serial. The clockgen
 * audio lifecycle is layered on top of the DdD RF lifecycle via the helpers
 * declared here.
 *
 * Detection mirrors ddd-capture-toolkit config.find_clockgen_device(): an ALSA
 * card whose name matches clockgen / cxadc / pcm270x patterns, or whose
 * /proc/asound/cardN/stream0 advertises the 78125 Hz Clockgen Lite rate.
 *
 * Audio is captured at ~78125 Hz / 2ch (L+R) and packed into the GUI's fixed
 * 4ch s24le interleaved BUF_CAPTURE_AUDIO frame (CH1=L, CH2=R, CH3=0, CH4=0),
 * matching the format contract the audio writer/monitor thread expects.
 */

#ifndef GUI_DDD_CLOCKGEN_H
#define GUI_DDD_CLOCKGEN_H

#ifdef ENABLE_DDD

#include <stdbool.h>
#include "../core/gui_app.h"   /* device_info_t, gui_app_t */

// Sentinel stored in device_info_t.serial to mark the synthetic
// "[DdD] Clockgen" device entry (reuses DEVICE_TYPE_DDD for the RF path).
#define DDD_CLOCKGEN_MARKER_SERIAL "DDD_CLOCKGEN"

// True iff dev is the synthetic "[DdD] Clockgen" entry (DdD type + marker
// serial). Shared by gui_capture.c (enumerate/start/stop) and gui_ui.c
// (mode label) so they agree on which DdD entry is the clockgen variant.
bool gui_ddd_clockgen_device_mode(const device_info_t *dev);

// Detect a Clockgen Lite audio device on the host (ALSA card-name match or
// 78125 Hz sample-rate support). Returns true if one is present.
// Honors the MISRC_DDD_CLOCKGEN_ALSA_DEVICE env override (forces that ALSA
// device id, skipping the probe).
bool gui_ddd_clockgen_detect(void);

// Start the Clockgen Lite audio capture: ensures BUF_CAPTURE_AUDIO is
// initialised, opens the ALSA device at ~78125 Hz / 2ch, sets
// app->audio_sample_rate, and launches the audio thread.
// Returns 0 on success, -1 if no clockgen/ALSA unavailable (caller may
// continue RF-only).
int gui_ddd_clockgen_start(gui_app_t *app);

// Stop the Clockgen Lite audio capture (joins the audio thread, closes ALSA).
void gui_ddd_clockgen_stop(gui_app_t *app);

// True if the Clockgen Lite audio capture is currently running.
bool gui_ddd_clockgen_is_running(void);

#endif // ENABLE_DDD

#endif // GUI_DDD_CLOCKGEN_H
