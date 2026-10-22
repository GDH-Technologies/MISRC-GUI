# DEV NOTE: Windows modal move/size loop render driver (the move/resize freeze fix)

Status: fixed + user-confirmed (2026-10-20, real-GUI: window keeps rendering
during move/resize). Read this before touching `gui_win32_modal.c/.h`, the
`app_modal_frame`/`app_build_render_commands` helpers in misrc_gui.c, or the
window-proc/timer wiring.

Guard: `ci_guard_tests.py` `Windows modal move/size render contract`.

## Root cause (verified against the vendored raylib 5.5 + GLFW source)

On Windows a title-bar drag or edge resize enters a MODAL LOOP inside
`DefWindowProc` (WM_ENTERSIZEMOVE .. WM_EXITSIZEMOVE): the message pump runs
inside DefWindowProc and never returns to the app for the whole drag. raylib
drains the OS event queue at the tail of every frame (`EndDrawing` ->
`PollInputEvents` -> `glfwPollEvents` -> `DispatchMessage`); the caption
mouse-down is dispatched from inside that drain, so the render loop blocks
for the entire drag: no frames are swapped, the panels freeze (their data
threads keep running) and the audio monitor stutters. GLFW itself does
nothing here (its WM_ENTERSIZEMOVE handling only toggles cursor state);
Linux/macOS have no modal loop, hence unaffected.

Facts that make the fix safe (all verified in the raylib/GLFW source, not
assumed):

- Modal entry is ALWAYS at the event-drain dispatch stage — AFTER the
  current frame flushed its render batch (`rlDrawRenderBatchActive`) and
  swapped — so the nested timer frames start from an empty batch.
- The GL context stays current on this (render) thread for the whole modal
  loop: the WM_TIMER handler runs on the same thread that renders frames.
- raylib's `WindowSizeCallback` is dispatched inside the modal loop and
  already runs `SetupViewport` + updates `CORE.Window.screen`, so timer
  frames render at the live size during a resize.
- raylib runs `timeBeginPeriod(1)`, so the 15 ms timer actually fires ~15 ms
  (~60 Hz live frames).
- `BeginDrawing`'s time bookkeeping advances `CORE.Time.previous` in every
  nested frame, so the post-drag dt stays sane.

## The fix (contracts in force)

1. SUBCLASS + TIMER, NOTHING FANCIER. `gui_win32_modal.c` subclasses the
   GLFW window proc (`SetWindowLongPtrW` + `GWLP_WNDPROC` on
   `GetWindowHandle()`), starts an app-private 15 ms `SetTimer` on
   WM_ENTERSIZEMOVE, calls the registered frame callback on each WM_TIMER,
   kills the timer on WM_EXITSIZEMOVE, and passes every message to the
   original proc via `CallWindowProcW`. `gui_win32_modal_shutdown` restores
   the original proc before `CloseWindow`.
2. THE MODAL FRAME IS RENDER-ONLY. `app_modal_frame` (misrc_gui.c) = audio
   monitor pump (`gui_audio_update_playback`) + fresh `Clay_SetLayoutDimensions`
   + the SHARED layout build (`app_build_render_commands` — the same
   function the main loop uses) + `BeginDrawing` + `ClearBackground` +
   `Clay_Raylib_Render` + `rlDrawRenderBatchActive` + `SwapScreenBuffer`.
   It must NEVER poll events and must not run the normal frame tail: a
   nested PeekMessage inside DefWindowProc's modal loop is reentrancy, and
   the timer already paces (no FPS wait). The guard enforces this on the
   frame body AND on the module.
3. ONE LAYOUT, TWO DRIVERS. The Clay layout build lives in
   `app_build_render_commands` and is called by BOTH the main loop and the
   modal frame — never fork a second layout path.
4. WINDOWS-ONLY TU, PORTABLE HEADER. `gui_win32_modal.c` compiles only on
   windows/cygwin (the meson source-list pattern); `gui_win32_modal.h`
   provides inline no-op stubs elsewhere (install returns true — nothing is
   needed on platforms without a modal loop), so the call sites are
   unguarded.
5. windows.h INCLUDE ORDER. In the TU: `raylib.h` FIRST, then `windows.h`
   behind `WIN32_LEAN_AND_MEAN + NOMINMAX + NOGDI` and the
   `CloseWindow`/`ShowCursor` shadow defines (the repo's established
   pattern — wingdi's Rectangle/LoadImage/DrawText macros and winuser's
   CloseWindow/ShowCursor collide with raylib names). Use `GWLP_WNDPROC`
   (the pointer-sized index) with `SetWindowLongPtrW` — `GWL_WNDPROC` does
   not exist on 64-bit.

## Scope limits (accepted, recorded)

- Move + resize only (WM_ENTERSIZEMOVE covers both); WM_ENTERMENULOOP is
  not driven (menus freeze as before).
- Main-loop logic (stats text, input processing, interactions) pauses for
  the drag; input is intentionally frozen during it. The PANELS animate
  during the drag because their data threads keep running and the modal
  frame re-renders state.
- After the drag the blocked frame completes normally; the per-drag
  `[WIN32] modal move/size: rendered N live frames` line lands on stderr.

## Verification (2026-10-20)

1. `python misrc_tools/test/ci_guard_tests.py --static-only` — 42/42 PASS
   (incl. the new contract).
2. `pwsh -File scripts/build-local.ps1` — guard gate + compile + link +
   `--smoke-test` OK; `meson test -C build-local` — 34/34.
3. Real GUI (user): move + resize confirmed rendering live during the drag.
