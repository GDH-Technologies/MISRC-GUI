# Prompt log — Windows move/resize freeze fix (2026-10-20)

User input this session:

1. "Investigate and fix the Windows move/resize freeze issue." (backlog item 2,
   user-prioritised 2026-10-19.)
2. "thats fixed too commit and push" — real-GUI confirmation (move + resize
   render live during the drag).

## Investigation (hard data, vendored raylib 5.5 + GLFW source)

- raylib source located at `.deps/raylib` (shallow clone, tag 5.5; backend
  `src/platforms/rcore_desktop_glfw.c`, GLFW at `src/external/glfw/src/`).
- `EndDrawing` (src/rcore.c:887): `rlDrawRenderBatchActive` -> (custom-frame
  control block: `SwapScreenBuffer` + FPS wait + `PollInputEvents`) ->
  frameCounter. `PollInputEvents` (rcore_desktop_glfw.c:1126) resets input
  state + `glfwPollEvents()`; `SwapScreenBuffer` = `glfwSwapBuffers` (public).
- GLFW win32_window.c: WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE only toggle the
  `frameAction` cursor state — GLFW does NOT pump or render during the modal
  loop; there is no WM_TIMER workaround in GLFW.
- `WindowSizeCallback` (rcore_desktop_glfw.c:1712) runs `SetupViewport` +
  updates `CORE.Window.screen` + `resizedLastFrame` — dispatched inside the
  modal loop (messages ARE dispatched there), so the viewport/size stay live
  for timer frames.
- raylib runs `timeBeginPeriod(1)` (ClosePlatform pairs `timeEndPeriod(1)`),
  so a 15 ms SetTimer really fires ~15 ms.
- Modal entry point: the caption/edge mouse-down is dispatched from inside
  the per-frame `PollInputEvents` — i.e. AFTER the current frame flushed
  (`rlDrawRenderBatchActive`) and swapped — so nested timer frames start
  from an EMPTY render batch; the GL context stays current on the render
  thread for the whole loop.

## Root cause

The Windows modal move/size loop inside `DefWindowProc`
(WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE) pumps messages without returning to the
app: raylib's per-frame event drain blocks inside `DispatchMessage` for the
whole drag; no frames are swapped (frozen window; the panels' data threads
keep running but nothing renders them; the audio monitor stutters).
Linux/macOS have no modal loop, hence unaffected.

## Fix (plan id 8baf4014-1ce7-432d-b925-3ce1de2009e0)

- NEW `misrc_tools/misrc_gui/core/gui_win32_modal.c` + `.h`: subclass the
  GLFW window proc (`SetWindowLongPtrW`/`GWLP_WNDPROC` on
  `GetWindowHandle()`), `SetTimer(15 ms)` on WM_ENTERSIZEMOVE, call the
  registered frame callback on WM_TIMER (render-only), `KillTimer` +
  live-frame log on WM_EXITSIZEMOVE, `CallWindowProcW` passthrough,
  shutdown restores the proc. Non-Windows platforms: inline no-op stubs in
  the header (install returns true).
- `misrc_gui.c`: `rlgl.h` + `gui_win32_modal.h` includes; the Clay layout
  build extracted into `app_build_render_commands` (shared by the main loop
  and the modal frame); `app_modal_frame` = `gui_audio_update_playback` +
  fresh `Clay_SetLayoutDimensions` + the shared layout + `BeginDrawing` +
  `ClearBackground` + `Clay_Raylib_Render` + `rlDrawRenderBatchActive` +
  `SwapScreenBuffer` (EndDrawing's core WITHOUT event polling or the FPS
  wait — a nested PeekMessage inside DefWindowProc's loop is reentrancy);
  install + register before the main loop; shutdown after it.
- `meson.build`: the TU only on windows/cygwin (the
  simple_capture_mediafoundation.c source-list pattern).
- `ci_guard_tests.py`: new `Windows modal move/size render contract`
  (module messages/timer/passthrough/handle; the module itself must never
  mention the event-poll functions; misrc_gui wiring; the modal frame body
  MUST contain the render core and MUST NOT contain the normal frame tail
  or event polling; the header stubs; the meson source) — 42 checks total.

## Validation (commands + results)

1. `python misrc_tools/test/ci_guard_tests.py --static-only`:
   - iteration 1: the new contract FAILED on my own module doc (it named the
     event-pump function in a comment; the guard is deliberately blunt) —
     comment reworded;
   - final: 42/42 PASS.
2. `pwsh -File scripts/build-local.ps1`:
   - iteration 1: gui_win32_modal.c compile errors — the raylib/windows.h
     macro clash (Rectangle/CloseWindow/ShowCursor/LoadImage/DrawText:
     windows.h was included BEFORE raylib.h) + `GWL_WNDPROC` undeclared on
     64-bit (it is `GWLP_WNDPROC`). Fixed: raylib.h FIRST, then windows.h
     behind WIN32_LEAN_AND_MEAN + NOMINMAX + NOGDI + the repo's
     CloseWindow/ShowCursor shadow defines; GWLP_WNDPROC both sites.
   - iteration 2 (final): guard gate + compile + link + `--smoke-test` OK;
     `meson test -C build-local` 34/34, 0 failures.
3. Real GUI (user): "thats fixed too" — move + resize render live during
   the drag.

## Post-confirmation artifacts (per the user's rules)

- Dev note: `misrc_tools/misrc_gui/dev/windows_modal_resize_render.md`
  (root cause + the verified-safety facts + 5 contracts + scope limits) +
  `dev_notes_README.md` entry.
- Backlog: item 2 marked DONE in `cvbs_decoder_realtime_sync_rules.md`
  (1 and 2 done; 3 needs the user's raw excerpts; 4 SECAM remains).
- Restore point: `C:\Users\Harry\MISRC-GUI-restore-points\
  win32-modal-resize-2026-10-20\` (zip of every changed file + the note).
- This log. Commit + push; the push itself triggers CI (the main-branch
  push trigger) — the pushed run must be green before close.
