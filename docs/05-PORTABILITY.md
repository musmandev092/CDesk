# DankC — Portability, Dependencies, Packaging & Default Apps

niri-only at runtime, but **buildable and packageable on Arch, Fedora, Debian/Ubuntu, openSUSE, Void,
and Nix**. The main portability risks are (a) the sd-bus provider on non-systemd distros and (b) a couple
of libs that aren't packaged and must be vendored.

## 1. Dependency table (dev packages; always resolve by pkg-config module, not package name)

| Lib | pkg-config | Arch | Fedora | Debian/Ubuntu | openSUSE | Void | Nix |
|---|---|---|---|---|---|---|---|
| wayland-client | `wayland-client` | wayland | wayland-devel | libwayland-dev | wayland-devel | wayland-devel | wayland |
| wayland-protocols | `wayland-protocols` | wayland-protocols | wayland-protocols-devel | wayland-protocols | wayland-protocols-devel | wayland-protocols | wayland-protocols |
| wayland-scanner | `wayland-scanner` | wayland | wayland-devel | libwayland-bin | wayland-devel | wayland-devel | wayland |
| xkbcommon | `xkbcommon` | libxkbcommon | libxkbcommon-devel | libxkbcommon-dev | libxkbcommon-devel | libxkbcommon-devel | libxkbcommon |
| EGL | `egl` | mesa | mesa-libEGL-devel | libegl-dev | Mesa-libEGL-devel | MesaLib-devel | libGL |
| GLES v2/3 | `glesv2` | mesa | mesa-libGLES-devel | libgles-dev | Mesa-libGLESv2-devel | MesaLib-devel | libGL |
| wayland-egl | `wayland-egl` | wayland | wayland-devel | libwayland-dev | wayland-devel | wayland-devel | wayland |
| freetype2 | `freetype2` | freetype2 | freetype-devel | libfreetype-dev | freetype2-devel | freetype-devel | freetype |
| fontconfig | `fontconfig` | fontconfig | fontconfig-devel | libfontconfig-dev | fontconfig-devel | fontconfig-devel | fontconfig |
| harfbuzz | `harfbuzz` | harfbuzz | harfbuzz-devel | libharfbuzz-dev | harfbuzz-devel | harfbuzz-devel | harfbuzz |
| pango/pangocairo | `pangocairo` | pango | pango-devel | libpango1.0-dev | pango-devel | pango-devel | pango |
| pipewire ≥0.3.60 | `libpipewire-0.3` | pipewire | pipewire-devel | libpipewire-0.3-dev | pipewire-devel | pipewire-devel | pipewire |
| PAM | (link `-lpam`) | pam | pam-devel | libpam0g-dev | pam-devel | pam-devel | pam |
| curl | `libcurl` | curl | libcurl-devel | libcurl4-openssl-dev | libcurl-devel | libcurl-devel | curl |
| sqlite3 | `sqlite3` | sqlite | sqlite-devel | libsqlite3-dev | sqlite3-devel | sqlite-devel | sqlite |
| cJSON | `libcjson` | cjson | cjson-devel | libcjson-dev | cjson-devel | libcjson-devel | cjson |
| sd-bus | `libsystemd` | systemd-libs | systemd-devel | libsystemd-dev | systemd-devel | libsystemd¹ | systemd |
| meson/ninja | — | meson ninja | meson ninja-build | meson ninja-build | meson ninja | meson ninja | meson ninja |

¹ Void-glibc only; Void-musl has no libsystemd → use basu (below).

**Vendor (in-tree / meson subprojects):**
- **nanovg** — not packaged anywhere reliable; tiny; vendoring also lets us pick the GLES3 backend via
  `#define`.
- **stb_image.h / stb_image_resize2.h** — header-only by design.
- **Material Color Utilities** (`cpp/`) — vendored, compiled as our C++ island (`03-THEMING.md`).
- **cJSON** — prefer system (`dependency('libcjson')`) but add a `subprojects/cjson.wrap` fallback (the
  package name is a classic churn trap). Always detect by pkg-config module `libcjson`.

**Dep-tree note:** pango drags in cairo+glib+fribidi+gobject (the biggest subtree). If we want to stay
leaner we can use HarfBuzz+FreeType directly for the hot text path and reserve PangoCairo for rich/i18n
panels only (`02-RENDERING.md §5`).

## 2. sd-bus provider abstraction (the key portability item)

Three ABI/API-compatible providers of `sd_bus_*`:
1. **libsystemd** — Arch/Fedora/Debian/openSUSE/Void-glibc. `<systemd/sd-bus.h>`.
2. **libelogind** — ships `libelogind.pc` + a `systemd/` compat header dir; sd-bus API works (some
   journal/manager symbols are non-functional stubs, irrelevant to us). For Artix/Gentoo-OpenRC etc.
3. **basu** (emersion) — standalone sd-bus, no login manager, `<basu/sd-bus.h>`. Fallback for musl/
   pure-non-systemd (Void-musl, Alpine, Chimera).

Meson combo option `-Dsd-bus-provider={auto,libsystemd,libelogind,basu}` probing
`libsystemd → libelogind → basu`; in C guard the include:
```c
#if defined(HAVE_BASU)
#  include <basu/sd-bus.h>
#else
#  include <systemd/sd-bus.h>
#endif
```
This is exactly how Sway, mako, and gamemode do it. (DMS itself detects the init system at runtime via
`cat /proc/1/comm` in `DesktopService.qml` — mirror that for autostart-target logic.)

**Gotchas:** Debian renamed `libegl1-mesa-dev`→`libegl-dev`; require pipewire ≥0.3.60 (0.3 API churn);
Void-musl `auto` must fall back to basu.

## 3. Default applications & GNOME app integration

**Mechanism:** MIME type → desktop-file-ID via the freedesktop mime-apps spec. `.desktop` files in
`$XDG_DATA_HOME/applications` + `$XDG_DATA_DIRS/applications`. Defaults/associations in `mimeapps.list`
(search: `~/.config/mimeapps.list` → `~/.config/<desktop>-mimeapps.list` → `/etc/xdg/…` → data dirs),
sections `[Default Applications]`,`[Added Associations]`,`[Removed Associations]`. URL schemes are
pseudo-MIME `x-scheme-handler/<scheme>`. Tools: `xdg-mime`, `xdg-settings`, `gio mime`,
`update-desktop-database`.

**DankC approach:** own a small `mimeapps.list` writer (like DMS's `core/internal/desktop/mimeapps.go`)
exposed via IPC `defaultApp` (browser/fileManager/textEditor/imageViewer/videoPlayer/musicPlayer/
pdfReader/mail/calendar/terminal). Terminal is special — no MIME; write `~/.config/xdg-terminals.list`
for `xdg-terminal-exec`. Browser/FileManager pickers filter by `.desktop Categories=` to avoid junk.

**GNOME native apps as the default set** (per the plan; browser deferred). Verify IDs with
`ls /usr/share/applications/org.gnome.*` (GNOME renamed several):

| Role | Desktop ID | Package |
|---|---|---|
| Files | `org.gnome.Nautilus.desktop` | nautilus |
| Text editor | `org.gnome.TextEditor.desktop` | gnome-text-editor |
| Image viewer | `org.gnome.Loupe.desktop` | loupe |
| Calculator | `org.gnome.Calculator.desktop` | gnome-calculator |
| Documents/PDF | `org.gnome.Papers.desktop` (was Evince) | papers |
| Terminal | `org.gnome.Console.desktop` (`kgx`) | gnome-console |
| Video | `org.gnome.Showtime.desktop` (was Totem) | showtime |
| Archives | `org.gnome.FileRoller.desktop` | file-roller |

**What GNOME apps need on niri:**
- Portals: `xdg-desktop-portal` + `xdg-desktop-portal-gtk` (fallback + Settings) + `xdg-desktop-portal-gnome`
  (ScreenCast; its FileChooser uses Nautilus). Route via `/usr/share/xdg-desktop-portal/niri-portals.conf`
  (`default=gnome;gtk`). **Don't** set `GDK_BACKEND` globally (breaks screencast). `gnome-keyring` for Secret.
- Dark mode without gnome-settings-daemon: `gsettings set org.gnome.desktop.interface color-scheme
  'prefer-dark'` → xdg-desktop-portal-gtk serves `org.freedesktop.appearance color-scheme` → libadwaita/
  GTK4 apps go dark. Also set `gtk-theme`, `icon-theme 'Adwaita'`, `cursor-theme`, `font-name`. DankC's
  theme engine drives this (`03-THEMING.md §5`), so dark/light propagates to GNOME apps automatically.
- Icons: install `adwaita-icon-theme` + `hicolor-icon-theme`; resolved via the `icon-theme` gsettings key.
- Nautilus specifics: needs **gvfs** (+gvfs-mtp/smb) for trash/mounts/network and a live D-Bus session
  (`dbus-update-activation-environment`); hardcodes `/usr/bin/kgx` for "Open Terminal" (install
  gnome-console or symlink `kgx`); thumbnails via GNOME's `.thumbnailers` (add `ffmpegthumbnailer`).

These are **runtime recommendations / an install profile**, not build deps — DankC only writes the
mimeapps/gsettings wiring; the user (or the installer, a later phase) installs the GNOME apps + portals.

## 4. Packaging plan (later phase)

Meson `install` targets (binary → `/usr/bin`, `.desktop` + icon, systemd user unit `dankc.service`
`ExecStart=dankc`, shell completions for `dankctl`). Per-distro packaging mirrors DMS's `distro/` layout
(Arch PKGBUILD, Fedora spec, Debian/OBS, openSUSE, Void template, Nix flake + home-manager module). The
sd-bus provider option makes the same source build on all of them. Packaging is Milestone 8.
