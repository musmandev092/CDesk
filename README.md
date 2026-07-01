# DankC

A lightweight desktop shell for the **niri** Wayland compositor, written in C.

DankC reimplements the core of [DankMaterialShell](https://github.com/AvengeMedia/DankMaterialShell)
(QML/Quickshell + Go) as a single native C binary — the same Material You look and feel, a fraction of
the footprint. On the reference machine, the QML shell used ~588 MB RAM and pulled in ~193 MB of Qt;
DankC targets one process at ~30–80 MB with no Qt.

- **Compositor:** niri only (by design — see `docs/00-OVERVIEW.md`).
- **UI:** custom Wayland `wlr-layer-shell` + EGL/OpenGL ES + nanovg + Pango/HarfBuzz text.
- **Services:** sd-bus (D-Bus), libpipewire (audio), direct `/proc` (system stats).
- **Language:** ~99% C; one isolated C++ file for Material color math.

## Status

Early development. See `docs/06-ROADMAP.md` for milestones. The design is fully specified in `docs/`
(architecture, rendering, services, theming, the complete 424-setting inventory, the exact visual design
system, and the UX flow) before implementation.

## Build

Dependencies (Arch names): `base-devel meson ninja pkgconf wayland wayland-protocols libxkbcommon mesa
libsystemd freetype2 fontconfig harfbuzz pango pipewire pam curl sqlite cjson` (+ `grim` for dev
screenshots). Runtime target: a running niri session.

```sh
# Preferred (once meson is installed):
meson setup build
ninja -C build
./build/dankc

# Fallback bring-up (no meson needed, uses gcc + wayland-scanner directly):
make
./bin/dankc
```

Testing model (safe alongside an existing DankMaterialShell install): `dms kill` → `./bin/dankc` →
`Ctrl+C` → `dms run --session` to restore. No logout required.

## Repository layout

```
dankc/
├── docs/        # full design specification (00–11)
├── protocol/    # vendored Wayland protocol XML (generated code in protocol/generated/)
├── src/
│   ├── core/    # event loop, logging, config
│   ├── wayland/ # layer-shell surfaces, EGL, input
│   ├── render/  # nanovg, text, animation
│   ├── ui/      # widget toolkit + panels (bar, …)
│   ├── services/# sd-bus, pipewire, /proc
│   ├── theme/   # Material color engine
│   ├── niri/    # niri IPC client
│   └── ipc/     # dankctl control socket
├── dankctl/     # CLI client
└── scripts/     # dev helpers
```

## License

MIT — see `LICENSE`.
