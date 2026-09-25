# MISRC-GUI Server/Client feature — prompt log

This file is the working log for the server/client networking feature task
(per the workflow rule to keep a prompt readme on the host). It records the
request, the design decisions, and the commands/changes made.

## Request (input)
Add the server/client features originally found in
`github.com/namazso/cxadc_vhs_server` so any MISRC-GUI instance can act as
either the host server or the client. A Server/Client control is added to the
info ("About MISRC Capture") page: a client/server switch + port number, with
stock mode running in client/local mode. Device selection and controls must be
mirrored as a proper master/slave control system based on the original HTTP
server — not a one-way data feed.

## Reference protocol (verified against local source)
Local source of truth: `/home/harry/cxadc-capture-server/src/{http.c,main.c}`.
HTTP/1.0, GET-only, socket→bind→listen→accept→per-client pthread. Endpoints:
`/`, `/version`, `/start`, `/stop`, `/cxadc?N`, `/baseband`, `/stats`.

## Design (approved plan, plan_id 1f0a4ede-...)
- 3-way mode: Local (default) / Server (master, owns HW) / Client (slave, mirrors + ingests).
- New module `misrc_gui/net/gui_net.{c,h}` (cross-platform sockets).
- HTTP control endpoints set atomic command flags on `gui_app_t`; the main loop
  in `misrc_gui.c` polls and executes them (reuses the `dropout_stop_requested`
  pattern) so no control mutation happens from worker threads.
- Server data tap: mutex-guarded broadcast queue fed from `gui_capture_callback`
  only while server mode active; per-client cursors stream via `/rf` + `/baseband`.
- Client ingests `/rf`+`/baseband` into `BUF_CAPTURE_RF`/`BUF_CAPTURE_AUDIO` via
  a virtual capture backend modeled on `gui_simulated_start`, reusing the
  extraction/display/record pipeline unchanged.
- Settings: `net_mode`, `net_server_port`, `net_client_host`, `net_client_port`
  (+ edit strings) persisted via the existing fprintf/find_value JSON.
- UI: new Network section in `render_version_info_window` (mode cycle, port
  field, host:port field, status line) + handlers in `gui_handle_interactions`.
- Build: add `gui_net.c` to `sources_gui`; `-lws2_32` on Windows.
- v1 LAN-only, no auth/encryption (mirrors reference warning).

## Change log

### Build wiring
- `misrc_tools/meson.build`: added `misrc_gui/net/gui_net.c` to `sources_gui`; added `-lws2_32` to the Windows `gui_ldflags` for winsock.

### Settings (`misrc_gui/core/gui_app.h`, `core/gui_settings.c`)
- Added `net_mode` (0/1/2), `net_server_port`, `net_server_port_str`, `net_client_host`, `net_client_port_str`, `net_client_port` to `gui_settings_t`.
- Defaults (Local, port 8080) in `gui_settings_init_defaults`; persisted in `gui_settings_save`; parsed in `gui_settings_load` via `find_value` (with string-mirror re-sync for older files).
- Added `net_state` + `net_cmd_*` + `net_peer_*` + `net_connected`/`net_error` atomics to `gui_app_t` for cross-thread control/mirror.

### Net module (`misrc_gui/net/gui_net.{c,h}`)
- Cross-platform sockets (POSIX + winsock) + portable mutex/condvar + `thrd_t`/`net_thread_detach`.
- Broadcast fanout (refcounted chunk list) for the RF/audio data tap.
- Server: listen thread -> per-client HTTP/1.0 threads. Endpoints `/`, `/version`, `/stats`, `/devices`, `/controls`, `/start`, `/stop`, `/record?on=`, `/device?N`, `/rf`, `/baseband`. Control endpoints set atomic `net_cmd_*` flags (main thread executes).
- Client: worker thread polls `/stats` `/devices` `/controls`, mirrors device list + controls + state; forwards queued `net_cmd_*` to the server. Ingest pump threads (RF 4-byte aligned, audio frame-aligned from `/stats audio_frame_bytes`) write into `BUF_CAPTURE_RF`/`BUF_CAPTURE_AUDIO`; main-thread `poll_mirror` starts/stops the ingest backend (extraction+display+audio) modeled on `gui_simulated_start`.

### Integration
- `input/gui_capture.c`: `gui_capture_callback` taps RF+audio into the server fanout after `bufmgr_write_end` (no-op when not server). `gui_app_init` inits net globals + resumes saved mode; `gui_app_cleanup` stops net + cleans globals. `gui_app_start_capture`/`stop_capture`/`start_recording`/`stop_recording` forward to the server in client mode.
- `ui/gui_dropdown.c`: device selection forwards to the server in client mode.
- `core/misrc_gui.c`: main loop calls `gui_net_poll_commands` + `gui_net_poll_mirror` each frame; device-timeout watchdog inhibited for client ingest.
- `ui/gui_ui.c`: new Network section in the "About MISRC Capture" info window (mode cycle, server port field, client host:port fields, status line) + click handlers + `UI_TEXT_FIELD_NET_*` text fields wired into the existing edit machinery.

### Verification
- `ninja misrc_gui` builds clean (no new warnings/errors in changed files).
- `misrc_tools/test/ci_guard_tests.py` -> all PASS (incl. settings persistence contract, Windows GUI link contract).
- `misrc_gui --smoke-test` and `--version` -> exit 0.
- Real-world GUI + end-to-end server<->client behavior: PENDING user confirmation (per workflow rule on user-interactable elements).

### Follow-up fixes (after user GUI test)
- **Mode-cycle freeze fixed**: the blocking `accept()`/`connect()`/`recv()` held worker
  threads, so `thrd_join()` from the UI thread hung the app during mode switches.
  Switched the listen loop to non-blocking `accept()` + `select()` (200ms), and
  `client_connect()` to non-blocking connect + `select()` (2s). All client/server
  sockets now get `SO_RCVTIMEO`/`SO_SNDTIMEO` so pump/worker threads wake to
  check their stop flags. `server_stop()` also `shutdown()`s the listen fd.
  Verified: SIGTERM shutdown now exits in ~300ms (was an indefinite hang).
- **Info window width fixed**: reverted the window back to the original 460/380
  max/min width (the 620/520 widen had shifted the "Download Latest" button and
  made the window too wide/long). Network fields use fixed widths so the window
  stays narrow.
- **Discovery selection added**: the Server now broadcasts a UDP beacon
  (`MISRC\n<tcpport>\n<hostname>` every 2s on port 8091); the Client runs a
  UDP listener that collects beacons into a discovered-server list. The Client
  section of the info window now shows a clickable "Discovered servers" list
  (with a compact manual host:port fallback) instead of requiring typed
  host:port. Clicking a discovered server sets it as the target and connects.
  Verified: beacon received from `192.168.8.245` (`MISRC\n8090\ndecode`) every
  ~2s by an external UDP listener.

### Follow-up fixes (round 3: discovery + width + connect)
- **Window width**: set to 560/460 (was 460 -> clipping, 620 -> too wide/shifted
  Download button). 560 fits the Network section without squishing/clipping.
- **Connect button added**: the Client manual host:port row now ends with a
  Connect/Reconnect button (was just a "manual" label).
- **Client discovery now always runs**: entering Client mode starts the UDP
  discovery listener even with no host selected, so the discovered-server list
  populates without needing a host typed first. The stats/ingest worker only
  starts once a host is selected (by clicking a discovered server or Connect).
- **SO_REUSEPORT** on the discovery UDP socket so multiple clients on the same
  host all receive broadcast beacons (without it the kernel delivers each
  datagram to only one bound socket).
- **Status string** now reflects Client mode even when not connected:
  "Client mode: scanning for servers on the LAN..." / "Client mode: N server(s)
  found - select one" (was showing "Local (no network)" because s_client was
  never created with an empty host).
- Verified end-to-end: server on :8095 broadcasts; client (no host) receives
  beacon `[NET] discovery: found server 192.168.8.245:8095 (decode)` within ~5s
  and does NOT auto-connect (settings host stays empty until the user clicks).

### Follow-up fixes (round 4: settings mirror + visual feed + CLI check)
- **Settings mirroring**: the client worker now mirrors ALL server controls from
  `/controls` (rf_bits_a/b, cxadc_tenbit_mode_card, enable_resample_a/b,
  resample_rate_a/b, use_flac, flac_level, misrc_mode) into the local
  in-memory settings, not just `misrc_mode`. Added a `json_bool` helper.
- **Visual data feed**: verified end-to-end with the server actively capturing:
  server `/start` -> state=1, total_samples flowing; client connects, sees
  `peer state -> 1`, starts ingest backend, RF pump streams (frame=4), audio
  pump streams (frame=12). Data flows pump -> BUF_CAPTURE_RF -> extraction ->
  BUF_DISPLAY -> display thread -> waveform. The client only ingests when the
  server is capturing (peer state >= 1) — correct master/slave behavior.
- **CLI sanity check**: `--smoke-test`, `--device-list`, and `--help` all exit 0
  and produce correct output. The headless CLI capture mode (GUI binary runs
  as the full `misrc_capture` CLI when capture args are passed) is intact.

### Follow-up fixes (round 5: stability + stock-local + compact discovery)
- **Connection stability**: the client worker no longer tears down ingest on
  a single transient /stats failure (which flapped the whole ingest
  start/stop cycle on every network blip). It now keeps a miss_streak
  counter and only marks disconnected / stops ingest after 5 consecutive
  misses (~5s of no contact). Poll interval raised from 500ms to 1s to
  halve TCP connection churn. Backoff capped at 2000ms.
- **Stock loadup = Local**: gui_app_init now forces net_mode=Local on every
  startup and saves it, so a restart never auto-resumes a saved server/client
  state (which could be stale/broken from a prior session). net_mode is a
  per-session setting; the user re-enables Server/Client from the info page.
  Verified: app with saved net_mode=2 starts in Local (net_mode rewritten to
  0, no NET log lines, no port listener).
- **Compact discovery rows**: the discovered-server list rows now use
  CLAY_SIZING_FIT(max 320) + CLAY_TEXT_WRAP_NONE instead of GROW-to-window,
  so each row is only as wide as the "ip:port (name)" text needs (capped at
  320) instead of stretching across the full 560 window width.

### Follow-up fixes (round 6: constant connection/feed)
- **Pump threads reconnect on stream drop**: the /rf and /baseband pump
  threads no longer exit permanently when recv returns 0 (stream closed) or
  a real error. They now close the socket, log "stream dropped,
  reconnecting...", back off, and re-connect + re-send GET in an outer loop.
  Only pump_stop (set on real disconnect / mode change) exits the thread.
  This keeps the data feed alive across transient drops.
- **Ingest runs continuously while connected**: ingest_want is now driven by
  the connection state (cli->connected), NOT peer capture state (st >= 1).
  Previously every server capture start/stop flapped the entire client ingest
  (tear down pump + extraction + display, then rebuild) - unstable. Now the
  pump streams stay open continuously while connected: they just see no data
  (recv timeout) while the server is idle, and data flows immediately when the
  server captures again - no teardown/rebuild per capture state change. Ingest
  is only torn down on real disconnect (5 consecutive /stats misses) or mode
  change.

### Follow-up fixes (round 7: stop messing with the bottom status bar)
- **Dedicated net status line**: added `gui_net_set_status()` which writes to
  `app->net_status` (shown ONLY in the info window's Network status row).
  Replaced ALL `gui_app_set_status()` calls in the net module + the 5
  client-mode control-forwarding calls (start/stop/record/device) with
  `gui_net_set_status()`. Server/client activity (listening, connecting,
  scanning, "Requested ... on server", ingest running/stopped) now never
  touches `app->status_message` — the bottom status bar is owned solely by
  the capture/record/device path as before. Verified: grep for net status
  calls hitting the bottom bar returns empty.

### Follow-up fixes (round 8: full readouts at standard scaling)
- **Status bar compact breakpoint**: the compact (short-label) readouts were
  kicking in below 1450px, but the default window is 1425px — so at standard
  (100%) UI scaling the status bar fell into COMPACT mode and showed short
  readouts ("Samp:", "F:", ...) instead of the full ones ("Samples:",
  "Frames:", ...). Lowered `GUI_UI_STATUS_COMPACT_BREAKPOINT` from 1450 to
  1400 so the default 1425px window at 100% scale shows FULL readouts
  (1425 >= 1400). This was a pre-existing breakpoint (set in commit d00f4db,
  not by the server/client work) but violated the rule that standard scaling =
  full readouts. Smoke + CI pass.

### Follow-up fixes (round 9: connect visibility + status clarity)
- **Info window widened moderately again**: updated the About panel sizing to
  max/min 680/560 so Network controls are readable without clipping while
  keeping the modal bounded by screen margins.
- **Discovery list capped for usability**: in Client mode, render at most 6
  discovered server rows and show a compact `N more server(s) not shown`
  indicator when additional beacons exist. This prevents the list from pushing
  key controls out of view.
- **Connect action made explicit and always visible**: moved the
  `VersionInfoNetConnectButton` to its own `Action:` row below the host:port
  fields so it remains visible even when discovery entries are present.
- **Status fallback hardened**: `gui_net_status_string()` now falls back by
  configured mode (`Server mode: not active` / `Client mode: starting
  discovery...`) before the final Local fallback, and `gui_net_apply_mode()`
  explicitly sets `Local (no network)` when Local mode is selected.
- **Debug noise cleanup**: removed temporary first-contact `/devices` and
  staged-device mirror debug prints from `gui_net.c` after the body-read fix
  was validated.

### Verification (round 9)
- `bash /home/harry/MISRC-GUI/scripts/build-local.sh`  
  PASS: build completed and smoke test passed, output binary
  `/home/harry/MISRC-GUI/build-local/misrc_gui`.
- `python3 /home/harry/MISRC-GUI/misrc_tools/test/ci_guard_tests.py --static-only`  
  PASS: all static guard checks passed.

### Auto-load visual hold test session (round 10)
- Rebuilt against the rebased branch before launch:
  - `meson compile -C /home/harry/MISRC-GUI/build-local`
  - `/home/harry/MISRC-GUI/build-local/misrc_gui --smoke-test`
- Created temporary auto-load configs:
  - `/tmp/misrc-net-tests/server_config.json` (net_mode=1, port=8095)
  - `/tmp/misrc-net-tests/client_config.json` (net_mode=2, host=127.0.0.1, port=8095)
- Launched held GUI sessions with `nohup`:
  - server pid file: `/tmp/misrc-net-tests/server.pid` -> `62726`
  - client pid file: `/tmp/misrc-net-tests/client.pid` -> `62749`
  - server logs: `/tmp/misrc-net-tests/server.stdout.log`, `/tmp/misrc-net-tests/server.stderr.log`
  - client logs: `/tmp/misrc-net-tests/client.stdout.log`, `/tmp/misrc-net-tests/client.stderr.log`
- Runtime checks:
  - process check: both `misrc_gui --config ...server_config.json` and `...client_config.json` running
  - socket check: `LISTEN 0.0.0.0:8095` present
  - server stderr highlights: `[NET] server listening on :8095`
  - client stderr highlights:
    - `[NET] client worker started -> 127.0.0.1:8095`
    - `[NET] client pump /rf streaming (frame=4)`
    - `[NET] client pump /baseband streaming (frame=12)`
    - `[NET] discovery: found server 192.168.8.245:8095 (decode)`

### Follow-up fixes (round 11: connect/disconnect cycle + live status)
- **Action button cycle logic corrected**:
  - previous behavior toggled based on worker-running state, which could leave
    the button stuck showing `Disconnect`.
  - updated behavior now toggles by real connection state:
    - if connected -> disconnect (stop worker + ingest, keep discovery active)
    - if disconnected -> connect/reconnect using current host:port
- **Live status behavior corrected**:
  - `gui_net_status_string()` now prioritizes computed runtime state for
    server/client and shows real-time `Connecting...` / `Connected...` /
    `Client idle ... (press Connect)` transitions instead of sticking on a
    stale cached status message.
- **UI labeling corrected**:
  - Action button label is now driven by `gui_net_active(app)` so it reflects
    actual connection state in real time (`Connect` vs `Disconnect`).
- **Build validation**:
  - `meson compile -C /home/harry/MISRC-GUI/build-local`
  - `/home/harry/MISRC-GUI/build-local/misrc_gui --smoke-test`
  - both completed successfully.

### Launch-hold refresh (round 12)
- Relaunched clean held instances for continued manual GUI testing:
  - server: `/home/harry/MISRC-GUI/build-local/misrc_gui --config /tmp/misrc-net-tests/server_config.json`
  - client: `/home/harry/MISRC-GUI/build-local/misrc_gui --config /tmp/misrc-net-tests/client_config.json`
- Current PIDs:
  - server: `72412`
  - client: `72426`
- Verification:
  - both processes present via `pgrep -af`
  - server listen socket present on `0.0.0.0:8095`

### Follow-up fixes (round 13: toolbar reflect/set server capture state)
- Root cause for \"Disconnect stuck\" in the top toolbar: that button was using
  `app->is_capturing`, which in client mode tracks local ingest runtime rather
  than the peer server's capture state.
- Fixes:
  - added `gui_net_client_peer_capturing()` in `gui_net.{h,c}` to expose
    whether the connected peer reports `/stats state >= 1`.
  - toolbar `ConnectButton` label/color now uses peer capture state in client
    mode (still uses local capture state for non-client mode).
  - toolbar click handler now decides start/stop by peer capture state in
    client mode, so clicking the toolbar button sends `/start` or `/stop`
    against the actual server state.
  - hardened stale-state cleanup by forcing `peer_state=0` when the client
    worker disconnects or reaches failure cutoff.
- Validation:
  - `meson compile -C /home/harry/MISRC-GUI/build-local`
  - `/home/harry/MISRC-GUI/build-local/misrc_gui --smoke-test`
  - relaunch held server/client instances and verify:
    - both processes present (`79279`, `79296`)
    - server listening on `0.0.0.0:8095`

### Quick fix (round 14: About tab Download Latest alignment)
- Request: move `Download Latest` left so it is visually uniform under the
  update controls.
- Change made in `render_version_info_window`:
  - in `VersionInfoUpdateRow`, moved `VersionInfoDownloadButton` to render
    immediately after `VersionInfoUpdateLabel` and before
    `VersionInfoUpdateStatus`.
  - this removes the prior right-shift caused by status-first grow layout and
    keeps the button left-aligned with the update control cluster.
- Verification:
  - `meson compile -C /home/harry/MISRC-GUI/build-local`
  - `/home/harry/MISRC-GUI/build-local/misrc_gui --smoke-test`

### Quick fix (round 15: align with status, not left-offset)
- Follow-up request: aligned, not offset left of the `Available` message.
- Adjustment:
  - in `VersionInfoUpdateRow`, restored order to:
    - `VersionInfoUpdateLabel`
    - `VersionInfoUpdateStatus`
    - `VersionInfoDownloadButton`
  - constrained status width to a bounded fit
    (`CLAY_SIZING_FIT(.min = 0, .max = 260)`) so the button sits to the right
    of the availability text without drifting to the far edge.
- Rebuild verification:
  - `bash /home/harry/MISRC-GUI/scripts/build-local.sh`
  - pass: build + smoke test succeeded.

### Known v1 limits
- LAN-only, no auth/encryption (mirrors the reference cxadc-capture-server warning).
- Client audio ingest uses the server-reported `audio_frame_bytes` for re-framing; correct for the common 24-bit/4ch (12-byte) case.
- Mirrored device list replaces the local dropdown in client mode (intended master/slave); switching back to Local re-enumerates local devices.
- Client-side local recording of the ingested stream is not wired in v1 (the record control forwards to the server, master/slave).
