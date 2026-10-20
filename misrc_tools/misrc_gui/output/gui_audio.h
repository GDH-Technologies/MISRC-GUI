#ifndef GUI_AUDIO_H
#define GUI_AUDIO_H

#include "../core/gui_app.h"
#include "../../common/buffer_manager.h"

// Start audio writer/monitor thread.
// If no audio outputs are enabled in settings, this is a no-op and returns 0.
int gui_audio_start(gui_app_t *app, buffer_manager_t *bufmgr);

// Stop audio writer/monitor thread if running.
void gui_audio_stop(gui_app_t *app);

// True if audio thread is running.
bool gui_audio_is_running(void);

// Enable/disable playback monitoring to system audio device.
// This affects live monitoring only (not file writing).
void gui_audio_set_playback_enabled(gui_app_t *app, bool enabled);

// Pump audio playback (call from main thread once per frame).
void gui_audio_update_playback(gui_app_t *app);

// Push demodulated audio (stereo int16 @ 48k) into the live monitor queue so
// the "Audio Mon" button plays it. Safe to call from the render thread.
// The user must enable Audio Mon to hear it (mirrors device-audio monitoring).
void gui_audio_push_demod(const int16_t *stereo_48k, size_t frames);

// Push drag-and-drop playback audio (stereo int16 @ 48k) into the SAME live
// monitor queue. The position-synced feed runs on the playback thread
// (gui_playback.c); the producers are mutually exclusive (capture audio
// thread / demod render path / playback feed). Same contract: Audio Mon
// gates audibility, drop-oldest on overflow.
void gui_audio_push_playback(const int16_t *stereo_48k, size_t frames);

// Drop everything currently queued in the monitor queue (called by the
// playback audio feed on a seek/loop jump so stale audio never plays).
void gui_audio_reset_playback_queue(void);

#endif // GUI_AUDIO_H
