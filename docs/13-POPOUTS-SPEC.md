# DankC — Popout/Panel Parity Spec (from user's live DMS screenshots, 2026-07-02)

Reference screenshots (user-provided, on disk):
`~/Pictures/Screenshots/Screenshot from 2026-07-02 14-17-*.png` … `14-19-*.png`
(control center, battery, notifications, clipboard, dashboard Media/Overview/Weather/
Wallpapers, launcher). QML sources: `quickshell/Modules/{ControlCenter,Notifications,
Dashboard or DankDash,AppDrawer,...}`. Verify exact tokens in QML before implementing.

## 0. Global popout rules (bottom bar!)
- Every popout opens ABOVE the bottom bar, anchored near its bar widget (CC → bottom-right,
  battery → above battery chip, clipboard/notif → near their icons, dashboard → center,
  launcher → bottom-left). dankc currently anchors popouts top-right with fixed margins —
  must become bar-position-aware (open on the bar's side, offset = barThickness + spacing + gap).
- Style: surfaceContainer bg (user popupTransparency=1 → opaque), radius 12+, elevation shadow,
  cards inside use surfaceContainerHigh/Highest. Green theme accents throughout.

## 1. Control Center (14-17-26) — clicking the CC pill
- User header card: avatar circle, username, "Unknown" (hostname/uptime), right icons:
  lock, power, settings gear, edit pencil.
- Two sliders side by side: volume (speaker icon) + brightness (gear icon?) — green fill,
  rounded, full-width halves.
- Tile grid 2 columns: [wifi "BAIHQ 73%" | bluetooth "Enabled · No devices"]
  [audio out "Built-in Audio Analog St… 100%" | mic "…Muted" (muted icon, dim)]
  [Night Mode (ACTIVE = solid green tile) | Dark Mode (ACTIVE = solid green tile)].
  Active tiles = filled primary with dark text; inactive = dark card with green icon chip.
- dankc has a basic CC (2×2 + sliders): needs user header, audio in/out device tiles,
  active-tile styling, exact ordering per user config
  (volumeSlider, brightnessSlider, wifi, bluetooth, audioOutput, audioInput, nightMode, darkMode).

## 2. Battery popout (14-17-35) — clicking the battery chip — NEW surface
- Header: big "94%" + "Fully Charged" + battery icon (green), X close.
- Two stat cards: "Health 64%" | "Capacity 31.8 Wh" (sysfs energy_full/_design).
- Power-profile segmented buttons: Power Saver / Balanced / Performance (✓ active, green)
  → power-profiles-daemon over D-Bus (or `powerprofilesctl`).

## 3. Notification Center (14-17-48)
- Header: "Notifications" + bell, right: info, settings, "Clear" button (with icon).
- Tabs: pill "Current (3)" (active green) / "History (5)".
- Cards: app icon circle, "app-name • time", title (bold), body (dim), X top-right,
  "Dismiss" (and "Open" when actionable) text-buttons bottom-right.
- dankc has: flat 64-entry list + Clear-all. Needs tabs/current-vs-history, card anatomy,
  per-card actions.

## 4. Clipboard History (14-18-06)
- Header: clipboard icon + "Clipboard History (75)", right: refresh?, info, clear, X.
- Search field: rounded, green outline when focused, magnifier icon.
- Entries: numbered green circle, thumbnail for images ("[[ image 79 KiB png 485x608 ]]"
  label + actual preview) or text preview ("Text"/"Long Text" tag), pin + delete per row.
- dankc has: basic picker. Needs search, image thumbnails, pin, delete, count.

## 5. Dashboard ("DankDash") — NEW surface, tabs on top
Tabs: Overview / Media / Wallpapers / Weather / Settings (green underline on active).
- **Clock chip click → Overview** (14-18-46): big time (HH/MM stacked, green), date;
  weather card (icon, temp, condition); user card (avatar, name, "on Niri", uptime);
  month calendar (today = green circle); vertical meters column (cpu/temp/ram icons below);
  media mini-card (art, title, artist, progress, transport).
- **Music chip click → Media tab** (14-18-37): blurred album-art bg, circular art,
  title (bold), artist (green), progress bar + elapsed/total, prev / play(green circle) / next,
  right rail: 3 small round buttons (queue/output/…).
- **Weather chip click → Weather tab** (14-18-52): current card (big temp + condition +
  "Feels Like 32°" + location; humidity/wind/pressure/precip/sunrise/sunset grid),
  date-time spinner row, twilight arc graphic, Daily/Hourly pills, 7-day forecast cards
  (day, icon, hi°/lo°; today highlighted green). Needs forecast data (Open-Meteo daily API —
  extend services/weather.c).
- **Wallpapers tab** (14-19-05): grid browser, empty state "No wallpapers found" +
  folder-browse button in footer.
- Settings tab = existing settings UI.

## 6. App Launcher (14-19-19) — clicking apps chip on a bottom bar
- Anchored bottom-left above the bar (not centered spotlight!) when opened from the bar.
- Search field top (rounded, green focus outline). "Applications 24" section label,
  view-mode toggles right (list/grid/compact).
- Rows: icon, name (bold), generic description (dim). Selected row = green tint bg.
- Footer: filter pills "All / Apps (active) / Files / Plugins", right hints
  "↑↓ nav · ⏎ open · Tab actions".
- dankc has: centered spotlight with fuzzy search. Needs: bottom-left anchor mode,
  descriptions, footer, view modes (list first).

## 7. Delivery order (after bar S5/S6)
P4a popout-anchor rework (bar-position-aware, all existing popouts) →
P4b Control Center parity → P4c Notification Center parity → P4d Clipboard parity →
P4e Battery popout (new) → P4f Dashboard shell + Overview/Media/Weather tabs →
P4g Launcher parity. Each: QML-check → implement (Sonnet) → grim vs reference → commit.
