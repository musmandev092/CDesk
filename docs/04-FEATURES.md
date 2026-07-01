# DankC — Feature Behavior, Config Schema & IPC

Behavioral spec distilled from DankMaterialShell so DankC reproduces the *feel*, not just the look.
File references below point into the DMS repo (the reference implementation).

## 1. DankBar

`DankBar → DankBarWindow(per output) → sections(L/C/R) → widget host → widgets`.

- **Layout model:** three ordered lists `leftWidgets/centerWidgets/rightWidgets`; each entry is a
  widgetId string or `{id,enabled,...props}`; array order = visual order. `barConfigs` is an **array**
  (multiple bars, id `"default"`). Per-monitor via `screenPreferences` (`["all"]` = every output).
- **Window:** layer-shell namespace `dms:bar` (we use `dankc:bar`); anchors per `position` enum
  (Top/Bottom/Left/Right + *Center variants). Vertical bars (Left/Right) swap section geometry.
- **Thickness:** base `barHeight=48`; `effectiveBarThickness = max(widgetThickness+innerPadding+4,
  barHeight-4-(8-innerPadding))` snapped to device px. `innerPadding` default 4.
- **Exclusive zone:** `-1` when hidden/auto-hidden (no reservation), else `thickness+spacing+bottomGap`.
- **Auto-hide:** `autoHide` per bar; reveal when `hover || popoutPinsReveal || revealSticky ||
  ipcReveal`; `autoHideDelay` default 250 ms hold; `showOnWindowsOpen` reveals when windows exist;
  `openOnOverview` reveals in niri overview. Slide animation on x/y.
- **Click-through:** input region built only over actual widget rects so gaps pass clicks to windows.
- **Default widgets:** L: `launcherButton, workspaceSwitcher, focusedWindow`; C: `music, clock, weather`;
  R: `systemTray, clipboard, cpuUsage, memUsage, notificationButton, battery, controlCenterButton`.
- **Widget catalog (33):** clock, workspaceSwitcher, focusedWindow(app title/id), media(MPRIS),
  weather, systemTray(SNI), clipboard, cpu/ram/cpuTemp/gpuTemp/disk/network monitors(/proc),
  notificationButton, battery(UPower), controlCenterButton(composite status icons),
  keyboardLayoutName(niri), idleInhibitor, capsLock, powerMenu, notepad, privacyIndicator, vpn,
  systemUpdate, runningApps/appsDock, colorPicker, audioVisualization(cava), launcherButton.
  Each widget: a data source (a service or niri state) + click/scroll behavior (toggle a popout, cycle
  workspace, play/pause, etc.).
- **Widget behaviors of note:** workspaceSwitcher → `niri.currentOutputWorkspaces`, click switches
  (`FocusWorkspace`), scroll cycles, optional app icons (max 3); media → scroll adjusts volume, wave
  progress + scrolling title; battery → percent/time + charge-limit; controlCenterButton → composite of
  network/BT/audio/VPN/etc icons per `controlCenterShow*Icon`.

Ref: `quickshell/Modules/DankBar/**`, `SettingsSpec.js` bar keys.

## 2. Notifications (DankC is the daemon)

Protocol surface: `03-SERVICES.md §7`. Behavior to replicate (`NotificationService.qml`):
- On `Notify`: evaluate policy (per-app rules: drop/hide/disable-popup, urgency); **dedupe** (5 s burst
  window, key = app+summary+body); **rate-limit** (max ~20/s, queue cap 32); play sound honoring
  `suppress-sound` (distinct critical vs normal); build a wrapper with `popup = !popupsDisabled &&
  !doNotDisturb && !policy.disablePopup`; keep in center unless `transient`/`hideFromCenter`.
- **Timeout:** app `expire_timeout` if ≥0 else per-urgency: `notificationTimeoutLow/Normal`=5000,
  `notificationTimeoutCritical`=0 (never).
- **History:** JSON at `$XDG_CACHE_HOME/DankC/notification_history.json`; images cached to a dir; limit
  `notificationHistoryMaxCount`=50, max-age 7 days; per-urgency save toggles; grouped-by-app model.
- **Popups:** max ~4 visible with a queue; position `notificationPopupPosition` (default Top); enter/exit
  durations from the notification animation axis (`02-RENDERING.md §8`).
- Center + Popup are separate UI surfaces; actions routed back via `ActionInvoked`.

## 3. Control Center

Popout of toggle tiles + sliders. Default `controlCenterWidgets`:
`volumeSlider, brightnessSlider, wifi, bluetooth, audioOutput, audioInput, nightMode, darkMode`
(each `{enabled,width:50}`, width = % in a 2-col grid). Data: audio(PipeWire), brightness/night(logind/
gamma), wifi(NM), bluetooth(BlueZ), darkMode(session). Composite status-icon flags `controlCenterShow*Icon`.
Tile color mode `controlCenterTileColorMode="primary"`. Trigger: bar button or IPC `control-center toggle`.

## 4. OSDs

One transient overlay per channel: Volume, MediaVolume, MicVolume, AudioOutput, Brightness, CapsLock,
IdleInhibitor, MediaPlayback, PowerProfile. **Trigger = reactive**, not IPC: e.g. VolumeOSD shows when
`audio.sink.volume/mute` changes, then a hide timer. Each has an enable flag (`osdVolumeEnabled`, …);
position `osdPosition` default BottomCenter; `osdAlwaysShowValue` forces numeric readout.

## 5. Launcher

Two surfaces: `spotlight` (full launcher, `Mod+Space`) and `spotlight-bar` (compact, `Alt+Space`).
App data from parsed `.desktop` entries (`$XDG_DATA_DIRS/applications`), usage-history ranking, app-id
substitutions for icons. `appLauncherViewMode="list"`, `appLauncherGridColumns=4`, alpha-sort option.
`spotlightCloseNiriOverview` closes niri overview on launch. Fuzzy match (port DMS's `fzf.js` or use a
small C fuzzy matcher).

## 6. Lock, clipboard, screenshot, color picker (mechanisms)

- **Lock:** ext-session-lock surfaces per output + PAM via the polkit-helper path (`03-SERVICES.md §10`);
  subscribe logind Session `Lock`/`Unlock`; `SetLockedHint(true)` while locked.
- **Clipboard:** wlr/ext-data-control watcher; history in **SQLite** at `$XDG_STATE_HOME/DankC/`.
  Modal toggled by IPC `clipboard toggle` (`Mod+V`).
- **Screenshot:** wlr-screencopy/ext-image-copy → encode PNG (libpng/stb_image_write) → optional region
  overlay → open editor (`satty`/`swappy`) if configured (`DMS_SCREENSHOT_EDITOR`-style).
- **Color picker:** screencopy 1-px sample + magnifier overlay; copy hex to clipboard.
- **Night mode:** wlr-gamma-control ramp from color temperature (2500–6000 K); automation manual/time/
  location (geoclue). Keys under `night*`.

## 7. niri config & keybind generation

DankC regenerates KDL fragments under `$XDG_CONFIG_HOME/niri/dms/` (like DMS), all with a
`// AUTO-GENERATED — DO NOT EDIT` header, included by the user's `config.kdl` in this order:
`colors.kdl, layout.kdl, alttab.kdl, binds.kdl, outputs.kdl, cursor.kdl`.
- `layout.kdl`: `gaps` (= `max(4, bar spacing)` or override), border/focus-ring width
  (`niriLayoutBorderSize`, def 2), window-rule `geometry-corner-radius` (= `cornerRadius`/override, def 12),
  `clip-to-geometry true`, `draw-border-with-background false`.
- `colors.kdl`: from the color engine (`03-THEMING.md §5`).
- `binds.kdl`: DankC-managed keymap; each shell action is `spawn "dankctl" "<target>" "<fn>" [args]`.
  DankC binds win over user duplicates but conflicts are surfaced. User overrides go in a separate
  `binds-user.kdl`.
- `outputs.kdl`/`cursor.kdl`/`alttab.kdl`: display/cursor/alt-tab settings.
Regeneration is debounced (~100 ms) and triggered by the relevant `onChange` config hooks. Validate a
generated binds file with `niri validate -c <tmp>` before replacing.
Ref: `NiriService.qml` (generation), `core/internal/keybinds/providers/niri.go` (parse/edit),
`core/internal/config/embedded/niri*.kdl` (defaults).

## 8. Config engine (`settings.json` / `session.json`)

Reuse DMS's format. `core/config.c` mirrors `SettingsSpec.js`: a table `key → {default, type, coerce,
onChange, persist}`. ~381 keys. Files:
`$XDG_CONFIG_HOME/DankC/settings.json` (persistent), `$XDG_STATE_HOME/DankC/session.json` (mode, DND,
per-monitor wallpaper, locale). Load applies defaults for missing keys; save stamps a `configVersion`
and supports versioned migration.

**`onChange` hooks to implement** (reactive triggers): `applyStoredTheme`, `regenSystemThemes`,
`updateCompositorLayout` (→ regenerate `dms/layout.kdl`), `updateCompositorCursor`, `updateBarConfigs`,
`applyStoredIconTheme`. Changing e.g. `cornerRadius` must call `updateCompositorLayout`.

**Key categories:** theme/color (`currentThemeName`,`matugenScheme`,`cornerRadius`,transparency, layout
overrides), matugen template toggles (~25 `matugenTemplate<App>` bools), bar widget visibility
(`show*` flags + `controlCenterShow*Icon`), the `barConfigs` array, workspace appearance, control-center
widgets, **animations** (`animationSpeed`,`customAnimationDuration`,`syncComponentAnimationSpeeds`,
`popout/modal/notificationAnimationSpeed`+custom durations, `animationVariant`,`motionEffect`,
`enableRippleEffects`), clock/locale, notifications, OSD, power/idle timeouts, lock screen, dock,
launcher, fonts, sounds, display/output, desktop widgets, clipboard, frame mode.

**Animation table (port verbatim; the "same as DMS, user-controllable" requirement):** see
`02-RENDERING.md §8` for the full duration table and per-category scaling factors. `animationSpeed`
enum None/Short/Medium/Long/Custom; `Custom` collapses all tiers to `customAnimationDuration`.

## 9. IPC surface (`dankctl` ↔ `dankc`)

Unix socket at `$XDG_RUNTIME_DIR/dankc-<session>.sock`, newline-JSON `{id,target,fn,args}` →
`{id,ok,result}`. `dankctl <target> <fn> [args]` mirrors `dms ipc call`. Targets to implement (from
DMS's IPC reference): `audio, brightness, night, mpris, lock, inhibit, powerprofile, wallpaper, profile,
theme, bar, dock, widget, spotlight, spotlight-bar, clipboard, notifications, processlist, powermenu,
control-center, notepad, settings, dash, dankdash, file, color-picker, keybinds, niri, outputs, plugins
(later), systemupdater, toast, sessions, defaultApp`. Each `open/close/toggle` where applicable; audio/
brightness/night/mpris carry action verbs (`03-SERVICES.md`, DMS `docs/IPC.md`).

niri keybinds call these, e.g. `Mod+Space → spawn "dankctl" "spotlight" "toggle"`,
`XF86AudioRaiseVolume → spawn "dankctl" "audio" "increment" "5"` (with `allow-when-locked=true` on
media/volume/brightness binds).
