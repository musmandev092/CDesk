# dankc Notepad — Implementation Plan (2026-07-04)

Group-1 feature ported from DMS. Crux: dankc has NO multi-line text editor — must build one.

## Architecture (new files)
- `src/ui/text_edit.c/.h` — reusable multi-line editor widget: UTF-8 buffer (contiguous, memmove
  insert/delete, 1 MiB cap), visual-row layout cache (via nvgTextBreakLines), cursor (byte offset),
  scroll, dirty+last_edit_ms. Draws into a caller rect; NO Wayland/surface code. Cursor↔(row,x)
  mapping via nvgTextGlyphPositions (currently unused primitive). RTL display via dc_shape_draw_text
  but cursor uses unshaped advances (v1 LTR-correct, documented).
- `src/services/notepad_storage.c/.h` — session meta (`~/.local/state/dankc/notepad-session.json`:
  tabs[]+currentTabIndex) + per-tab `.txt` under notepad-files/ (DMS-mirroring for later dms_import).
  Atomic tmp+rename. Model after history.c (paths) + config.c (cJSON).
- `src/ui/notepad.c/.h` — popout panel, structural CLONE of clip_picker.c (layer OVERLAY, ALIGN_END,
  480x600, EXCLUSIVE kbd, ns "dankc:notepad"): header + tab strip (128px chips) + embedded text_edit
  + footer status; 2000ms autosave debounce via main.c tick; flush on tab-switch/close/Ctrl+S/quit.

## Modified (serialized) files
- wl.c/h (T0: track key RELEASES + repeat_info timerfd + dc_wayland_ctrl_down/shift_down helpers —
  xkb_state is public; dc_key_cb signature UNCHANGED). main.c (T0/T4/T5/T6). launcher.c (T5). bar.c/h
  + render/icons.h (T6). meson.build (T1/T2/T3).

## Key facts found
- Keyboard: wl.c delivers (keysym,utf8) on PRESS only; releases dropped; NO key repeat anywhere; NO
  Ctrl-shortcut handling; NO IME/compose (shell-wide limit). main.c handle_key (~L333) dispatches by
  panel-visibility priority. Notepad inserts after clip_picker (~L345); being visible+exclusive it
  intercepts keys so Return=newline is safe.
- Text: nvgTextBreakLines (\n + wrap), nvgTextGlyphPositions (cursor/hit-test — unused today),
  dc_shape_draw_text (RTL display). fontstash measurement touches glyph atlas → EVERY notepad event
  handler must dc_egl_make_current before calling text_edit (precedent: cp_purge_thumbnails).
- Wiring: bar find_widget() ~L1687 + dc_bar_region enum; main.c ctl dispatch ~L407 + region-click ~L602;
  launcher builtin (calc-row precedent ~L113/565). meson.build ~L118-137 (add files).

## Task breakdown (one Sonnet agent each)
- **NT0 — key repeat + modifier helpers** (wl.c/h, main.c, loop fd): track releases, timerfd honoring
  repeat_info (default 25Hz/600ms, xkb_keymap_key_repeats gate), dc_wayland_ctrl_down/shift_down.
  Additive (held arrows repeat everywhere) — smallest diff. Do first (main.c quiet).
- **NT1 — text_edit.c/.h** (parallel): §2 design minus selection/undo. + tests/test_text_edit.c pure
  ops (UTF-8 boundary nav, insert/delete, buffer growth) per bin/test_calc precedent. meson.build.
- **NT2 — notepad_storage.c/.h** (parallel): §3. meson.build.
- **NT3 — notepad.c/.h** (after NT1+NT2): clone clip_picker lifecycle; header/tabstrip/editor/footer;
  handle_key routes Escape/Ctrl-S/N/W/Tab, forwards rest to text_edit w/ ctrl/shift from NT0.
- **NT4 — main.c wiring** (after NT3; serialized main.c): create/destroy(flush), handle_key chain,
  4 pointer chains (click/motion/scroll/leave) + outside-click dismiss, ctl "notepad [toggle]", tick.
- **NT5 — launcher builtin entry** (after NT4; launcher.c/h + main.c): generic builtin table
  {"Notepad",icon,action,keywords} + dc_launcher_set_builtin_cb → dc_notepad_toggle (calc-row style).
- **NT6 — bar widget** (after NT4; icons.h/bar.c/h/main.c + subset font): DC_ICON_EDIT_NOTE (edit_note;
  fallback DC_ICON_EDIT 0xf097 if subset regen unavailable), DC_BAR_REGION_NOTEPAD, "notepad" widget in
  find_widget() (clone clipboard pill), region click → toggle. Config: users add "notepad" to widget
  arrays (unknown ids already skipped → no schema change).
- **NT7 — selection + clipboard (v1.1)**: sel_anchor, Shift+arrows/Ctrl+A/drag, Ctrl+C/X (needs
  dc_clipboard_set_text via wlr-data-control OR wl-copy fallback), Ctrl+V newest entry.
- **NT8 — polish (later)**: undo stack, cursor blink, tab rename, drag-reorder, settings.c toggle,
  dms_import session, markdown preview.

## v1 CUT = NT0-NT6 (tabbed notepad: cursor edit, wrap, scroll, autosave, persist, launcher+ctl+bar).
NT7 (selection/copy-paste) is first follow-up (editor w/o copy feels broken). Markdown deferred.

## Risks: text_edit is the risk concentrator (isolate cursor math in 2 fns, pure unit tests);
EGL-current-before-layout rule; RTL cursor = unshaped (LTR-correct v1); key-repeat touches global input
(smallest additive diff); icon subset needs fonttools (fallback icon); atomic writes + flush-on-quit +
1MiB cap for data safety.
