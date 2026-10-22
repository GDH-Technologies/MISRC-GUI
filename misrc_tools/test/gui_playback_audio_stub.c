/*
 * MISRC GUI - gui_playback harness audio stub
 *
 * The gui_playback harnesses compile gui_playback.c directly into their own
 * TU (no gui_audio.c: it needs raylib's AudioStream + the soxr monitor
 * resampler). The drag-and-drop audio feed's only gui_audio dependency is
 * the monitor-queue pair below; these stubs satisfy the link so the
 * harnesses keep testing the playback machinery without the audio monitor.
 */
#include <stddef.h>
#include <stdint.h>

#include "../misrc_gui/output/gui_audio.h"

void gui_audio_push_playback(const int16_t *stereo_48k, size_t frames) {
    (void)stereo_48k; (void)frames;
}

void gui_audio_reset_playback_queue(void) {
}
