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
- [ ] **T2 Exact bar background.** Sample DMS's bar pixel colour (compare screenshots) and match the
  bar's background + transparency (DMS uses surfaceContainer w/ panelTransparency 0.85). Verify.
- [ ] **T3 Widget pill containers.** DMS groups bar widgets in rounded surfaceContainer pills with
  padding (docs/10). Wrap clock, battery, etc. in pill backgrounds matching DMS shape/radius (cornerRadius
  12) and spacing (4/8/12).
- [ ] **T4 Clock + date + format.** Match DMS: time (respect `use24HourClock`), plus date "Wed 1" segment;
  read format from settings.json if present. Center section.
- [ ] **T5 Focused app: icon + name.** Show the app icon (needs T6) + app name/title like DMS
  ("Alacritty · <title>"). Until icons: app_id + title, styled like DMS.
- [ ] **T6 App-icon resolution (XDG icon theme).** New src/services/icons.c: resolve a .desktop/app_id
  or tray IconName to a file via the icon theme (index.theme lookup), decode with stb_image/nanosvg,
  cache as an nvg image. Used by launcher/dock/tray/focused-app.
- [ ] **T7 Right status cluster (icons).** Draw the DMS right-side icons using Material Symbols: network/
  wifi, bluetooth, volume, notifications, battery(icon form). State can be static/placeholder until the
  sd-bus services land (T10+); shape/placement must match DMS now.
- [ ] **T8 CPU + RAM widgets** from /proc (src/services/sysmon.c) — like DMS cpuUsage/memUsage.
- [ ] **T9 Animation engine.** src/render/anim.c: tween + easing + the DMS duration table
  (docs/02-RENDERING §8). Animate workspace focus + widget hover.

## Then — services (M3, sd-bus; libsystemd present)
- [ ] **T10 sd-bus scaffolding** (src/services/dbus.c): system+user bus, generic PropertiesChanged router.
- [ ] **T11 UPower battery** (replace sysfs), **T12 PipeWire audio** (volume/mute), **T13 NetworkManager**
  (wifi state/signal), **T14 BlueZ** (state), **T15 MPRIS** media, **T16 StatusNotifier tray**,
  **T17 logind** (brightness/power/idle).

## Then — panels (M4+)
- [ ] **T18 Control Center popout**, **T19 OSDs** (volume/brightness), **T20 Notifications daemon**,
  **T21 App launcher**, **T22 Lock screen**, **T23 clipboard/screenshot/color-picker/night-mode**.

## Later
- [ ] **T24 Material color engine** (C++ MCU, colors from wallpaper — matches DMS when matugen active).
- [ ] **T25 dankctl IPC + niri keybind generation.**  **T26 settings.json config engine (cJSON).**
  **T27 Settings UI.**  **T28 meson build.**  **T29 plugins (.so ABI).**  **T30 packaging.**
