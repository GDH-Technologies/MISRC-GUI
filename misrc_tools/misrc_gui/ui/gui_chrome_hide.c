/*
 * MISRC GUI - Shared double-click chrome-hide tracker implementation
 */

#include "gui_chrome_hide.h"

// threading.h uses NULL in its Windows inline helpers without declaring it
// (every other includer gets it transitively from earlier headers); include
// <stddef.h> first so this small TU compiles standalone, e.g. in the layout
// harness executables.
#include <stddef.h>
#include "../../common/threading.h"  // get_time_ms()

#include <math.h>

bool gui_chrome_hide_hidden(const gui_chrome_hide_t *tracker) {
    return tracker && tracker->hidden;
}

void gui_chrome_hide_reset(gui_chrome_hide_t *tracker) {
    if (!tracker) return;
    // Keep the hidden flag: a reset only cancels a pending double-click.
    tracker->last_click_ms = 0;
}

bool gui_chrome_hide_feed_click(gui_chrome_hide_t *tracker, Vector2 click) {
    if (!tracker) return false;
    // Same detection as the VHS FM overlay toggle (gui_cvbs.c:cvbs_vtable_
    // handle_click): two clicks within 350 ms and 6 px toggle; the toggling
    // click clears the tracker so restoring needs two fresh clicks.
    uint64_t now_ms = get_time_ms();
    bool double_click =
        tracker->last_click_ms != 0 &&
        now_ms >= tracker->last_click_ms &&
        now_ms - tracker->last_click_ms <= 350 &&
        fabsf(click.x - tracker->last_click_x) <= 6.0f &&
        fabsf(click.y - tracker->last_click_y) <= 6.0f;
    tracker->last_click_ms = now_ms;
    tracker->last_click_x = click.x;
    tracker->last_click_y = click.y;
    if (double_click) {
        tracker->hidden = !tracker->hidden;
        tracker->last_click_ms = 0;
        return true;
    }
    return false;
}
