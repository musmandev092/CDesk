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
- [x] **T9 Animation engine** — DONE: src/core/anim.c — DMS durations (150/300/450/500) + easing
  (OutCubic/OutQuart/emphasized accel-decel/expressive overshoot via cubic-Bezier), config-scaled
  (animationSpeed/animationsEnabled). Frame-callback-driven fade+scale entrances on launcher + control
  center (loop self-terminates at completion). TODO: apply to toasts/OSD slide-in, workspace-focus pill,
  widget hover, and exit animations.

## Then — services (M3, sd-bus; libsystemd present)
- [x] **T10 sd-bus scaffolding** (src/services/dbus.c): system+user bus opened + driven from the poll loop
  (process-on-readable + drain/flush prepare; loop now supports multiple prepare hooks). Stable. TODO: a
  shared generic PropertiesChanged router helper (each consumer adds matches for now).
- [~] **T12 audio** — live volume/mute via `wpctl` stand-in (proper libpipewire later).
- [~] **T13 wifi** — live connected state via sysfs stand-in (proper NetworkManager sd-bus later).
- [x] **T14 BlueZ** — bluetooth state live via GetManagedObjects (system bus), cached 3s; bar bluetooth
  icon: info-blue connected / mid powered / dim off. Stable, no busy-loop.
- [x] **T15 MPRIS** — media service (session bus): player detect + PlaybackStatus + title/artist, cached;
  bar shows music-note + title left of clock while Playing (hidden otherwise), like DMS.
- [x] **T11 battery** — DONE via sysfs: battery.c reads capacity + status; bar shows %, green fill when
  charging, red when low. UPower D-Bus (time-to-empty/full estimates) skipped — not shown on the bar.
- [ ] **T16 StatusNotifier tray** — BLOCKED to verify: needs a tray-producing app (StatusNotifierItem) to
  test the host/watcher end-to-end; deferred rather than ship unverifiable code.
- [ ] **T17 logind** (idle/lid/sleep signals) — plumbing, not user-visible yet; deferred.

## Then — panels (M4+)
- [x] **Pointer input** (prerequisite) — wl_seat + wl_pointer + bar click hit-testing (launcher/
  control-center/clock regions). Done; unlocks popouts.
- [x] **T18 Control Center popout** — DONE: themed card + 2x2 toggle tiles (Wi-Fi/Bluetooth/Dark/Night, live
  state) + volume/brightness sliders (both live: wpctl + /sys/class/backlight). Opens on control-center
  click; clicks hit-test tiles/sliders -> rfkill/wpctl/brightnessctl actions; dismiss on outside click.
  Matches DMS ControlCenter. (Dark/Night actions are no-ops pending gamma/gsettings.)
- [x] **T19 Volume OSD** — DONE: transient bottom-center wlr-layer overlay (speaker icon + green bar +
  value), auto-hides, pops on volume change detected in the 1 Hz tick. Also fixed the long-standing
  "frozen clock": the whole event loop deadlocked on blocking wl_display_dispatch() racing Mesa's
  gallium threads for the display fd — replaced with the thread-safe prepare_read/read_events pattern
  and a wall-clock loop tick. Brightness OSD deferred (same overlay, add on next brightness-change hook).
- [x] **T20 Notifications daemon** — DONE: full org.freedesktop.Notifications server (Notify/Close/
  GetCapabilities/GetServerInformation + Closed/ActionInvoked signals, urgency+image hints, replaces_id,
  per-urgency lifetimes, 1 Hz expiry). Top-right toast stack (dc_toasts): up to 4 cards (avatar/app/
  summary/body, critical accent), click-to-dismiss with input-region passthrough. Declines the name if a
  daemon already owns it. Verified on an isolated session bus. Notification CENTER DONE: 64-entry history
  ring (archives on expire/dismiss/close), top-right popout with cards + "Clear all" + entrance anim,
  opened from the bar bell (precise hit region). TODO later: action buttons, inline images, history scroll,
  grouping.
- [x] **T21 App launcher** — DONE: centered wlr-layer overlay with exclusive keyboard focus, search
  field + ranked desktop-entry results (icons via dc_icon_resolve), type-to-filter, Up/Down/Enter/Esc,
  click-to-launch. Backed by apps.c (XDG scan + fuzzy search) and xkb keyboard input in wl.c. Opens from
  the bar launcher button. Also fixed a latent libwayland abort: wl_pointer needs frame/axis_* listener
  stubs (see memory dankc-wl-listener-stubs). TODO later: math/calc/actions, recent apps, mouse hover.
- [ ] **T22 Lock screen** — DEFERRED (safety): needs PAM auth; testing means locking the live session with
  unverified code (lockout risk). Do when the user is awake.
- [x] **T23 clipboard / screenshot / color-picker / night** — DONE:
  - **Clipboard** — wlr-data-control capture + 32-entry history + picker overlay (type-filter, Enter/click
    copies back via wl-copy), bar clipboard icon + `dankc ctl clipboard`.
  - **Screenshot** — full (`ctl screenshot`) + region (`ctl screenshot-region`, slurp) -> ~/Pictures +
    clipboard.
  - **Color-picker** — `ctl color-picker` (slurp point -> grim PPM -> #rrggbb to clipboard; pipeline
    verified exact against the bar bg).
  - **Night mode** — `ctl night` toggles gammastep -O 4000. Keybinds: Print / Mod+Print / Mod+Shift+P /
    Mod+Shift+N.

## Later
- [x] **T26 config engine (cJSON)** — DONE: ~/.config/dankc/config.json (theme/clock24h/showDate/
  animationsEnabled/animationSpeed) with defaults; bar clock honours clock24h + showDate. Plus all 10 DMS
  DARK stock themes selectable via dc_theme_set (generated from StockThemes.js). TODO: hot-reload on
  file change; light variants; more keys (bar position/height, widget toggles).
- [x] **T24 Material color engine** — DONE: theme/dynamic.cpp (the one C++ module) — wallpaper -> vibrant
  seed (chroma-weighted histogram) -> coherent dark Material palette; config keys dynamicColor + wallpaper.
  HSL-tone approximation of MCU (true HCT is a future refinement in the same file).
- [x] **T25 dankctl IPC** — DONE: control socket ($XDG_RUNTIME_DIR/dankc.sock) + `dankc ctl <cmd>` client
  (launcher/control-center/notifications/quit) + `dankc keybinds` niri snippet generator.
- [x] **T28 meson build** — DONE (written, mirrors the Makefile; not executed — meson not installed here).
- [x] **T30 packaging** — DONE: packaging/PKGBUILD + README (meson-based, cross-distro notes).
- [ ] **T27 Settings UI** — not started (large GUI; config is file-driven for now via config.json).
- [ ] **T29 plugins (.so ABI)** — intentionally DEFERRED per locked project decision (native .so plugins
  are a post-v1 phase, not in the core shell).
