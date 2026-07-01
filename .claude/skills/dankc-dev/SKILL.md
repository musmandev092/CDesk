---
name: dankc-dev
description: Build, run, screenshot, and iterate on DankC (the C niri shell). Use when working in ~/dankc — for building the shell, testing a change live on niri, capturing screenshots, checking RAM/perf, or picking up the next milestone. Encodes the project's build workflow and conventions so the tree need not be re-scanned.
---

# DankC development workflow

DankC is a lightweight C desktop shell for the **niri** Wayland compositor (reimplementation of
DankMaterialShell's core). Read `AGENTS.md` first — it holds the live project state and milestone status.
Full design is in `docs/00-11`. Coding standards: `CONVENTIONS.md`.

## Build & run
- **Build:** `make` (fallback: gcc + wayland-scanner, works without meson) → `./bin/dankc`. Primary build
  system will be meson (`meson.build`) once meson is installed.
- **Run live:** `scripts/dev.sh bg` builds and runs in the background (logs `/tmp/dankc.log`); look at the
  screen, then `scripts/dev.sh stop`. Safe alongside a running DankMaterialShell — restore with `pkill dankc`.
- **Deps:** `scripts/install-deps.sh` (per-distro). Missing on the reference box: `meson ninja
  wayland-protocols cjson grim`; everything else is present.

## The iterate loop (do this for every visible change)
1. Make the change (clean C, `dc_<module>_<verb>()` naming — see CONVENTIONS.md).
2. `scripts/dev.sh bg` — rebuild + relaunch.
3. `scripts/dev.sh shot <dir>` — screenshot **all** outputs via `dms screenshot` (grim isn't installed;
   `dms` is). Then Read the PNG to inspect it visually.
4. `scripts/dev.sh rss` — proportional memory (Pss). Keep it light; investigate regressions.
5. Check the log for protocol errors (`scripts/dev.sh log`).
6. When it looks right and builds clean, commit (`feat(scope): …`, small logical commits).

## Verifying UI/UX
- The bar/panels are wlr-layer-shell surfaces; each element is its own surface (docs/11-UX-FLOW z-order).
- Compare look against the exact tokens in `docs/10-DESIGN-SYSTEM.md` (spacing 4/8/12/16/24, radius 12,
  bar height 48, the 5-level elevation table, per-widget state colors) and motion against the duration
  table (popout 150ms, OSD 450ms, dock 225ms, etc.).
- HiDPI: the internal panel is scale 2 — verify crispness (fractional-scale handling, docs/02 §3).

## Conventions (must follow)
- C11, 4-space indent, 100 cols, `clang-format` before commit. Warnings matter.
- One module = `.c`+`.h` under `src/<area>/`; minimal public surface; everything else `static`.
- Explicit ownership: every `dc_x_create()` has `dc_x_destroy()`. No malloc in the render hot path.
- Never block the event loop; D-Bus async; re-arm frame callbacks only while animating (idle = 0% CPU).
- Never commit `protocol/generated/` or `bin/`.

## Next milestone
See `AGENTS.md` → "Milestones". Currently M1 done; M2 = text + widget toolkit + nanovg + animation +
niri IPC + Material color engine (a real bar with clock and workspaces).
