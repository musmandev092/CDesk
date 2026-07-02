# Packaging DankC

DankC builds with **meson** (primary) or the **Makefile** (fallback, no meson
needed). One binary, `dankc`, is produced.

## Runtime dependencies
- wayland, libxkbcommon, mesa (EGL/GLES2), systemd-libs (sd-bus)
- niri (the only supported compositor)
- Optional CLI helpers used by widgets/actions: wireplumber (`wpctl`),
  `brightnessctl`, `bluez`, `rfkill`

## Build dependencies
- meson, ninja, wayland-protocols, pkgconf, a C11 + C++17 toolchain

## Build & install (meson)
```sh
meson setup build
meson compile -C build
meson install -C build      # installs dankc + bundled fonts to $prefix
```

## Build (Makefile fallback)
```sh
make            # -> ./bin/dankc
```

## Arch Linux
```sh
cd packaging && makepkg -si
```
For a tagged release, set `source=(...)` + `sha256sums=(...)` in the PKGBUILD;
the current file builds from a local checkout.

## Other distros
`libsystemd` provides sd-bus; on non-systemd distros swap the meson dependency
for `libelogind` or `basu` (same sd-bus API). Everything else is portable.

## Autostart under niri
Add to `~/.config/niri/config.kdl`:
```kdl
spawn-at-startup "dankc"
```
Generate keybinds for the launcher/panels with:
```sh
dankc keybinds        # prints a niri binds { } snippet
```
