# DankC — Gap Analysis, Feature Inventory, Fonts & Default-Apps Profile

What the first 6 design docs did **not** yet fully cover, plus the decisions to close each gap.

## 1. Genuine gaps found (ordered by importance)

| # | Gap | Status in docs 00–06 | Decision / where it goes |
|---|---|---|---|
| G1 | **App-icon resolution** (XDG icon-theme lookup: turn a `.desktop` `Icon=name` or a tray `IconName` into an actual PNG/SVG file) — needed by tray, launcher, dock, focused-app | not specced | **New service `services/icons.c`** — parse `index.theme`, resolve by name+size+scale, cache to GL textures. Fonts give us glyph icons; this gives us *app* icons. Add to M3. |
| G2 | **Fonts / fontconfig / bundled font set** | not specced | **§3 below.** Bundle Inter + Material Symbols + a Nerd Font; ship a fontconfig conf; expose font roles in settings. |
| G3 | **Settings UI interface** (tabs, layout, per-control→config wiring) | mentioned, not laid out | **`08-SETTINGS-UI.md`** (full interface spec + mockup). |
| G4 | **Settings import from DMS** ("settings should carry over") | said "config-compatible" only | **Decision:** on first run, if `~/.config/DankMaterialShell/settings.json` exists, offer to import it (keys overlap by design). Also export/share via `dankctl settings export/import`. |
| G5 | **Default-apps GNOME profile** as a concrete, all-free/libre set + wiring | partial in 05 | **§4 below** — the exact app list, portals, gsettings, all OFL/GPL. |
| G6 | **Full feature inventory with v1/deferred status** (dock, dashboard, notepad, process list, weather, calendar, power menu, privacy, session, welcome wizard, keybind cheatsheet, workspace overview, window-rules editor, system update, printer, VPN/Tailscale, desktop widgets, greeter) | scattered across roadmap | **§2 below** — one table, each marked v1 / later / optional. |
| G7 | **Sounds** (notification + system sounds) | not specced | **Decision:** freedesktop sound-theme lookup + play via libcanberra *or* a tiny PipeWire player. Minor; M4 with notifications. |
| G8 | **Wallpaper management** (per-monitor, cycling, transition shaders) | listed, not detailed | **Decision:** `services/wallpaper.c` + `session.json` per-monitor paths + cycling timer + the `Shaders/` transition set ported to GLSL (M6). |
| G9 | **xdg-desktop-portal** role | unclear | **Decision:** DankC does **not** ship a portal backend in v1. Screencast/filechooser handled by `xdg-desktop-portal-gnome`+`-gtk` (§4). Our screenshot/color-pick are direct via wlr-screencopy. |
| G10 | **i18n / translations** | not addressed | **Deferred.** English-only v1; wrap user-facing strings in a `tr()` seam now so translations can be added later (M8). |
| G11 | **Sound/mic/camera privacy indicators, power menu, session mgmt** | listed as widgets only | **Decision:** privacy = PipeWire/portal active-stream watch; power menu + session = logind (`03-SERVICES §4`). v1 (small). |

None of these change the architecture — they're additions that slot into existing modules.

## 2. Complete feature inventory (v1 vs later)

**✅ v1 (Milestones 1–6):** bar + all core widgets, control center, notifications (daemon), OSDs,
launcher (spotlight + spotlight-bar), theming (matugen-equivalent + dank16 + custom themes), lock screen,
clipboard + history, screenshot, color picker, night mode, idle inhibit, power menu, session
(logout/suspend/reboot), privacy indicators, audio/network/bluetooth/battery/brightness/MPRIS/tray,
system monitors, default-apps wiring, icon-theme resolution, fonts, settings UI (core tabs), niri KDL
generation, `dankctl` IPC.

**🟡 Later (Milestone 8 unless pulled forward):** dock, dashboard (overview/media/weather tabs), desktop
widgets (clock, system-monitor), notepad, process list / task manager, weather + geolocation, calendar,
workspace overview overlay, window-rules editor, keybind cheatsheet overlay, system-update UI, welcome/
first-launch wizard, wallpaper cycling + transition shaders, blur.

**⬜ Optional (feature-gated / on request):** printing (CUPS), Tailscale, VPN detail panels, greeter
(login screen), terminal-mux integration, audio visualizer (cava), app color-theming templates for
GTK/Qt/terminals, i18n.

**❌ Not doing:** plugins in v1 (native `.so`, designed, **M7**); non-niri compositors; X11.

## 3. Fonts (G2)

**Font roles** (settings-configurable; defaults bundled, all free/OFL):

| Role | Default font | License | Used for |
|---|---|---|---|
| UI / sans | **Inter** | OFL | all interface text (bar, panels, settings) |
| Monospace | **JetBrains Mono** | OFL | clock digits option, code, terminal-ish readouts |
| Icon | **Material Symbols Rounded** | Apache-2.0 | all glyph icons (variable `FILL/wght/GRAD/opsz`) |
| Nerd/symbols | **Symbols Nerd Font** | OFL/MIT (patched) | powerline + extra glyphs some apps emit |
| Fallback | **Noto Sans + Noto Color Emoji** | OFL | CJK / emoji / missing-glyph fallback |

- **Bundled** under `share/dankc/fonts/`; loaded directly via FreeType/Fontconfig without requiring
  system install, but system copies are used if present.
- **fontconfig:** ship `share/dankc/fontconfig/60-dankc.conf` setting the fallback chain (Inter → Noto →
  emoji) so missing glyphs never tofu. Material Symbols/Nerd loaded as private faces for the icon atlas
  (not in the general fallback, to avoid hijacking text).
- **Settings** (Typography tab, `08-SETTINGS-UI.md`): `fontFamily`, `monoFontFamily`, `fontScale`
  (global multiplier), `iconWeight` (Material Symbols `wght`), `fontHinting`. Google-hosted fonts (Inter,
  JetBrains Mono, Noto) are all OFL — free to bundle and redistribute.

## 4. Default-apps GNOME profile (G5) — all free/libre

Rationale (your call): you already daily-drive GNOME apps; using them as the default set avoids constant
app-switching and keeps everything GPL/OFL/free. DankC only **writes the wiring**; the apps + portals are
an install profile (the installer, M8, can automate it).

**App set** (verify IDs with `ls /usr/share/applications/org.gnome.*`):

| Role | Desktop ID | Package | License |
|---|---|---|---|
| Files | `org.gnome.Nautilus` | nautilus | GPL |
| Text editor | `org.gnome.TextEditor` | gnome-text-editor | GPL |
| Image viewer | `org.gnome.Loupe` | loupe | GPL/MPL |
| Calculator | `org.gnome.Calculator` | gnome-calculator | GPL |
| Documents / PDF | `org.gnome.Papers` | papers | GPL |
| Terminal | `org.gnome.Console` (`kgx`) | gnome-console | GPL |
| Video | `org.gnome.Showtime` | showtime | GPL |
| Archives | `org.gnome.FileRoller` | file-roller | GPL |
| Browser | **deferred** (your later plan) | — | — |

**Wiring DankC writes** (`services/mime`, exposed via IPC `defaultApp`):
- `~/.config/mimeapps.list` `[Default Applications]` mapping each MIME/scheme to the IDs above.
- `~/.config/xdg-terminals.list` → `org.gnome.Console.desktop` (terminal has no MIME).
- gsettings for theme follow: `org.gnome.desktop.interface color-scheme` (dark/light, driven by our theme
  engine), `icon-theme 'Adwaita'`, `cursor-theme`, `font-name`.

**Runtime deps the profile needs** (installed, not built): `xdg-desktop-portal` +
`xdg-desktop-portal-gtk` + `xdg-desktop-portal-gnome` (screencast/filechooser; route via
`/usr/share/xdg-desktop-portal/niri-portals.conf` `default=gnome;gtk`), `gnome-keyring` (secrets),
`gvfs` (+gvfs-mtp/smb for Nautilus trash/mounts/network), `adwaita-icon-theme` + `hicolor-icon-theme`,
`ffmpegthumbnailer` (thumbnails). Do **not** set `GDK_BACKEND` globally.

This whole profile is one section of the installer / a `dankctl setup default-apps` command (M8).

## 5. What is NOT missing (already fully covered in docs 00–06)

Event loop, rendering/EGL/fractional-scale, text shaping, input, animation (DMS-exact table), all D-Bus
services + niri IPC, Material color engine + dank16, config engine + onChange hooks, the IPC command
surface, portability/sd-bus, packaging outline, and the deferred plugin ABI.
