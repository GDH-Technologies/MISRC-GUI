/* The USB Reference Video settings dialog. See gui_usbref_settings.h for why
 * this is its own module rather than more rows in upstream's settings panel.
 *
 * Nearly everything below was moved verbatim out of ui/gui_ui.c so the move
 * could be reviewed as a move. The comments are the originals; where one talks
 * about "the settings panel" it now means this window.
 */

#include "gui_usbref_settings.h"
#include "gui_ui.h"
#include "gui_ui_scale.h"

#include "../core/gui_app.h"
#include "../core/gui_settings.h"
#include "../input/gui_preview_v4l2.h"
#include "../net/gui_net.h"
#include "../output/gui_cc_record.h"
#include "../output/gui_video_record.h"
#include "../streaming/gui_mediamtx.h"
#include "../streaming/gui_rtsp_stream.h"

#include <clay.h>
#include <raylib.h>
#include <stdio.h>
#include <string.h>

/* Local copies, exactly as ui/gui_popup.c keeps its own: these are two-line
 * conversions and exporting them from gui_ui.c would be a wider change to
 * upstream's file than this module is worth. */
static inline Clay_Color to_clay_color(Color c)
{
    return (Clay_Color){ (float)c.r, (float)c.g, (float)c.b, (float)c.a };
}

static Clay_String make_string(const char *str)
{
    return (Clay_String){ .isStaticallyAllocated = false,
                          .length = (int32_t)strlen(str),
                          .chars = str };
}

static Color ui_disabled_color(Color c)
{
    // Dim and slightly transparent.
    return (Color){ (unsigned char)(c.r * 0.55f), (unsigned char)(c.g * 0.55f),
                    (unsigned char)(c.b * 0.55f), (unsigned char)(c.a * 0.80f) };
}

/* ------------------------------------------------------------------ state */

static bool s_open = false;

/* Which list picker is up, if any.
 *
 * Device, mode, input and standard are all "choose one of N", and N runs from
 * two (the jacks) to twenty (a webcam's modes). Click-to-cycle is fine at two
 * and unusable at twenty -- the settings panel's own device box carried a
 * comment admitting as much -- so they share ONE sheet instead of four bespoke
 * controls. Adding a fifth list is a case in five small functions, not another
 * popover. */
typedef enum {
    PICK_NONE = 0,
    PICK_DEVICE,
    PICK_MODE,
    PICK_INPUT,
    PICK_STANDARD
} pick_kind_t;

static pick_kind_t s_pick = PICK_NONE;

/* Asks once, the first time the stream is pointed at the network. After the
 * answer is remembered in settings this stays false forever. */
static bool s_rtsp_lan_confirm_open = false;
/* The stream's video encoder settings, which are too many for the row they
 * belong to and are all baked in when ffmpeg is spawned. */
static bool s_rtsp_codec_window_open = false;

/* Bitrate stepper bounds. Kept inside the range gui_settings.c will accept on
 * reload (0, or 100..100000) so a value set here survives a restart, and well
 * inside what is sensible for a 720x576 preview. */
#define RTSP_BITRATE_DEFAULT_KBPS 2000
#define RTSP_BITRATE_MIN_KBPS      500
#define RTSP_BITRATE_MAX_KBPS    20000
#define RTSP_BITRATE_STEP_KBPS     250

void gui_usbref_settings_open(void)  { s_open = true; }
void gui_usbref_settings_close(void) { s_open = false; }
bool gui_usbref_settings_is_open(void) { return s_open; }

bool gui_usbref_settings_handle_escape(gui_app_t *app)
{
    (void)app;
    /* Innermost first: a sheet over the dialog is what ESC is asking about. */
    if (s_rtsp_codec_window_open) { s_rtsp_codec_window_open = false; return true; }
    if (s_rtsp_lan_confirm_open) {
        /* Deliberately does NOT set usbref_rtsp_lan_acknowledged: declining is not an
         * answer worth remembering. */
        s_rtsp_lan_confirm_open = false;
        return true;
    }
    if (s_open) { s_open = false; return true; }
    return false;
}

/* ---------------------------------------------------------------- helpers */

/* Remember which USB preview device is selected, so the next launch can reopen
 * it. Stored by path: /dev/videoN survives a reboot, an enumeration index does
 * not. */
static void gui_ui_remember_preview_device(gui_app_t *app)
{
    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    int sel = gui_preview_selected_device();
    if (sel < 0 || (size_t)sel >= n) return;
    snprintf(app->settings.usbref_device_path, sizeof(app->settings.usbref_device_path),
             "%s", devs[sel].path);
    gui_settings_save(&app->settings);
}

/* The three ways to watch the stream, in the order the panel lists them. One
 * place to ask which row is which, so the layout, the click handler and the
 * clipboard cannot drift apart. */
#define RTSP_URL_COUNT 3

static const char *rtsp_url_kind_label(int kind)
{
    switch (kind) {
        case 0:  return "RTSP";
        case 1:  return "WebRTC";
        default: return "HLS";
    }
}

static const char *rtsp_url_for_kind(const gui_rtsp_stream_status_t *st, int kind)
{
    if (!st) return "";
    switch (kind) {
        case 0:  return st->url_rtsp;
        case 1:  return st->url_webrtc;
        default: return st->url_hls;
    }
}

/* raylib's OpenURL() pastes the string into a shell command and runs it through
 * system(), guarding only against a single quote. These URLs are ours -- a
 * fixed scheme, a fixed path, a port number -- but the host is gethostname() in
 * LAN mode, and gui_rtsp_stream_opts_t::reader_host is a public field a future
 * caller could wire to a settings string a user edits by hand. Stating what a
 * URL may contain is a sounder contract than trusting one blacklisted
 * character, so nothing reaches the shell that is not plainly a URL we built. */
static bool rtsp_url_is_safe_to_open(const char *url)
{
    if (!url || !url[0] || strlen(url) >= 256) return false;

    size_t off;
    if      (strncmp(url, "rtsp://", 7) == 0) off = 7;
    else if (strncmp(url, "http://", 7) == 0) off = 7;
    else return false;
    if (!url[off]) return false;   /* a scheme with no host is not openable */

    for (const char *p = url + off; *p; p++) {
        const bool ok = (*p >= 'a' && *p <= 'z') ||
                        (*p >= 'A' && *p <= 'Z') ||
                        (*p >= '0' && *p <= '9') ||
                        *p == '.' || *p == '-' || *p == '_' ||
                        *p == ':' || *p == '/';
        if (!ok) return false;
    }
    return true;
}


/* Copy one reader URL and say which one, so the status line is not a bare
 * "copied" that leaves you guessing which of the three you got. */
static void rtsp_url_copy(gui_app_t *app, int kind, const char *url)
{
    char msg[64];
    SetClipboardText(url);
    snprintf(msg, sizeof(msg), "%s URL copied", rtsp_url_kind_label(kind));
    gui_app_set_status(app, msg);
}

/* Start or stop the stream from the panel toggle.
 *
 * Acts immediately rather than arming a flag the way the reference-recording
 * toggle does: the panel shows live counters and URLs, and a toggle that only
 * took effect at the next capture would leave both lying. */
bool gui_usbref_set_rtsp_stream(gui_app_t *app, bool want)
{
    if (!want) {
        if (!gui_rtsp_stream_is_running()) return true;
        gui_rtsp_stream_request_stop();
        gui_rtsp_stream_finish();
        app->settings.usbref_rtsp_enabled = false;
        gui_settings_save(&app->settings);
        gui_app_set_status(app, "Stream stopped");
        return true;
    }
    if (gui_rtsp_stream_is_running()) return true;

    if (!gui_mediamtx_probe()) {
        gui_app_set_status(app, "mediamtx was not found; the stream cannot be started");
        return false;
    }

    preview_status_t ps = gui_preview_get_status();
    if (ps.width == 0 || ps.height == 0) {
        /* The stream is a tee off the preview; without a connected device there
         * is no geometry to negotiate and nothing to publish. */
        gui_app_set_status(app, "Connect the USB preview before starting the stream");
        return false;
    }

    gui_rtsp_stream_opts_t opts = {0};
    opts.width = ps.width;
    opts.height = ps.height;
    opts.pitch = gui_preview_negotiated_pitch();
    if (opts.pitch == 0) opts.pitch = ps.width * 2;
    opts.fps_num = ps.fps_num ? ps.fps_num : 25;
    opts.fps_den = ps.fps_den ? ps.fps_den : 1;
    opts.video_device = ps.device_path;
    opts.audio_device = app->settings.usbref_rtsp_audio_device;
    opts.encoder = (rtsp_encoder_t)app->settings.usbref_rtsp_encoder;
    opts.bitrate_kbps = (uint32_t)app->settings.usbref_rtsp_bitrate_kbps;
    opts.deinterlace = app->settings.usbref_rtsp_deinterlace;
    opts.ports = gui_mediamtx_default_config();
    opts.ports.lan = app->settings.usbref_rtsp_lan;
    if (app->settings.usbref_rtsp_port > 0) {
        opts.ports.rtsp = (uint16_t)app->settings.usbref_rtsp_port;
    }
    opts.reader_host = "";
    opts.want_password = app->settings.usbref_rtsp_password;

    char err[192] = {0};
    if (gui_rtsp_stream_start(&opts, err, sizeof(err)) != 0) {
        gui_app_set_status(app, err[0] ? err : "the stream could not be started");
        return false;
    }
    app->settings.usbref_rtsp_enabled = true;
    gui_settings_save(&app->settings);
    /* start() only launches. Whether ffmpeg survived shows up in the panel a
     * moment later, via gui_rtsp_stream_poll(). */
    gui_app_set_status(app, "Starting the stream...");
    return true;
}

static void gui_ui_toggle_rtsp_stream(gui_app_t *app)
{
    (void)gui_usbref_set_rtsp_stream(app, !gui_rtsp_stream_is_running());
}

/* Everything about how the picture is encoded, in one place.
 *
 * These are all read when ffmpeg is spawned, so they are editable only while the
 * stream is stopped -- offering a control that silently would not apply until
 * the next start is worse than greying it out. */
static void render_rtsp_codec_window(gui_app_t *app)
{
    if (!s_rtsp_codec_window_open) return;

    gui_rtsp_stream_status_t cs = gui_rtsp_stream_get_status();
    bool locked = cs.running || cs.starting;
    bool nvenc_ok = gui_rtsp_stream_has_nvenc();

    CLAY(CLAY_ID("RtspCodecBackdrop"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP },
            .zIndex = 34
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 150}
    }) {}

    CLAY(CLAY_ID("RtspCodecWindow"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(.min = 460, .max = 560), CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 18, 18, 16, 16 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER },
            .zIndex = 35
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        CLAY(CLAY_ID("RtspCodecHeader"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 }
        }) {
            CLAY_TEXT(CLAY_STRING("Stream video codec"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING, .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY(CLAY_ID("RtspCodecHeaderSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
            CLAY(CLAY_ID("RtspCodecClose"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        /* Encoder: three explicit choices rather than a cycling box, so the one
         * in force is visible without clicking through the others. */
        CLAY(CLAY_ID("RtspCodecEncRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 }
        }) {
            CLAY(CLAY_ID("RtspCodecEncLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(CLAY_STRING("Encoder"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            for (int e = 0; e < 3; e++) {
                bool avail = (e != 1) || nvenc_ok;
                bool on = (app->settings.usbref_rtsp_encoder == e);
                Color bg = on ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                if (locked || !avail) bg = ui_disabled_color(bg);
                CLAY(CLAY_IDI("RtspCodecEnc", e), {
                    .layout = { .sizing = { CLAY_SIZING_FIXED(86), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                    .backgroundColor = to_clay_color(bg), .cornerRadius = CLAY_CORNER_RADIUS(4)
                }) {
                    CLAY_TEXT(make_string(e == 0 ? "Auto" : (e == 1 ? "NVENC" : "x264")),
                        CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color((locked || !avail) ? ui_disabled_color(COLOR_TEXT) : COLOR_TEXT) }));
                }
            }
        }
        if (!nvenc_ok) {
            CLAY_TEXT(CLAY_STRING("NVENC is unavailable in this ffmpeg build"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        /* Bitrate: software encoder only, which the hint below says plainly
         * rather than leaving it to be discovered. */
        CLAY(CLAY_ID("RtspCodecRateRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 }
        }) {
            CLAY(CLAY_ID("RtspCodecRateLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(CLAY_STRING("Bitrate"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            Color step_bg = locked ? ui_disabled_color(COLOR_BUTTON) : COLOR_BUTTON;
            CLAY(CLAY_ID("RtspCodecRateMinus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(step_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            /* Fixed width, like every other number in this panel. */
            static char rate_buf[32];
            int eff = app->settings.usbref_rtsp_bitrate_kbps > 0
                    ? app->settings.usbref_rtsp_bitrate_kbps : RTSP_BITRATE_DEFAULT_KBPS;
            snprintf(rate_buf, sizeof(rate_buf), "%d kbit/s%s", eff,
                     app->settings.usbref_rtsp_bitrate_kbps > 0 ? "" : " (default)");
            CLAY(CLAY_ID("RtspCodecRateValue"), { .layout = { .sizing = { CLAY_SIZING_FIXED(160), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(make_string(rate_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY(CLAY_ID("RtspCodecRatePlus"), { .layout = { .sizing = { CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(step_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }
        CLAY_TEXT(CLAY_STRING("Bitrate applies to x264 only; NVENC is driven by quality, not a target."),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(COLOR_TEXT_DIM) }));

        /* Deinterlace */
        CLAY(CLAY_ID("RtspCodecDeintRow"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 }
        }) {
            CLAY(CLAY_ID("RtspCodecDeintLabel"), { .layout = { .sizing = { CLAY_SIZING_FIXED(96), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY_TEXT(CLAY_STRING("Deinterlace"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            Color d_bg = app->settings.usbref_rtsp_deinterlace ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
            if (locked) d_bg = ui_disabled_color(d_bg);
            CLAY(CLAY_ID("RtspCodecDeint"), { .layout = { .sizing = { CLAY_SIZING_FIXED(86), CLAY_SIZING_FIXED(30) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(d_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                CLAY_TEXT(app->settings.usbref_rtsp_deinterlace ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY_TEXT(CLAY_STRING("bwdif - smoother motion, more latency"),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        CLAY_TEXT(locked
                    ? CLAY_STRING("These are fixed while the stream runs. Stop it to change them.")
                    : CLAY_STRING("These take effect the next time the stream starts."),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(locked ? COLOR_SYNC_RED : COLOR_TEXT_DIM) }));
    }
}

/* Putting a tape on the network is a different kind of act from the toggles
 * around it, so it is worth one deliberate answer. Only ever shown once: after
 * that usbref_rtsp_lan_acknowledged is set and the bind box behaves like any other
 * toggle. Floats above the settings panel it is launched from -- that panel
 * sits at the implicit zIndex 0, and the gear popover already uses 20, so this
 * takes 30 to clear both. */
static void render_rtsp_lan_confirm(gui_app_t *app)
{
    (void)app;
    if (!s_rtsp_lan_confirm_open) return;

    CLAY(CLAY_ID("RtspLanConfirmBackdrop"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP },
            .zIndex = 36
        },
        .backgroundColor = (Clay_Color){0, 0, 0, 170}
    }) {}

    CLAY(CLAY_ID("RtspLanConfirmWindow"), {
        .layout = {
            .sizing = { CLAY_SIZING_FIT(.min = 420, .max = 520), CLAY_SIZING_FIT(0) },
            .layoutDirection = CLAY_TOP_TO_BOTTOM,
            .padding = { 18, 18, 16, 16 },
            .childGap = 10
        },
        .floating = {
            .attachTo = CLAY_ATTACH_TO_ROOT,
            .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER },
            .zIndex = 37
        },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8)
    }) {
        CLAY_TEXT(CLAY_STRING("Share this stream on the network?"),
            CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING, .textColor = to_clay_color(COLOR_TEXT) }));

        CLAY(CLAY_ID("RtspLanConfirmBody"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 6 }
        }) {
            CLAY_TEXT(CLAY_STRING("Anyone on this network will be able to watch the tape you are capturing."),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY_TEXT(CLAY_STRING("The video is not encrypted, and by default there is no password."),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY_TEXT(CLAY_STRING("Customers' tapes are private. Only do this on a network you trust."),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_SYNC_RED) }));
            CLAY_TEXT(CLAY_STRING("You will only be asked once."),
                CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }

        CLAY(CLAY_ID("RtspLanConfirmButtons"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 }
        }) {
            CLAY(CLAY_ID("RtspLanConfirmSpacer"), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } }
            }) {}
            CLAY(CLAY_ID("RtspLanConfirmCancel"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(110), CLAY_SIZING_FIXED(32) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("Keep private"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
            }
            CLAY(CLAY_ID("RtspLanConfirmAccept"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(130), CLAY_SIZING_FIXED(32) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON_ACTIVE),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("Share on LAN"),
                    CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }
    }
}


/* ------------------------------------------------------------ list picker */

/* The selected device, or NULL when nothing is enumerated. Every picker but
 * PICK_DEVICE describes a property OF that device. */
static const preview_device_t *pick_device(void)
{
    size_t n = 0;
    const preview_device_t *devs = gui_preview_devices(&n);
    int sel = gui_preview_selected_device();
    if (sel < 0 || (size_t)sel >= n) return NULL;
    return &devs[sel];
}

static int pick_count(pick_kind_t k)
{
    const preview_device_t *d = pick_device();
    switch (k) {
        case PICK_DEVICE: { size_t n = 0; (void)gui_preview_devices(&n); return (int)n; }
        case PICK_MODE:     return d ? d->n_modes : 0;
        case PICK_INPUT:    return d ? d->n_inputs : 0;
        case PICK_STANDARD: return d ? d->n_stds : 0;
        default:            return 0;
    }
}

static const char *pick_label(pick_kind_t k, int i)
{
    const preview_device_t *d = pick_device();
    static char buf[96];
    switch (k) {
        case PICK_DEVICE: {
            size_t n = 0;
            const preview_device_t *devs = gui_preview_devices(&n);
            if (i < 0 || (size_t)i >= n) return "";
            snprintf(buf, sizeof(buf), "%s  %s", devs[i].card, devs[i].path);
            return buf;
        }
        case PICK_MODE:     return (d && i < d->n_modes)  ? d->modes[i].label : "";
        case PICK_INPUT:    return (d && i < d->n_inputs) ? d->inputs[i].name : "";
        case PICK_STANDARD: return (d && i < d->n_stds)   ? d->stds[i].name : "";
        default:            return "";
    }
}

static int pick_selected(pick_kind_t k)
{
    const preview_device_t *d = pick_device();
    switch (k) {
        case PICK_DEVICE:   return gui_preview_selected_device();
        case PICK_MODE:     return gui_preview_selected_mode();
        case PICK_INPUT:    return gui_preview_selected_input();
        case PICK_STANDARD: return d ? d->std_index : -1;
        default:            return -1;
    }
}

static const char *pick_title(pick_kind_t k)
{
    switch (k) {
        case PICK_DEVICE:   return "Capture device";
        case PICK_MODE:     return "Picture mode";
        case PICK_INPUT:    return "Input";
        case PICK_STANDARD: return "Video standard";
        default:            return "";
    }
}

/* What the row shows when the sheet is shut: the value in force, with an
 * ellipsis because clicking opens a list rather than cycling. Prefers the
 * device's readback over the selection -- see the panel overlay's note. */
static const char *pick_current_label(pick_kind_t k, char *out, size_t cap)
{
    const preview_device_t *d = pick_device();
    preview_status_t st = gui_preview_get_status();
    const char *cur = NULL;

    if (k == PICK_INPUT && st.input_name[0]) cur = st.input_name;
    else if (k == PICK_STANDARD && st.std_name[0]) cur = st.std_name;
    else {
        int sel = pick_selected(k);
        if (sel >= 0 && sel < pick_count(k)) cur = pick_label(k, sel);
    }
    if (!cur || !cur[0]) cur = (d || k == PICK_DEVICE) ? "(none)" : "(no device)";
    snprintf(out, cap, "%s...", cur);
    return out;
}

/* Is this list changeable right now? The answers differ and each has a reason:
 *   - the device resizes everything downstream, so not while a tee holds it;
 *   - the mode likewise;
 *   - the standard changes the raster AND every SDTV driver refuses S_STD
 *     while streaming, so it needs a disconnect;
 *   - the input is switchable live, which is the whole point of it. */
static bool pick_enabled(const gui_app_t *app, pick_kind_t k)
{
    if (gui_net_is_client(app)) return false;
    const preview_device_t *d = pick_device();
    bool tee_live = gui_rtsp_stream_is_running() || gui_video_record_is_running();
    preview_status_t st = gui_preview_get_status();
    bool streaming = (st.state == PREVIEW_STATE_STREAMING ||
                      st.state == PREVIEW_STATE_STALLED ||
                      st.state == PREVIEW_STATE_CONNECTING ||
                      st.state == PREVIEW_STATE_POPPED_OUT);
    switch (k) {
        case PICK_DEVICE:   return pick_count(k) > 0 && !tee_live && !streaming;
        case PICK_MODE:     return pick_count(k) > 0 && !tee_live && !streaming;
        case PICK_STANDARD: return d && d->is_sdtv && d->n_stds > 1 && !tee_live && !streaming;
        case PICK_INPUT:    return d && d->is_sdtv && d->n_inputs > 1 &&
                                   st.state != PREVIEW_STATE_POPPED_OUT;
        default:            return false;
    }
}

static void pick_remember(gui_app_t *app)
{
    const preview_device_t *d = pick_device();
    if (!d) return;
    snprintf(app->settings.usbref_device_path, sizeof(app->settings.usbref_device_path),
             "%s", d->path);
    int in = gui_preview_selected_input();
    if (in >= 0 && in < d->n_inputs) {
        snprintf(app->settings.usbref_input, sizeof(app->settings.usbref_input),
                 "%s", d->inputs[in].name);
    }
    if (d->std_index >= 0 && d->std_index < d->n_stds) {
        snprintf(app->settings.usbref_standard, sizeof(app->settings.usbref_standard),
                 "%s", d->stds[d->std_index].name);
    }
    gui_preview_mode_spec(app->settings.usbref_mode_spec,
                          sizeof(app->settings.usbref_mode_spec));
    gui_settings_save(&app->settings);
}

static void pick_choose(gui_app_t *app, pick_kind_t k, int i)
{
    switch (k) {
        case PICK_DEVICE:   gui_preview_select(i, 0); break;
        case PICK_MODE:     gui_preview_select(gui_preview_selected_device(), i); break;
        case PICK_INPUT:    gui_preview_select_input(i); break;
        case PICK_STANDARD: gui_preview_select_standard(i); break;
        default: return;
    }
    pick_remember(app);
}

/* One row: a dim label and a box carrying the current value. */
static void pick_row(const gui_app_t *app, pick_kind_t k, Clay_ElementId row_id,
                     Clay_ElementId box_id, const char *label, char *buf, size_t cap)
{
    bool on = pick_enabled(app, k);
    Color bg = on ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON);
    Color fg = on ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
    pick_current_label(k, buf, cap);

    CLAY(row_id, { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
                               .layoutDirection = CLAY_LEFT_TO_RIGHT,
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 } }) {
        CLAY(CLAY_IDI("UsbRefPickLabel", (int)k), {
            .layout = { .sizing = { CLAY_SIZING_FIXED(74), CLAY_SIZING_FIXED(28) },
                        .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } }
        }) {
            CLAY_TEXT(make_string(label), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS,
                                                             .textColor = to_clay_color(COLOR_TEXT_DIM) }));
        }
        CLAY(box_id, { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) },
                                   .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                                   .padding = { 8, 8, 0, 0 } },
                       .backgroundColor = to_clay_color(bg),
                       .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
            CLAY_TEXT(make_string(buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1,
                                                           .textColor = to_clay_color(fg) }));
        }
    }
}

/* The sheet itself. Modal over the dialog: while a list is up it is the only
 * thing that may be clicked, which is why it carries a backdrop the dialog
 * deliberately does not. */
static void render_pick_sheet(gui_app_t *app)
{
    (void)app;
    if (s_pick == PICK_NONE) return;
    int n = pick_count(s_pick);
    if (n <= 0) { s_pick = PICK_NONE; return; }

    CLAY(CLAY_ID("UsbRefPickBackdrop"), {
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
        .floating = { .attachTo = CLAY_ATTACH_TO_ROOT,
                      .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP,
                                        .parent = CLAY_ATTACH_POINT_LEFT_TOP },
                      .zIndex = 32 },
        .backgroundColor = (Clay_Color){0, 0, 0, 120}
    }) {}

    CLAY(CLAY_ID("UsbRefPickWindow"), {
        .layout = { .sizing = { CLAY_SIZING_FIT(.min = 380, .max = 620),
                                CLAY_SIZING_FIT(0) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .padding = { 16, 16, 14, 14 }, .childGap = 6 },
        .floating = { .attachTo = CLAY_ATTACH_TO_ROOT,
                      .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                        .parent = CLAY_ATTACH_POINT_CENTER_CENTER },
                      .zIndex = 33 },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = { 1, 1, 1, 1 }, .color = to_clay_color(COLOR_BUTTON_ACTIVE) }
    }) {
        CLAY_TEXT(make_string(pick_title(s_pick)),
                  CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING,
                                     .textColor = to_clay_color(COLOR_TEXT) }));
        int sel = pick_selected(s_pick);
        if (n > 24) n = 24;   /* the sheet is a list, not a scroll view */
        for (int i = 0; i < n; i++) {
            bool chosen = (i == sel);
            CLAY(CLAY_IDI("UsbRefPickOpt", i), {
                .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26) },
                            .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER },
                            .padding = { 8, 8, 0, 0 } },
                .backgroundColor = to_clay_color(chosen ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(make_string(pick_label(s_pick, i)),
                          CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1,
                                             .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }
    }
}


/* ----------------------------------------------------------------- render */

void gui_usbref_settings_render(gui_app_t *app)
{
    /* The two sub-windows outlive the dialog that launches them: closing this
     * window while the codec sheet is up would strand it. */
    render_rtsp_codec_window(app);
    render_rtsp_lan_confirm(app);

    if (!s_open) { s_pick = PICK_NONE; return; }

    /* No backdrop, deliberately -- see the header. The window alone floats, at
     * the same layer the codec sheet uses to clear the settings panel (0) and
     * the channel gear popover (20). */
    int max_w = gui_ui_modal_max_extent(gui_ui_get_layout_width(), 720);
    int max_h = gui_ui_modal_max_extent(gui_ui_get_layout_height(), 640);
    int min_w = gui_ui_clamp_int(max_w, 1, 520);

    CLAY(CLAY_ID("UsbRefWindow"), {
        .layout = { .sizing = { CLAY_SIZING_FIT(.min = min_w, .max = max_w),
                                CLAY_SIZING_FIT(.min = 0, .max = max_h) },
                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    .padding = { 18, 18, 16, 16 }, .childGap = 10 },
        .floating = { .attachTo = CLAY_ATTACH_TO_ROOT,
                      .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                        .parent = CLAY_ATTACH_POINT_CENTER_CENTER },
                      .zIndex = 31 },
        .backgroundColor = to_clay_color(COLOR_PANEL_BG),
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .width = { 1, 1, 1, 1 }, .color = to_clay_color(COLOR_BUTTON_ACTIVE) }
    }) {
        CLAY(CLAY_ID("UsbRefHeader"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_LEFT_TO_RIGHT,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 }
        }) {
            CLAY_TEXT(CLAY_STRING("USB Reference Video"),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_HEADING,
                                         .textColor = to_clay_color(COLOR_TEXT) }));
            CLAY(CLAY_ID("UsbRefHeaderSpacer"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
            CLAY(CLAY_ID("UsbRefClose"), {
                .layout = { .sizing = { CLAY_SIZING_FIXED(28), CLAY_SIZING_FIXED(28) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } },
                .backgroundColor = to_clay_color(COLOR_BUTTON),
                .cornerRadius = CLAY_CORNER_RADIUS(4)
            }) {
                CLAY_TEXT(CLAY_STRING("X"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL,
                                                               .textColor = to_clay_color(COLOR_TEXT) }));
            }
        }

        CLAY(CLAY_ID("UsbRefBody"), {
            .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .layoutDirection = CLAY_TOP_TO_BOTTOM, .childGap = 8 },
            .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() }
        }) {
            CLAY_TEXT(CLAY_STRING("Device"),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL,
                                         .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            // The device the picture comes from. Both the reference
            // recording and the RTSP stream tee off this one preview, so
            // the picker belongs here rather than only inside the
            // Preview pane -- an RTSP toggle that cannot be armed until
            // you visit another pane and click Connect is a strange
            // prerequisite for a setting in the settings dialog.
            {
                size_t n_pv = 0;
                const preview_device_t *pv = gui_preview_devices(&n_pv);
                int pv_sel = gui_preview_selected_device();
                preview_status_t pv_st = gui_preview_get_status();
                bool pv_live = (pv_st.state == PREVIEW_STATE_STREAMING ||
                                pv_st.state == PREVIEW_STATE_STALLED ||
                                pv_st.state == PREVIEW_STATE_CONNECTING ||
                                pv_st.state == PREVIEW_STATE_POPPED_OUT);
                /* On a net client the preview devices are the SERVER's
                 * hardware: no connect/rescan here, and the box shows
                 * the path the server has remembered, read-only. */
                bool pv_is_client = gui_net_is_client(app);
                CLAY(CLAY_ID("PreviewDeviceRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                    if (!pv_is_client) {
                        CLAY(CLAY_ID("PreviewConnectBtn"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(pv_live ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(pv_live ? CLAY_STRING("CONNECTED") : CLAY_STRING("CONNECT"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                    CLAY_TEXT(pv_is_client ? CLAY_STRING("Server device") : CLAY_STRING("Device"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(COLOR_TEXT) }));
                    // Click to cycle. A dropdown would be nicer with
                    // many devices; there is realistically one dongle.
                    CLAY(CLAY_ID("PreviewDeviceBox"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color((Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                        static char pv_label[96];
                        if (pv_is_client) {
                            snprintf(pv_label, sizeof(pv_label), "%s",
                                     app->settings.usbref_device_path[0]
                                       ? app->settings.usbref_device_path
                                       : "(none remembered on the server)");
                        } else if (n_pv == 0) {
                            snprintf(pv_label, sizeof(pv_label), "%s",
                                     app->settings.usbref_device_path[0]
                                       ? app->settings.usbref_device_path
                                       : "(no USB video device)");
                        } else if (pv_sel >= 0 && (size_t)pv_sel < n_pv) {
                            snprintf(pv_label, sizeof(pv_label), "%s  %s",
                                     pv[pv_sel].card, pv[pv_sel].path);
                        } else {
                            snprintf(pv_label, sizeof(pv_label), "(select a device)");
                        }
                        CLAY_TEXT(make_string(pv_label), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color((n_pv || pv_is_client) ? COLOR_TEXT : COLOR_TEXT_DIM) }));
                    }
                    if (!pv_is_client) {
                        CLAY(CLAY_ID("PreviewRescanBtn"), { .layout = { .sizing = { CLAY_SIZING_FIXED(66), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                            CLAY_TEXT(CLAY_STRING("Rescan"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                        }
                    }
                }
            }

            /* Which jack. Live-switchable, so it stays usable with a tape
             * running; it lives here now rather than on the panel overlay so
             * every control for this device is in one place. */
            {
                static char in_buf[96];
                pick_row(app, PICK_INPUT, CLAY_ID("UsbRefInputRow"),
                         CLAY_ID("UsbRefInputBox"), "Input", in_buf, sizeof(in_buf));
            }

            CLAY_TEXT(CLAY_STRING("Picture"),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL,
                                         .textColor = to_clay_color(COLOR_TEXT_DIM) }));
            {
                /* The standard fixes the active raster, so the mode list is
                 * rebuilt whenever it changes -- they belong next to each
                 * other and in that order. */
                static char std_buf[96];
                static char mode_buf[96];
                pick_row(app, PICK_STANDARD, CLAY_ID("UsbRefStdRow"),
                         CLAY_ID("UsbRefStdBox"), "Standard", std_buf, sizeof(std_buf));
                pick_row(app, PICK_MODE, CLAY_ID("UsbRefModeRow"),
                         CLAY_ID("UsbRefModeBox"), "Mode", mode_buf, sizeof(mode_buf));
            }

            /* Picture shape. These are set once for a deck and then
             * left alone, which is why they live here rather than on
             * the panel, where Input and Standard are changed per tape.
             *
             * Aspect reaches the preview, the reference MKV and the
             * RTSP stream alike, so the three cannot disagree. Crop
             * reaches ONLY the preview: the recording keeps the full
             * active raster so it stays frame-comparable with a
             * tbc-video-export of the same tape. */
            CLAY(CLAY_ID("PreviewGeometryRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 } }) {
                CLAY_TEXT(CLAY_STRING("Aspect"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                CLAY(CLAY_ID("PreviewAspectBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(72), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                    int am = app->settings.usbref_aspect;
                    const char *aspect_label = (am == 1) ? "4:3"
                                             : (am == 2) ? "16:9"
                                             : (am == 3) ? "Square" : "Auto";
                    CLAY_TEXT(make_string(aspect_label), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                /* Preview-only, and labelled so nobody expects it in
                 * the file. Each box steps by 2 and wraps at 32. */
                CLAY_TEXT(CLAY_STRING("Crop (preview only)"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                static char crop_t_buf[8], crop_b_buf[8], crop_l_buf[8], crop_r_buf[8];
                snprintf(crop_t_buf, sizeof(crop_t_buf), "T%d", app->settings.usbref_crop_top);
                snprintf(crop_b_buf, sizeof(crop_b_buf), "B%d", app->settings.usbref_crop_bottom);
                snprintf(crop_l_buf, sizeof(crop_l_buf), "L%d", app->settings.usbref_crop_left);
                snprintf(crop_r_buf, sizeof(crop_r_buf), "R%d", app->settings.usbref_crop_right);
                CLAY(CLAY_ID("PreviewCropTop"), { .layout = { .sizing = { CLAY_SIZING_FIXED(46), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                    CLAY_TEXT(make_string(crop_t_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                CLAY(CLAY_ID("PreviewCropBottom"), { .layout = { .sizing = { CLAY_SIZING_FIXED(46), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                    CLAY_TEXT(make_string(crop_b_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                CLAY(CLAY_ID("PreviewCropLeft"), { .layout = { .sizing = { CLAY_SIZING_FIXED(46), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                    CLAY_TEXT(make_string(crop_l_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                }
                CLAY(CLAY_ID("PreviewCropRight"), { .layout = { .sizing = { CLAY_SIZING_FIXED(46), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                    CLAY_TEXT(make_string(crop_r_buf), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                }
            }

            CLAY_TEXT(CLAY_STRING("Stream"),
                      CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL,
                                         .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                                // Stream: the same picture as the reference recording,
                                // published live so a tape can be watched from another
                                // machine. Greyed out when no mediamtx was found, for the
                                // same reason as the ffmpeg toggle above -- it must not arm
                                // into a state that would later refuse to start.
                                /* On a net client the stream runs on the server; the
                                 * toggle shows the server's saved switch and the settings
                                 * row below it edits the server's. mediamtx here is not
                                 * the point. */
                                bool rs_is_client = gui_net_is_client(app);
                                bool mtx_ok = rs_is_client ? true : gui_mediamtx_probe();
                                /* Warm the encoder cache while the panel is merely being
                                 * looked at, so the click does not pay for an ffmpeg
                                 * -encoders popen. Both probes are cached. */
                                if (!rs_is_client) (void)gui_rtsp_stream_probe();
                                gui_rtsp_stream_status_t rs_st = gui_rtsp_stream_get_status();
                                if (rs_is_client) {
                                    rs_st.running = app->settings.usbref_rtsp_enabled;
                                    rs_st.starting = false;
                                }
                                CLAY(CLAY_ID("ToggleRowRtsp"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                                    Color rs_bg = (rs_st.running || rs_st.starting) ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                                    if (!mtx_ok) rs_bg = ui_disabled_color(rs_bg);
                                    Color rs_fg = mtx_ok ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                                    CLAY(CLAY_ID("ToggleRtspStream"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(rs_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                        /* Three states: ffmpeg is spawned but not yet known
                                         * to have survived its first frames, and saying ON
                                         * during that would be a claim we cannot support. */
                                        CLAY_TEXT(rs_st.starting ? CLAY_STRING("...")
                                                                 : (rs_st.running ? CLAY_STRING("ON")
                                                                                  : CLAY_STRING("OFF")),
                                                  CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(rs_fg) }));
                                    }
                                    CLAY_TEXT(CLAY_STRING("RTSP Stream"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(rs_fg) }));
                                    /* Opens the codec submenu rather than cycling. It still
                                     * shows which encoder is in force, so the row loses no
                                     * information by gaining a place to put the rest. */
                                    CLAY(CLAY_ID("RtspEncoderBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(76), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(mtx_ok ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON)), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                        const char *enc_label = app->settings.usbref_rtsp_encoder == 1 ? "NVENC..."
                                                              : app->settings.usbref_rtsp_encoder == 2 ? "x264..." : "Auto...";
                                        CLAY_TEXT(make_string(enc_label), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rs_fg) }));
                                    }
                                    // Loopback vs LAN. There is no auth either way, so the
                                    // hint line below says so rather than hiding it.
                                    CLAY(CLAY_ID("RtspBindBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(84), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(mtx_ok ? COLOR_BUTTON : ui_disabled_color(COLOR_BUTTON)), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                        CLAY_TEXT(app->settings.usbref_rtsp_lan ? CLAY_STRING("LAN") : CLAY_STRING("Loopback"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(rs_fg) }));
                                    }
                                /* A FIXED box, always present, whatever it says.
                                 *
                                 * The settings panel is CLAY_SIZING_FIT, so it measures itself
                                 * from its contents: an unconstrained label that changes every
                                 * frame re-measures the whole panel, and because the panel is
                                 * centre-attached it grows and shrinks around the middle. That
                                 * is what made the window breathe. A fixed box cannot influence
                                 * the layout however long its text gets, and keeping it present
                                 * while stopped means starting the stream does not resize the
                                 * row either.
                                 *
                                 * Monospaced for the same reason at a smaller scale: in a
                                 * proportional face "1111" and "8888" are not the same width. */
                                {
                                    static char rs_live[32];
                                    if (!rs_st.running || rs_st.starting) {
                                        rs_live[0] = '\0';
                                    } else {
                                        /* Sampled from mediamtx's loopback metrics endpoint
                                         * every two seconds -- the only place the real
                                         * published rate exists. 0 until the first pair of
                                         * samples exists to difference. */
                                        uint32_t kbps = gui_mediamtx_stream_kbps();
                                        if (kbps == 0) {
                                            snprintf(rs_live, sizeof(rs_live), "-- kbit/s");
                                        } else if (kbps < 1000) {
                                            snprintf(rs_live, sizeof(rs_live), "%u kbit/s",
                                                     (unsigned)kbps);
                                        } else {
                                            snprintf(rs_live, sizeof(rs_live), "%.1f Mbit/s",
                                                     (double)kbps / 1000.0);
                                        }
                                    }
                                    CLAY(CLAY_ID("RtspLiveBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(104), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                                        CLAY_TEXT(make_string(rs_live), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                                    }
                                }
                                }
                                /* Require a password to watch. Off by default, and only
                                 * meaningful in LAN mode -- on loopback the only things that
                                 * can reach the stream are already on this machine. The
                                 * password itself is generated fresh each time the stream
                                 * starts and is never written to settings. */
                                CLAY(CLAY_ID("ToggleRowRtspPassword"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                                    Color pw_bg = app->settings.usbref_rtsp_password ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
                                    if (!mtx_ok) pw_bg = ui_disabled_color(pw_bg);
                                    Color pw_fg = mtx_ok ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
                                    CLAY(CLAY_ID("ToggleRtspPassword"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(pw_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                        CLAY_TEXT(app->settings.usbref_rtsp_password ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                                                  CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(pw_fg) }));
                                    }
                                    CLAY_TEXT(CLAY_STRING("Password"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(pw_fg) }));

                                    /* Read back from the RUNNING server, not from settings, so
                                     * the panel can never show a password that is not the one
                                     * actually being enforced. Fixed width for the same reason
                                     * as the bitrate box. */
                                    static char rs_pw[48];
                                    const char *live_pw = gui_mediamtx_read_password();
                                    snprintf(rs_pw, sizeof(rs_pw), "%s", live_pw[0] ? live_pw : "");
                                    CLAY(CLAY_ID("RtspPasswordBox"), { .layout = { .sizing = { CLAY_SIZING_FIXED(190), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color(rs_pw[0] ? (Color){25,25,30,255} : COLOR_PANEL_BG), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                        CLAY_TEXT(make_string(rs_pw), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                                    }
                                    if (rs_pw[0]) {
                                        CLAY(CLAY_ID("RtspPasswordCopy"), { .layout = { .sizing = { CLAY_SIZING_FIXED(62), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(Clay_PointerOver(CLAY_ID("RtspPasswordCopy")) ? COLOR_BUTTON_HOVER : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                            CLAY_TEXT(CLAY_STRING("Copy"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                                        }
                                    }
                                }

                                // The URLs a viewer actually types, one per row: at a size
                                // worth reading, three of them will not sit side by side.
                                // Clicking copies, Ctrl+click hands the URL to whatever the
                                // desktop registered for the scheme -- VLC for rtsp://, the
                                // browser for http:// -- so a tape can be watched without
                                // anyone retyping a port and landing on capture-node's
                                // stream by accident.
                                if (rs_st.running && !rs_st.starting) {
                                    // rs_st is a copy on this frame's stack, and Clay does not
                                    // copy the text it is handed -- it keeps the pointer and
                                    // reads it back in Clay_EndLayout(), by which time this
                                    // frame is gone. Reading straight out of rs_st drew boxes
                                    // of exactly the right width, because layout measured a
                                    // live string, holding '?' -- raylib's stand-in for
                                    // whatever bytes had since landed on that stack slot.
                                    static char rs_urls[RTSP_URL_COUNT][256];
                                    for (int u = 0; u < RTSP_URL_COUNT; u++) {
                                        snprintf(rs_urls[u], sizeof(rs_urls[u]), "%s",
                                                 rtsp_url_for_kind(&rs_st, u));
                                        // Reads last frame's boxes, so the tint trails the
                                        // pointer by a frame. For "this responds to a click"
                                        // that is imperceptible.
                                        bool u_hot = Clay_PointerOver(CLAY_IDI("RtspUrlBox", u));
                                        CLAY(CLAY_IDI("RtspUrlRow", u), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 8 } }) {
                                            CLAY(CLAY_IDI("RtspUrlLabel", u), { .layout = { .sizing = { CLAY_SIZING_FIXED(58), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER } } }) {
                                                CLAY_TEXT(make_string(rtsp_url_kind_label(u)), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                                            }
                                            CLAY(CLAY_IDI("RtspUrlBox", u), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 8, 8, 0, 0 } }, .backgroundColor = to_clay_color(u_hot ? COLOR_BUTTON : (Color){25,25,30,255}), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                                CLAY_TEXT(make_string(rs_urls[u]), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .fontId = 1, .textColor = to_clay_color(COLOR_TEXT) }));
                                            }
                                            CLAY(CLAY_IDI("RtspUrlCopy", u), { .layout = { .sizing = { CLAY_SIZING_FIXED(62), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(Clay_PointerOver(CLAY_IDI("RtspUrlCopy", u)) ? COLOR_BUTTON_HOVER : COLOR_BUTTON), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
                                                CLAY_TEXT(CLAY_STRING("Copy"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .textColor = to_clay_color(COLOR_TEXT) }));
                                            }
                                        }
                                    }
                                    // Neither gesture is discoverable on its own, so say both.
                                    CLAY(CLAY_ID("RtspUrlHelpRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
            #if defined(__APPLE__)
                                        CLAY_TEXT(CLAY_STRING("click a URL to copy it, Cmd+click to open it"),
            #else
                                        CLAY_TEXT(CLAY_STRING("click a URL to copy it, Ctrl+click to open it"),
            #endif
                                                  CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(COLOR_TEXT_DIM) }));
                                    }
                                }
                                // One line that says what is wrong, or what is about to be
                                // shared with the whole network.
                                CLAY(CLAY_ID("RtspHintRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
                                    static char rs_hint[260];
                                    Color hint_fg = COLOR_TEXT_DIM;
                                    if (!mtx_ok) {
                                        snprintf(rs_hint, sizeof(rs_hint),
                                                 "mediamtx not found - install it or set usbref_mediamtx_path in the settings file");
                                        hint_fg = COLOR_SYNC_RED;
                                    } else if (rs_st.error) {
                                        snprintf(rs_hint, sizeof(rs_hint), "stream error: %s", rs_st.err_text);
                                        hint_fg = COLOR_SYNC_RED;
                                    } else if (rs_st.starting) {
                                        snprintf(rs_hint, sizeof(rs_hint), "starting the stream...");
                                    } else if (rs_st.running && rs_st.frames_dropped > 0) {
                                        /* The dropped counter used to sit in the row above and
                                         * was half of why it jittered. It still matters -- it is
                                         * the only sign the encoder cannot keep up -- so it
                                         * moves here, deliberately WITHOUT the number: a live
                                         * count in this line would just move the jitter. */
                                        snprintf(rs_hint, sizeof(rs_hint),
                                                 "frames are being dropped - the encoder is not keeping up");
                                        hint_fg = COLOR_SYNC_RED;
                                    } else if (rs_st.running && !rs_st.audio_active) {
                                        snprintf(rs_hint, sizeof(rs_hint), "video only - %s",
                                                 rs_st.audio_note[0] ? rs_st.audio_note : "no audio device");
                                    } else if (app->settings.usbref_rtsp_lan) {
                                        snprintf(rs_hint, sizeof(rs_hint),
                                                 "LAN: anyone on the network can watch this stream - there is no password");
                                        hint_fg = COLOR_SYNC_RED;
                                    } else {
                                        snprintf(rs_hint, sizeof(rs_hint), "mediamtx: %s   loopback only",
                                                 gui_mediamtx_binary_path());
                                    }
                                    CLAY_TEXT(make_string(rs_hint), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(hint_fg) }));
                                }
        }
    }

    /* After the window, so it floats over it. */
    render_pick_sheet(app);
}

/* ------------------------------------------------------ the captions row */

/* Closed captions ride the recording the same way the reference video does,
 * so their switch sits under the Reference video row in the settings panel
 * rather than in this dialog. The row stays in this file because gui_ui.c is
 * upstream's: the panel calls these two and names none of the row's ids. */
void gui_usbref_settings_render_captions_row(gui_app_t *app)
{
    // Closed captions: EIA-608 line-21 data read off the capture
    // dongle's RAW VBI node by ffmpeg. Not from the RF, and not
    // from the preview picture -- the chip clamps capture to 480
    // active lines, so line 21 is never in a frame we could see.
    // Greyed out unless the whole chain probes clean, so the
    // toggle cannot be armed into a state that would refuse a
    // recording. The probe is cached; this costs a compare.
    gui_cc_record_set_ffmpeg(gui_video_record_ffmpeg_path());
    gui_cc_record_set_preview_device(app->settings.usbref_device_path);
    gui_cc_record_set_device(app->settings.usbref_cc_vbi_device);
    bool cc_ok = (gui_cc_record_probe() == CC_PROBE_OK);
    CLAY(CLAY_ID("ToggleRowCaptions"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .layoutDirection = CLAY_LEFT_TO_RIGHT, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
        Color cc_bg = app->settings.usbref_cc_enabled ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON;
        if (!cc_ok) cc_bg = ui_disabled_color(cc_bg);
        CLAY(CLAY_ID("ToggleCcRecord"), { .layout = { .sizing = { CLAY_SIZING_FIXED(80), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } }, .backgroundColor = to_clay_color(cc_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
            CLAY_TEXT((app->settings.usbref_cc_enabled && cc_ok) ? CLAY_STRING("ON") : CLAY_STRING("OFF"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(cc_ok ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT)) }));
        }
        CLAY_TEXT(CLAY_STRING("Closed captions"), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_NORMAL, .textColor = to_clay_color(cc_ok ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT)) }));
        Color ctag_bg = app->settings.auto_names_enabled ? (Color){25,25,30,255} : ui_disabled_color((Color){25,25,30,255});
        Color ctag_fg = app->settings.auto_names_enabled ? COLOR_TEXT : ui_disabled_color(COLOR_TEXT);
        CLAY(CLAY_ID("CcTagField"), { .layout = { .sizing = { CLAY_SIZING_FIXED(100), CLAY_SIZING_FIXED(28) }, .childAlignment = { .x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER }, .padding = { 6, 6, 0, 0 } }, .backgroundColor = to_clay_color(ctag_bg), .cornerRadius = CLAY_CORNER_RADIUS(4) }) {
            const char *ctag = app->settings.usbref_cc_tag[0] ? app->settings.usbref_cc_tag : "(tag)";
            if (gui_ui_is_text_field_active(UI_TEXT_FIELD_CC_TAG) && app->settings.auto_names_enabled) {
                gui_ui_render_active_text(UI_TEXT_FIELD_CC_TAG, ctag, FONT_SIZE_STATS, 1, ctag_fg);
            } else {
                CLAY_TEXT(make_string(ctag), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_STATS, .fontId = 1, .textColor = to_clay_color(ctag_fg) }));
            }
        }
    }
    // Names the node, or says exactly which link in the chain is
    // missing -- ffmpeg, the v4l2vbi input device, the scc muxer,
    // the node itself, or another program already holding it.
    CLAY(CLAY_ID("CaptionsHintRow"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) }, .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
        /* static: Clay draws text from this pointer after layout
         * returns, so an automatic buffer would be a dangling read. */
        static char cc_hint[240];
        snprintf(cc_hint, sizeof(cc_hint), "%s", gui_cc_record_probe_hint());
        CLAY_TEXT(make_string(cc_hint), CLAY_TEXT_CONFIG({ .fontSize = FONT_SIZE_VU_CLIP, .textColor = to_clay_color(cc_ok ? COLOR_TEXT_DIM : COLOR_SYNC_RED) }));
    }
}

bool gui_usbref_settings_handle_captions_click(gui_app_t *app)
{
    if (Clay_PointerOver(CLAY_ID("ToggleCcRecord"))) {
        /* Refuse rather than arm: the same probe gates the record
         * preflight, so arming here would only move the refusal to
         * the moment the operator presses RECORD. The hint says
         * which link in the chain is missing. */
        if (gui_cc_record_probe() != CC_PROBE_OK) {
            gui_app_set_status(app, gui_cc_record_probe_hint());
        } else {
            app->settings.usbref_cc_enabled = !app->settings.usbref_cc_enabled;
            gui_settings_save(&app->settings);
        }
        return true;
    }
    if (Clay_PointerOver(CLAY_ID("CcTagField")) && app->settings.auto_names_enabled) {
        gui_ui_begin_text_edit(app, UI_TEXT_FIELD_CC_TAG, CLAY_ID("CcTagField"), 6.0f, 6.0f);
        return true;
    }
    return false;
}

/* ----------------------------------------------------------- interactions */

bool gui_usbref_settings_handle_interactions(gui_app_t *app)
{
    /* The sub-windows are modal over this one and are tested first, exactly as
     * they used to be tested before the settings-panel block. */
    if (s_rtsp_codec_window_open) {
        gui_rtsp_stream_status_t cw = gui_rtsp_stream_get_status();
        bool cw_locked = cw.running || cw.starting;
        if (Clay_PointerOver(CLAY_ID("RtspCodecClose"))) {
            s_rtsp_codec_window_open = false;
        } else if (!cw_locked) {
            /* Everything here is read when ffmpeg is spawned, so none of it
             * may move while a stream is up. */
            for (int e = 0; e < 3; e++) {
                if (!Clay_PointerOver(CLAY_IDI("RtspCodecEnc", e))) continue;
                if (e == 1 && !gui_rtsp_stream_has_nvenc()) {
                    gui_app_set_status(app, "This ffmpeg cannot encode with NVENC");
                    break;
                }
                app->settings.usbref_rtsp_encoder = e;
                gui_settings_save(&app->settings);
                break;
            }
            if (Clay_PointerOver(CLAY_ID("RtspCodecRateMinus")) ||
                Clay_PointerOver(CLAY_ID("RtspCodecRatePlus"))) {
                int cur = app->settings.usbref_rtsp_bitrate_kbps > 0
                        ? app->settings.usbref_rtsp_bitrate_kbps
                        : RTSP_BITRATE_DEFAULT_KBPS;
                cur += Clay_PointerOver(CLAY_ID("RtspCodecRatePlus"))
                     ? RTSP_BITRATE_STEP_KBPS : -RTSP_BITRATE_STEP_KBPS;
                if (cur < RTSP_BITRATE_MIN_KBPS) cur = RTSP_BITRATE_MIN_KBPS;
                if (cur > RTSP_BITRATE_MAX_KBPS) cur = RTSP_BITRATE_MAX_KBPS;
                app->settings.usbref_rtsp_bitrate_kbps = cur;
                gui_settings_save(&app->settings);
            }
            if (Clay_PointerOver(CLAY_ID("RtspCodecDeint"))) {
                app->settings.usbref_rtsp_deinterlace = !app->settings.usbref_rtsp_deinterlace;
                gui_settings_save(&app->settings);
            }
        }
        return true;
    }

    /* Answered before anything else: while this is up it is the only thing
     * on screen that may be clicked. */
    if (s_rtsp_lan_confirm_open) {
        if (Clay_PointerOver(CLAY_ID("RtspLanConfirmAccept"))) {
            app->settings.usbref_rtsp_lan_acknowledged = true;
            app->settings.usbref_rtsp_lan = true;
            gui_settings_save(&app->settings);
            s_rtsp_lan_confirm_open = false;
            gui_app_set_status(app, "The stream will be shared on the network");
        } else if (Clay_PointerOver(CLAY_ID("RtspLanConfirmCancel"))) {
            /* Deliberately does NOT set usbref_rtsp_lan_acknowledged: declining is
             * not an answer worth remembering, and the warning should come
             * back if they change their mind later. */
            s_rtsp_lan_confirm_open = false;
            gui_app_set_status(app, "The stream stays on this machine");
        }
        return true;
    }

    if (!s_open) return false;

    /* A list is up: it is modal over the dialog, so nothing else is asked. */
    if (s_pick != PICK_NONE) {
        int n = pick_count(s_pick);
        if (n > 24) n = 24;
        for (int i = 0; i < n; i++) {
            if (Clay_PointerOver(CLAY_IDI("UsbRefPickOpt", i))) {
                pick_choose(app, s_pick, i);
                s_pick = PICK_NONE;
                return true;
            }
        }
        /* Anywhere else, including the backdrop, dismisses without choosing. */
        s_pick = PICK_NONE;
        return true;
    }

    {
        const struct { const char *id; pick_kind_t kind; } pick_boxes[] = {
            { "PreviewDeviceBox", PICK_DEVICE },
            { "UsbRefInputBox",   PICK_INPUT },
            { "UsbRefStdBox",     PICK_STANDARD },
            { "UsbRefModeBox",    PICK_MODE },
        };
        for (size_t bi = 0; bi < sizeof(pick_boxes) / sizeof(pick_boxes[0]); bi++) {
            if (!Clay_PointerOver(Clay_GetElementId(make_string(pick_boxes[bi].id)))) continue;
            /* The same predicate the row was drawn with, so a greyed box
             * cannot be clickable. */
            pick_kind_t k = pick_boxes[bi].kind;
            /* An empty device list is usually a dongle plugged in after launch;
             * rescan rather than making them find the Rescan button first. */
            if (k == PICK_DEVICE && pick_count(k) == 0) gui_preview_refresh_devices();

            if (pick_enabled(app, k)) {
                s_pick = k;
            } else if (pick_count(k) == 0) {
                gui_app_set_status(app, "no USB video device found");
            } else if (gui_net_is_client(app)) {
                gui_app_set_status(app, "the server owns the capture device");
            } else if (gui_rtsp_stream_is_running() || gui_video_record_is_running()) {
                /* Both outputs are tees off this device; swapping it under them
                 * would change geometry mid-encode. */
                gui_app_set_status(app, "stop the stream and reference video first");
            } else if (k != PICK_INPUT) {
                gui_app_set_status(app, "disconnect the preview before changing that");
            }
            return true;
        }
    }

    if (Clay_PointerOver(CLAY_ID("UsbRefClose"))) {
        s_open = false;
        return true;
    }

    if (Clay_PointerOver(CLAY_ID("PreviewRescanBtn"))) {
        gui_preview_refresh_devices();
        size_t n_pv = 0;
        (void)gui_preview_devices(&n_pv);
        gui_app_set_status(app, n_pv ? "USB video devices rescanned"
                                     : "no USB video device found");
    }
    /* Aspect cycles Auto -> 4:3 -> 16:9 -> Square. It is safe at any
     * time: it changes how frames are presented, never the capture, so
     * a live recording keeps its geometry and simply gets the new
     * display aspect from the next spawn onwards. */
    if (Clay_PointerOver(CLAY_ID("PreviewAspectBox"))) {
        app->settings.usbref_aspect = (app->settings.usbref_aspect + 1) % 4;
        gui_preview_set_aspect_mode(app->settings.usbref_aspect);
        gui_settings_save(&app->settings);
    }
    /* Crop steps by 2 and wraps at 32: an edge mask is a handful of
     * lines, and a wrap is how you get back to 0 without a second
     * control. Preview-only, so nothing here can touch a recording. */
    {
        struct { const char *id; int *field; } crop_boxes[] = {
            { "PreviewCropTop",    &app->settings.usbref_crop_top },
            { "PreviewCropBottom", &app->settings.usbref_crop_bottom },
            { "PreviewCropLeft",   &app->settings.usbref_crop_left },
            { "PreviewCropRight",  &app->settings.usbref_crop_right },
        };
        for (size_t ci = 0; ci < sizeof(crop_boxes) / sizeof(crop_boxes[0]); ci++) {
            if (!Clay_PointerOver(Clay_GetElementId(make_string(crop_boxes[ci].id)))) continue;
            *crop_boxes[ci].field = (*crop_boxes[ci].field + 2) % 34;
            gui_preview_set_crop(app->settings.usbref_crop_top,
                                 app->settings.usbref_crop_bottom,
                                 app->settings.usbref_crop_left,
                                 app->settings.usbref_crop_right);
            gui_settings_save(&app->settings);
            break;
        }
    }
    if (Clay_PointerOver(CLAY_ID("PreviewConnectBtn"))) {
        preview_status_t pv_st = gui_preview_get_status();
        bool pv_live = (pv_st.state == PREVIEW_STATE_STREAMING ||
                        pv_st.state == PREVIEW_STATE_STALLED ||
                        pv_st.state == PREVIEW_STATE_CONNECTING ||
                        pv_st.state == PREVIEW_STATE_POPPED_OUT);
        if (pv_live) {
            if (gui_rtsp_stream_is_running()) {
                gui_app_set_status(app, "stop the RTSP stream before disconnecting the preview");
            } else {
                gui_preview_disconnect();
            }
        } else {
            size_t n_pv = 0;
            (void)gui_preview_devices(&n_pv);
            if (n_pv == 0) {
                gui_preview_refresh_devices();
                (void)gui_preview_devices(&n_pv);
            }
            if (n_pv == 0) {
                gui_app_set_status(app, "no USB video device found");
            } else if (gui_preview_connect() != 0) {
                preview_status_t ps = gui_preview_get_status();
                gui_app_set_status(app, ps.err_text[0] ? ps.err_text
                                                       : "the USB preview could not be opened");
            } else {
                gui_ui_remember_preview_device(app);
            }
        }
    }
    if (Clay_PointerOver(CLAY_ID("ToggleRtspStream"))) {
        if (gui_net_is_client(app)) {
            /* Flipping the server's saved switch through /set starts
             * or stops its stream (gui_ui_apply_remote_setting). */
            if (gui_ui_settings_locked(app)) {
                gui_app_set_status(app, "Not connected, or the server is recording");
            } else {
                app->settings.usbref_rtsp_enabled = !app->settings.usbref_rtsp_enabled;
                gui_settings_save(&app->settings);
                gui_app_set_status(app, app->settings.usbref_rtsp_enabled
                    ? "Requested the stream start on the server"
                    : "Requested the stream stop on the server");
            }
        } else if (gui_rtsp_stream_get_status().starting) {
            /* Tearing down an attempt that has not resolved yet would
             * race the poll that is about to judge it. */
            gui_app_set_status(app, "the stream is still starting");
        } else {
            gui_ui_toggle_rtsp_stream(app);
        }
    }
    if (Clay_PointerOver(CLAY_ID("RtspEncoderBox"))) {
        /* Opens even while streaming: the window shows the settings in
         * force and greys them out, which is more use than a button that
         * does nothing. */
        s_rtsp_codec_window_open = true;
        return true;
    }
    if (Clay_PointerOver(CLAY_ID("ToggleRtspPassword")) && !gui_rtsp_stream_is_running() &&
        !gui_rtsp_stream_get_status().starting) {
        /* Only while stopped: the credential is baked into mediamtx's
         * config at startup, so changing it mid-stream would show a
         * password the server is not enforcing. */
        app->settings.usbref_rtsp_password = !app->settings.usbref_rtsp_password;
        gui_settings_save(&app->settings);
        gui_app_set_status(app, app->settings.usbref_rtsp_password
            ? "Viewers will need a password; it appears when the stream starts"
            : "The stream will be open to anyone who can reach it");
    }
    if (Clay_PointerOver(CLAY_ID("RtspPasswordCopy"))) {
        const char *pw = gui_net_is_client(app) ? "" : gui_mediamtx_read_password();
        if (gui_net_is_client(app)) {
            gui_app_set_status(app, "The stream password is shown on the server");
        } else if (pw[0]) {
            SetClipboardText(pw);
            gui_app_set_status(app, "Stream password copied");
        }
    }
    if (Clay_PointerOver(CLAY_ID("RtspBindBox")) && !gui_rtsp_stream_is_running() &&
        !gui_rtsp_stream_get_status().starting) {
        if (!app->settings.usbref_rtsp_lan && !app->settings.usbref_rtsp_lan_acknowledged) {
            /* Going off-box for the first time. Ask rather than flip:
             * the toggle is one click away from putting a customer's
             * tape on the network. */
            s_rtsp_lan_confirm_open = true;
        } else {
            app->settings.usbref_rtsp_lan = !app->settings.usbref_rtsp_lan;
            gui_settings_save(&app->settings);
        }
    }
    {
        /* Read the status once: three rows asking separately could see
         * three different frames if the stream stopped mid-loop. */
        gui_rtsp_stream_status_t url_st = gui_rtsp_stream_get_status();
        for (int u = 0; u < RTSP_URL_COUNT; u++) {
            const char *url = rtsp_url_for_kind(&url_st, u);
            if (Clay_PointerOver(CLAY_IDI("RtspUrlCopy", u))) {
                rtsp_url_copy(app, u, url);
            } else if (Clay_PointerOver(CLAY_IDI("RtspUrlBox", u))) {
                if (!gui_ui_primary_mod_down()) {
                    rtsp_url_copy(app, u, url);
                } else if (rtsp_url_is_safe_to_open(url)) {
                    char msg[80];
                    OpenURL(url);
                    snprintf(msg, sizeof(msg), "Opening the %s URL...",
                             rtsp_url_kind_label(u));
                    gui_app_set_status(app, msg);
                } else {
                    /* Copying still works: the clipboard is not a shell. */
                    gui_app_set_status(app,
                        "That URL will not be opened - it is not a plain "
                        "rtsp:// or http:// address");
                }
            }
        }
    }
    if (Clay_PointerOver(CLAY_ID("VideoCodecBox"))) {
        if (gui_video_record_probe()) {
            app->settings.usbref_codec = (app->settings.usbref_codec == 1) ? 0 : 1;
            gui_settings_save(&app->settings);
        }
    }

    /* Dead space inside the window still belongs to the window. */
    if (Clay_PointerOver(CLAY_ID("UsbRefWindow"))) return true;

    /* Outside: dismiss WITHOUT consuming, so the same press also reaches
     * whatever was clicked. The channel gear popover behaves the same way, and
     * for a non-modal window it is the difference between one gesture and two. */
    s_open = false;
    return false;
}
