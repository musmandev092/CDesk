# DankC — Project State (read this first)

This file is the canonical, always-current index of the project so a fresh session or subagent does
**not** need to re-scan the whole tree. Update it whenever state changes.

## What this is
A lightweight desktop shell for the **niri** Wayland compositor, written in C — a reimplementation of
DankMaterialShell's core (QML/Quickshell + Go) as one native binary. Full spec in `docs/` (00–11).
niri-only, ~99% C (one C++ file for Material colors), plugins deferred.

## Current status — Milestone 1 complete ✅
- **Builds clean** (gcc + wayland-scanner, `make` → `./bin/dankc`, ~85 KB, zero warnings).
- **Runs on niri**: connects via Wayland, binds globals, brings up EGL/GLES3, places a top-anchored
  layer-shell bar (`dankc:bar`, 48px, exclusive zone) on every output, rendering the themed
  surfaceContainer background. Verified live on a dual-monitor setup (external 2560@1x + internal HiDPI
  @2x).
- **Footprint:** Pss ≈ **23 MB** (Private 8 MB) for two GPU bars, vs DMS `qs` Pss ≈ 477 MB. Goal met.

## Milestones (see docs/06-ROADMAP.md)
- M0 core loop/log — done. M1 hello bar — done. **Next: M2** (text via Pango/HarfBuzz, widget toolkit,
  nanovg, animation engine + DMS duration table, niri IPC EventStream, Material color engine → real bar
  with clock + workspaces). Then M3 services, M4 panels, M5 lock/clipboard/screenshot, M6 theming/settings,
  M7 plugins, M8 packaging.

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
