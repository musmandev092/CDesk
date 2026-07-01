# DankC — Project State (read this first)

This file is the canonical, always-current index of the project so a fresh session or subagent does
**not** need to re-scan the whole tree. Update it whenever state changes.

## What this is
A lightweight desktop shell for the **niri** Wayland compositor, written in C — a reimplementation of
DankMaterialShell's core (QML/Quickshell + Go) as one native binary. Full spec in `docs/` (00–11).
niri-only, ~99% C (one C++ file for Material colors), plugins deferred.

## Current status — Milestone 2 in progress 🔨 (clock + workspaces working)
- **Builds clean** (gcc + wayland-scanner, `make` → `./bin/dankc`, ~1.2 MB with nanovg, zero warnings).
- **Runs on niri**: layer-shell bar per output rendering via **nanovg** — themed surfaceContainer
  background + centered **live clock** (HH:MM, timerfd) + left-aligned **workspace pills** from the
  **niri IPC EventStream** (per-output filtered, focused=primary/urgent=error). Verified live on a
  dual-monitor setup (external 2560@1x "P24q-10" + internal HiDPI@2x "0x0599").
- **Footprint:** Pss ≈ **30 MB** for two GPU bars, vs DMS `qs` Pss ≈ 477 MB.
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
- Event loop uses `wl_display_dispatch` on readable; migrate to `prepare_read`/`read_events` when adding
  D-Bus/pipewire fds (docs/01-ARCHITECTURE §1).
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
