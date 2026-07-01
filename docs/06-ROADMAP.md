# DankC — Roadmap, Milestones & Deferred Plugin Design

Milestones are ordered so there's a **runnable, testable result early**, and each is safe to develop
alongside the existing DankMaterialShell (which stays installed as the fallback). Test model: `dms kill`
→ run `./build/dankc` → `Ctrl+C` → `dms run --session` to restore. No logout needed until DankC becomes
the daily default (one line in `~/.config/niri/config.kdl`).

## Milestone 0 — Bootstrap (build skeleton)
- Meson project, `protocol/` with vendored wlr XML + wayland-scanner targets, `subprojects/` wraps
  (nanovg, stb, cjson fallback), the `-Dsd-bus-provider` option, C++ island stub compiling.
- `core/loop.c` (poll loop + fd registration), `core/log.c`, `core/config.c` (load/save settings.json).
- **Outcome:** `dankc` builds and runs, connects to Wayland + both D-Bus + niri socket, logs, exits clean.

## Milestone 1 — Hello bar (foundation proof) ⭐ first visible result
- `wayland/wl.c` (registry/globals/outputs), `wayland/layer.c` (top bar surface + handshake),
  `wayland/egl.c` (EGL context, frame loop, fractional-scale/viewport), `render/nvg.c` (nanovg init).
- Draw a themed empty bar strip (background color from config) on each output. `dankctl ping`.
- **Outcome:** a real bar appears on niri via OpenGL. Proves the whole rendering pipeline on Intel/Mesa.

## Milestone 2 — Real bar
- `render/text.c` (Pango/HarfBuzz atlas + Material Symbols icons), `ui/widget.c` + `ui/layout.c` +
  `ui/theme.c`, `render/anim.c` (tween engine + DMS duration table), `niri/niri.c` (EventStream + state).
- Widgets: clock, workspaceSwitcher (niri), focusedWindow, launcherButton (stub). Color engine
  (`theme/material.cpp` + dank16) themes the bar from the wallpaper.
- **Outcome:** a themed, animated bar with working clock + niri workspaces. Looks like a basic DMS bar.

## Milestone 3 — Core services (daily-drivable)
- `services/dbus.c` (generic PropertiesChanged router), `audio.c`(PipeWire), `upower.c`, `logind.c`
  (brightness/power), `nm.c`(Wi-Fi), `bluez.c`, `notify.c`(daemon), `mpris.c`, `tray.c`(SNI), `sysmon.c`.
- Bar widgets: battery, media, systemTray, cpu/ram/temp/disk/network, controlCenterButton status icons.
- **Outcome:** audio/network/bluetooth/battery/brightness/notifications/tray/media all work. Usable daily.

## Milestone 4 — Panels
- `ui/controlcenter/` (toggles+sliders), `ui/osd/` (volume/brightness/etc reactive overlays),
  `ui/notifications/` (popups + center), `ui/launcher/` (spotlight + .desktop parsing + fuzzy),
  `ui/dash/`. Popout/xdg-popup plumbing. IPC targets wired.
- **Outcome:** control center, OSDs, notification center, app launcher — feels complete.

## Milestone 5 — Security & capture
- `wayland/lock.c` (ext-session-lock) + PAM (polkit-helper path), `services/polkit.c` (agent),
  `wayland/idle.c` (idle+inhibit), `wayland/screencopy.c` (screenshot + color picker),
  `wayland/gamma.c` (night mode), `wayland/datactrl.c` + `services/*` clipboard (SQLite history).
- **Outcome:** lock, screenshot, color picker, clipboard history, night mode, idle inhibit. Core parity.

## Milestone 6 — Theming polish & settings
- App color templates (feature-gated), portal dark/light sync, wallpaper background layer + transitions,
  optional blur (dual-Kawase, off by default), full `ui/settings/` UI, niri KDL generation
  (`04-FEATURES.md §7`).
- **Outcome:** full theming + settings UI + the "same-as-DMS animations, user-configurable" model complete.

## Milestone 7 — Plugin system (was deferred) — design below
- Implement the native `.so` ABI + loader + settings integration + `dankctl plugins` commands.

## Milestone 8 — Packaging & extras
- Meson install, systemd unit, per-distro packages (`05-PORTABILITY.md §4`), greeter mode, dock,
  process list, notepad, weather/geo, optional cava/cups/tailscale, default-apps install profile.

---

## Deferred: native `.so` plugin design (Milestone 7)

Chosen mechanism: **compiled C shared objects via `dlopen` against a fixed C ABI** (per your decision).
Not in v1; captured here so the core is built with the right seams.

**Discovery:** scan `$XDG_CONFIG_HOME/DankC/plugins/<id>/` and `/etc/xdg/dankc/plugins/<id>/`; each has a
`plugin.json` manifest + a `plugin.so`.

**Manifest** (`plugin.json`, parsed with cJSON):
```json
{ "id":"myWidget", "name":"My Widget", "version":"1.0.0", "author":"...",
  "type":"widget|daemon|launcher|desktop", "abi":1, "so":"./plugin.so",
  "permissions":["settings_read","settings_write"] }
```

**ABI (versioned `abi` int; host refuses mismatches):**
```c
// dankc-plugin.h — the stable C ABI the host exposes to plugins
#define DANKC_PLUGIN_ABI 1
typedef struct dankc_host dankc_host;   // opaque; host services (theme, config, toast, exec, ipc)
typedef struct {
    int abi;                            // must == DANKC_PLUGIN_ABI
    const char *id;
    int  (*init)(dankc_host *h, void **state);
    void (*deinit)(void *state);
    // widget: measure + draw with the host's NVGcontext (no direct GL from plugins)
    void (*measure)(void *state, float *w, float *h);
    void (*draw)(void *state, void *nvg, float w, float h);
    void (*event)(void *state, const dankc_input_event *ev);
    // launcher: query→items, execute
    int  (*get_items)(void *state, const char *query, dankc_item **out, int *n);
    void (*execute)(void *state, const dankc_item *item);
} dankc_plugin;
// entry point resolved by dlsym:
const dankc_plugin *dankc_plugin_entry(void);
```
Host provides `dankc_host` accessors: `theme colors/tokens`, `config get/set` (gated by permissions),
`toast`, `exec-detached`, `ipc-call`, per-plugin persistent state (JSON under
`$XDG_STATE_HOME/DankC/<id>_state.json`). **Plugins never touch GL/Wayland directly** — they draw through
the host's nanovg context and receive already-dispatched input events, so the host keeps control of the
render loop and safety.

**Hot-reload:** `dankctl plugins reload <id>` → `deinit` → `dlclose` → `dlopen` → `init`. Runtime control
`dankctl plugins list|enable|disable|status <id>`.

**Types:** `widget` (bar/control-center draw), `daemon` (background init/deinit only), `launcher`
(get_items/execute), `desktop` (background-layer draw). Composite = a plugin exposing multiple callback
sets.

**Trade-off acknowledged:** native `.so` is the fastest and most powerful but plugin authors must compile
against `dankc-plugin.h` and match the ABI version — heavier than DMS's QML/Lua-style plugins. That's the
explicit choice; ABI stability is maintained via the `abi` int + host-side version gating.

---

## Immediate next actions (once you say go)

1. **M0 + M1 scaffold** — create the meson project + vendored protocols + the "hello bar" so you can
   `dms kill && ./build/dankc` and watch a real bar render on your niri.
2. In parallel, a **dependency install list** for your Arch box (build tools + the few missing `-devel`
   headers) so M0 compiles first try.

Everything above is the plan; nothing is built yet. Approve and I start at Milestone 0/1.
