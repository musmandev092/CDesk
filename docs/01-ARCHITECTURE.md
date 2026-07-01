# DankC — Internal Architecture

## 1. Process & event-loop model

DankC is **single-process, single-threaded, event-driven**. One loop multiplexes every fd:

```
poll() over:
  ├─ wl_display fd        (wl_display_get_fd)          → Wayland events, input, frame callbacks
  ├─ system D-Bus fd      (sd_bus_get_fd, system)      → UPower/logind/BlueZ/NM/PPD/polkit
  ├─ session D-Bus fd     (sd_bus_get_fd, user)        → Notifications(server)/MPRIS/tray(SNI)
  ├─ pipewire loop fd     (pw_loop_get_fd)             → audio volume/device changes
  ├─ niri socket fd       (NIRI_SOCKET, EventStream)   → workspaces/windows/outputs deltas
  ├─ dankctl socket fd    (our unix listen socket)     → CLI/keybind commands
  └─ N timerfd            (clocks, key-repeat, tweens, poll-position, auto-hide)
```

**Loop skeleton** (recommended: hand-rolled `poll()` so we own the Wayland fd directly; `sd-event`
is an alternative but wrapping Wayland into it is fiddly):

```c
for (;;) {
    // 1. flush pending Wayland requests
    while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
    wl_display_flush(dpy);

    // 2. drain D-Bus ready work (both buses), compute their fds/events/timeouts
    while (sd_bus_process(sysbus, NULL) > 0);
    while (sd_bus_process(usrbus, NULL) > 0);

    // 3. build pollfd set (wayland, buses, pipewire, niri, dankctl, timerfds)
    //    timeout = min(sd_bus_get_timeout(...) [absolute→relative, round up], animation deadline)
    int n = poll(pfds, nfds, timeout_ms);

    // 4. dispatch by readiness
    if (wl_ready) wl_display_read_events(dpy), wl_display_dispatch_pending(dpy);
    else          wl_display_cancel_read(dpy);
    // sd_bus_process again for ready buses; pw handled by its own embedded loop or pw_loop_iterate;
    // service niri socket, dankctl, timerfds.
}
```

**Idle = 0% CPU.** We only ever arm a `wl_surface.frame` callback while an animation/tween is running
(`02-RENDERING.md §animation`). When nothing animates and no event arrives, `poll()` blocks. A clock
tick is a `timerfd`, not a spin.

**Blocking work.** D-Bus calls that could stall the UI (polkit `BeginAuthentication`, NM connect,
BlueZ pair, `wpctl` exec) use `sd_bus_call_method_async` or a spawned child + fd, never a sync call on
the loop thread. If any library forces true blocking IO, isolate it in a tiny worker-thread pool that
posts results back via an `eventfd`. Default is: no threads.

## 2. Directory layout (source tree)

```
dankc/
├── meson.build
├── protocol/                 # vendored Wayland protocol XML (wlr-layer-shell, etc.) + generated
├── subprojects/              # meson wraps: nanovg (vendored), cjson fallback, stb headers
├── src/
│   ├── main.c                # startup: greeter-vs-shell, connect wayland/dbus/pw/niri, run loop
│   ├── core/
│   │   ├── loop.c/.h         # the poll() event loop + fd registration API
│   │   ├── config.c/.h       # settings.json + session.json load/save (cJSON) + change hooks
│   │   ├── log.c/.h          # leveled logger (stderr / sd-journal)
│   │   ├── util.c/.h         # string/array/arena helpers
│   │   └── timer.c/.h        # timerfd wrappers
│   ├── wayland/
│   │   ├── wl.c/.h           # registry, globals, seats, outputs (+ hotplug)
│   │   ├── layer.c/.h        # zwlr_layer_surface helper (bar/dock/popup/overlay/OSD)
│   │   ├── egl.c/.h          # EGL context, wl_egl_window, frame loop, fractional-scale/viewport
│   │   ├── input.c/.h        # wl_pointer/keyboard/touch + xkbcommon + hit-testing dispatch
│   │   ├── lock.c/.h         # ext-session-lock-v1
│   │   ├── idle.c/.h         # ext-idle-notify + idle-inhibit
│   │   ├── screencopy.c/.h   # wlr-screencopy / ext-image-copy-capture (screenshot, color pick)
│   │   ├── gamma.c/.h        # wlr-gamma-control (night mode)
│   │   └── datactrl.c/.h     # wlr/ext-data-control (clipboard)
│   ├── render/
│   │   ├── nvg.c/.h          # nanovg context lifecycle + helpers (rounded rect, shadow, gradient)
│   │   ├── text.c/.h         # Pango/HarfBuzz/FreeType glyph atlas → GL; icon-font glyphs
│   │   ├── image.c/.h        # stb_image decode → GL texture (wallpaper, album art, tray pixmaps)
│   │   └── anim.c/.h         # tween/easing/spring engine + M3 motion curves
│   ├── ui/
│   │   ├── widget.c/.h       # base widget: rect, layout, hover/press/scroll, draw vtable
│   │   ├── layout.c/.h       # row/column/anchor flexbox-lite
│   │   ├── theme.c/.h        # live color roles + tokens (radius/spacing/font sizes) from config
│   │   ├── bar/              # DankBar: window, sections (L/C/R), widget host, per-monitor
│   │   │   └── widgets/      # clock, workspaces, focused-window, media, battery, tray, monitors…
│   │   ├── dock/             # dock
│   │   ├── controlcenter/    # quick settings popout
│   │   ├── notifications/    # popups + center
│   │   ├── osd/              # volume/brightness/etc transient overlays
│   │   ├── launcher/         # spotlight + spotlight-bar
│   │   ├── lock/             # lock screen UI (+ PAM via services/polkit path)
│   │   ├── dash/             # dashboard
│   │   └── settings/         # settings UI
│   ├── services/
│   │   ├── dbus.c/.h         # sd-bus connect (sys+user), generic PropertiesChanged router
│   │   ├── upower.c/.h       # battery/power
│   │   ├── logind.c/.h       # brightness, suspend/reboot/lock, idle-inhibit, sleep signals
│   │   ├── ppd.c/.h          # power-profiles-daemon
│   │   ├── bluez.c/.h        # bluetooth
│   │   ├── nm.c/.h           # NetworkManager (raw sd-bus, no libnm)
│   │   ├── notify.c/.h       # org.freedesktop.Notifications SERVER
│   │   ├── mpris.c/.h        # MPRIS2 client
│   │   ├── tray.c/.h         # StatusNotifierWatcher + Host + dbusmenu
│   │   ├── polkit.c/.h       # polkit agent (+ polkit-agent-helper-1)
│   │   ├── audio.c/.h        # libpipewire volume/devices (+ optional wpctl actions)
│   │   ├── sysmon.c/.h       # /proc + /sys/hwmon metrics
│   │   ├── weather.c/.h      # libcurl + cJSON
│   │   └── geo.c/.h          # geoclue / IP location
│   ├── theme/
│   │   ├── material.cpp      # THE C++ island: MCU wrapper (image→scheme), C ABI
│   │   ├── material.h        # C header for material.cpp
│   │   ├── dank16.c/.h       # 16-color ANSI palette generation (matugen never produced this)
│   │   └── apply.c/.h        # write niri dms/colors.kdl + optional app templates
│   ├── niri/
│   │   └── niri.c/.h         # NIRI_SOCKET: request/reply + EventStream, state maps
│   └── ipc/
│       ├── server.c/.h       # dankctl control socket (line-JSON), command dispatch
│       └── commands.c/.h     # command table (mirrors dms ipc targets)
└── dankctl/
    └── main.c                # the CLI client
```

## 3. Module boundaries & data flow

- **Services never draw; UI never talks to D-Bus.** Each service owns its state struct and emits a
  change callback (`on_change(void*)`). UI widgets subscribe. Example: `upower.c` keeps
  `{percentage, state, time_to_empty}`, calls the battery widget's invalidate on `PropertiesChanged`.
- **A single generic `PropertiesChanged` router** (`services/dbus.c`) keyed by `(path, interface)`
  fans out to UPower/BlueZ/NM/logind/PPD/MPRIS handlers — one match rule instead of dozens.
- **niri state** lives in `niri.c` as maps keyed by `id` (workspaces, windows, outputs), updated from
  the EventStream (plural event = snapshot, singular = delta). Bar widgets read it directly.
- **Config is the source of truth for appearance/behavior.** `core/config.c` loads `settings.json`,
  exposes typed getters, and runs **change hooks** (e.g. `cornerRadius` / `niriLayout*` → regenerate
  `~/.config/niri/dms/layout.kdl`; theme keys → re-run color engine). This mirrors DMS's
  `SettingsSpec.js` `onChange` model (`04-FEATURES.md §config`).
- **Invalidation → frame.** Any state change marks affected widgets dirty and, if a transition is
  wanted, starts a tween which arms the frame callback. Static changes just request one redraw.

## 4. Build system

**Meson + Ninja.** Rationale: light, first-class Wayland-scanner integration, easy subprojects/wraps
for vendored deps, trivial cross-distro. Key pieces:

- `wayland-scanner` custom targets generate `*-client-protocol.h` / `*-protocol.c` from XML in
  `protocol/` (core + wayland-protocols stable/staging + vendored wlr-* / ext-* XML).
- `subprojects/`: **nanovg** (vendored, GLES3 backend via `#define`), **stb_image / stb_image_resize2**
  (header-only), **cjson** wrap as fallback when the system `libcjson` isn't found.
- `material.cpp` compiled as C++17 in its own static lib; MCU sources listed explicitly (no Bazel).
  Abseil avoided by patching the one `absl::StrCat` in MCU `utils.cc` to `snprintf`.
- A meson combo option `-Dsd-bus-provider={auto,libsystemd,libelogind,basu}` selects the D-Bus
  backend for portability (`05-PORTABILITY.md §2`).
- Feature options to compile-out optional services (`-Dweather`, `-Dcups`, `-Dtailscale`, `-Dcava`).

## 5. Startup sequence (`main.c`)

1. Parse args / env (`DMS_RUN_GREETER`-style greeter mode is a later phase).
2. Load `settings.json` + `session.json`.
3. `wl_display_connect(NULL)` → registry → bind globals (compositor, layer-shell, seats, outputs,
   viewporter, fractional-scale, xdg-wm-base, session-lock, idle, screencopy, gamma, data-control).
4. Open EGL display/context.
5. `sd_bus_open_system` + `sd_bus_open_user`; register the generic PropertiesChanged router; start
   each service (initial `GetManagedObjects`/property reads, then match rules).
6. `pw_init` + connect PipeWire; bind default-metadata + default node params.
7. Connect `NIRI_SOCKET`; open a second connection for `EventStream`; prime state.
8. Run the color engine for the current wallpaper; publish theme.
9. Create bar surface(s) per output; create the `dankctl` listen socket.
10. Enter the event loop.
