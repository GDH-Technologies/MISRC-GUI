/*
 * MISRC GUI - Shared double-click chrome-hide tracker
 *
 * Mirrors the VHS FM overlay toggle (signal/gui_cvbs.c) for the analysis
 * panels: a double-click (<=350 ms, <=6 px) inside a panel toggles ALL of
 * that panel's overlay chrome (buttons, dropdowns, labels, stats text) while
 * the data visualization keeps rendering. Each panel embeds one tracker in
 * its own state struct, gates its chrome drawing on the hidden flag, feeds
 * clicks that hit no control, and resets the tracker on control hits and on
 * clicks outside the panel bounds.
 */

#ifndef GUI_CHROME_HIDE_H
#define GUI_CHROME_HIDE_H

#include <stdbool.h>
#include <stdint.h>
#include "raylib.h"

typedef struct {
    bool hidden;              // Chrome currently hidden (controls not drawn)
    uint64_t last_click_ms;   // 0 = no click pending (one click never toggles)
    float last_click_x;       // Position of the last fed click
    float last_click_y;
} gui_chrome_hide_t;

// True while the panel's overlay chrome is hidden. The panel's render path
// skips its chrome drawing (control rectangles then stay stale, so the click
// handler must guard control hits on this flag); the data visualization
// keeps rendering.
bool gui_chrome_hide_hidden(const gui_chrome_hide_t *tracker);

// Forget a pending double-click (a consumed control hit, or a click outside
// the panel bounds). Leaves the hidden flag untouched: resetting only cancels
// a pending toggle.
void gui_chrome_hide_reset(gui_chrome_hide_t *tracker);

// Feed a click that hit no control inside the panel bounds. Returns true
// when the click completed a double-click (<=350 ms and <=6 px from the
// previous fed click) and toggled the hidden flag; the caller should treat
// the click as consumed (and close its open dropdowns). The toggling click
// also clears the tracker, so restoring the chrome needs two fresh clicks.
bool gui_chrome_hide_feed_click(gui_chrome_hide_t *tracker, Vector2 click);

#endif // GUI_CHROME_HIDE_H
