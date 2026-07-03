# Packaging DankC

DankC builds with **meson** (primary) or the **Makefile** (fallback, no meson
needed). One binary, `dankc`, is produced.

## Runtime dependencies
- wayland, libxkbcommon, mesa (EGL/GLES2), systemd-libs (sd-bus)
- niri (the only supported compositor)
- `noto-fonts`, `noto-fonts-cjk`, `ttf-dejavu` — fontconfig-located script
  fallback fonts dankc's own renderer loads at startup (Latin/Cyrillic/
  Greek/Arabic/Urdu/Devanagari/Thai/most other scripts + CJK, with DejaVu
  as a last-resort safety net); see "System-wide fontconfig" below.
- Optional CLI helpers used by widgets/actions: wireplumber (`wpctl`),
  `brightnessctl`, `bluez`, `rfkill`
- Optional: `noto-fonts-emoji` for COLOR emoji in browsers/GTK/Qt apps —
  dankc's own renderer is monochrome-only (see src/render/nvg.c) and
  vendors its own NotoEmoji subset, so this isn't needed for dankc's UI
  itself, only for the rest of the desktop.

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

## System-wide fontconfig
The Arch package installs `packaging/fontconfig/49-dankc-fonts.conf` to
`/etc/fonts/conf.d/49-dankc-fonts.conf`. This is a standard fontconfig
drop-in — it does **not** change how dankc itself finds fonts (dankc's own
renderer locates its fallback chain directly via libfontconfig at startup,
see `src/render/nvg.c`); it exists so the *rest* of the desktop (browsers,
GTK/Qt apps, terminals) gets the same broad, non-tofu language coverage:
- `sans-serif`/`serif`/`monospace` prefer Inter (dankc's own UI font, if
  installed system-wide), then Noto Sans/Serif, then a Noto face for
  essentially every script `noto-fonts`/`noto-fonts-cjk` ship (Arabic,
  Hebrew, Devanagari, Bengali, Gurmukhi, Gujarati, Tamil, Telugu, Kannada,
  Malayalam, Sinhala, Thai, Lao, Khmer, Myanmar, Armenian, Georgian,
  Ethiopic, Cherokee, symbols/math, CJK), then DejaVu as a last resort.
- A strong `lang=ur`/`lang=ar` rule pins Noto Sans Arabic so Urdu never
  falls back to DejaVu, which is missing the Urdu-only letters
  (U+06C1/U+06D2).
- Strong `lang=zh/ja/ko` rules pick the correct Noto (Sans|Serif) CJK
  region sub-face.
- Noto Color Emoji is appended to every generic family so emoji render in
  color in browsers/GTK/Qt apps (dankc's own bar/panels stay monochrome).

If you run your own fontconfig setup and don't want this, delete or mask
it (e.g. `rm /etc/fonts/conf.d/49-dankc-fonts.conf`, or drop your own
higher-numbered conf.d file to override specific rules) — dankc's own
font loading is unaffected either way.

## Autostart under niri
Add to `~/.config/niri/config.kdl`:
```kdl
spawn-at-startup "dankc"
```
Generate keybinds for the launcher/panels with:
```sh
dankc keybinds        # prints a niri binds { } snippet
```
