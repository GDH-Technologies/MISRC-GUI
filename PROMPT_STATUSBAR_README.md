# PROMPT README — Status bar right-side collapse/clipping fix (session log)

## Input (user request)
Fix the one outstanding issue: dynamic auto dead/empty-space use when the window scales, and prevent the right side of the bottom status bar from collapsing/being clipped off the window. Do NOT alter counter visibility or formatting (full stats readouts stay exactly as-is).

## Diagnosis (verified from source, not assumed)
- `render_status_bar` in `misrc_tools/misrc_gui/ui/gui_ui.c` budgeted each right-side value at its small `CLAY_SIZING_FIXED` width (e.g. Frames 32px, SampleRate 80px), but the actual drawn text is wider (e.g. "123K" ~55px, "192.0 MSPS" ~108px, "26.60M" ~75px, "100%" ~43px).
- Clay (verified in `ui/clay.h` `Clay__CloseElement`, lines ~1895-1903) does not clip text to containers: text children overflow a `FIXED` parent and draw past it. Result: the drawn right edge ran ~100px past the accounted width.
- Consequence: the 192-pass budget loop believed everything fit (compaction appeared "ignored"), and the rightmost readouts drew off the window edge — the reported right-side collapse/clipping.
- Confirmed Clay `CLAY_SIZING_FIT(min)` clamps content width to at least `min` with unbounded max (`clay.h:1896-1899`), so FIT(min) containers grow with text and never clip it.

## Changes (gui_ui.c only, render_status_bar + helpers)
1. Right-side readout strings (samples, frames, missed, errors, RF %, Aud %) are now formatted once before the budget loop (previously formatted inline at draw time).
2. Real rendered text width of every right-side value measured (font 1) and accounted via effective value width = max(stable min, text width) in every group-width computation.
3. All seven value containers changed `CLAY_SIZING_FIXED(w)` -> `CLAY_SIZING_FIT(w)`: stable minimum widths preserved (compaction still works), but containers now grow to their text so nothing bleeds/overlaps or draws off-window.
4. Compaction chain skips shrinking a minimum below the current text width (no-op steps), so passes are never wasted and compaction visibly engages when real width pressure exists.
5. Sync value budget now uses max(measured "OK", "--") widths (sync renders either).
- Counter strings, visibility, colors, and formatting are unchanged (no counter alterations).

## Commands run
- `bash scripts/build-local.sh` -> OK: build + smoke test passed (only pre-existing warnings elsewhere).
- `python3 misrc_tools/test/ci_guard_tests.py` -> all 34 guards PASS (incl. UI scale policy runtime).
- `meson test` (build-local) -> 3/3 OK (ddd_protocol, gui_ddd_async_policy, gui_ddd_async_fault).

## Status
CONFIRMED FIXED by user (real-world GUI check, 2026-09-02): "Right side issue is fixed!" — right-side collapse/clipping of the bottom status bar is resolved; counter readouts unchanged.

## Follow-up (2026-09-02, same session)
User confirmed right-side fix ("Right side issue is fixed!") then reported one remaining item: the toolbar Audio Mon button must fall back to "Mon" when small.
- Diagnosis: the `GUI_UI_TOOLBAR_TIER_ULTRA_NARROW` profile sets `audio_mon_width = 56` but leaves `very_narrow = false`, so the long "Audio Mon" text still rendered in the 56px button (Clay does not clip text -> overflow).
- Fix (`gui_ui.c:4599`): audio mon label condition is now `(toolbar_very_narrow || toolbar_ultra_narrow) ? "Mon" : "Audio Mon"` — ultra-narrow and tiny tiers show "Mon"; full/narrow/very-narrow tiers unchanged. No other behavior altered.
- Validations: `bash scripts/build-local.sh` -> OK build + smoke test (pre-existing warnings only); `python3 misrc_tools/test/ci_guard_tests.py` -> all 34 PASS.
- Pending: user GUI confirmation at small window widths that the button reads "Mon" and nothing else changed.

## Follow-up 2 (2026-09-02, same session): toolbar dead-space-first scaling
User confirmed Audio Mon fallback, then reported the top bar is not dynamic enough: it is not using the dead space in the middle when scaled smaller.
- Diagnosis: tier selection required each tier's device dropdown at its natural (min-clamped) width, so a tier was rejected while it could still fit with the device name ellipsized harder; the compact tier then rendered with a dead gap in the middle (ToolbarSpacer2) — premature compaction + wasted space.
- Fix (`gui_ui.c` `render_toolbar`):
  1. Tier fit check now uses the dropdown's `dd_min_width` floor (the row's only compressible element) — a tier stays active until the row cannot fit even with the dropdown fully shortened.
  2. After choosing a tier, the leftover slack (`toolbar_width - chosen_required_width`) is handed to the device dropdown first (grow from dd_min toward natural, capped at dd_max), then the label is ellipsized to the final width; remaining slack stays in the middle spacer. Device name un-truncates dynamically as the window widens.
  3. Removed the now-unused per-tier dd measurement from the selection loop; two-row fallback threshold uses the floor-based tiny requirement.
- Validations: `bash scripts/build-local.sh` -> OK build + smoke test (pre-existing warnings only); `python3 misrc_tools/test/ci_guard_tests.py` -> all 34 PASS; `meson test -C build-local` -> 3/3 OK.
- Pending: user GUI confirmation that the toolbar uses the middle dead space while shrinking (dropdown compresses before labels/meters compact) and nothing clips or bounces.

## Follow-up 3 (2026-09-02, same session): Audio Mon double-stack fix
User reported Audio Mon does not fall back to "Mon" — it double-stacks "Audio"/"Mon" top/bottom inside the same box.
- Diagnosis: "Audio Mon" at FONT_SIZE_NORMAL (18px) is wider than the 90px FULL/NARROW button, so Clay wraps it into two lines (default wrap mode) instead of falling back. The dead-space-first tier selection keeps the roomier tiers active at narrower widths, exposing this wrap; the tier-flag fallback condition was the wrong trigger.
- Fix (`gui_ui.c:4599-4607`): the label falls back to "Mon" whenever `gui_ui_measure_text_width(app, "Audio Mon", toolbar_text_size, 0) > audio_mon_width - 4` — measured fit at every width — with the very-narrow/ultra-narrow tier flags kept as a short-circuit. Single line guaranteed; no wrap possible.
- Validations: `bash scripts/build-local.sh` -> OK build + smoke test; `python3 misrc_tools/test/ci_guard_tests.py` -> all 34 PASS; `meson test -C build-local` -> 3/3 OK.
- Pending: user GUI confirmation that the button shows a single-line "Audio Mon" or "Mon" at every width (never stacked).

## Follow-up 4 (2026-09-02): committed a552e7e, then status bar readout hysteresis
User said "good commit and push" — committed `a552e7e` (status bar clipping + toolbar dead-space-first + Audio Mon measured fit, 5 files, incl. PROMPT_STATUSBAR_README.md) and pushed to `origin/capture-server-client-modes` (upstream set). The zip restore point stays untracked (binary artifact).
Then: make the bottom bar less twitchy for setting the small readouts.
- Diagnosis (from the budget loop in `render_status_bar`): (1) the three tiered readouts (sample rate, samples, free space) shortened sequentially, so one readout collapsed through all its tiers while the others stayed long; (2) the tier choice was recomputed from scratch every frame from live ticking data (samples/frames counters), so at a boundary width the small readouts flapped between full and short forms frame-to-frame.
- Fix (`gui_ui.c`):
  1. New persisted static `s_status_readout_level` (0 full .. 2 shortest) — the budget loop now seeds all three readout tiers from it each frame.
  2. Level-based shortening: the contraction chain raises the single level (all readouts shorten in lockstep) instead of collapsing one readout at a time.
  3. Hysteresis: pressure raises the level immediately (kept for next frame); it relaxes at most one step per frame and only when the bar fit on the first pass with >= 48px slack — a dead band wide enough that ticking counters cannot flap the tiers.
- Validations: `bash scripts/build-local.sh` -> OK build + smoke test; `python3 misrc_tools/test/ci_guard_tests.py` -> all 34 PASS; `meson test -C build-local` -> 3/3 OK.
- User feedback: "It triggers way too soon on when width is scaled I want it to maintain all counters when space is possible."
- Follow-up fix (same round):
  1. Premature trigger: the fit test reserved the full `min_message_width` (96px) for the status message even when the message is short ("Ready" ~35px), forcing readouts to shorten while dead space remained. The message now claims its natural rendered width (measured once, capped at the readable minimum) — counters stay full whenever the real space allows it (`status_message_natural_width` / `status_message_required` in `render_status_bar`).
  2. Flicker band in the relax path: relaxing the level seeded the longer lower-tier text next frame, which could then not fit, flipping the level back every two frames while scaling. The relax now requires first-pass fit with >= 48px slack PLUS the measured width step between the current and lower tier (from the tier width arrays), so the dead band always exceeds the tier step.
- Re-validated: build + smoke OK, all 34 guards PASS, meson tests 3/3 OK.
- User feedback: "Sync / Samples is looking good but the rest gets kicked down to small size far too fast when there is plenty of space to work with."
- Follow-up fix (same round): `status_compact_seed` (a hard width breakpoint) pre-seeded compact labels ("Frames:"->"F:", "Missed:"->"M:", "Audio Buffer:"->"Aud:"), tighter gaps, and smaller containers for every width below the compact breakpoint — even with dead space available. Sync/Samples looked fine only because their compact forms are mild.
  1. Removed the compact-seed/narrow pre-seeding: gaps, container minimums, free-space floor, and message minimum all start at their full values and are contracted by the budget loop only under measured pressure (tiny-width absolute floors kept).
  2. The compact-label flip is now persisted hysteresis state (`s_status_labels_compact`): earned when the loop flips under real pressure, relaxed only on first-pass fit with >= 24px slack beyond the labels' own measured width step; at most one state (labels or level) relaxes per frame so the pair cannot over-relax and flap back.
- Re-validated: build + smoke OK, all 34 guards PASS, meson tests 3/3 OK.
- Pending: user GUI confirmation that full labels/spacing hold until the bar genuinely runs out of space, then compact forms engage only as needed.

## Restore point
`statusbar-rightside-fix-2026-09-02.zip` (repo root) — go-back-to snapshot containing the changed files (`gui_ui.c`, `gui_ui_scale.h`, `gui_ui_scale_harness.c`, `dev_notes_README.md`), full git diff, and this log. NOTE: snapshot predates Follow-ups 2-4 (toolbar dead-space, Audio Mon measured fit, readout hysteresis); refresh the zip after user confirmation.

## Rollback
Options, newest first:
1. Unzip `statusbar-rightside-fix-2026-09-02.zip` over the repo root to restore the confirmed-fixed state.
2. `git diff` in /home/harry/MISRC-GUI shows this session's edits; `git checkout -- misrc_tools/misrc_gui/ui/gui_ui.c` reverts to pre-fix.
