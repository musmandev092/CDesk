# DankC — Design Overview

> A lightweight desktop shell for **niri**, written in C, replacing DankMaterialShell's
> Go (`dms`) + QML/Quickshell (`qs`) stack with a single native binary.

## 1. Why

Measured on the target laptop (Intel i5-8350U, UHD 620, Arch + niri) running DankMaterialShell 1.4.6:

| Metric | DankMaterialShell today | DankC target |
|---|---|---|
| Shell UI RAM (`qs`) | **588 MB** | **~30–80 MB** |
| Backend RAM (`dms`) | 43 MB | merged into the one binary |
| Qt6 on disk | **~193 MB** | **0** (no Qt) |
| Processes | 2 (`dms` + `qs`) | 1 |
| Language | Go + QML/JS | **~99% C** + 1 isolated C++ file |

The weight comes from the Qt/QML runtime and from Go+QML abstraction over what are, underneath,
all **native C libraries** (Wayland, D-Bus, PipeWire, NetworkManager, PAM, polkit). DankC talks to
those libraries directly and draws its own UI on the GPU.

## 2. Goals

1. **One C binary** = the whole shell (backend services + UI + CLI helper), niri-only.
2. **Feature parity** with the DankMaterialShell *core* (bar, dock, control center, notifications,
   OSDs, launcher, lock, clipboard, screenshot, color picker, theming, settings). See `04-FEATURES.md`.
3. **Visually match DMS** — Material You colors, rounded cards, the *same* animation model — but with
   **user-controllable intensity** (speed/effects/toggles), exactly like DMS exposes today.
4. **Lightweight & smooth** — GPU-composited, event-driven, ~0% CPU when idle.
5. **Portable across distros** (Arch, Fedora, Debian/Ubuntu, openSUSE, Void, Nix) even though it only
   targets the niri compositor. See `05-PORTABILITY.md`.
6. **Config-compatible** where sensible — reuse DMS's `settings.json` shape so migration is easy.

## 3. Non-goals (explicitly out of scope)

- **Other compositors** (Hyprland, Sway, Mango, labwc, Scroll, Miracle). niri only. This is where most
  of DMS's complexity — and weight — lives.
- **Plugins in v1.** The plugin system (native `.so` modules, dlopen + fixed C ABI) is **designed but
  deferred to a later phase**. See `06-ROADMAP.md`.
- **Browser / specific default-app choices** (Brave etc.) — deferred to a later plan. DankC only wires
  up the *mechanism* for default apps and leans on **GNOME native apps** as the default set
  (see `05-PORTABILITY.md §3`).
- X11. niri is Wayland-only.

## 4. Architecture — one process, modular inside

```
                         ┌───────────────────────── dankc (single binary) ─────────────────────────┐
   niri (compositor) ◄───┤  wayland/  ── layer-shell surfaces, EGL/GL rendering, input (xkbcommon)   │
     $NIRI_SOCKET   ◄────┤  niri/     ── niri IPC client (JSON): workspaces, windows, outputs        │
                         │  ui/       ── widget toolkit + panels (bar, dock, CC, OSD, launcher, lock)│
   system D-Bus    ◄─────┤  services/ ── sd-bus: UPower, logind, BlueZ, NetworkManager, PPD, polkit  │
   session D-Bus   ◄─────┤            ── sd-bus: Notifications(server), MPRIS(client), tray(SNI)     │
   PipeWire        ◄─────┤  audio/    ── libpipewire volume/devices                                  │
   /proc, /sys     ◄─────┤  sysmon/   ── CPU/RAM/temp/disk/net (replaces dgop)                       │
                         │  theme/    ── Material color engine (1 C++ file) + config                 │
                         │  core/     ── single epoll/sd-event loop, config, logging                 │
   dankctl (CLI)   ◄────►┤  ipc/      ── unix socket control surface (replaces `dms ipc`)            │
                         └──────────────────────────────────────────────────────────────────────────┘
```

**Everything runs on one event loop** (see `01-ARCHITECTURE.md`): the Wayland fd, both D-Bus fds
(system + session), the PipeWire loop fd, the niri socket fd, timers (`timerfd`), and the `dankctl`
control socket are all polled together. No goroutine/thread soup; a small worker pool only for
genuinely blocking IO if needed.

## 5. The one C++ island

The only non-C code is a single wrapper file around Google's **Material Color Utilities** (C++17),
used to turn a wallpaper into a Material 3 color scheme (same algorithm as matugen). It is compiled
separately and exposed to the rest of the program behind a plain C ABI. Everything else is C.
Details + the "you must also generate dank16 yourself" caveat: `03-THEMING.md`.

## 6. Component / binary layout

| Binary | Role |
|---|---|
| `dankc` | the shell daemon (spawned by niri at startup) + all services + UI |
| `dankctl` | tiny CLI that talks to `dankc` over its unix socket (keybinds, scripts) |

niri's config `spawn-at-startup "dankc"`; keybinds call `spawn "dankctl" "..."` (mirrors how DMS uses
`dms ipc call`). See `04-FEATURES.md §IPC` and `05-PORTABILITY.md`.

## 7. Document map

- `01-ARCHITECTURE.md` — process/event-loop model, directory layout, module boundaries, build system.
- `02-RENDERING.md` — Wayland layer-shell, EGL/GLES, nanovg, Pango/HarfBuzz text, input, animation engine.
- `03-SERVICES.md` — every backend service: exact D-Bus/PipeWire/niri interfaces to implement.
- `03-THEMING.md` — Material color engine, dank16, color roles, settings-driven theming.
- `04-FEATURES.md` — behavioral spec for bar, notifications, control center, OSD, launcher + config schema + IPC surface.
- `05-PORTABILITY.md` — dependencies per distro, sd-bus abstraction, packaging, default-apps/GNOME integration.
- `06-ROADMAP.md` — milestones, task breakdown, plugin-system design (deferred), Milestone-1 starter.
