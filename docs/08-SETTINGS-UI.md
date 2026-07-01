# DankC — Settings Interface

How the settings window is structured and how every control maps to a config key + service. Built from
DankC's own widget toolkit (no Qt/GTK), opened as an `xdg_toplevel` window (or a large layer popup),
toggled by `dankctl settings toggle` (`Mod+Comma`).

## 1. Window layout

A left **category sidebar** → a **tab list** → a scrollable **content pane** on the right. A search box
at the top filters across all settings (like DMS's settings search index). Every control is data-bound to
one config key; changing it calls `config_set(key,value)` which runs the key's `onChange` hook and
persists to `settings.json` (`04-FEATURES.md §8`).

```
┌───────────────────────────── DankC Settings ─────────────────────────────┐
│  🔍 Search settings…                                                       │
├───────────────┬───────────────────────────────────────────────────────────┤
│ PERSONALIZATION│  Theme & Colors                                           │
│  • Theme&Colors│  ┌──────────────────────────────────────────────────────┐ │
│  • Wallpaper   │  │ Theme       ( Auto ▾ )   ○ Light ● Dark               │ │
│  • Typography  │  │ Palette     [green ▾]    Scheme [tonal-spot ▾]        │ │
│  • Icons       │  │ Contrast    ├──●──────┤  Corner radius ├────●──┤ 12   │ │
│ DANK BAR       │  │ Custom theme  [ /path/theme.json ]   [Browse]         │ │
│  • Bar Layout  │  │ Transparency  Popout ├──●─┤  Dock ├───●┤             │ │
│  • Appearance  │  │ Widget color  ( Primary ▾ )                          │ │
│  • Widgets     │  └──────────────────────────────────────────────────────┘ │
│ PANELS         │                                                           │
│  • Control Ctr │  (content scrolls; sidebar + tab list stay fixed)         │
│  • Notifications                                                           │
│  • OSD         │                                                           │
│ DOCK&LAUNCHER  │                                                           │
│ SYSTEM         │                                                           │
│ POWER&SECURITY │                                                           │
│ ADVANCED       │                                                           │
└───────────────┴───────────────────────────────────────────────────────────┘
```

**Control types** (each a widget in `ui/settings/widgets`): dropdown, toggle, slider (+unit), color
swatch/picker, text field, file picker, segmented button, list editor (drag-reorder), keybind capture.
All read/write config keys and re-render live.

## 2. Categories → tabs → contents (with config keys & data source)

> **Authoritative checklist:** every individual option (all **424**) is enumerated in
> `09-DMS-SETTINGS-INVENTORY.md`, extracted verbatim from DMS's `settings_search_index.json`. The
> grouping below is the UI structure; doc 09 is the per-option truth. Real DMS categories and counts:
> Theme & Colors **71**, Dank Bar **38**, Personalization **36**, Launcher **34**, Dock **29**,
> Notifications **28**, Lock Screen **23**, Frame **20**, Power & Sleep **18**, Typography & Motion **17**,
> Greeter **17**, Time & Weather **16**, System **12**, On-screen Displays **12**, Sounds **10**,
> Network **6**, Displays **5**, Users **5**, Media Player **4**, Audio **3**, Locale **3**,
> Multiplexers **3**, Running Apps **2**, System Updater **2**, Autostart **2**, and singletons
> (Keyboard Shortcuts, Plugins, About, Desktop Widgets, Default Apps, Dank Dash).
>
> **niri-only saves work:** settings gated `isHyprland` (4) and `isMango` (3) are dropped; the 15
> `isNiri`-gated ones are kept. Other gates: `matugenAvailable`, `soundsAvailable`, `cupsAvailable`,
> `keybindsAvailable`, `windowRulesCapable`, `dmsConnected`.
>
> **Things this revealed that were under-specified earlier** (now all in scope): a full **Frame** tab
> (20 — screen-edge border/decoration), **Sounds** tab (10 — system/notification sound theme),
> **Greeter** tab (17 — login screen), combined **Time & Weather** (16), **Media Player** (4),
> **Running Apps** (2), **Multiplexers/Mux** (3), and **cursor** settings + **matugen per-app template
> toggles** living under Theme & Colors.

Legend: **[v1]** ships Milestones 1–6 · **[later]** M8 · **[opt]** optional/feature-gated.

### PERSONALIZATION
- **Theme & Colors [v1]** — mode (Auto/Light/Dark → `session.isLightMode`, theme automation),
  palette (`currentThemeName`,`currentThemeCategory`), Material scheme (`matugenScheme`),
  contrast (`matugenContrast`), corner radius (`cornerRadius`), custom theme file
  (`customThemeFile`), transparencies (`popupTransparency`,`dockTransparency`), widget color modes
  (`widgetColorMode`,`buttonColorMode`,`controlCenterTileColorMode`). → theme engine (`03-THEMING`).
- **Wallpaper [v1 set / later cycling]** — set global + per-monitor (`session` per-monitor paths),
  fill mode (`wallpaperFillMode`), cycling on/off + interval [later], transition style [later]. → `services/wallpaper.c`.
- **Typography & Motion [v1]** — UI font (`fontFamily`), monospace (`monoFontFamily`), font scale
  (`fontScale`), icon weight (`iconWeight`), hinting (`fontHinting`); **animation speed**
  (`animationSpeed` None/Short/Medium/Long/Custom) + `customAnimationDuration`,
  `syncComponentAnimationSpeeds`, `popout/modal/notificationAnimationSpeed` (+custom durations),
  `enableRippleEffects`, `animationVariant`, `motionEffect`. → `render/anim.c`, `render/text.c`.
  *(This is the "identical-to-DMS animations, user-controllable" surface — the full duration table lives
  in `02-RENDERING §8`.)*
- **Icons [v1]** — icon theme name (`iconTheme`) → `services/icons.c` (XDG icon-theme resolution).

### DANK BAR
- **Bar Layout [v1]** — per bar in `barConfigs[]`: position (Top/Bottom/Left/Right + Center variants),
  per-monitor (`screenPreferences`), and the three ordered widget lists
  (`leftWidgets/centerWidgets/rightWidgets`) edited via a drag-reorder list + enable toggles. Add/remove
  bars. → bar module.
- **Appearance [v1]** — height/thickness, `spacing`,`innerPadding`,`bottomGap`, transparency,
  `squareCorners`/`gothCorners*`, border (`borderEnabled/Color/Opacity/Thickness`), shadow
  (`shadowIntensity/Opacity/ColorMode`), auto-hide (`autoHide`,`autoHideDelay`,`showOnWindowsOpen`,
  `reserveExclusiveWhenAutoHidden`), `fontScale`,`iconScale`, scroll behavior.
- **Widgets [v1]** — per-widget config: clock (`use24HourClock`,`showSeconds`,`clockDateFormat`,
  `clockCompactMode`), weather (`weatherEnabled`, location, `useFahrenheit`,`windSpeedUnit`),
  workspace appearance (index/name/icons, per-state colors, `maxWorkspaceIcons`), monitors
  (`selectedGpuIndex`,`enabledGpuPciIds`), tray tint. Also global `show*` visibility flags.

### PANELS
- **Control Center [v1]** — tile set + order (`controlCenterWidgets`, drag-reorder, per-tile width),
  tile color mode, composite status-icon flags (`controlCenterShow*Icon`).
- **Notifications [v1]** — timeouts (`notificationTimeoutLow/Normal/Critical`), Do-Not-Disturb, history
  (`notificationHistoryMaxCount`, max-age, per-urgency save), popup position + max visible, sounds,
  per-app rules editor (drop/hide/disable-popup). → `services/notify.c`.
- **OSD [v1]** — per-OSD enable (`osdVolumeEnabled`,`osdBrightnessEnabled`,…), `osdPosition`,
  `osdAlwaysShowValue`.
- **Dashboard [later]** — enabled tabs (overview/media/weather), overview cards.
- **Desktop Widgets [later]** — clock / system-monitor instances (position, size, opacity, click-through).

### DOCK & LAUNCHER
- **Dock [later]** — enable, position, auto-hide, pinned apps, trash/overflow buttons.
- **Launcher [v1]** — view mode (`appLauncherViewMode` list/grid), `appLauncherGridColumns`,
  `sortAppsAlphabetically`, spotlight sizing/badges, `spotlightCloseNiriOverview`, plugin visibility [M7].

### SYSTEM
- **Audio [v1]** — output/input device pick + default, per-device volume/mute, visualizer/cava [opt].
  → `services/audio.c`.
- **Network [v1]** — Wi-Fi (scan/connect/forget), Ethernet status, VPN [opt], connectivity status. → `services/nm.c`.
- **Bluetooth [v1]** — power toggle, discovery, pair/connect/forget, per-device battery. → `services/bluez.c`.
- **Displays [v1]** — per-output resolution/refresh, scale (fractional), position (drag canvas),
  transform, VRR → writes niri `dms/outputs.kdl` (`04-FEATURES §7`).
- **Night Mode [v1]** — enable, temperature (2500–6000K), automation (manual/time/location), schedule,
  location. → `wayland/gamma.c` + `services/geo.c`.
- **Default Apps [v1]** — per-role app pick (GNOME set, `07-GAPS §4`), terminal. → `services/mime`.
- **Locale [v1]** — language [i18n later], `use24HourClock`, `firstDayOfWeek`, temperature/wind units.
- **System Update [later]**, **Printer [opt]**, **Users/Accounts [later]**, **Auto Start [later]**.

### POWER & SECURITY
- **Power & Sleep [v1]** — idle timeouts (dim/lock/sleep), power profiles (`powerprofile`, PPD),
  lid/suspend behavior. → `services/logind.c` + `ppd.c`.
- **Lock Screen [v1]** — auto-lock timeout, appearance, PAM (`04-FEATURES §6`, `03-SERVICES §10`).
- **Battery [v1]** — charge-limit indicator, low-battery thresholds, show percent/time.

### ADVANCED
- **Window Rules [later]** — niri window-rules editor → `dms/windowrules.kdl`.
- **Workspaces [later]** — naming / per-workspace settings.
- **Keybinds [v1]** — cheatsheet + edit user binds → `dms/binds-user.kdl`; capture widget.
- **Plugins [M7]** — scan/enable/disable/reload (native `.so`), per-plugin settings (`06-ROADMAP §7`).
- **Greeter [opt]**, **Terminal Mux [opt]**.
- **About / Diagnostics [v1-lite]** — version, `dankctl doctor` output, config paths, import DMS settings.

## 3. How a setting flows (implementation)

```
user drags "Corner radius" slider
   → slider widget calls config_set("cornerRadius", 14)
      → config.c writes root, runs onChange hook "updateCompositorLayout"
         → regenerates ~/.config/niri/dms/layout.kdl (debounced) + marks theme tokens dirty
      → persists settings.json (atomic tmp+rename), stamps configVersion
   → theme.c token change invalidates affected widgets → one redraw (or a tween if animated)
```

Each tab is a small module registering its controls + the config keys it owns; the search box indexes
control labels → tab, so typing "gap" jumps to Bar Appearance. Settings that map to niri KDL are
validated (`niri validate`) before the generated file is swapped in.

## 4. Build order for the settings UI

Settings tabs come online with the features they configure — Theme/Bar/Typography/Notifications/OSD/
Audio/Network/Bluetooth/Night/Launcher/Lock/Power/Battery/Displays/DefaultApps/Icons in v1 (M2–M6);
Dashboard/Dock/DesktopWidgets/Update/Printer/Users/AutoStart/WindowRules/Workspaces/Plugins/Greeter as
their features land (M7–M8). The window shell + sidebar + search + one tab (Theme) is built first, in M4.
