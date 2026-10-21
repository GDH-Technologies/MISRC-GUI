/* Production pooled UI routes: lazy creation, duplicate views, independent
 * systems/channels, pressure gating and last-subscriber teardown during copy. */
#include "../misrc_gui/signal/gui_cvbs.c"
#include "../misrc_gui/processing/gui_rf_preview_pressure.c"
#include "../misrc_gui/visualization/gui_panel.c"
#include <assert.h>
typedef struct { gui_rf_format_t format; gui_rf_profile_t profile; atomic_bool paused, audio_enabled; atomic_int submits; } route_worker_t;
static const void *monitor_source;
static atomic_int route_calls, creates, destroys, resets;
static atomic_bool hold_copy, copy_entered, detach_done;
static vhs_fm_decoder_t *detach_decoder;
static gui_app_t route_app;
static size_t simulated_fill;
static int16_t route_a[2]={123,-456}, route_b[2]={789,-123};
static gui_rf_block_info_t route_info[2]={{40000000,123456,42,false},{20000000,654321,43,true}};
gui_rf_preview_worker_t *gui_rf_preview_worker_create(gui_rf_format_t format) {
    route_worker_t *w=calloc(1,sizeof(*w)); assert(w); w->format=format;
    atomic_init(&w->audio_enabled,true);
    atomic_init(&w->submits,0);
    atomic_fetch_add(&creates,1); return (gui_rf_preview_worker_t*)w;
}
gui_rf_profile_t gui_vhs_fm_get_profile(const vhs_fm_decoder_t *d) {
    if(d->overlay.selected_tape==1 && d->overlay.beta_super)
        return d->overlay.beta_hifi ? GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI : GUI_RF_PROFILE_NTSC_SUPERBETA;
    return d->overlay.selected_tape==1 ? (d->overlay.beta_hifi ? GUI_RF_PROFILE_NTSC_BETAMAX_HIFI : GUI_RF_PROFILE_NTSC_BETAMAX)
                                     : (d->overlay.selected_system ? GUI_RF_PROFILE_NTSC_VHS : GUI_RF_PROFILE_PAL_VHS);
}
bool gui_rf_profile_has_hifi(gui_rf_profile_t profile) {
    return profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI || profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI;
}
gui_rf_format_t gui_rf_profile_format(gui_rf_profile_t p) { return p==GUI_RF_PROFILE_PAL_VHS ? GUI_RF_PAL : GUI_RF_NTSC; }
gui_rf_preview_worker_t *gui_rf_preview_worker_create_profile(gui_rf_profile_t profile) {
    gui_rf_preview_worker_t *worker=gui_rf_preview_worker_create(gui_rf_profile_format(profile));
    ((route_worker_t*)worker)->profile=profile;
    return worker;
}
void gui_rf_preview_worker_set_audio_callback(gui_rf_preview_worker_t *w, gui_rf_preview_audio_callback_t cb, void *ctx) {
    assert(w && cb && ctx==w);
}
void gui_rf_preview_worker_set_audio_enabled(gui_rf_preview_worker_t *w,bool enabled) {
    assert(w); atomic_store(&((route_worker_t*)w)->audio_enabled,enabled);
}
void gui_audio_set_hifi_source(const void *source) { monitor_source=source; }
void gui_audio_push_hifi(const void *source,const float *pcm,size_t frames) { assert(source && pcm && frames); }
void gui_audio_reset_hifi(const void *source) { assert(source); }
void gui_rf_preview_worker_destroy(gui_rf_preview_worker_t *worker) {
    assert(worker); atomic_fetch_add(&destroys,1); free(worker);
}
bool gui_rf_preview_worker_set_budget(double cores) { assert(cores>=.5 && cores<=8); return true; }
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
    atomic_fetch_add(&((route_worker_t*)worker)->submits,1);
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
static bool route_audio_enabled(const vhs_fm_decoder_t *decoder) {
    assert(decoder->worker);
    return atomic_load(&((route_worker_t*)decoder->worker)->audio_enabled);
}
// Use the production panel cache transitions with lightweight panel factories:
// the route fixtures below own their video states and need no render resources.
static void *cache_waveform_create(void) { void *state=calloc(1,1); assert(state); return state; }
static void cache_video_destroy(void *state) { video_detach_rf(&((video_panel_state_t*)state)->vhs_fm.decoder); }
const panel_vtable_t *panel_get_vtable(panel_view_type_t type) {
    static const panel_vtable_t waveform={.create=cache_waveform_create,.destroy=free};
    static const panel_vtable_t video={.destroy=cache_video_destroy};
    return type==PANEL_VIEW_WAVEFORM ? &waveform : type==PANEL_VIEW_CVBS ? &video : NULL;
}
static void test_cached_hifi_panel_routes(void) {
    video_panel_state_t a={0},b={0};
    video_panel_state_t *panels[]={&a,&b};
    channel_panel_config_t *configs[]={&route_app.panel_config_a,&route_app.panel_config_b};
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    for(size_t i=0;i<2;i++) {
        atomic_init(&panels[i]->active_plugin,VIDEO_PLUGIN_VHS_FM);
        vhs_fm_decoder_t *d=&panels[i]->vhs_fm.decoder;
        d->bound_channel=-1;
        d->overlay.selected_tape=1;d->overlay.selected_system=1;d->overlay.beta_hifi=true;
        *configs[i]=(channel_panel_config_t){0};
        configs[i]->left_view=PANEL_VIEW_CVBS;configs[i]->left_state=panels[i];
        configs[i]->left_cached_view=configs[i]->right_cached_view=PANEL_VIEW_COUNT;
    }
    a.vhs_fm.decoder.overlay.beta_super=true;
    route_app.is_capturing=true;route_app.settings.audio_monitor_playback=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && monitor_source==a.vhs_fm.decoder.worker);
    assert(route_audio_enabled(&a.vhs_fm.decoder) && !route_audio_enabled(&b.vhs_fm.decoder));
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&((route_worker_t*)a.vhs_fm.decoder.worker)->submits)==1);
    assert(atomic_load(&((route_worker_t*)b.vhs_fm.decoder.worker)->submits)==1);

    // Switching to waveform parks the exact panel, but its RF/audio route must
    // detach. Channel B becomes audible and is the sole remaining submit target.
    panel_config_set_left_view(configs[0],PANEL_VIEW_WAVEFORM);
    assert(configs[0]->left_cached_state==&a && configs[0]->left_cached_view==PANEL_VIEW_CVBS);
    gui_video_update_rf_routes(&route_app);
    assert(!a.vhs_fm.decoder.worker && atomic_load(&destroys)==destroyed+1);
    assert(monitor_source==b.vhs_fm.decoder.worker && route_audio_enabled(&b.vhs_fm.decoder));
    for(int profile=0;profile<GUI_RF_PROFILE_COUNT;profile++) assert(!atomic_load(&s_rf_routes[0][profile]));
    int calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&route_calls)==calls+1);
    assert(atomic_load(&((route_worker_t*)b.vhs_fm.decoder.worker)->submits)==2);

    // Restoring the parked state retains all Beta options, creates the matching
    // profile worker and restores A's source priority while disabling B audio.
    panel_config_set_left_view(configs[0],PANEL_VIEW_CVBS);
    assert(configs[0]->left_state==&a && configs[0]->left_cached_view==PANEL_VIEW_WAVEFORM);
    vhs_fm_decoder_t *d=&a.vhs_fm.decoder;
    assert(d->overlay.selected_tape==1 && d->overlay.selected_system==1);
    assert(d->overlay.beta_super && d->overlay.beta_hifi);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+3 && ((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    assert(monitor_source==d->worker && route_audio_enabled(d) && !route_audio_enabled(&b.vhs_fm.decoder));
    calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&route_calls)==calls+2);
    assert(atomic_load(&((route_worker_t*)d->worker)->submits)==1);
    assert(atomic_load(&((route_worker_t*)b.vhs_fm.decoder.worker)->submits)==3);
    panel_config_cleanup(configs[0]);panel_config_cleanup(configs[1]);
    assert(!monitor_source && atomic_load(&destroys)==destroyed+3);
    calls=atomic_load(&route_calls);
    gui_video_submit_rf(&route_app,route_a,route_b,2,2,route_info,true);
    assert(atomic_load(&route_calls)==calls);
}
static void test_superbeta_option_routes(void) {
    video_panel_state_t panel={0};
    vhs_fm_decoder_t *d=&panel.vhs_fm.decoder;
    assert(!d->overlay.beta_super && !d->overlay.beta_hifi);
    atomic_store(&panel.active_plugin,VIDEO_PLUGIN_VHS_FM);
    d->bound_channel=-1;d->overlay.selected_tape=1;
    route_app.panel_config_a.left_state=&panel;
    route_app.panel_config_a.left_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.split=false;
    route_app.panel_config_b.left_view=PANEL_VIEW_WAVEFORM;
    route_app.panel_config_b.right_view=PANEL_VIEW_WAVEFORM;
    route_app.settings.audio_monitor_playback=true;
    route_app.is_capturing=true;
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_BETAMAX);
    assert(!monitor_source && !route_audio_enabled(d));
    d->overlay.beta_super=true;
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA);
    assert(!monitor_source && !route_audio_enabled(d));
    d->overlay.beta_hifi=true;
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    assert(monitor_source==d->worker && route_audio_enabled(d));
    d->overlay.beta_super=false;
    gui_video_update_rf_routes(&route_app);
    assert(d->overlay.beta_hifi && ((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    assert(monitor_source==d->worker && route_audio_enabled(d));
    // Remembered Beta options cannot alter the VHS profile or enable RF HiFi.
    d->overlay.beta_super=true;d->overlay.selected_tape=0;d->overlay.selected_system=1;
    gui_video_update_rf_routes(&route_app);
    assert(((route_worker_t*)d->worker)->profile==GUI_RF_PROFILE_NTSC_VHS);
    assert(!monitor_source && !route_audio_enabled(d));
    atomic_store(&panel.active_plugin,VIDEO_PLUGIN_CVBS);
    gui_video_update_rf_routes(&route_app);
    assert(!d->worker && !monitor_source);
}
static void test_superbeta_routes(void) {
    video_panel_state_t a_left={0},a_right={0},b_left={0},b_right={0};
    video_panel_state_t *panels[]={&a_left,&a_right,&b_left,&b_right};
    int created=atomic_load(&creates),destroyed=atomic_load(&destroys);
    for(size_t i=0;i<4;i++) {
        atomic_store(&panels[i]->active_plugin,VIDEO_PLUGIN_VHS_FM);
        panels[i]->vhs_fm.decoder.bound_channel=-1;
        assert(!panels[i]->vhs_fm.decoder.overlay.beta_super);
        panels[i]->vhs_fm.decoder.overlay.selected_tape=1;
        panels[i]->vhs_fm.decoder.overlay.beta_super=true;
    }
    route_app.panel_config_a.left_state=&a_left;
    route_app.panel_config_a.right_state=&a_right;
    route_app.panel_config_a.split=true;
    route_app.panel_config_b.left_state=&b_left;
    route_app.panel_config_b.right_state=&b_right;
    route_app.panel_config_b.left_view=route_app.panel_config_b.right_view=PANEL_VIEW_WAVEFORM;
    route_app.panel_config_b.split=true;
    route_app.settings.audio_monitor_playback=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+1 && a_left.vhs_fm.decoder.worker==a_right.vhs_fm.decoder.worker);
    assert(((route_worker_t*)a_left.vhs_fm.decoder.worker)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA);
    assert(!monitor_source && !route_audio_enabled(&a_left.vhs_fm.decoder));
    a_right.vhs_fm.decoder.overlay.beta_hifi=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+2 && a_left.vhs_fm.decoder.worker!=a_right.vhs_fm.decoder.worker);
    assert(monitor_source==a_right.vhs_fm.decoder.worker && route_audio_enabled(&a_right.vhs_fm.decoder));
    assert(((route_worker_t*)monitor_source)->profile==GUI_RF_PROFILE_NTSC_SUPERBETA_HIFI);
    b_left.vhs_fm.decoder.overlay.selected_tape=1;
    b_left.vhs_fm.decoder.overlay.beta_super=false;
    b_left.vhs_fm.decoder.overlay.beta_hifi=b_right.vhs_fm.decoder.overlay.beta_hifi=true;
    route_app.panel_config_b.left_view=route_app.panel_config_b.right_view=PANEL_VIEW_CVBS;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+4 && monitor_source==a_right.vhs_fm.decoder.worker);
    assert(!route_audio_enabled(&b_left.vhs_fm.decoder) && !route_audio_enabled(&b_right.vhs_fm.decoder));
    // A-left wins across both HiFi profile kinds; the previous SuperBeta source is disabled.
    a_left.vhs_fm.decoder.overlay.beta_super=false;a_left.vhs_fm.decoder.overlay.beta_hifi=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==created+5 && monitor_source==a_left.vhs_fm.decoder.worker);
    assert(route_audio_enabled(&a_left.vhs_fm.decoder) && !route_audio_enabled(&a_right.vhs_fm.decoder));
    gui_rf_preview_worker_t *saved[4];
    for(size_t i=0;i<4;i++) saved[i]=panels[i]->vhs_fm.decoder.worker;
    // Muting keeps every video profile, worker and reference intact.
    route_app.settings.audio_monitor_playback=false;gui_video_update_rf_routes(&route_app);
    for(size_t i=0;i<4;i++) assert(panels[i]->vhs_fm.decoder.worker==saved[i] && !route_audio_enabled(&panels[i]->vhs_fm.decoder));
    assert(atomic_load(&creates)==created+5 && monitor_source==saved[0]);
    route_app.settings.audio_monitor_playback=true;route_app.is_capturing=false;
    gui_video_update_rf_routes(&route_app);
    for(size_t i=0;i<4;i++) assert(!route_audio_enabled(&panels[i]->vhs_fm.decoder));
    route_app.is_capturing=true;gui_video_update_rf_routes(&route_app);
    assert(route_audio_enabled(&a_left.vhs_fm.decoder) && !route_audio_enabled(&a_right.vhs_fm.decoder));
    // Releasing each source transfers ownership in visible A-left/A-right/B-left/B-right order.
    for(size_t i=0;i<4;i++) {
        atomic_store(&panels[i]->active_plugin,VIDEO_PLUGIN_CVBS);
        gui_video_update_rf_routes(&route_app);
        assert(monitor_source==(i+1<4 ? saved[i+1] : NULL));
        if(i+1<4) assert(route_audio_enabled(&panels[i+1]->vhs_fm.decoder));
        for(size_t j=i+2;j<4;j++) assert(!route_audio_enabled(&panels[j]->vhs_fm.decoder));
    }
    assert(atomic_load(&creates)==created+5 && atomic_load(&destroys)==destroyed+5);
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
    // Same-channel Beta views share only identical full profiles.
    video_panel_state_t beta_left={0}, beta_right={0};
    atomic_store(&beta_left.active_plugin,VIDEO_PLUGIN_VHS_FM);
    atomic_store(&beta_right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    beta_left.vhs_fm.decoder.overlay.selected_tape=1;
    beta_right.vhs_fm.decoder.overlay.selected_tape=1;
    route_app.panel_config_a.left_state=&beta_left;
    route_app.panel_config_a.right_state=&beta_right;
    route_app.panel_config_a.left_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.right_view=PANEL_VIEW_CVBS;
    route_app.panel_config_a.split=true;
    route_app.panel_config_b.left_view=PANEL_VIEW_WAVEFORM;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==4 && beta_left.vhs_fm.decoder.worker==beta_right.vhs_fm.decoder.worker);
    assert(!monitor_source);
    beta_right.vhs_fm.decoder.overlay.beta_hifi=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==5 && beta_left.vhs_fm.decoder.worker!=beta_right.vhs_fm.decoder.worker);
    assert(monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(((route_worker_t*)monitor_source)->profile==GUI_RF_PROFILE_NTSC_BETAMAX_HIFI);
    assert(!atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    route_app.settings.audio_monitor_playback=true;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    route_app.settings.audio_monitor_playback=false;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==5 && monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(!atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    route_app.settings.audio_monitor_playback=true;
    video_panel_state_t beta_b={0};
    atomic_store(&beta_b.active_plugin,VIDEO_PLUGIN_VHS_FM);
    beta_b.vhs_fm.decoder.overlay.selected_tape=1;
    beta_b.vhs_fm.decoder.overlay.beta_hifi=true;
    route_app.panel_config_b.left_view=PANEL_VIEW_CVBS;
    route_app.panel_config_b.left_state=&beta_b;
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==6 && monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    assert(!atomic_load(&((route_worker_t*)beta_b.vhs_fm.decoder.worker)->audio_enabled));
    atomic_store(&beta_right.active_plugin,VIDEO_PLUGIN_CVBS);
    gui_video_update_rf_routes(&route_app);
    assert(monitor_source==beta_b.vhs_fm.decoder.worker);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    atomic_store(&beta_right.active_plugin,VIDEO_PLUGIN_VHS_FM);
    gui_video_update_rf_routes(&route_app);
    assert(atomic_load(&creates)==7 && monitor_source==beta_right.vhs_fm.decoder.worker);
    assert(atomic_load(&((route_worker_t*)monitor_source)->audio_enabled));
    assert(!atomic_load(&((route_worker_t*)beta_b.vhs_fm.decoder.worker)->audio_enabled));
    video_detach_rf(&beta_left.vhs_fm.decoder);
    video_detach_rf(&beta_right.vhs_fm.decoder);
    video_detach_rf(&beta_b.vhs_fm.decoder);
    assert(atomic_load(&destroys)==7 && !monitor_source);
    test_superbeta_routes();
    test_superbeta_option_routes();
    test_cached_hifi_panel_routes();
    puts("RF routes: shared profiles, optional SuperBeta HiFi, cached panels, source priority, mute, pressure and concurrent last-release passed");
}
