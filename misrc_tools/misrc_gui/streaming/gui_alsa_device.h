/*
 * MISRC GUI - dongle audio device resolution
 *
 * The RTSP stream's audio must come from the same physical device as its
 * picture. That is what makes A/V sync free: one dongle, one clock. Get it
 * wrong on this host and you stream the CXADC clock-gen -- the RF audio path,
 * on a different clock entirely -- over the dongle's video.
 *
 * The stable identifier is the USB bus address. V4L2 reports it verbatim in
 * VIDIOC_QUERYCAP's bus_info, and ALSA prints the same string in
 * /proc/asound/cards:
 *
 *   v4l2   Bus info : usb-0000:00:14.0-13.3
 *   asound MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed
 *
 * Card INDICES are deliberately never used or stored. They move across reboots
 * and replugs, so a remembered hw:3,0 fails intermittently and points at
 * whatever card inherited the number -- the worst failure shape available.
 */

#ifndef GUI_ALSA_DEVICE_H
#define GUI_ALSA_DEVICE_H

#include <stddef.h>

/* Resolve the ALSA capture device sharing `v4l2_bus_info`'s USB address.
 *
 * `cards_text` is the content of /proc/asound/cards. Split out from the file
 * read so the matching can be tested against fixtures, including the two cases
 * that actually bite: a second card on the same controller, and card indices
 * moving between boots.
 *
 * Writes e.g. "plughw:CARD=MS210x,DEV=0" -- plughw rather than hw, because hw
 * demands an exact format match and fails outright when the dongle's native
 * rate is not the one asked for, where plughw inserts the conversion ffmpeg
 * would have applied anyway.
 *
 * Returns 0 on success. On failure returns -1 and sets out to "" -- the caller
 * streams video-only and says why, rather than guessing at a device. */
int gui_alsa_resolve_from_cards(const char *cards_text, const char *v4l2_bus_info,
                                char *out, size_t cap);

/* As above, reading /proc/asound/cards and querying `video_device` for its
 * bus_info. Returns 0 on success, -1 if either lookup fails or no card shares
 * the address. */
int gui_alsa_resolve_for_video_device(const char *video_device, char *out, size_t cap);

#endif /* GUI_ALSA_DEVICE_H */
