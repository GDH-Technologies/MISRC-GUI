/* Production pooled UI routes: lazy creation, duplicate views, independent
 * systems/channels, pressure gating and last-subscriber teardown during copy. */
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/processing/gui_rf_preview_pressure.c"
#include <assert.h>
typedef struct { gui_rf_format_t format; atomic_bool paused; } route_worker_t;
static atomic_int route_calls, creates, destroys, resets;
static atomic_bool hold_copy, copy_entered, detach_done;
static vhs_fm_decoder_t *detach_decoder;
static gui_app_t route_app;
static size_t simulated_fill;
static int16_t route_a[2]={123,-456}, route_b[2]={789,-123};
static gui_rf_block_info_t route_info[2]={{40000000,123456,42,false},{20000000,654321,43,true}};
gui_rf_preview_worker_t *gui_rf_preview_worker_create(gui_rf_format_t format) {
    route_worker_t *w=calloc(1,sizeof(*w)); assert(w); w->format=format;
    atomic_fetch_add(&creates,1); return (gui_rf_preview_worker_t*)w;
}
void gui_rf_preview_worker_destroy(gui_rf_preview_worker_t *worker) {
    assert(worker); atomic_fetch_add(&destroys,1); free(worker);
}
bool gui_rf_preview_worker_set_budget(double cores) { assert(cores>=.5 && cores<=1); return true; }
void gui_rf_preview_worker_set_paused(gui_rf_preview_worker_t *worker,bool paused) {
    atomic_store(&((route_worker_t*)worker)->paused,paused);
}
void gui_rf_preview_worker_reset(gui_rf_preview_worker_t *worker) { assert(worker); atomic_fetch_add(&resets,1); }
bool gui_vhs_fm_bind_worker(vhs_fm_decoder_t *decoder,gui_rf_preview_worker_t *worker) {
    decoder->worker=worker; return true;
}
size_t bufmgr_fill_level(buffer_manager_t *manager,buffer_id_t id) { (void)manager; (void)id; return simulated_fill; }
bool gui_rf_preview_worker_submit(gui_rf_preview_worker_t *worker,const int16_t *samples,size_t count,const gui_rf_block_info_t *info) {
    assert(worker && count==2);
    assert((samples==route_a && info->sample_rate_hz==40000000 && info->start_sample==123456 && info->epoch==42) ||
           (samples==route_b && info->sample_rate_hz==20000000 && info->start_sample==654321 && info->epoch==43));
    if (atomic_load(&((route_worker_t*)worker)->paused)) return false;
    atomic_fetch_add(&route_calls,1); atomic_store(&copy_entered,true);
    while (atomic_load(&hold_copy)) thrd_sleep_ms(1);
    return true;
}
static int route_producer(void *unused) { (void)unused; gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); return 0; }
static int route_detach(void *unused) { (void)unused; video_detach_rf(detach_decoder); atomic_store(&detach_done,true); return 0; }
static video_panel_state_t *new_panel(void) {
    video_panel_state_t *panel=calloc(1,sizeof(*panel)); assert(panel);
    atomic_init(&panel->active_plugin,VIDEO_PLUGIN_CVBS); panel->vhs_fm.decoder.bound_channel=-1;
    return panel;
}
static void wait_flag(atomic_bool *flag) {
    uint64_t begin=get_time_ms(); while(!atomic_load(flag)) { if(get_time_ms()-begin>2000) exit(1); thrd_sleep_ms(1); }
}
int main(void) {
    video_panel_state_t *left=new_panel(), *right=new_panel(), *b=new_panel();
    route_app.is_capturing=true; route_app.settings.rf_preview_budget_percent=100;
    route_app.panel_config_a.left_view=route_app.panel_config_a.right_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.left_state=left; route_app.panel_config_a.right_state=right; route_app.panel_config_a.split=true;
    gui_video_update_rf_routes(&route_app); assert(atomic_load(&creates)==0); // No inactive RF allocation.
    atomic_store(&left->active_plugin,VIDEO_PLUGIN_VHS_FM); atomic_store(&right->active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==1 && left->vhs_fm.decoder.worker==right->vhs_fm.decoder.worker);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==1);
    right->vhs_fm.decoder.overlay.selected_system=1; gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==2 && atomic_load(&destroys)==0);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==3);
    atomic_store(&left->active_plugin,VIDEO_PLUGIN_CVBS); gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&destroys)==1);
    left->vhs_fm.decoder.overlay.selected_system=1; atomic_store(&left->active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_video_update_rf_routes(&route_app); assert(atomic_load(&creates)==2);
    // Hiding a still-existing split view releases its reference, not the other view.
    route_app.panel_config_a.split=false; gui_video_update_rf_routes(&route_app);
    assert(!right->vhs_fm.decoder.worker && atomic_load(&destroys)==1);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==4);
    // Severe capture pressure gates actual decode before a copy.
    route_app.buffers.initialized[BUF_CAPTURE_RF]=true; route_app.buffers.buffers[BUF_CAPTURE_RF].buffer_size=100;
    simulated_fill=95; atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_written,95); gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==4 && atomic_load(&s_rf_pressure_paused));
    route_app.is_capturing=false; simulated_fill=0; atomic_store(&route_app.buffers.stats[BUF_CAPTURE_RF].bytes_written,0); gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true);
    assert(atomic_load(&route_calls)==5 && !atomic_load(&s_rf_pressure_paused)); route_app.is_capturing=true;
    // Wrapped ring head/tail can look full while monotonic byte totals are empty.
    route_app.is_recording=true; route_app.settings.capture_a=true;
    route_app.buffers.initialized[BUF_RECORD_A]=true; route_app.buffers.buffers[BUF_RECORD_A].buffer_size=1024;
    atomic_store(&route_app.buffers.buffers[BUF_RECORD_A].head,0);
    atomic_store(&route_app.buffers.buffers[BUF_RECORD_A].tail,1024);
    atomic_store(&route_app.buffers.stats[BUF_RECORD_A].bytes_read,1024);
    atomic_store(&route_app.buffers.stats[BUF_RECORD_A].bytes_written,1024);
    assert(video_buffer_percent(&route_app,BUF_RECORD_A)==0);
    // Old record drops stay in the snapshot during stop/start, causing no new pause.
    atomic_store(&route_app.buffers.stats[BUF_RECORD_A].write_drops,3);
    route_app.is_capturing=false; gui_video_submit_rf(&route_app,NULL,NULL,0,0,route_info,true);
    route_app.is_capturing=true; route_app.is_recording=false;
    gui_video_submit_rf(&route_app,NULL,NULL,0,0,route_info,true);
    route_app.is_recording=true; gui_video_submit_rf(&route_app,NULL,NULL,0,0,route_info,true);
    assert(!atomic_load(&s_rf_pressure_paused));
    // Independent B source preserves its own metadata and receives one copy.
    route_app.panel_config_b.left_view=PANEL_VIEW_CVBS; route_app.panel_config_b.left_state=b;
    atomic_store(&b->active_plugin,VIDEO_PLUGIN_VHS_FM); gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==3);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true); assert(atomic_load(&route_calls)==7);
    video_detach_rf(&b->vhs_fm.decoder); assert(atomic_load(&destroys)==2);
    detach_decoder=&left->vhs_fm.decoder;
    atomic_store(&copy_entered,false); atomic_store(&hold_copy,true);
    thrd_t producer,detacher; assert(thrd_create_with_priority(&producer,route_producer,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    wait_flag(&copy_entered); assert(thrd_create_with_priority(&detacher,route_detach,NULL,THRD_PRIORITY_NORMAL)==thrd_success);
    uint64_t begin=get_time_ms(); while(atomic_load(&s_rf_routes[0][1])) { if(get_time_ms()-begin>2000) exit(1); thrd_sleep_ms(1); }
    assert(!atomic_load(&detach_done) && atomic_load(&destroys)==2);
    atomic_store(&hold_copy,false); thrd_join(producer,NULL); thrd_join(detacher,NULL);
    assert(atomic_load(&destroys)==3 && atomic_load(&route_calls)==8);
    free(left); free(right); free(b);
    gui_video_submit_rf(&route_app,route_a,NULL,2,0,route_info,true); assert(atomic_load(&route_calls)==8);
    puts("RF routes: lazy shared workers, systems/channels, pressure, hidden view and concurrent last-release passed");
}
