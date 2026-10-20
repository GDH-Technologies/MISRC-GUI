# Prompt log — Playback drag-and-drop: per-channel zones + audio (2026-10-20)

User input this session (backlog item 1, user-prioritised 2026-10-19):

1. "continue continu with adding new features" (the four-item backlog; items 1+2
   tractable this session, 3 needs the user's raw captures, 4 is a decoder-sized
   follow-up — scoped this session to item 1).
2. "drag Drop should be for each panel channel so 2 zones, also load in audio
   if added, so this can fully play back a full capture set of any config."
3. "continue" (proceed through the plan).
4. "drag adn drop works" — real-GUI confirmation.

## Design (researched before coding; plan id 75489558-9915-47d1-b8a0-112dd83b8b52)

- Zones: the two channel panel rows (A top / B bottom); the drop point (logical,
  `gui_ui_get_mouse_position` — the only GetMousePosition call site) vs the
  cached `panel_config_a/b` bounds (union of left+right when split); nearest row
  vertically outside the canvases.
- Classification by content: sniffer + resolved rates. Audio-rate (<= 400 kHz
  true rate; WAV 1-8 ch incl. 46.875 kHz clockgen; native FLAC) -> the monitor
  source; RF containers -> the zone channel. First file per slot wins.
- Action: assign only slots that received a file, save settings, select the
  Playback device, stop anything running, (re)start via gui_app_start_capture.
- Audio feed: position-driven (playhead seconds x audio rate) on the playback
  thread; private WAV reader (parse_wav out_channels extension) or private FLAC
  decoder + shared IO layer (no STREAMINFO patch) + seek_absolute per span
  (contiguous skips the seek); linear resample to 48 k stereo into the shared
  play_q (gui_audio_push_playback); jump detection (50 ms back / 4 s fwd)
  flushes (gui_audio_reset_playback_queue) + re-anchors; speed folds into the
  step (48 k frames/s at every speed, tape pitch semantics); Max = silent.
- Persistence: settings.playback_audio_file + save/load keys + Settings row
  (path tail + X clear).
- Guard: `playback drag-and-drop contract` in ci_guard_tests.py.

## Implementation (files)

- misrc_gui/input/gui_playback.c: the drag-and-drop section (~3846-4390) —
  audio source (typedef, FLAC cbs, open/destroy, wav/flac span decoders,
  push_span, feed), zone mapping, classifier, public
  gui_playback_handle_dropped_files; ctx member `audio`; parse_wav
  out_channels param; prepare opens the audio source (non-fatal);
  release destroys; the playback thread feeds after each paced batch.
- misrc_gui/input/gui_playback.h: the public drop-handler declaration
  (float x/y — keeps the header raylib-free).
- misrc_gui/output/gui_audio.c/.h: gui_audio_push_playback +
  gui_audio_reset_playback_queue (same play_q as the demod feed).
- misrc_gui/core/gui_app.h: settings.playback_audio_file.
- misrc_gui/core/gui_settings.c: save (playback_file_b comma flip; the new
  key is the last line) + load.
- misrc_gui/core/misrc_gui.c: the IsFileDropped poll beside the per-frame
  input snapshot (wrapper pointer).
- misrc_gui/ui/gui_ui.c: the Audio settings row + clear handler + the
  section header hint.
- misrc_tools/meson.build + test/gui_playback_audio_stub.c: the
  gui_playback harnesses (resolve/watchdog/seek/start/reconnect) compile
  gui_playback.c as their own TU without gui_audio.c (raylib AudioStream) —
  the stub TU satisfies the monitor-queue pair.
- test/ci_guard_tests.py: the contract + registration (41 checks).

## Validation (commands + results)

1. python misrc_tools/test/ci_guard_tests.py --static-only — 41/41 PASS
   (incl. the new `playback drag-and-drop contract`).
2. LIBFLAC_ENABLED=0 translation proof for gui_playback.c (exact
   compile_commands.json flags, the define flipped): EXIT 0 (only the
   pre-existing mingw header warnings). Honest note: this proof compiles the
   DEP-LESS configuration only — everything inside the LIBFLAC guard is
   absent there, so the two =1-only compile breaks below were invisible to
   it; the real build (step 3, iteration 1) caught them. A macro-off proof
   never replaces a real compile of the enabled config.
3. Build iterations (pwsh -File scripts/build-local.ps1; meson test):
   - iteration 1: gui_playback.c compile errors — 'channels' undeclared
     (parse_wav tail used the fmt-block-scoped local; fixed with a
     function-scoped wav_channels) + implicit gui_audio declarations
     (gui_playback.c never included ../output/gui_audio.h; include added).
   - iteration 2: main GUI built + smoke OK; the gui_rf_playback_start_test
     link failed on the new gui_audio symbols — the five gui_playback
     harnesses compile gui_playback.c as their own TU without gui_audio.c;
     fixed with test/gui_playback_audio_stub.c linked into the five targets.
   - iteration 3 (final): guards PASS, build + smoke OK,
     meson test 34/34, 0 failures.
4. Real GUI (user): "drag adn drop works" — confirmed.

## Post-confirmation artifacts (per the user's rules)

- Dev note: misrc_tools/misrc_gui/dev/playback_drag_drop_audio.md (contracts
  + known limitations + verification) + dev_notes_README.md entry.
- Backlog: item 1 marked DONE in cvbs_decoder_realtime_sync_rules.md
  (items 2-4 remain: Windows move/resize freeze; real-media sync robustness
  — needs the user's raw excerpts; separate SECAM CVBS mode).
- Restore point: C:\Users\Harry\MISRC-GUI-restore-points\
  playback-drag-drop-2026-10-20\ (zip of every changed file + the note).
- This log. Commit + push; the push itself triggers CI (the main-branch push
  trigger added 2026-10-19) — the pushed run must be green before close.
