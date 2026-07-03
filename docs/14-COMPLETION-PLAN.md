# DankC — Completion Plan (structure-first, 2026-07-03)

Master gap list vs DankMaterialShell (DMS), reference: `~/Downloads/DankMaterialShell-master/quickshell/`.
Baseline: AGENTS.md "BACKLOG COMPLETE" @ `189045d` — bar, all 12 panels, and the P1–P7 polish/perf
backlog are done. This plan covers what's left of the **structure**: settings-parity gaps, a few
genuinely missing daily-use flows (Wi-Fi password entry, Bluetooth pairing, polkit prompts), and visual
fidelity (true HCT color, light theme). **No performance work is included** — P7 is done and further
optimization is explicitly deferred per the user's "structure first" instruction.

Tasks are grouped into **waves**: everything in a wave touches disjoint files and can be dispatched to
parallel cheap agents (1 task = 1 agent, ~1–3h). Waves are ordered — later waves may assume earlier waves
merged, either because of a real dependency or because two tasks would otherwise touch the same file.

Legend: **DONE** (not listed below) / **PARTIAL** (exists, gap noted) / **MISSING** (net new) / **SKIP**
(see "Deliberately out of scope").

---

## Wave 0 — Foundation (sequential, 1 agent, blocks Wave 2)

### W0.1 — Split `settings.c` into a per-tab module + pre-register all new tabs
- **Why**: `src/ui/settings.c` (1369 lines) has a single `TAB_*` enum + `TABS[]` table + one big
  render/click switch. Wave 2 adds ~8 new tabs; if every task edits this one file in parallel, every
  agent conflicts with every other agent. This task removes that bottleneck once, up front.
- **DMS reference**: `Modules/Settings/*Tab.qml` (30+ tab files), `Modals/Settings/SettingsSidebar.qml`
  (tab list), `Modals/Settings/SettingsContent.qml` (dispatch).
- **dankc files**: `src/ui/settings.c`, `src/ui/settings.h` → split into `src/ui/settings/core.c`
  (window/sidebar/dispatch, keeps the `dc_settings_*` public API in `settings.h` unchanged) +
  `src/ui/settings/tab_<name>.c` per existing tab (personalization, time, bar, widgets, weather,
  displays, notifications, launcher, power, about) + **stub** `tab_<name>.c` files (render a
  "Coming soon" placeholder) for every tab Wave 2/3 will fill in: `dock`, `frame`, `osd`, `typography`,
  `network`, `system`, `audio_locale`, `default_apps_autostart`, `lockscreen`, `theme_colors`.
- **Acceptance**:
  - `make` and `meson` both build clean; `dankc ctl settings` opens, all existing 9 tabs behave
    identically to before the split (byte-for-byte same rendering logic, just relocated).
  - All 10 new stub tabs appear in the sidebar (icon + label matching DMS's `SettingsSidebar.qml`
    order) and open without crashing.
  - A short comment block at the top of `core.c` documents "add a new tab" as: create
    `tab_x.c` implementing `dc_settings_tab_x_render/click`, register one line in the `TABS[]` table —
    so Wave 2 tasks touch **only their own new file** plus that one pre-existing registration line
    (already added by this task, so Wave 2 doesn't need to touch `core.c` at all).
- **Size**: M

---

## Wave 1 — High-impact PARTIAL fixes (parallel, independent files)

### W1.1 — Wi-Fi password entry for secured networks
- **Gap**: `src/ui/controlcenter.c:141-145` — clicking an unknown secured SSID only shows a "Needs
  Password" hint; there is no inline password field or connect call. Users cannot join a new secured
  Wi-Fi network at all today.
- **DMS reference**: `Modals/WifiPasswordModal.qml`, `Services/NetworkService.qml`
  (`connectToNetwork`/`nmcli device wifi connect ... password ...`).
- **dankc files**: `src/services/net.c` (add `dc_net_connect_wifi(ssid, psk)` via `nmcli device wifi
  connect <ssid> password <psk>` or `iwd`'s `iwctl`), `src/ui/controlcenter.c` (inline password field
  in the expanded Wi-Fi row, reusing the existing text-input pattern from the launcher search field).
- **Acceptance**:
  - Clicking a secured, not-yet-known SSID opens an inline password field (Enter to submit, Esc to
    cancel); wrong password shows an inline error and stays open; correct password connects and the
    row flips to "Connected".
  - Known/already-configured secured networks still one-click connect (no regression).
- **Size**: M

### W1.2 — XDG autostart
- **Gap**: no autostart handling anywhere in `src/` — DMS/most DEs launch `~/.config/autostart/*.desktop`
  and `/etc/xdg/autostart/*.desktop` at session start; dankc launches nothing.
- **DMS reference**: `Modules/Settings/AutoStartTab.qml`.
- **dankc files**: new `src/services/autostart.c`/`.h`, one call from `src/main.c` after the Wayland
  connection + niri IPC are up.
- **Acceptance**:
  - On startup, every `.desktop` in `~/.config/autostart` and `/etc/xdg/autostart` is launched
    (`Exec=`, minus field codes) unless `Hidden=true`, `X-GNOME-Autostart-enabled=false`, or
    `NotShowIn`/`OnlyShowIn` excludes/fails to include `niri`.
  - Verified with one throwaway `.desktop` (e.g. `Exec=notify-send hi`) actually firing on next login.
- **Size**: S

### W1.3 — Notification sounds
- **Gap**: no sound playback anywhere in `src/` (`grep -rn canberra|pw-play|paplay` empty). DMS plays a
  freedesktop sound-theme sound on notify/critical/dismiss.
- **DMS reference**: `Services/AudioSoundPlayers.qml`, `Modules/Settings/SoundsTab.qml`,
  `assets/sounds/freedesktop/`, `assets/sounds/plasma/`.
- **dankc files**: new `src/services/sound.c`/`.h` (resolve freedesktop sound-theme name → file via the
  same theme-dir search pattern as `services/icons.c`, play via `pw-play`/`paplay` fork+exec, fire-and-
  forget), hook one call into `src/services/notifications.c` on `Notify()`.
- **Acceptance**:
  - A normal notification plays `message-new-instant` (or theme equivalent); a critical one plays a
    distinct sound; config key `notif_sounds_enabled` (default on) mutes it.
  - Missing sound-theme/binary degrades silently (no crash, no log spam beyond one warning).
- **Size**: S

### W1.4 — Notification Center grouping + scroll
- **Gap**: POLISH.md P4 lists "grouping by app… scroll for long history" as not done; `ui/notifcenter.c`
  currently renders a flat list clipped to the visible card height.
- **DMS reference**: `Modules/Notifications/Center/*`.
- **dankc files**: `src/ui/notifcenter.c` only.
- **Acceptance**:
  - History longer than the card height scrolls via mouse wheel (matches the existing scroll pattern
    already used in `ui/processes.c` or `ui/dashboard.c`).
  - Consecutive entries from the same app collapse into a group header with a count badge, matching
    DMS's grouping visually (expand-on-click acceptable if full inline expand is out of scope).
- **Size**: S

### W1.5 — Keybind cheat-sheet overlay
- **Gap**: `dankc keybinds` only prints/generates the niri KDL snippet; there is no on-screen visual
  cheat sheet (DMS's `?`/Mod+/ overlay).
- **DMS reference**: `Modals/KeybindsModal.qml`, `Modals/KeybindsContent.qml`,
  `Modals/KeybindsModalOverlay.qml`.
- **dankc files**: new `src/ui/keybinds_modal.c`/`.h`, one dispatch case added to `src/ipc/control.c`
  (`dankc ctl keybinds-overlay`).
- **Acceptance**:
  - `dankc ctl keybinds-overlay` opens a centered card listing categorized shortcuts (bar/launcher/
    control-center/lock/screenshot/clipboard — reuse the list already generated for the KDL snippet),
    dismisses on outside-click/Esc, matches the existing modal chrome (rounded card + material bg).
- **Size**: S

---

## Wave 2 — Settings tab content (parallel; each task owns exactly one new `tab_*.c` file from W0.1)

Each task below fills in one of the stub tabs W0.1 created. None touch `core.c` or any other task's
file, so all can run fully in parallel once W0.1 merges. "Full parity" is not the bar — wire the handful
of settings with real user-visible effect per category; leave clearly cosmetic/niche keys as a `TODO`
comment referencing the exact `docs/09-DMS-SETTINGS-INVENTORY.md` line.

### W2.1 — Dock settings tab
- **DMS ref**: `Modules/Settings/DockTab.qml` (29 settings, mostly cosmetic — wire the load-bearing ones).
- **dankc files**: `src/ui/settings/tab_dock.c`, reads/writes existing `config.h` keys
  (`dock_enabled`, `dock_auto_hide`, `dock_icon_size`, `dock_pinned[]`).
- **Accept**: toggle dock on/off and auto-hide live-applies (dock surface created/destroyed without
  restart); icon-size slider changes dock immediately; pinned-apps list add/remove persists to
  `config.json`.
- **Size**: S

### W2.2 — Frame settings tab
- **DMS ref**: `Modules/Settings/FrameTab.qml` (20 settings — wire the 3 dankc actually implements).
- **dankc files**: `src/ui/settings/tab_frame.c`, existing `config.h` keys (`frame_enabled`,
  `frame_radius`, `material_blur`).
- **Accept**: toggling frame/blur live-applies without restart; radius slider updates the corner overlay.
- **Size**: S

### W2.3 — On-screen Displays settings tab
- **DMS ref**: `Modules/Settings/OSDTab.qml` (12 settings: position, size, auto-hide timeout per OSD type).
- **dankc files**: `src/ui/settings/tab_osd.c`, new `config.h` keys (`osd_timeout_ms`, `osd_position`),
  `src/ui/osd.c` reads them instead of hardcoded constants.
- **Accept**: changing the timeout slider changes how long the next volume/brightness OSD stays up;
  position toggle (bottom-center vs bottom-left etc.) moves the OSD surface.
- **Size**: S

### W2.4 — Typography & Motion settings tab (extends existing MOTION section)
- **DMS ref**: `Modules/Settings/TypographyMotionTab.qml` (17 settings).
- **dankc files**: `src/ui/settings/tab_typography.c` (merge the existing `personalization` MOTION
  section's sliders in here per DMS's grouping), `config.h` (`font_scale`, `icon_weight`), `render/nvg.c`
  reads `font_scale` when sizing text.
- **Accept**: font-scale slider visibly changes bar/panel text size live; animation-speed control
  (already in personalization) is reachable from here too (moved, not duplicated).
- **Size**: S

### W2.5 — Network + Running Apps settings tab
- **DMS ref**: `Modules/Settings/NetworkStatusTab.qml`/`NetworkWifiTab.qml` (6 settings),
  `Modules/Settings/RunningAppsTab.qml` (2 settings).
- **dankc files**: `src/ui/settings/tab_network.c`.
- **Accept**: shows current interface/IP/link-speed (already known to `services/net.c`) read-only, plus
  a toggle for "show running apps in taskbar-style widget" if such a widget exists, else documents it's
  a no-op placeholder with a `TODO`.
- **Size**: S

### W2.6 — System settings tab
- **DMS ref**: `Modules/Settings/PowerSleepTab.qml`/system-level entries not already in the Power tab
  (12 settings — idle timeouts, suspend-on-lid, etc.).
- **dankc files**: `src/ui/settings/tab_system.c`, extends `src/services/logind.c` / `src/services/
  power.c` idle-timeout config already partially present.
- **Accept**: idle-timeout slider changes when `IdleService`-equivalent triggers lock/DPMS; verified via
  a short (10s) test timeout.
- **Size**: S

### W2.7 — Audio + Locale settings tab
- **DMS ref**: `Modules/Settings/AudioTab.qml` (3), `Modules/Settings/LocaleTab.qml` (3).
- **dankc files**: `src/ui/settings/tab_audio_locale.c`.
- **Accept**: default sink/source picker (wpctl set-default) works; locale display is read-only (i18n
  itself stays deferred per docs/07 G10 — this task is UI plumbing only, not translation).
- **Size**: S

### W2.8 — Default Apps + Autostart settings tab
- **DMS ref**: `Modules/Settings/DefaultAppsTab.qml` (1), `Modules/Settings/AutoStartTab.qml` (2).
- **dankc files**: `src/ui/settings/tab_default_apps_autostart.c`. Depends on W1.2 (autostart service)
  landing for the autostart half to be functional; if W1.2 hasn't merged yet, ship the tab with the
  autostart toggle wired to a stub that's a no-op with a log line — do not block on it.
- **Accept**: default-apps section writes `~/.config/mimeapps.list` entries per docs/07-GAPS §4's GNOME
  profile table when a role (Files/Terminal/Browser/etc.) is picked from a dropdown of installed apps.
- **Size**: S

---

## Wave 3 — Larger standalone surfaces (parallel; independent files — see ordering notes)

### W3.1 — Bluetooth pairing flow
- **Ordering note**: touches `src/ui/controlcenter.c` — **do not run concurrently with W1.1** (same
  file); schedule after W1.1 merges.
- **Gap**: `src/services/bluez.c:181` explicitly notes "agent/pairing fallbacks dankc doesn't implement" —
  only already-paired/connected devices are listed; there's no way to discover/pair a new device.
- **DMS reference**: `Modals/BluetoothPairingModal.qml`, `Services/BluetoothService.qml`.
- **dankc files**: `src/services/bluez.c` (add `StartDiscovery`/`StopDiscovery`, a minimal
  `org.bluez.Agent1` implementation registered via `RegisterAgent`/`RequestDefaultAgent` handling the
  `DisplayPasskey`/`RequestConfirmation`/no-input-no-output "just works" cases, `Pair()`+`Trust()`+
  `Connect()`), `src/ui/controlcenter.c` ("Pair new device" row → nearby unpaired list → tap to pair).
- **Accept**: a real nearby Bluetooth device (phone/earbuds) appears in a "nearby" list within a few
  seconds of clicking "Pair new device", and tapping it completes pairing + connects, without an
  external `bluetoothctl` session running.
- **Size**: M

### W3.2 — Lock Screen settings tab
- **DMS reference**: `Modules/Settings/LockScreenTab.qml` (23 settings).
- **dankc files**: `src/ui/settings/tab_lockscreen.c`, `config.h` additions (`lock_blur_amount`,
  `lock_show_media_controls`, `lock_clock_format`), `src/ui/lock.c` reads them.
- **Accept**: at least clock format, blur amount, and "show media controls on lock" are live-editable
  and visible next time `dankc ctl lock` is triggered.
- **Size**: M

### W3.3 — Theme & Colors deep settings tab (UI plumbing only — sits on top of W4.1's color engine)
- **DMS reference**: `Modules/Settings/ThemeColorsTab.qml`, `ColorDropdownRow.qml`,
  `Modules/DankColorPickerModal.qml` (71 settings — the single largest category).
- **dankc files**: `src/ui/settings/tab_theme_colors.c`, `src/theme/theme.h` (per-role override struct if
  not already present).
- **Accept**: primary/secondary/surface role color swatches are pickable (reuse
  `Modals/DankColorPickerModal.qml`'s HSV-picker idea — a simple hue/sat/val picker is enough, doesn't
  need to be pixel-identical) and override the active theme live; "reset to theme default" works per
  swatch.
- **Size**: L

### W3.4 — Niri window-rules editor
- **DMS reference**: `Modules/Settings/WindowRulesTab.qml`, `Modals/WindowRuleModal.qml`.
- **dankc files**: new `src/ui/window_rules.c`/`.h`, `src/niri/niri.c` (read/write the user's niri KDL
  window-rule block, or a dankc-managed included KDL fragment to avoid clobbering hand-written config).
- **Accept**: adding a rule (app-id match → float/workspace/opacity) through the UI writes a KDL fragment
  niri picks up on reload (`niri msg action load-config-file` or equivalent), without touching the rest
  of the user's `config.kdl`.
- **Size**: M

### W3.5 — Polkit authentication agent
- **Gap**: fully speced in `docs/03-SERVICES.md §10` but **zero implementation** — no polkit agent runs
  today, so any privileged GUI action system-wide (mount a drive in a file manager, package-manager GUI,
  etc.) silently fails with no prompt. This is the highest-impact MISSING item in the whole plan.
- **DMS reference**: `Modals/PolkitAuthModal.qml`, `Modals/PolkitAuthContent.qml`,
  `Modals/PolkitAuthSurfaceModal.qml`, `Services/PolkitService.qml`.
- **dankc files**: new `src/services/polkit.c`/`.h` (registers as the session's polkit agent via
  `org.freedesktop.PolicyKit1.Authority.RegisterAuthenticationAgent`, exposes
  `org.freedesktop.PolicyKit1.AuthenticationAgent.BeginAuthentication`/`CancelAuthentication` on the
  session bus, shells out to `/usr/lib/polkit-1/polkit-agent-helper-1 <user>` and feeds the password to
  its stdin per docs/03 §10), new `src/ui/polkit_modal.c` (password prompt card, reuses lock screen's
  PAM-adjacent UI pattern).
- **Accept**: `pkexec true` (or a real privileged GUI action, e.g. mounting a drive in Nautilus) pops a
  dankc modal instead of failing silently; correct password approves and the action completes; wrong
  password re-prompts with an inline error; verified with no other polkit agent (e.g. polkit-gnome)
  running to avoid a registration race.
- **Size**: L

---

## Wave 4 — Visual/color depth (parallel, independent files)

### W4.1 — True HCT / Material Color Utilities dynamic color
- **Gap**: `src/theme/dynamic.cpp` (179 lines) is documented as an "HSL-tone approximation of MCU" —
  AGENTS.md explicitly lists "true HCT/MCU dynamic color" as a known remaining gap.
- **DMS reference**: `matugen/` (the actual matugen tool DMS shells out to / its config templates),
  DMS's own fallback color math if present in `Services/`.
- **dankc files**: `src/theme/dynamic.cpp` (rewrite the seed→palette step: sRGB→CAM16/HCT conversion,
  tonal palette generation at DMS's exact tone stops, source-color selection via Celebi quantization
  instead of a plain chroma-weighted histogram).
- **Accept**: for 3 test wallpapers, the generated primary/surface tones visually match `matugen image
  <wp> -m dark -j hex` output (if `matugen` binary is available; else match the tone-stop table in
  `docs/10-DESIGN-SYSTEM.md`) — no more washed-out/off-hue palettes vs DMS's dynamic color on the same
  wallpaper.
- **Size**: L

### W4.2 — Light theme variants + dark/light toggle
- **Gap**: `src/theme/theme.c` is 78 lines, dark-only; AGENTS.md lists "light theme variants" as a known
  remaining gap. All 10 stock themes only have their DARK variant generated (`stock_themes.inc`).
- **DMS reference**: `matugen/templates` / DMS's `StockThemes.js`-equivalent light palettes (check both
  DARK and LIGHT rows per stock theme).
- **dankc files**: `src/theme/theme.c`, `src/theme/theme.h`, the `stock_themes.inc` generator script
  (find it under `scripts/`), `config.h` (`dark_mode` bool), the existing personalization/theme settings
  section (add a dark/light toggle — this touches the *existing* tab, so land this after Wave 0's split
  so it's editing a small standalone `tab_personalization.c`, not the monolith).
- **Accept**: toggling dark/light in settings live-swaps every open surface (bar/panels) to the light
  variant of the current theme and persists; all 10 stock themes have a light variant, not just green.
- **Size**: M

---

## Wave 5 — Lower-priority MISSING (only if time remains; independent files)

### W5.1 — Multiplexer tab (tmux/zellij session list)
- **DMS ref**: `Modules/Settings/MuxTab.qml`, `Modals/MuxModal.qml`, `Services/MuxService.qml` (3 settings).
- **dankc files**: new `src/ui/settings/tab_mux.c`. **Size**: S.

### W5.2 — System Updater tab
- **DMS ref**: `Modules/Settings/SystemUpdaterTab.qml`, `Services/SystemUpdateService.qml` (2 settings) —
  pacman/checkupdates count badge, relevant since the dev machine is Arch.
- **dankc files**: new `src/ui/settings/tab_system_updater.c`. **Size**: S.

### W5.3 — Printer (CUPS) tab
- **DMS ref**: `Modules/Settings/PrinterTab.qml`, `Services/CupsService.qml` (low priority — no evidence
  of printer use on this machine; include only if all higher waves finish). **Size**: M.

### W5.4 — Users tab
- **DMS ref**: `Modules/Settings/UsersTab.qml`, `Services/UsersService.qml`, `Modals/SwitchUserModal.qml`
  (5 settings — mostly irrelevant on a single-user desktop; lowest priority in the whole plan).
  **Size**: S.

---

## Deliberately out of scope

- **T29 plugins (native `.so` ABI)** — locked project decision, post-v1 by design (see AGENTS.md,
  memory `project-dms-c-rewrite`). Not included anywhere above.
- **Hyprland / Mango(WC) / Sway-gated settings** (`isHyprland`/`isMango`/`isSway` in
  `docs/09-DMS-SETTINGS-INVENTORY.md` — ~40 settings: `hyprlandLayout*`, `mangoLayout*`,
  `hyprlandResizeOnBorder`, etc.) — dankc is niri-only by design; these compositors' settings have no
  meaning here.
- **Greeter (greetd login screen)** — `Modules/Greetd/*`, `Modals/Greeter/*` are DMS's own separate QML
  entrypoint run *by* greetd before session start, not a panel inside the running shell. Reimplementing
  a full greetd-integrated login UI is a distinct binary/project, not "shell structure."
- **Desktop Widgets / `BuiltinDesktopPlugins`** (standalone desktop clock, system-monitor blob widget) —
  DMS implements these through its plugin component system (`Modules/Plugins/*`); bundled under the T29
  plugin deferral rather than special-cased.
- **VPN detail panels / Tailscale** — `Modules/Settings/NetworkVpnTab.qml`, `Services/VPNService.qml`,
  `Services/TailscaleService.qml`. Niche, no evidence of use; add post-v1 on request.
- **Calendar event backends** (khal integration — `Services/CalendarKhalBackend.qml`) — the dashboard
  calendar date-grid card already exists (`ui/dashboard.c:draw_calendar_card`); wiring a real event
  *source* is a distinct, lower-value feature than the settings/daily-use gaps above.
- **Niri workspace-overview overlay** (`Modules/WorkspaceOverlays/NiriOverviewOverlay.qml`) — niri ships
  its own native overview (Mod+O); DMS's overlay mainly serves Hyprland/Sway, which lack one.
- **Generic File Browser modal** (`Modals/FileBrowser/*`) — dankc already has purpose-built pickers for
  the flows that need one (wallpaper grid, clipboard history); a generic open/save dialog is deferred
  until a concrete flow needs it.
- **Performance / optimization work of any kind** — P7 perf is done (AGENTS.md, 2026-07-03); the user
  explicitly asked for structure first, optimization later. No task above touches render-loop
  performance, polling cadence, or memory footprint.

---

## Wave summary (compact, for dispatch)

- **W0** (sequential, blocks W2): settings.c tab-module split + stub-register 10 new tabs
- **W1** (parallel): Wi-Fi password entry · XDG autostart · notification sounds · notif-center
  grouping/scroll · keybind cheat-sheet overlay
- **W2** (parallel, after W0): Dock tab · Frame tab · OSD tab · Typography & Motion tab · Network +
  Running Apps tab · System tab · Audio + Locale tab · Default Apps + Autostart tab
- **W3** (parallel; W3.1 after W1.1 merges): Bluetooth pairing flow · Lock Screen tab · Theme & Colors
  deep tab · Niri window-rules editor · Polkit authentication agent
- **W4** (parallel): true HCT/MCU dynamic color · light theme variants + dark/light toggle
- **W5** (parallel, stretch/only-if-time): Multiplexer tab · System Updater tab · Printer tab · Users tab
- **Out of scope**: plugins (T29) · Hyprland/Mango/Sway settings · Greeter · Desktop Widgets/plugin-based
  widgets · VPN/Tailscale · calendar event backends · niri workspace-overview overlay · generic File
  Browser · any performance work

---

## COMPLETION LOG (2026-07-03)

**ALL WAVES COMPLETE.** Every task above merged to main, both build systems zero-warning.
Delivered W0–W5 plus the pre-plan polish waves. Only intentional deferrals remain:
- **T29 native .so plugins** — post-v1 by project decision.
- **Performance optimization** — explicitly deferred by the user: "structure first, then
  optimize." Footprint is RSS ~170MB / Pss ~77MB (vs DMS qs ~595MB); the next phase is
  reducing it further (Mesa/glyph-cache trimming, damage-tracking already partially done in
  the P7 pass, poll consolidation, lazy panel EGL teardown).
- Hyprland/Mango/Sway-gated settings, greeter binary, VPN/Tailscale, calendar event
  backends, niri workspace-overview (native Mod+O), generic file browser — out of scope.
