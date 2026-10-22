#include "gui_rf_preview_pressure.h"
#include <string.h>
unsigned gui_rf_preview_pressure_fill_percent(uint64_t written, uint64_t read, uint64_t capacity) {
    if (!capacity || written<=read) return 0;
    uint64_t fill=written-read;
    return fill>=capacity ? 100 : (unsigned)((double)fill*100.0/capacity);
}
bool gui_rf_preview_pressure_update(gui_rf_preview_pressure_t *s,
                                    uint64_t now, unsigned raw, unsigned record,
                                    uint64_t drops, bool capturing) {
    if (!s) return false;
    if (!capturing) { memset(s,0,sizeof(*s)); return false; }
    if (now < s->last_ms) { s->tracking_high=false; s->tracking_low=false; }
    s->last_ms=now;
    bool new_drop=s->have_drops && drops>s->last_drops;
    s->last_drops=drops; s->have_drops=true;
    unsigned pressure=raw>record?raw:record;
    if (new_drop || pressure>=85) {
        s->paused=true; s->tracking_high=false; s->tracking_low=false;
    } else if (!s->paused) {
        s->tracking_low=false;
        if (pressure>=60) {
            if (!s->tracking_high) { s->tracking_high=true; s->high_since_ms=now; }
            if (now-s->high_since_ms>=150) { s->paused=true; s->tracking_low=false; }
        } else s->tracking_high=false;
    } else {
        s->tracking_high=false;
        if (pressure<=35) {
            if (!s->tracking_low) { s->tracking_low=true; s->low_since_ms=now; }
            if (now-s->low_since_ms>=2000) { s->paused=false; s->tracking_low=false; }
        } else s->tracking_low=false;
    }
    return s->paused;
}
