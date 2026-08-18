/*
 * MISRC GUI - USB video preview (V4L2)
 *
 * A display-only live picture from a USB analog capture dongle, shown next to
 * the RF scopes so you can see the tape while it is being captured. It is NOT
 * an RF source and can never be one: such a device delivers far too little
 * bandwidth, cannot meet the transport's line-count floor, and is an analog
 * digitiser rather than a bit-transparent pipe.
 *
 * This module deliberately does NOT use misrc_capture/simple_capture, which
 * has four properties that are fine for the RF ingest path and unacceptable
 * for a preview running beside it:
 *
 *   - its capture thread requests SCHED_FIFO at maximum priority, which would
 *     put a 25fps preview in the same scheduling class as RF ingest;
 *   - its loop treats select() timeout and error identically and never reads
 *     errno, so an unplugged device makes select() return readable forever and
 *     the thread spins at 100% CPU reporting nothing;
 *   - teardown blocks for up to the 2s select timeout because nothing wakes
 *     the thread;
 *   - its data_info struct has no error field, so device loss is invisible to
 *     the consumer.
 *
 * Accordingly this reader runs at default priority, classifies every errno and
 * exits the loop on any fatal class, wakes its thread through an eventfd so
 * teardown is immediate, and surfaces device loss as state the panel paints.
 *
 * One device, one stream: the module is a singleton. Several panels may show
 * the same picture, and the first one to render each frame performs the single
 * texture upload.
 *
 * Threading: the capture thread only ever writes malloc'd RGBA slots. Every GL
 * call lives on the render thread. The panel vtable's process hook is NULL, so
 * the display thread never touches any of this.
 */

#ifndef GUI_PREVIEW_V4L2_H
#define GUI_PREVIEW_V4L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "raylib.h"

typedef enum {
    PREVIEW_STATE_UNSUPPORTED = 0, /* not a Linux build */
    PREVIEW_STATE_NO_DEVICE,       /* enumeration found nothing usable */
    PREVIEW_STATE_DISCONNECTED,    /* a device is selected but not opened */
    PREVIEW_STATE_CONNECTING,      /* streaming on, no frame published yet */
    PREVIEW_STATE_STREAMING,
    PREVIEW_STATE_STALLED,         /* connected but no frame recently; recoverable */
    PREVIEW_STATE_ERROR,           /* open failed or the device went away */
    PREVIEW_STATE_POPPED_OUT       /* a child process owns the device */
} preview_state_t;

typedef struct {
    uint32_t w, h;
    uint32_t fps_num, fps_den;
    char     label[24];            /* "720x576 @ 25" - prebuilt for the picker */
} preview_mode_t;

#define PREVIEW_MAX_MODES   32
#define PREVIEW_MAX_DEVICES 16

typedef struct {
    char           path[32];       /* "/dev/video0" */
    char           card[40];       /* v4l2_capability.card */
    preview_mode_t modes[PREVIEW_MAX_MODES];
    int            n_modes;        /* YUYV, discrete, best first */
} preview_device_t;

typedef struct {
    preview_state_t state;
    char     device_path[32];
    char     device_card[40];
    uint32_t width, height;
    uint32_t fps_num, fps_den;     /* as read back, never as requested */
    uint64_t frames, drops, seq_gaps;
    double   fps_measured;
    double   last_frame_time;      /* GetTime() when the last frame published */
    int      err_errno;
    char     err_text[128];
    int      child_pid;            /* non-zero only while popped out */
    int      viewers;              /* panels currently showing preview */
} preview_status_t;

/* ---- lifecycle ---------------------------------------------------------- */

void gui_preview_init(const char *argv0);   /* stash argv[0] for popout */
void gui_preview_shutdown(void);            /* kills any child, releases device */
void gui_preview_tick(void);                /* once per frame, render thread */
bool gui_preview_supported(void);           /* false on non-Linux */

/* ---- enumeration -------------------------------------------------------- */

void                    gui_preview_refresh_devices(void);
/* Borrowed; valid until the next refresh. Panels hold indices, not pointers. */
const preview_device_t *gui_preview_devices(size_t *count);
int  gui_preview_selected_device(void);
int  gui_preview_selected_mode(void);
void gui_preview_select(int device_index, int mode_index);

/* ---- stream ------------------------------------------------------------- */

int  gui_preview_connect(void);     /* 0 on success; sets state/err_text on failure */
void gui_preview_disconnect(void);  /* render thread only; returns in microseconds */
preview_status_t gui_preview_get_status(void);
/* Negotiated bytes-per-line. May exceed width*2; a consumer that assumes
 * width*2 will shear the picture. */
uint32_t gui_preview_negotiated_pitch(void);

/* ---- panel facing ------------------------------------------------------- */

void gui_preview_panel_attach(void);
/* Never blocks: it runs under panel_config_lock, so the actual teardown is
 * deferred to the next gui_preview_tick(). */
void gui_preview_panel_detach(void);

/* Consumes a published frame and uploads it. Idempotent within a frame, so N
 * panels cost one upload. Returns true if a new frame was uploaded. */
bool gui_preview_frame_sync(void);

/* The shared draw path: aspect-fit picture plus every placeholder state.
 * Must not touch gui_app_t, Clay or gui_text_* -- the popout child has none
 * of them. */
void gui_preview_draw(Rectangle bounds, bool with_status);

/* ---- frame tap ----------------------------------------------------------- */
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
 * publishes fn and user together so neither can be seen half-updated. */
void gui_preview_tap_install(const preview_tap_t *tap);
/* Returns only once no tap callback is still in flight. */
void gui_preview_tap_remove(void);

/* ---- popout ------------------------------------------------------------- */

int  gui_preview_popout(void);
void gui_preview_reclaim(void);
int  gui_preview_child_main(const char *device, const char *fmt_spec, int parent_pid);

/* ---- headless diagnostics ----------------------------------------------- */
/* Run without a window so the reader's unplug, teardown and scheduling
 * behaviour can be verified without a GUI. */
int gui_preview_probe_main(void);
int gui_preview_selftest_main(void);
int gui_preview_tap_test_main(const char *device, int seconds);
int gui_preview_dump_frame_main(const char *device, const char *out_png);
int gui_preview_probe_stream_main(const char *device, int seconds);

#endif /* GUI_PREVIEW_V4L2_H */
