# DankC — Bar Parity Spec (user's live DMS bar, extracted 2026-07-02)

Synthesized from: the user's live DMS config (`~/.config/DankMaterialShell/settings.json`),
the DMS QML (`Modules/DankBar/`), and an audit of `src/ui/bar/bar.c`. This is the
**implementation contract** for bar pixel-parity. Reference screenshots:
session scratchpad `shots/dms_bar_{left,mid,right,bottom}.png` (eDP, physical px, scale 1.25).

## 0. User's live configuration (MUST replicate)
- Bar **position: bottom** (DMS enum 1). Shown on all outputs.
- `spacing: 4, innerPadding: 4, widgetPadding: 8, transparency: 1.0, widgetTransparency: 1.0`
- `squareCorners: false, noBackground: false, border: off, gothCorners: off, autoHide: off`
- Widgets (exact order):
  - left:   `launcherButton, workspaceSwitcher, focusedWindow`
  - center: `music, clock, weather`
  - right:  `systemTray, clipboard, cpuUsage, memUsage, notificationButton, battery, controlCenterButton`
- Theme: stock **green**, dark. Fonts: Inter Variable / Fira Code, weight 400, scale 1.0.
- Clock: 24h, no seconds, default date format ("ddd d" → "Wed 2").
- Weather: enabled, Celsius, fixed location "New York, NY" `40.7128,-74.0060`.
- Media: enabled (`showMusic`), scrollTitle on, wave visualizer on, mediaSize 1.
- Workspaces: no index, no names, no per-ws app icons, show all (not occupied-only), per-monitor.
- Battery: shown, percent shown (screenshot shows "94%"). No network/keyboard-layout widgets on bar.
- Animation: speed preset 4 with `customAnimationDuration: 100`, **ripple OFF**, blur OFF.
- Dock disabled. Popups opaque (`popupTransparency: 1`).

## 1. Bar container geometry (from DankBarWindow.qml / BarCanvas.qml)
All values at defaults (innerPadding=4, dpr=1):
- `widgetThickness = max(20, 26 + innerPadding*0.6)` = **28** (pill/chip height)
- `effectiveBarThickness = max(widgetThickness + innerPadding + 4, 48 - 4 - (8 - innerPadding))` = **40**
- Window height = effectiveBarThickness + spacing(4) = **44**; exclusive zone = **44** (+bottomGap 0).
- **Floating rect**: 4px margin on left + right + the OUTER edge (bottom edge for a bottom bar);
  0 margin on the inner (desktop-facing) edge. Rounded rect radius **12** on all corners.
- Background: `surfaceContainer` × transparency (user: opaque). Widgets use `surfaceContainerHigh`
  (two-tone look). Elevation **level 2** shadow (blur 8, offset 4 toward outer edge, alpha 0.25 +
  ambient blur 14, alpha 0.125) — nvgBoxGradient approximation acceptable for now.
- Content inset from bar-rect edge to first widget: `max(4, innerPadding*0.8)` = **4** each side.
- Bottom bar flips only: anchor edge, gap side, shadow direction. Everything else identical.

## 2. Layout
- Three sections: left pinned to left inset, right pinned to right inset, center **true-centered**
  on the bar rect (index centering: center around the middle widget of the center array).
- Spacing between widgets within a section: **4**.
- Widget width = content width + 2×horizontalPadding; `horizontalPadding = widgetPadding(8) ×
  (widgetThickness/30)` = **7**.

## 3. BasePill (every widget's container)
- h = 28 (widgetThickness), radius 12 (clamps to stadium), bg `surfaceContainerHigh` ×
  widgetTransparency, vertically centered in the 40px bar rect.
- **Hover** bg: `withAlpha(blend(base, primary, 0.10), max(0.30, alpha))`. Pointer-hand cursor.
- Press: ripple only (user has ripple OFF → no press visual). No 0.08/0.12 state layer on bar pills.
- Icon default color `surfaceText`; text color `surfaceText`.
- Sizes at 40px bar: `barIconSize(-6)` = **15px**, `barIconSize(-4)` = **17px** (battery, bell,
  launcher, CC icons), `barTextSize` = **12px** (fontSizeSmall).
  General formula: `round((barThickness/48) × (26 + offset) × scale)` where barThickness=48-ref;
  DMS: `barIconSize(offset) = round(widgetThickness×(21+offset)/28 …)` — match the computed
  defaults above; keep a single helper in one header, no inline literals.

## 4. Per-widget spec (user's widgets only)
### launcherButton
Icon-only pill: `apps` glyph 17px, color surfaceText.

### workspaceSwitcher
Row of individually colored **capsules** inside one pill container, per-monitor workspaces:
- chip height **28**; active width `max(28×1.05, 15×1.6)` ≈ **30**; inactive **20** = max(19.6,18)→20;
  radius 12 → stadium.
- Active `primary` (green theme → green pill), others dim `surfaceText` low-alpha
  (`surfaceTextAlpha`; hover ×0.7). No index numbers, no app icons.
- Width animates on switch (medium duration, emphasized easing) — frame-callback driven.

### focusedWindow
Text-only (no icon in horizontal bar): `AppName • Title`, 12px, appName+title `surfaceText`,
"•" `outlineButton` (0.5α). Strip trailing " - AppName" from title. Max width 456, elide right.

### music (media)
20×20 `music_note` icon colored `primary` + `Title • Artist` 12px surfaceText +
transport: prev/next 20×20 transparent circles (icon 12px, hover bg only), play/pause **24×24
circle**: bg `primary` when playing (icon color = `background`), `primaryHover` when paused
(icon `primary`). Marquee scroll for overflow (2s pause, 60ms/px) — phase 2; static+elide first.

### clock
Row spacing 8: time "HH:MM" 24h (fixed-width digit cells: each digit width = fontSize×0.6),
"•" 12px `outlineButton`, date "Wed 2". All 12px surfaceText.

### weather
Icon (weather-code glyph, 15px) + "NN°C" 12px. Needs a small weather service:
open-meteo/wttr fetch for 40.7128,-74.0060 (user coords), cache ≥15 min, async (no blocking).
Check DMS WeatherService.qml for the icon mapping + API.

### systemTray
Per-icon 21×21 chips (icon 15px centered, radius→circle), transparent idle, hover bg formula,
letter fallback 10px. Tooltip per item (phase 2).

### clipboard
Icon-only pill (`content_paste`), 17px.

### cpuUsage / memUsage
Read `Widgets/CpuMonitor.qml` / `RamMonitor.qml` for exact anatomy (icon + percent text 12px).
Data from /proc/stat and /proc/meminfo, 3s poll only while visible.

### notificationButton
`notifications` 17px (`notifications_off` when DND, color primary). Unread dot: 6×6 r3 `error`
at icon top-right.

### battery
Material Symbols battery glyph (17px, `battery_*` ligature per level/charging) — REPLACE the
hand-drawn pictograph. Color: `error` low, `primary` charging, else surfaceText. "NN%" 12px.

### controlCenterButton
Compound pill, icons 17px, spacing 4, default-visible sub-icons: **network, vpn(when up),
bluetooth, audio, screenSharing(when active)** (user: controlCenterShowBatteryIcon false,
ShowNetworkIcon true). Each icon `primary` when active/connected else `surfaceText`.
This REPLACES dankc's standalone wifi/bt/volume icons and the dead cellular icon — remove them.

## 5. Interaction
- Hit region = each pill's rect (from the layout pass), not hardcoded x ranges.
- Hover: pointer-motion → track hovered widget → re-render with hover bg (damage-light: only
  re-render on hover change, not every motion event).
- Clicks: launcher→launcher, workspaces→switch ws (per-capsule), clock→notification center? (DMS:
  clock opens dashboard — keep current behavior), bell→notif center, clipboard→clipboard,
  CC pill→control center, media prev/play/next→mpris, battery→battery popout (phase 2: CC).
- Scroll on bar per user config: `scrollYBehavior: "workspace"` (wheel switches workspace),
  `scrollXBehavior: "column"` (niri column focus) — phase 2.

## 6. Icon glyphs (static codepoint font — add to icons.h as needed)
apps, music_note, skip_previous, play_arrow, pause, skip_next, content_paste, notifications,
notifications_off, battery glyphs (battery_full/6_bar/…/charging), wifi/signal_wifi_4_bar,
bluetooth, volume_up, screen_record, vpn_lock/vpn_key, memory (cpu), memory_alt/database (ram),
weather set (clear_day, clear_night, partly_cloudy_day/night, cloud, rain, snow, thunderstorm, fog).
Codepoints: grep the Material Symbols codepoints file or use `fc-query`/python fontTools on
`assets/fonts/MaterialSymbolsRounded*.ttf` (vendored in dankc assets).

## 7. Staged delivery (each stage = one commit, make + meson green, grim-verified)
- **S1 container**: floating rounded bar, 40/44px geometry, bottom anchor from config,
  surfaceContainer bg + shadow, config keys (position/spacing/innerPadding/widgetPadding/
  transparency/widgetTransparency).
- **S2 widget host**: BasePill helper + tokens header (barIconSize/barTextSize/hpad),
  3 sections driven by config widget arrays, layout pass emits per-widget hit rects,
  existing widgets rehomed (even if not yet pixel-perfect).
- **S3 widget parity A**: launcher, workspaces (capsules), focusedWindow, clock, battery
  (glyph), bell, clipboard, tray chips.
- **S4 widget parity B (new)**: music, weather (+service), cpuUsage, memUsage,
  controlCenterButton compound; remove dead cellular + standalone wifi/bt/volume.
- **S5 DMS config import**: read DMS settings.json (position, arrays, paddings, theme name,
  clock/weather/media settings) when present; dankc config.json overrides.
- **S6 interaction**: hover states + cursor, per-pill hit regions everywhere, workspace capsule
  width animation via frame callbacks, scroll behaviors.
