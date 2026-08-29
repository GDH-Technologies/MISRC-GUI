/*
 * MISRC GUI - preview frame tap contract
 *
 * Split out of gui_preview_v4l2.h so consumers of the tap need not pull in
 * raylib. The tap deals in raw YUYV and plain integers; nothing here touches a
 * GL type, and a unit test of a tap consumer should not need a window library
 * to compile.
 *
 * gui_preview_v4l2.h includes this, so existing includers see no change.
 */

#ifndef GUI_PREVIEW_TAP_H
#define GUI_PREVIEW_TAP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Tee the raw YUYV of every good frame, before the RGBA conversion. The
 * published RGBA slots are the wrong source for anything archival: the triple
 * buffer is deliberately lossy, and it has already discarded the YUYV.
 *
 * The callback runs ON THE CAPTURE THREAD, between VIDIOC_DQBUF and
 * VIDIOC_QBUF. It must not block, must not call GL, and must copy anything it
 * wants to keep -- the buffer is requeued the moment it returns.
 */
typedef void (*preview_tap_fn)(const uint8_t *yuyv, size_t pitch,
                               uint32_t w, uint32_t h, void *user);

typedef struct {
    preview_tap_fn fn;
    void          *user;
} preview_tap_t;

/* The tap must have static storage and outlive the install. One pointer
 * publishes fn and user together so neither can be seen half-updated.
 *
 * There is exactly ONE slot: a second install displaces the first, silently.
 * Application code should therefore register through gui_preview_tap_mux.h
 * rather than calling these directly -- the mux owns the single slot and fans
 * out to every consumer. */
void gui_preview_tap_install(const preview_tap_t *tap);
/* Returns only once no tap callback is still in flight. */
void gui_preview_tap_remove(void);

#endif /* GUI_PREVIEW_TAP_H */
