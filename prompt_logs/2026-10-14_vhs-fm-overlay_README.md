# Prompt Log — 2026-10-14 — VHS FM decoder overlay: stats selector, double-click hide, font scaling, Frame Active/Full

User requests (in order):
1. Make the VHS FM decoder stats readout selectable (All / Basic / Off).
2. Double-click on the preview window hides its controls (all overlay chrome).
3. Fix the text font/scaling for the stats readout (follow UI zoom).
4. Follow-up: the reason/"sync stat" line at the bottom was still present when controls were hidden — hide it with the rest of the chrome.
5. Add a "Frame: Active / Full" display mode so the VBI space (and colour burst, with colour decoding) is visible.

## Implementation

### gui_vhs_fm.h
- overlay struct extended: `stats_btn_rect`, `stats_opts_rect[3]`, `stats_dropdown_open`,
  `stats_view` (0=All, 1=Basic, 2=Off), `frame_rect`, `frame_mode` (0=Active, 1=Full),
  `controls_hidden`, `last_click_ms/x/y`.
- New API: `gui_vhs_fm_set_frame_mode(decoder, mode_select)`.

### gui_vhs_fm.c — stats rendering
- Font/metrics now scale with `gui_ui_get_scale_factor()`: `font_px` and logical
  `line_h = (font_px + 6) / ui_scale`.
- `VHS_DRAW_STAT` macro drops lines that no longer fit the panel width/height
  (instead of overflowing the panel).
- `stats_view` gates lines: All = full block, Basic = 2 lines (lock + queue/color),
  Off = no stats text.
- Reason/"sync stat" line: now drawn only when `!controls_hidden` — double-click hide
  removes ALL text (this was the reported lingering-line bug).

### gui_vhs_fm.c — frame mode API
- `gui_vhs_fm_set_frame_mode`: clamps to 0/1, stores `overlay.frame_mode`, forwards
  to the bound worker via `gui_rf_preview_worker_set_frame_mode` (worker locks
  frame_mutex, forwards to the DSP decoder, clears `ready_fields` so a mode switch
  never mixes modes inside one frame).
- `gui_vhs_fm_bind_worker`: re-applies the stored Full mode to each newly bound
  worker (fresh workers start Active; the DSP decoder preserves the flag across
  its own resets).

### gui_rf_preview.c/.h (DSP decoder)
- `gui_rf_decoder_set_frame_mode(decoder, full_frame)`; struct field `full_frame`;
  `gui_rf_decoder_reset` preserves it across resets.
- In Full mode `rf_render_field` uses `line_first = 0`, window 0.0 .. line_us
  instead of the active-area window; line map and colour burst loops offset by
  `line_first + y`; pixel loop maps the whole line.
  Active area (PAL: active_first 22 of 768 px; NTSC: 20 of 640) vs full line now
  both decode, VBI lines fall back to nominal positions (no H anchors in EQ /
  broad pulses), burst (PAL 5.6–7.85 us) renders decoded chroma in Full mode.

### gui_cvbs.c — overlay
- Stats dropdown button right-anchored beside the System button (arrow + 3 options,
  `gui_dropdown_option_color` selected/hover).
- "Frame: Active/Full" cycle button left of the Stats button.
- Click handler: control clicks reset `last_click_ms` (never trigger the
  double-click toggle); stats dropdown open/select/close; double-click
  (<=350 ms, <=6 px) anywhere else toggles `controls_hidden` (also closes the
  stats dropdown); Frame button cycles frame_mode via `gui_vhs_fm_set_frame_mode`.
- `render_vhs_decoder_overlay`: early-return when `controls_hidden`
  (buttons + stats + reason line all hidden for a clean video preview).

## Commands run and results

```
ninja -C /home/harry/MISRC-GUI/build-local misrc_gui gui_playback_resolve_test
  -> [23/23] linked misrc_gui, no compile errors (pre-existing warnings only)

/home/harry/MISRC-GUI/build-local/gui_playback_resolve_test
  -> gui_playback_resolve: 29 checks, 0 failures
```

## Files changed
- misrc_tools/misrc_gui/signal/gui_vhs_fm.h
- misrc_tools/misrc_gui/signal/gui_vhs_fm.c
- misrc_tools/misrc_gui/signal/gui_cvbs.c
- misrc_tools/misrc_gui/signal/gui_rf_preview.h
- misrc_tools/misrc_gui/signal/gui_rf_preview.c
- misrc_tools/misrc_gui/signal/gui_rf_preview_worker.h
- misrc_tools/misrc_gui/signal/gui_rf_preview_worker.c

## Session 2 (2026-10-14 later) — user run-21 feedback and measured root causes

User reported after run 21: (1) stats vertical position off a bit,
(2) stats text still unreadable scaling-wise, (3) massive queue drops
(long-standing), (4) CH A / CH B sliders/feeds look crossed in the panels.

### Investigation (hard data)
- Run-21 log (53 lines): NO capture/playback started — the font/position
  observations are from the idle panel; queue-drop/crossed observations are
  from earlier capture/playback sessions.
- Saved settings: ui_scale_percent=100 (so the previous "12 * ui_scale" font
  math was a no-op here), misrc_mode=true (sticky MISRC A/B swap),
  rf_preview_budget_percent unset (default).
- Font model verified in code: Clay_Raylib_Render wraps ALL custom panel
  drawing in rlScalef(ui_scale) — panel bounds are LOGICAL, gui_text_* font
  sizes are PHYSICAL, the global transform scales them. Correct pattern
  (oscilloscope/CVBS OSD): FIXED physical font size. The old stats code used
  12 px (smallest text in the app; buttons use 16) and "12 * ui_scale" would
  have double-scaled at any zoom != 100%.
- **New DSP throughput bench** (`gui_rf_preview_test --bench [s]`, added this
  session; one thread, locked steady state, 20 MSPS decode rate):
  - PAL:  2.88 s content in 3.249 s wall/cpu -> 0.89x realtime, 22.56 ms/field
  - NTSC: 2.94 s content in 3.135 s wall/cpu -> 0.94x realtime, 17.81 ms/field
  => one worker needs ~1.12 (PAL) / ~1.06 (NTSC) logical cores for realtime.
  The shared preview budget ceiling was 1.0 logical core (policy rejected
  cores > 1.0), FIFO-serialized across BOTH channels' workers -> saturation
  was guaranteed: the 200 ms queue fills, submit rejects the new block AND
  flags a full flush, the worker drops the whole backlog (each counted) and
  re-locks -> the "massive queue drops" counter. Machine: i9-7980XE, 36 cores.
- Crossed A/B root cause (playback): gui_playback_commit latched
  capture_mode_runtime_misrc = user_capture_mode_misrc, and the sticky saved
  misrc_mode=true keeps the MISRC hardware swap armed; gui_ui_sync_capture_
  mode_state forces the swap off for cxadc devices but NOT for the Playback
  device. In playback the per-channel seek sliders scrub their own FILE while
  the extraction swap crossed the panel feeds -> "sliders/feeds crossed".
  (Live 2-card cxadc capture was already forced unswapped by the cxadc
  profile + UI sync: card 0 -> CH A, card 1 -> CH B.)

### Fixes
1. gui_vhs_fm.c stats block: font now FIXED FONT_SIZE_DROPDOWN_OPT (16 px,
   matching the overlay buttons; the global transform scales it with zoom —
   no more double-scaling at zoom != 100%), logical line height derived from
   it, bottom-stacked layout (reason line owns the lowest slot, stats stack
   directly above it — no gap/overlap in All/Basic/Off), and long lines
   truncate with "..." (vhs_draw_stat_fitted) instead of vanishing on narrow
   or split panels.
2. Budget ceiling raised 1.0 -> 8.0 logical cores (gui_rf_preview_policy.c,
   policy test contract updated). Settings default is now machine-scaled
   (gui_settings.c): 12+ cores -> 300%, 6-11 -> 200%, else 100% (measured need:
   ~1.1 cores/worker, two shared-FIFO workers). Budget button now cycles
   50 -> 75 -> 100 -> 150 -> 200 -> 300 -> 50 (gui_cvbs.c vhs_budget_clamp/
   vhs_budget_next; routes, button label and click all use them). Your 36-core
   machine gets 300% by default (your settings had no saved budget value, so
   the default applies — no click needed).
3. Playback A/B swap leak fixed: gui_playback_commit forces
   capture_mode_runtime_misrc = false, and gui_ui_sync_capture_mode_state
   forces the MISRC swap off while a Playback device is selected (like cxadc,
   preserving the sticky settings.misrc_mode preference for hsdaoh sessions).
4. gui_rf_preview_test --bench [seconds]: new measured-throughput mode
   (PAL + NTSC) for future budget decisions.

### Verification
- ninja full build: OK (pre-existing warnings only; the unused-function
  warning for gui_ui_cxadc_base_rate_khz in test builds predates this session).
- meson test: 27/27 OK (includes gui_rf_preview_policy with the new ceiling
  contract, gui_rf_routes with the updated mock range).
- gui_rf_preview_test --bench 3: PAL 0.89x/1.12 cores, NTSC 0.94x/1.06 cores.
- With the 300% default on this machine, two realtime PAL-class workers need
  ~2.3 of the 3.0 budget -> no saturation -> no flush-thrash queue drops.

## State at commit (2026-10-14) — handoff for another system

Committed + pushed BEFORE GUI real-world confirmation of the session-2 fixes
(run 22 launched for verification but closed early; picking up on another
system). NOTHING in this session is user-confirmed yet — verify before
trusting. Verified so far: full build clean, meson test 27/27 OK, and the
measured bench numbers above (they are real data, not GUI claims).

### Pending verification checklist (next system)
1. Stats text: fixed 16 px font, bottom-stacked above the reason line, no
   overlap in All/Basic/Off; readable and correctly sized at UI zoom != 100%
   (the old code double-scaled there — that path is now fixed but unverified).
2. Double-click hides ALL chrome including the reason line; restores on the
   second double-click. Stats dropdown All/Basic/Off switches the readout.
3. Frame: Active/Full button: Full shows VBI rows + sync tip + colour burst
   with decoded chroma; Active returns to the cropped picture; mode persists
   across worker rebinds (System PAL/NTSC toggle) and DSP resets.
4. Queue drops during a REAL capture (video 20 MSPS + hifi): with the
   machine-scaled 300% budget default (36-core i9) the queue should hold and
   the drops counter should stay near zero. Budget button now cycles
   50 -> 75 -> 100 -> 150 -> 200 -> 300 -> 50. On 6-11-core machines the
   default is 200%; <=5 cores 100% (drops expected there by design).
5. Playback with files on BOTH channels: CH A panels show file A, CH B shows
   file B (the MISRC A/B swap no longer leaks into playback).
6. OPEN QUESTION from the user's report: the crossed CH A/CH B was only
   reproducible in code via the playback swap leak. If it ALSO appears during
   a live 2-card cxadc capture on the next system, that is a different,
   unfound path — check the wiring (card 0 = CH A, card 1 = CH B) and the
   MODE TRACE lines in the run log.

### Not done (deliberate)
- No zip restore point yet: per the user's rule that waits for a confirmed
  "fully working" state on the next system. After confirmation: update this
  log, then `git archive` -> prompt_logs/restore-point_YYYY-MM-DD_<sha>.zip.
- The flush-thrash mitigation (skip-to-latest instead of full-queue flush on
  saturation) was NOT needed for the 36-core machine at the 300% default; if
  drops still appear on weaker machines, that worker-side change is the next
  lever (see gui_rf_preview_worker.c rf_discard_queue / rf_release_queued).
