# DankC — Task Backlog (autonomous build)

Goal: make DankC's bar (then the whole shell) look and behave **exactly like DankMaterialShell**.
Reference image: `docs/dms_reference.png` (DMS bar, green theme). Reference code:
`/home/mosman092/Downloads/DankMaterialShell-master/quickshell`. No fakery — reproduce the real thing.

Each task: implement in clean C (CONVENTIONS.md) → `make` (fix all warnings) → run → screenshot →
compare to DMS → fix visual gaps → commit → check the box here → commit the check.

## Working rules
- Bundle assets under `assets/`. Vendor single-file libs under `third_party/`.
- Read colors/sizes/behaviour from DMS source, not guesses (docs/10-DESIGN-SYSTEM.md has the tokens).
- Keep it light (check `scripts/dev.sh rss`). Commit every completed task. Leave the session clean
  (`pkill -9 dankc`) at the end of each iteration.
- If blocked, write why under the task and move on.

## Done
- [x] M0 event loop + logger
- [x] M1 layer-shell bar + EGL
- [x] M2 nanovg text + live clock
- [x] M2 niri workspaces (per-output)
- [x] HiDPI (fractional-scale + viewport)
- [x] focused-window widget
- [x] battery widget (sysfs)
- [x] green theme palette (matches DMS)

## Next — bar fidelity (do in order)
- [x] **T1 Material Symbols icon font.** Loaded a second nanovg font "icons" + `dc_render_icon()` +
  `src/render/icons.h`. NOTE: the 14MB Material Symbols *variable* font segfaults stb_truetype (nanovg),
  so we bundled the *static* Material Icons Round (legacy codepoints match the common icons). For full
  variable-font + FILL/wght-axis fidelity later, render icons via the FreeType glyph path (docs/02 §5).
- [x] **T2 Exact bar background.** Verified: DMS bar bg is surfaceContainer #1d211b, OPAQUE (user's bar
  transparency = 1) — already exactly what DankC renders. Also fixed: DMS workspaces are a green focused
  PILL + grey dots (not numbered squares), sorted by idx — reworked to match.
- [x] **T3 Widget pill containers.** NOT NEEDED — comparing cropped bar strips shows DMS's bar is FLAT
  (widgets sit directly on #1d211b, no per-widget pill backgrounds). Skipped by design.
- [x] **T4 Clock + date.** Center shows "HH:MM  Www D" (24h time + date), centered as a group, matching
  DMS. Weather segment deferred to the weather/geolocation service. TODO: honour 12h when !use24HourClock.
- [x] **T5 Focused app: icon + name.** Shows the app ICON (PNG + SVG via nanosvg) + "AppName · Title" like
  DMS. Added the far-left apps-grid launcher icon too. Left section now fully mirrors DMS. Minor: some
  titles carry a stray box-drawing glyph — strip later.
- [x] **T6 App-icon resolution (XDG icon theme).** src/services/icons.c resolves app_id -> .desktop Icon=
  -> PNG file (icon-theme dirs + hicolor + pixmaps); bar decodes via nanovg/stb_image + caches per app.
  PNG works; SVG-only icons (Alacritty etc.) need nanosvg — FOLLOW-UP: vendor nanosvg for SVG icons.
- [x] **T7 Right status cluster (icons).** Matches DMS exactly: signal, clipboard, notification, battery+%,
  wifi, bluetooth, volume — same order + colours. State static until M3 services drive it (notification
  red-dot, real wifi/bt/volume state). Verified vs a 2x crop of the DMS reference.
- [x] **T8 CPU + RAM widgets** — NOT NEEDED. The user's DMS bar right side is icon-only (no CPU/RAM text
  widgets), confirmed from the reference crop. Skipped to stay faithful.
- [ ] **T9 Animation engine.** src/render/anim.c: tween + easing + the DMS duration table
  (docs/02-RENDERING §8). Animate workspace focus + widget hover.

## Then — services (M3, sd-bus; libsystemd present)
- [x] **T10 sd-bus scaffolding** (src/services/dbus.c): system+user bus opened + driven from the poll loop
  (process-on-readable + drain/flush prepare; loop now supports multiple prepare hooks). Stable. TODO: a
  shared generic PropertiesChanged router helper (each consumer adds matches for now).
- [~] **T12 audio** — live volume/mute via `wpctl` stand-in (proper libpipewire later).
- [~] **T13 wifi** — live connected state via sysfs stand-in (proper NetworkManager sd-bus later).
- [ ] **T11 UPower battery** (D-Bus; sysfs already works), **T14 BlueZ** (bluetooth on/connected ->
  colour the bar icon), **T15 MPRIS** media, **T16 StatusNotifier tray**, **T17 logind**
  (brightness/power/idle).

## Then — panels (M4+)
- [ ] **T18 Control Center popout**, **T19 OSDs** (volume/brightness), **T20 Notifications daemon**,
  **T21 App launcher**, **T22 Lock screen**, **T23 clipboard/screenshot/color-picker/night-mode**.

## Later
- [ ] **T24 Material color engine** (C++ MCU, colors from wallpaper — matches DMS when matugen active).
- [ ] **T25 dankctl IPC + niri keybind generation.**  **T26 settings.json config engine (cJSON).**
  **T27 Settings UI.**  **T28 meson build.**  **T29 plugins (.so ABI).**  **T30 packaging.**
