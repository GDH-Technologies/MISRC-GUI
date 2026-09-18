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
#include "gui_preview_sdtv.h"
#include "gui_preview_tap.h"
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

/* An analog input jack: Composite, S-Video, Tuner... A webcam reports exactly
 * one and it is not interesting; an SDTV dongle reports several and which one
 * is selected decides what you are actually looking at. */
typedef struct {
    uint32_t index;                /* v4l2_input.index; what S_INPUT takes */
    char     name[32];             /* v4l2_input.name, e.g. "Composite" */
    bool     is_sdtv;              /* V4L2_IN_CAP_STD */
} preview_input_t;

/* A video standard as ENUMSTD reports it. `id` is a v4l2_std_id, which is a
 * mask: several entries can share bits, so an id is only ever matched back to
 * an entry by exact equality. */
typedef struct {
    uint64_t id;
    char     name[24];             /* "NTSC", "PAL"... */
    uint32_t fps_num, fps_den;     /* from frameperiod, inverted to a rate */
    uint32_t framelines;
} preview_standard_t;

#define PREVIEW_MAX_MODES   32
#define PREVIEW_MAX_DEVICES 16
#define PREVIEW_MAX_INPUTS  8
#define PREVIEW_MAX_STDS    12

typedef struct {
    char           path[32];       /* "/dev/video0" */
    char           card[40];       /* v4l2_capability.card */
    preview_mode_t modes[PREVIEW_MAX_MODES];
    int            n_modes;        /* best first; for SDTV, derived per standard */

    /* Analog-capture facing. n_inputs is 0 on a device that reports none. */
    preview_input_t    inputs[PREVIEW_MAX_INPUTS];
    int                n_inputs;
    preview_standard_t stds[PREVIEW_MAX_STDS];
    int                n_stds;
    /* Which standard modes[] was derived for; -1 when the device has none.
     * The mode list is standard-specific, so the two travel together. */
    int                std_index;
    /* True when this is a standard-definition analog capture device rather than
     * a webcam: it has a video standard, its frame sizes are a scaler range
     * rather than a list, and Input/Standard are meaningful controls. */
    bool               is_sdtv;
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

    /* Analog state, as read back after connect -- never as requested. */
    int      input;                /* -1 when the device reports no inputs */
    char     input_name[32];
    char     std_name[24];         /* "" when the device has no standard */
    uint32_t sar_num, sar_den;     /* sample aspect of the negotiated raster */
    uint32_t dar_num, dar_den;     /* display aspect it should be shown at */
    uint32_t field;                /* negotiated v4l2_field; ANY means unknown */
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

/* ---- analog input and standard ------------------------------------------ */

int  gui_preview_selected_input(void);     /* index into the device's inputs[] */
int  gui_preview_selected_standard(void);  /* index into the device's stds[] */

/* Applied immediately when streaming: V4L2 lets a bridge switch jack without
 * stopping, and refusing to would make Composite/S-Video unusable mid-tape.
 * Returns 0 on success, -1 if the device rejected it (status carries why). */
int  gui_preview_select_input(int input_index);

/* A standard change resizes the raster (480 vs 576 lines) and every SDTV
 * driver refuses S_STD while streaming, so this only records the choice and
 * rebuilds the mode list; the caller must reconnect for it to take effect.
 * Returns true if the selection changed. */
bool gui_preview_select_standard(int std_index);

/* Restore a persisted selection by name rather than by index: an index is
 * meaningless across devices, and the standard list is device-specific. Both
 * are no-ops when the name is empty or unknown. */
void gui_preview_select_input_by_name(const char *name);
void gui_preview_select_standard_by_name(const char *name);

/* Select a mode from a persisted "YUYV:WxH@num/den" spec. Falls back to the
 * same geometry at any rate, and leaves the selection alone if neither
 * matches. Call it after the standard is chosen: the mode list is rebuilt
 * whenever the standard changes. */
void gui_preview_select_mode_by_spec(const char *spec);

/* The current mode in that same form, for persisting. Always NUL-terminates. */
void gui_preview_mode_spec(char *out, size_t cap);

/* ---- display geometry ---------------------------------------------------- */

/* Aspect override; one of preview_aspect_mode_t. Affects the preview, the
 * reference recording and the RTSP stream alike, so they cannot disagree. */
void gui_preview_set_aspect_mode(int mode);
int  gui_preview_aspect_mode(void);

/* Preview-only crop, in source pixels off each edge. This never reaches the
 * frame tap: a reference recording must keep the full active raster so it
 * stays comparable with a tbc-video-export of the same tape. Values are
 * clamped so at least a 2x2 window survives. */
void gui_preview_set_crop(int top, int bottom, int left, int right);
void gui_preview_get_crop(int *top, int *bottom, int *left, int *right);

/* The display aspect of the negotiated raster, as an "N:M" string for
 * ffmpeg's -aspect. Falls back to "4:3" before anything is negotiated. */
void gui_preview_aspect_arg(char *out, size_t cap);

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

/* A recording holds the stream open independently of the panels. Without this,
 * closing the preview panel mid-recording would silently end the video, and a
 * recording could not start at all unless a panel happened to be showing
 * Preview -- which is the usual state, not an edge case. */
void gui_preview_hold_acquire(void);
void gui_preview_hold_release(void);
int  gui_preview_hold_count(void);

/* Consumes a published frame and uploads it. Idempotent within a frame, so N
 * panels cost one upload. Returns true if a new frame was uploaded. */
bool gui_preview_frame_sync(void);

/* The shared draw path: aspect-fit picture plus every placeholder state.
 * Must not touch gui_app_t, Clay or gui_text_* -- the popout child has none
 * of them. */
void gui_preview_draw(Rectangle bounds, bool with_status);

/* ---- frame tap ----------------------------------------------------------- */
/* preview_tap_t and gui_preview_tap_install/remove live in gui_preview_tap.h,
 * included above: the tap contract is raylib-free, and a unit test of a tap
 * consumer should not have to link a window library to compile.
 *
 * There is one slot. Application code registers through
 * streaming/gui_preview_tap_mux.h, which owns it and fans out. */

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
