#include "../misrc_gui/processing/gui_rf_preview_pressure.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    assert(gui_rf_preview_pressure_fill_percent(1024,896,1024)==12);
    assert(gui_rf_preview_pressure_fill_percent(1024,1024,1024)==0);
    assert(gui_rf_preview_pressure_fill_percent(1024,1152,1024)==0); // read counter published first
    assert(gui_rf_preview_pressure_fill_percent(4096,0,1024)==100);
    assert(gui_rf_preview_pressure_fill_percent(1024,0,0)==0);
    gui_rf_preview_pressure_t s={0};
    assert(!gui_rf_preview_pressure_update(&s,0,0,0,0,true));
    assert(!gui_rf_preview_pressure_update(&s,10,60,0,0,true));
    assert(!gui_rf_preview_pressure_update(&s,159,60,0,0,true));
    assert(gui_rf_preview_pressure_update(&s,160,60,0,0,true));
    assert(gui_rf_preview_pressure_update(&s,200,0,35,0,true));
    assert(gui_rf_preview_pressure_update(&s,2199,0,35,0,true));
    assert(!gui_rf_preview_pressure_update(&s,2200,0,35,0,true));
    assert(gui_rf_preview_pressure_update(&s,2201,0,85,0,true));
    assert(gui_rf_preview_pressure_update(&s,2202,0,0,0,true));
    assert(gui_rf_preview_pressure_update(&s,4201,0,36,0,true));
    assert(gui_rf_preview_pressure_update(&s,4202,0,0,0,true));
    assert(!gui_rf_preview_pressure_update(&s,6202,0,0,0,true));
    assert(gui_rf_preview_pressure_update(&s,6203,0,0,1,true));
    assert(gui_rf_preview_pressure_update(&s,6204,0,0,0,true)); // counter reset is not a drop
    assert(!gui_rf_preview_pressure_update(&s,6205,0,0,0,false));
    assert(!gui_rf_preview_pressure_update(&s,1,59,59,0,true));
    assert(!gui_rf_preview_pressure_update(&s,2,60,0,0,true));
    assert(!gui_rf_preview_pressure_update(&s,3,59,0,0,true));
    assert(!gui_rf_preview_pressure_update(&s,200,60,0,0,true));
    assert(!gui_rf_preview_pressure_update(&s,300,60,0,0,true));
    assert(gui_rf_preview_pressure_update(&s,350,60,0,0,true));
    assert(gui_rf_preview_pressure_update(&s,10,0,0,0,true)); // clock rollback restarts recovery
    assert(!gui_rf_preview_pressure_update(&s,2010,0,0,0,true));
    puts("RF pressure: raw/record thresholds, new drops, hysteresis, clock/session reset passed");
}
