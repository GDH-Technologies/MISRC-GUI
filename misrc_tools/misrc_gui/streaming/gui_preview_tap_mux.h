/*
 * MISRC GUI - preview frame tap multiplexer
 *
 * gui_preview_v4l2.c publishes its tap through a single atomic slot, so a
 * second gui_preview_tap_install() silently displaces the first. With one
 * consumer that was fine; with two -- the reference MKV recording and the RTSP
 * stream -- it means starting a stream stops the recording receiving frames,
 * and nothing reports it. Silent data loss, not an error.
 *
 * The mux owns that single slot and fans out to every registered consumer:
 *
 *     capture thread --tap--> mux --+--> gui_video_record
 *                                   +--> gui_rtsp_stream
 *
 * It installs the underlying tap on the first add and removes it on the last,
 * so gui_preview_v4l2.c sees exactly the install/remove lifecycle it saw when
 * there was one consumer.
 *
 * Threading: add/remove are called from the render thread; the forwarding
 * callback runs on the capture thread and inherits the tap's contract -- it
 * must not block and must not call GL.
 */

#ifndef GUI_PREVIEW_TAP_MUX_H
#define GUI_PREVIEW_TAP_MUX_H

#include "../input/gui_preview_tap.h"

/* Register a consumer. The tap must have static storage and outlive the add,
 * exactly as gui_preview_tap_install() requires.
 * Returns 0 on success, -1 if the mux is full or tap is unusable. */
int gui_preview_mux_add(const preview_tap_t *tap);

/* Deregister a consumer. Returns only once that consumer's callback is no
 * longer in flight, so the caller may then free the state its `user` points at.
 * Removing a tap that was never added is a no-op. */
void gui_preview_mux_remove(const preview_tap_t *tap);

/* Registered consumer count. For status display and tests. */
int gui_preview_mux_count(void);

#endif /* GUI_PREVIEW_TAP_MUX_H */
