# DankC — Project State (read this first)

This file is the canonical, always-current index of the project so a fresh session or subagent does
**not** need to re-scan the whole tree. Update it whenever state changes.

## Autonomous build
Running an overnight self-paced loop that works through **docs/TASKS.md** (ordered backlog toward exact
DankMaterialShell fidelity). Each iteration: implement one task → build → run → screenshot → compare to
`docs/dms_reference.png` → fix → commit → tick the box → leave session clean. Reference code:
`/home/mosman092/Downloads/DankMaterialShell-master/quickshell`.

## What this is
A lightweight desktop shell for the **niri** Wayland compositor, written in C — a reimplementation of
DankMaterialShell's core (QML/Quickshell + Go) as one native binary. Full spec in `docs/` (00–11).
niri-only, ~99% C (one C++ file for Material colors), plugins deferred.

## Current status — SETTINGS-COMPLETENESS + SYSTEM THEMING COMPLETE ✅ (2026-07-04, main @ HEAD)
Both `make` and `meson` build zero-warning; repo clean; test suites pass (calc, text_edit, systheme).
- **Settings completeness DONE**: every system setting is changeable from the Settings app (~29 tabs).
  Added this round: Displays (niri IPC: res/scale/rotate/arrange/VRR/enable/save-default), Night Light
  (gammastep/wlsunset temp+schedule), Network (ethernet/hotspot/saved), Printers (CUPS), Firewall
  (ufw/firewalld via polkit), Mouse/Touchpad/Keyboard (niri input → ~/.config/niri/dankc-input.kdl),
  Date & Time (timedatectl NTP+timezone), Power idle/lid (logind.conf.d drop-in via pkexec). All
  system writes DANKC_*_DRYRUN-gated + backup + niri validate where relevant.
- **⭐ System-wide theming DONE** (docs/21): dankc natively writes each app's theme file from its own
  dc_theme palette (Approach B — no matugen binary), covering ~33 apps across 10 systheme_*.c emitters
  (toolkits GTK3/4+Qt5/6ct+Kvantum+KDE+GTK2, terminals alacritty/kitty/foot/ghostty/wezterm/konsole/
  xterm, editors VSCode/Zed/Helix/Neovim/Vim/Sublime/Emacs, launchers rofi/wofi/fuzzel/tofi, notify
  mako/dunst/swaync, browsers firefox/qutebrowser/discord, media btop/cava/zathura/spicetify) +
  categorized detected-first Settings UI (Theme & Colors → SYSTEM THEMING) + 33 config toggles + 59-check
  test (tests/test_systheme.c). Opt-in, backup-before-write, DANKC_THEME_DRYRUN. Palette source is
  dc_systheme_apply() at the end of apply_theme() in config.c (fires on wallpaper/theme/light-dark change).
- **OSD variants**: mic-mute, media play/pause, power-profile, audio-output-switch (osd.c, poll-in-tick).
- **Notepad** (docs/22, IN PROGRESS): text_edit.c multi-line editor widget (51-check test) + notepad_storage.c
  (tabs/autosave, 21-check test) + notepad.c popout panel + wl.c key-repeat/modifier helpers all merged;
  main.c wiring (NT4) + launcher entry (NT5) + bar widget (NT6) in flight/next.
- **Keybind editing** (docs/23): planned, service (KB-T1) in flight.
Gap triage + roadmap: docs/20. Group-1 remaining after Notepad/keybinds: DND scheduling, missing bar
widgets, battery protection + power&sleep depth, audio per-device, VPN/IPC/wallpaper/updater/screen-rec,
connected-frame chrome; then GREETER. DROPPED (user): desktop widgets, plugins, multi-bar, per-widget config.

## Previous status — STRUCTURE + PERFORMANCE + FONT/POLISH COMPLETE ✅ (2026-07-03)
All of docs/14 (structure), docs/15 (memory perf), docs/16 (runtime perf), plus font/i18n +
UI polish are done. Both `make`, `make release`, and `meson` build zero-warning; repo clean.

PERFORMANCE (measured, idle 2-output):
- Memory: font files mmap'd → Private ~40MB→~18MB (halved). Deep-dive verdict: RSS ~152MB is
  ~87% SHARED Mesa/libLLVM niri holds regardless — Pss ~48MB is the real number, at the floor.
- **Async wpctl audio**: eliminated a 35-40ms event-loop freeze that hit every ~10s (biggest
  responsiveness win). NM D-Bus wifi + wider audio cache = ~90% fewer background forks.
- **LTO + release flags**: dev binary 4.3MB → release 980KB stripped.
- **Startup**: autostart deferred past first bar paint → warm 100ms→78ms. (EGL-threading tried,
  dropped — within noise. -O3/LTO measured no startup effect. Idle CPU ~0.15%, frame pacing,
  damage tracking, input latency all measured already-optimal — no work needed.)
FONTS / i18n:
- Comprehensive system fontconfig (packaging/fontconfig/49-dankc-fonts.conf, 24 scripts→Noto,
  Urdu→Noto Arabic, CJK/Indic/Thai/Hebrew/emoji) + font deps (noto-fonts, noto-fonts-cjk,
  ttf-dejavu; noto-fonts-emoji optdep). Bundled: Inter + Material Symbols subset + mono NotoEmoji.
- Lazy load-on-first-use for CJK/Devanagari/Thai/Tamil/emoji (Urdu/Latin eager) → ~19ms warm.
- **Mixed-script fix**: shape.c now itemizes each bidi run by per-codepoint font coverage
  (was: one font per run → non-Arabic scripts blanked). Added missing Tamil fallback.
POLISH:
- Icon centering: added nvgTextInkBounds() (nanovg's nvgTextBounds discarded true ink-y for
  line metrics) — icons now center on real ink; play triangle measured dead-center (0.00px).
- **Default Apps**: 19-category manager (real .desktop MimeType=/Categories= parsing +
  xdg-mime/xdg-settings writes, empty-state handling). apps.c dc_apps_find_by_mime/by_category.
Deferred by design: T29 plugins. NOT-yet-done: alignment sweep only covered bar/dashboard/CC
(clean); battery-popout/launcher/notif/clipboard/settings/powermenu/dock/keybinds not re-swept.
## Previous status — bar parity DONE ✅ (S1–S6, 2026-07-02); panels parity next
Bar now pixel-matches the user's live DMS bottom bar (docs/12-BAR-SPEC.md, commits
07831c7..c412bb6): floating rounded 40px container + elevation shadow + bottom position,
config-driven widget host (BasePill chips, user's exact L/C/R widget arrays), workspace
capsules w/ morph animation, music+transport, weather (Open-Meteo svc), cpu/mem (sysmon svc),
CC compound pill, Material battery glyph + AC bolt, unread dot, hover states + cursor-shape
pointer + wheel-scroll workspace switching, DMS settings.json auto-import, Arabic/Urdu
fallback font (fontconfig) + cmap-coverage sanitizer. Full Material Symbols variable font
vendored (14.5MB — subset later, P7). NEXT: docs/13-POPOUTS-SPEC.md (panel parity vs user
screenshots in ~/Pictures/Screenshots/), starting with P4a bar-position-aware popout anchoring.

## Previous status — M2/M3/M4 (bar live, control center + OSD done)
- **Builds clean** (gcc + wayland-scanner, `make` → `./bin/dankc`, zero warnings).
- **Runs on niri**: layer-shell bar per output via **nanovg**, closely matching DMS. LEFT: apps-grid
  launcher · workspace green-pill + grey dots (sorted) · app icon (PNG+SVG) + "AppName · Title". CENTER:
  time + date (**live, 1 Hz**). RIGHT: DMS status cluster with real state — battery% (sysfs), wifi (sysfs),
  audio (wpctl), bluetooth (BlueZ). DMS **green palette** (#1d211b bg). Crisp on HiDPI (fractional-scale).
- **Control Center** popout (T18): themed card, 2×2 toggle tiles + volume/brightness sliders, interactive
  (rfkill/wpctl/brightnessctl), dismiss on outside click.
- **Volume OSD** (T19): transient bottom-center overlay, auto-hide, pops on volume change.
- **Notifications** (T20): full org.freedesktop.Notifications server + top-right toast stack (max 4
  cards, click-to-dismiss) + notification CENTER popout (64-entry history, cards + Clear-all, opened from
  the bar bell). Declines the name if a daemon already owns it. Action buttons/images deferred.
- **App launcher** (T21): centered spotlight overlay with keyboard focus (xkb), fuzzy desktop-entry
  search + icons, type/arrows/enter/click to launch. Opens from the bar launcher button.
- **Config + themes** (T26): ~/.config/dankc/config.json (cJSON) — theme/clock24h/showDate/animation
  prefs, DMS defaults. All 10 DMS DARK stock themes selectable via dc_theme_set (stock_themes.inc,
  generated). Bar clock honours 12/24h + showDate.
- **Animation engine** (T9): core/anim.c — DMS durations + easing (incl. expressive overshoot),
  config-scaled. Entrance + EXIT animations (fade+scale/slide) on launcher, control center, notif center,
  OSD, and toasts via self-terminating frame callbacks.
- **FIXED the frozen clock**: it was a loop-wide deadlock — blocking `wl_display_dispatch()` raced Mesa's
  gallium threads for the display fd. Now uses thread-safe prepare_read/read_events + a wall-clock loop
  tick. See memory `dankc-wayland-dispatch-deadlock`.
- **Footprint:** RSS ≈ **145 MB** for two GPU bars incl. Mesa (Pss lower), vs DMS `qs` ≈ 477 MB.
- **dankctl** (T25): control socket + `dankc ctl launcher|control-center|notifications|clipboard|
  screenshot|quit` + `dankc keybinds` (Mod+D/N/Shift+C/V, Print). **Dynamic color** (T24):
  theme/dynamic.cpp from wallpaper (config dynamicColor+wallpaper). **Clipboard** (T23): wlr-data-control
  history + picker (bar icon / Mod+V). **Screenshot** (T23): grim via `ctl screenshot` / Print.
  **Build**: meson.build (T28, verified) + packaging/ (T30).
- **T23 done**: clipboard, full+region screenshot, color-picker (slurp+grim), night mode (gammastep).
- **T16 tray done**: SNI host reads the watcher's items + renders icons on the bar (resolver now finds
  22x22). **T27 settings done**: theme picker + toggles + speed slider, live-apply + persist.
- **T22 lock done**: ext-session-lock + PAM (auth.c), clock+password UI, `dankc ctl lock`; F1/ctl-unlock
  escape hatch gated by DANKC_LOCK_ESCAPE. **T17 logind done**: lock on sleep / lock-session.
- **Only T29 plugins remains** — deferred by user ("skip plugins right now"); post-v1 by design.
- Polish (optional): workspace-pill + per-card toast animation, tray click-to-activate + pixmap icons,
  lock-before-sleep inhibitor, settings→full DMS parity, notification action buttons.
- **Whole UI visually verified via grim** (2026-07-02): bar, launcher, control center, clipboard picker all
  match DMS. Screenshots in the session scratchpad.
- Vendored: nanovg (GLES3, `third_party/nanovg`), cJSON (`third_party/cjson`); Inter bundled
  (`assets/fonts/InterVariable.ttf`).

## Milestones (see docs/06-ROADMAP.md)
- M0 core, M1 hello bar — done. **M2 in progress:** nanovg text ✅, live clock ✅, niri workspaces ✅.
  Remaining M2: more bar widgets (focused window, battery/media/tray placeholders), the animation engine
  (DMS duration table), the Material color engine (C++ MCU), and proper HiDPI. Then M3 services (sd-bus:
  audio/network/battery/…), M4 panels, M5 lock/clipboard/screenshot, M6 theming/settings, M7 plugins,
  M8 packaging.

## Known follow-ups (address in M2)
- **HiDPI:** bar currently renders buffer_scale=1 (blurry on the scale-2 internal panel). Implement
  fractional-scale / integer-scale handling (docs/02-RENDERING §3).
- ~~Event loop uses `wl_display_dispatch`~~ — DONE: migrated to `prepare_read`/`read_events` (was
  deadlocking against Mesa threads; see memory `dankc-wayland-dispatch-deadlock`).
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
