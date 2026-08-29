#include "gui_alsa_device.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- matching (pure, fixture-testable) ----------------------------------- */

/* /proc/asound/cards is two lines per card:
 *
 *    3 [MS210x         ]: USB-Audio - MS210x
 *                         MacroSilicon MS210x at usb-0000:00:14.0-13.3, high speed
 *      ^ id in brackets                           ^ the bus address we match on
 *
 * The id comes from a header line, the address from the line following it.
 */

/* A card header line: an index, then a bracketed id. Continuation lines have no
 * bracket, so this is enough to tell them apart. */
static bool parse_card_id(const char *line, size_t line_len, char *id, size_t id_cap)
{
    const char *open = memchr(line, '[', line_len);
    if (open == NULL) return false;
    const char *close = memchr(open, ']', (size_t)(line + line_len - open));
    if (close == NULL) return false;

    const char *start = open + 1;
    const char *end = close;
    while (start < end && *start == ' ') start++;
    while (end > start && end[-1] == ' ') end--;   /* ids are space-padded */

    size_t len = (size_t)(end - start);
    if (len == 0 || len >= id_cap) return false;
    memcpy(id, start, len);
    id[len] = '\0';
    return true;
}

/* The address must be followed by a delimiter. Without this, port 13.3 matches
 * the card at port 13.30, which is a different device. */
static bool line_has_bus_address(const char *line, size_t line_len, const char *bus_info)
{
    size_t bus_len = strlen(bus_info);
    if (bus_len == 0 || bus_len > line_len) return false;

    for (size_t i = 0; i + bus_len <= line_len; i++) {
        if (memcmp(line + i, bus_info, bus_len) != 0) continue;
        size_t after = i + bus_len;
        if (after == line_len) return true;
        char c = line[after];
        if (c == ',' || c == ' ' || c == '\t' || c == '\r' || c == '\n') return true;
    }
    return false;
}

int gui_alsa_resolve_from_cards(const char *cards_text, const char *v4l2_bus_info,
                                char *out, size_t cap)
{
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (cards_text == NULL || v4l2_bus_info == NULL || v4l2_bus_info[0] == '\0') return -1;

    char current_id[64] = {0};
    const char *p = cards_text;

    while (*p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);

        char id[64];
        if (parse_card_id(p, line_len, id, sizeof(id))) {
            snprintf(current_id, sizeof(current_id), "%s", id);
        }

        if (current_id[0] != '\0' && line_has_bus_address(p, line_len, v4l2_bus_info)) {
            int n = snprintf(out, cap, "plughw:CARD=%s,DEV=0", current_id);
            if (n < 0 || (size_t)n >= cap) {
                out[0] = '\0';   /* a truncated device name is not a device name */
                return -1;
            }
            return 0;
        }

        if (nl == NULL) break;
        p = nl + 1;
    }
    return -1;
}

/* ---- the live lookup ------------------------------------------------------ */

#if defined(__linux__)

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int read_whole_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return 0;
}

int gui_alsa_resolve_for_video_device(const char *video_device, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (video_device == NULL || video_device[0] == '\0') return -1;

    /* Read-only and non-blocking: QUERYCAP needs neither streaming rights nor
     * exclusivity, so this is safe while the preview owns the device. */
    int fd = open(video_device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;

    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(capability));
    int rc = ioctl(fd, VIDIOC_QUERYCAP, &capability);
    close(fd);
    if (rc < 0) return -1;

    char bus_info[64];
    snprintf(bus_info, sizeof(bus_info), "%s", (const char *)capability.bus_info);
    if (bus_info[0] == '\0') return -1;

    char cards[8192];
    if (read_whole_file("/proc/asound/cards", cards, sizeof(cards)) != 0) return -1;

    return gui_alsa_resolve_from_cards(cards, bus_info, out, cap);
}

#else

int gui_alsa_resolve_for_video_device(const char *video_device, char *out, size_t cap)
{
    (void)video_device;
    if (out != NULL && cap > 0) out[0] = '\0';
    return -1;
}

#endif /* __linux__ */
