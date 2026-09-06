/*
 * MISRC GUI - Server/Client networking (cxadc_vhs_server-style peer mode)
 *
 * Any MISRC-GUI instance can run in one of three modes (gui_settings_t.net_mode):
 *   0 = Local  (default; no networking, behaves exactly as before)
 *   1 = Server (host/master: owns hardware + capture, serves HTTP control + RF/audio streams)
 *   2 = Client (slave: connects to a Server, mirrors device list/controls/state,
 *               ingests RF+audio streams into the local pipeline, and forwards its
 *               own start/stop/record/device-select actions to the server)
 *
 * Protocol: HTTP/1.0, GET-only (adapted from the reference cxadc-capture-server).
 *   GET /version            -> MISRC version text
 *   GET /stats              -> JSON capture/device state snapshot
 *   GET /devices            -> JSON enumerated device list (for client dropdown mirror)
 *   GET /controls           -> JSON mirrored control state (misrc mode, bits, resample)
 *   GET /settings           -> JSON {generation, state, misrc_mode_effective, settings:{...}}:
 *                              every server-owned setting by its settings-file key
 *   GET /set?key=K&value=V  -> set one setting (V percent-encoded); applied on the
 *                              server's main thread with the settings panel's own
 *                              checks. 200 {ok,key,value,generation}; 400 unknown or
 *                              not remotely settable; 409 refused (recording, or a
 *                              per-key rule); 422 invalid value; 503 no answer in 1 s
 *   GET /start              -> request capture start (server executes on main thread)
 *   GET /stop               -> request capture stop
 *   GET /record?on=1|0      -> request recording on/off
 *   GET /device?N           -> request device selection N
 *   GET /rf                 -> chunked raw RF stream (tapped from BUF_CAPTURE_RF writes)
 *   GET /baseband           -> chunked raw audio stream (tapped from BUF_CAPTURE_AUDIO writes)
 *
 * Thread-safety: HTTP endpoints and the client mirror thread only ever set atomic
 * command flags on gui_app_t (net_cmd_*). The main render loop polls them via
 * gui_net_poll_commands() and gui_net_poll_mirror() and executes the real control
 * actions (gui_app_start_capture, device selection, etc.) on the main thread - the
 * same safe pattern already used by app->dropout_stop_requested. No device handles
 * or is_capturing state are mutated from worker threads.
 *
 * The server feeds /rf and /baseband through a buffer-manager write tap
 * (bufmgr_set_write_tap) installed by server_start(): every commit to
 * BUF_CAPTURE_RF / BUF_CAPTURE_AUDIO reaches the fanout on the writer's thread,
 * whichever capture backend made it. See gui_net_fanout.h for the fanout.
 *
 * v1 is LAN-only with no authentication or encryption (mirrors the reference
 * server's own warning: do not expose to the public internet).
 */
#ifndef GUI_NET_H
#define GUI_NET_H

#include "../core/gui_app.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Net mode constants (match gui_settings_t.net_mode). */
#define GUI_NET_MODE_LOCAL  0
#define GUI_NET_MODE_SERVER 1
#define GUI_NET_MODE_CLIENT 2

/* One-time global init/cleanup (WSAStartup/WSACleanup on Windows; SIGPIPE ignore
 * on POSIX). Safe to call multiple times; idempotent. */
void gui_net_init_globals(void);
void gui_net_cleanup_globals(void);

/* Apply the configured net mode to the app. Starts/stops the server or client so
 * that app state matches app->settings.net_mode + port/host. Called from the UI
 * on mode change and at startup. Returns 0 on success, negative on error. */
int gui_net_apply_mode(gui_app_t *app);

/* Tear down any active server/client (for gui_app_cleanup and mode switches). */
void gui_net_stop(gui_app_t *app);

/* True if a server is currently listening or a client is currently connected. */
bool gui_net_active(const gui_app_t *app);

/* True iff the app is running in Client mode (regardless of connect state). */
bool gui_net_is_client(const gui_app_t *app);

/* True iff the app is running in Server mode (regardless of listen state). */
bool gui_net_is_server(const gui_app_t *app);

/* Main-thread pollers: called every frame from the render loop.
 *  - poll_commands: executes queued net_cmd_* control requests on the main thread
 *    (server: act locally; client: the client worker already forwarded these, so
 *     this just clears them - the worker drains client-originated flags directly).
 *  - poll_mirror: applies mirrored peer state (device list, sample rate, capture
 *    state) into the local app for client mode, and updates the UI status text. */
void gui_net_poll_commands(gui_app_t *app);
void gui_net_poll_mirror(gui_app_t *app);

/* Client control forwarding: set the corresponding net_cmd flag so the client
 * worker thread sends the request to the server. Used by the UI/local control
 * path when running in client mode. */
void gui_net_client_request_start(gui_app_t *app);
void gui_net_client_request_stop(gui_app_t *app);
void gui_net_client_request_record(gui_app_t *app, bool on);
void gui_net_client_request_device(gui_app_t *app, int device_index);

/* Human-readable mode name and a status string for the info-page UI. */
const char *gui_net_mode_name(int mode);

/* Write the network status line shown ONLY in the info window's Network
 * section. Net code MUST use this instead of gui_app_set_status so
 * server/client activity never clobbers the bottom status bar. */
void gui_net_set_status(gui_app_t *app, const char *msg);

/* Read the current network status line into buf (for the info window UI). */
void gui_net_status_string(const gui_app_t *app, char *buf, size_t len);

/* LAN server discovery (client mode). The client listens for UDP beacons
 * broadcast by servers and exposes the found servers for the UI to render as a
 * selectable list, replacing manual host:port entry. */
int  gui_net_discovered_count(void);
bool gui_net_get_discovered(int index, char *host, size_t host_cap,
                            uint16_t *port, char *name, size_t name_cap);
void gui_net_select_discovered(gui_app_t *app, int index);
/* Client-mode connection toggles for the info-window Action button.
 * "connection running" means the client worker is active (connected or trying).
 * toggle_connection starts/stops that worker while keeping discovery active. */
bool gui_net_client_connection_running(const gui_app_t *app);
void gui_net_client_toggle_connection(gui_app_t *app);
/* True when client mode is connected and the peer server reports capture/record
 * state active (peer /stats state >= 1). */
bool gui_net_client_peer_capturing(const gui_app_t *app);
/* True when connected and the peer reports recording (state == 2). A client
 * never sets its own is_recording (that would start local WAV writers), so
 * every "are we recording" readout goes through gui_app_effective_recording. */
bool gui_net_client_peer_recording(const gui_app_t *app);

/* The server's settings on a client. The worker stages /settings; the main
 * thread applies the snapshot in gui_net_poll_mirror(). For the UI pass the
 * client swaps a copy of the snapshot into app->settings (view_begin) and
 * swaps its own settings back afterwards (view_end), sending every
 * server-owned field the pass changed as a /set. The client's own settings
 * file never holds a server value. */
void gui_net_client_view_begin(gui_app_t *app);
void gui_net_client_view_end(gui_app_t *app);
bool gui_net_client_peer_settings_valid(const gui_app_t *app);
uint32_t gui_net_client_peer_generation(const gui_app_t *app);
double gui_net_client_peer_settings_age_s(const gui_app_t *app);
/* -1 not known yet, 0 the server has no /settings (older build), 1 it does. */
int gui_net_client_server_settings_support(const gui_app_t *app);
void gui_net_client_request_settings_refresh(gui_app_t *app);
/* The most recent refused /set ("key: reason"), and how long ago. */
bool gui_net_client_last_set_error(const gui_app_t *app, char *buf, size_t cap, double *age_s);
/* Recording relay from the peer's /stats. */
bool gui_net_client_peer_record_pending(const gui_app_t *app);
bool gui_net_client_peer_record_finalizing(const gui_app_t *app);
uint32_t gui_net_client_peer_record_drops(const gui_app_t *app);
uint64_t gui_net_client_peer_disk_free(const gui_app_t *app);

/* Headless modes (return before InitWindow):
 *  - serve: run the server from a --config file with no window, pumping the
 *    main-thread pollers, for `seconds` (0 = until SIGTERM/SIGINT);
 *  - client probe: connect to host:port, mirror for `seconds`, print one JSON
 *    line with the snapshot and the peer state. Exit codes in gui_net.c. */
int gui_net_serve_main(int seconds);
int gui_net_client_probe_main(const char *host, int port, int seconds);

#endif /* GUI_NET_H */
