# DankC — Project State (read this first)

This file is the canonical, always-current index of the project so a fresh session or subagent does
**not** need to re-scan the whole tree. Update it whenever state changes.

## Autonomous build
Running an overnight self-paced loop that works through **docs/TASKS.md** (ordered backlog toward exact
DankMaterialShell fidelity). Each iteration: implement one task → build → run → screenshot → compare to
`docs/dms_reference.png` → fix → commit → tick the box → leave session clean. Reference code:
`/home/mosman092/Downloads/DankMaterialShell-master/quickshell`.

## What this is
A lightweight desktop shell for the **niri** Wayland compositor, written in C — a reimplementation of
DankMaterialShell's core (QML/Quickshell + Go) as one native binary. Full spec in `docs/` (00–11).
niri-only, ~99% C (one C++ file for Material colors), plugins deferred.

## Current status — M2/M3/M4 in progress 🔨 (bar live, control center + OSD done)
- **Builds clean** (gcc + wayland-scanner, `make` → `./bin/dankc`, zero warnings).
- **Runs on niri**: layer-shell bar per output via **nanovg**, closely matching DMS. LEFT: apps-grid
  launcher · workspace green-pill + grey dots (sorted) · app icon (PNG+SVG) + "AppName · Title". CENTER:
  time + date (**live, 1 Hz**). RIGHT: DMS status cluster with real state — battery% (sysfs), wifi (sysfs),
  audio (wpctl), bluetooth (BlueZ). DMS **green palette** (#1d211b bg). Crisp on HiDPI (fractional-scale).
- **Control Center** popout (T18): themed card, 2×2 toggle tiles + volume/brightness sliders, interactive
  (rfkill/wpctl/brightnessctl), dismiss on outside click.
- **Volume OSD** (T19): transient bottom-center overlay, auto-hide, pops on volume change.
- **Notifications** (T20): full org.freedesktop.Notifications server + top-right toast stack (max 4
  cards, avatar/app/summary/body, critical accent, click-to-dismiss). Declines the name if a daemon
  already owns it. History/center panel + action buttons deferred.
- **App launcher** (T21): centered spotlight overlay with keyboard focus (xkb), fuzzy desktop-entry
  search + icons, type/arrows/enter/click to launch. Opens from the bar launcher button.
- **Config + themes** (T26): ~/.config/dankc/config.json (cJSON) — theme/clock24h/showDate/animation
  prefs, DMS defaults. All 10 DMS DARK stock themes selectable via dc_theme_set (stock_themes.inc,
  generated). Bar clock honours 12/24h + showDate.
- **Animation engine** (T9): core/anim.c — DMS durations + easing (incl. expressive overshoot),
  config-scaled. Fade+scale entrance on launcher + control center via frame callbacks (self-terminating).
- **FIXED the frozen clock**: it was a loop-wide deadlock — blocking `wl_display_dispatch()` raced Mesa's
  gallium threads for the display fd. Now uses thread-safe prepare_read/read_events + a wall-clock loop
  tick. See memory `dankc-wayland-dispatch-deadlock`.
- **Footprint:** RSS ≈ **145 MB** for two GPU bars incl. Mesa (Pss lower), vs DMS `qs` ≈ 477 MB.
- **Next:** T23 clipboard history (wlr-data-control, safe/testable), then apply anim to toasts/OSD +
  workspace pill. T22 lock screen DEFERRED: needs the user awake to test PAM auth — locking the live
  session with unverified code risks a lockout.
- Vendored: nanovg (GLES3, `third_party/nanovg`), cJSON (`third_party/cjson`); Inter bundled
  (`assets/fonts/InterVariable.ttf`).

## Milestones (see docs/06-ROADMAP.md)
- M0 core, M1 hello bar — done. **M2 in progress:** nanovg text ✅, live clock ✅, niri workspaces ✅.
  Remaining M2: more bar widgets (focused window, battery/media/tray placeholders), the animation engine
  (DMS duration table), the Material color engine (C++ MCU), and proper HiDPI. Then M3 services (sd-bus:
  audio/network/battery/…), M4 panels, M5 lock/clipboard/screenshot, M6 theming/settings, M7 plugins,
  M8 packaging.

## Known follow-ups (address in M2)
- **HiDPI:** bar currently renders buffer_scale=1 (blurry on the scale-2 internal panel). Implement
  fractional-scale / integer-scale handling (docs/02-RENDERING §3).
- ~~Event loop uses `wl_display_dispatch`~~ — DONE: migrated to `prepare_read`/`read_events` (was
  deadlocking against Mesa threads; see memory `dankc-wayland-dispatch-deadlock`).
- Config engine (cjson) not yet wired — bar color is hardcoded; add `src/core/config.c` when cjson is
  installed.

## Build & run
```sh
make                      # or scripts/dev.sh build   (fallback: gcc + wayland-scanner, no meson yet)
scripts/dev.sh bg         # build + run in background, logs to /tmp/dankc.log
scripts/dev.sh shot /tmp  # screenshot all outputs via `dms screenshot` (grim not installed)
scripts/dev.sh rss        # proportional memory (Pss)
scripts/dev.sh stop       # kill it
```
Safe alongside DankMaterialShell: run dankc, look, `pkill dankc` to restore. No logout needed.

## Missing host tools (install once — see scripts/install-deps.sh)
`meson ninja wayland-protocols cjson grim` (+ later: pam, libsystemd already present). Everything else
(gcc, wayland-scanner, wayland/egl/gles/xkbcommon/freetype/fontconfig/harfbuzz/pango/pipewire/sqlite/curl)
is already on the machine.

## Layout
`docs/` spec · `protocol/` vendored XML (generated code in `protocol/generated/`, gitignored) ·
`src/{core,wayland,render,ui,services,theme,niri,ipc}/` · `dankctl/` CLI · `scripts/` helpers.
Coding standards: `CONVENTIONS.md`. Build: `Makefile` (fallback) + `meson.build` (primary, TODO).

## Git
Offline repo on `main`. Commits are small and conventional (`feat(bar): …`). Push is deferred (user will
push in the morning). Do not commit generated protocol code or `bin/`.
