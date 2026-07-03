# DankC vs DankMaterialShell — Remaining Feature Gap (2026-07-04)

Synthesized from exhaustive DMS module catalogs (Plugins/Notepad/Frame, Notifications/OSD,
Dock/AppDrawer/Launcher, all Settings tabs, DankBar widgets). dankc is now broadly at DMS
parity for core surfaces; below is what DMS has that dankc lacks, triaged.

## dankc is AHEAD of DMS in places
- Launcher **calculator** (DMS has none — it's a plugin there).
- Native C footprint (~48MB Pss vs DMS ~590MB).

## BUILD (clearly useful, moderate effort — do autonomously if budget allows)
| Feature | What it is | Effort |
|---|---|---|
| **Notepad** | Tabbed notes, autosave, session buffers, slideout+popout, markdown preview | M |
| **More OSD variants** | DMS has 9 (dankc has 2): mic mute, caps-lock, media play/pause, media-volume, power-profile, audio-output-switch, idle-inhibitor | S each |
| **Notif DND scheduling** | DND duration presets (15m/1h/until-8am/indefinite) + per-app mute rules + history time-filter chips + privacy mode + keyboard nav | M |
| **Bar widgets (missing)** | VPN, keyboard-layout, network-speed, disk-usage, cpu-temp, gpu-temp, privacy-indicator (mic/cam/screenshare), idle-inhibitor, caps-lock, color-picker, power-menu, system-update, notepad buttons | S each |
| **Battery protection** | Charge limit slider (pkexec), auto power-saver, low/critical thresholds+notifs, per-AC/battery profile auto-switch | M |
| **Audio per-device mgmt** | Rename/hide devices, per-device max-volume (100-200%), input+output cards | M |
| **Keybinds EDITING** | dankc has read-only overlay; DMS edits binds (capture chord, add/remove/reset, writes niri dms/binds.kdl) | M |
| **Power&Sleep depth** | Per-AC/Battery idle timeouts (fade-to-lock, monitor-off, suspend, hibernate), power-menu grid/visibility/hold-to-confirm/custom-actions | M |

## HOLD for user decision (big effort / preference / niche)
| Feature | Why hold |
|---|---|
| **Plugin system** (.so or QML-style) | Large framework; deferred by project design (T29); post-v1 |
| **Desktop widgets** (clock, sysmon on desktop) | Needs a desktop-layer + drag/resize framework (tied to plugins) |
| **Greeter** (greetd login manager) | Separate binary + system integration; big |
| **Connected-frame chrome** | Elaborate bezel stitching bar/dock/popouts; niche visual polish, L |
| **Multi-bar / per-monitor bar config** | DMS runs N bars each per-monitor with own widgets; dankc has 1 bar. Moderate-large |
| **System Updater full UI** | pacman/AUR/flatpak popout w/ live log + polkit; useful but M-L |
| **Screen recording** (screencast) | dankc has screenshot only; separate pipeline |
| **VPN full mgmt** | If user uses VPN — bar widget + popout + connect (could be BUILD if wanted) |
| **Per-widget config / overrides / hover-popouts / goth corners** | Deep bar-config system; many small; incremental |

## Recommended morning shortlist (highest value first)
1. Notepad (M) — genuinely useful daily tool
2. Missing OSD variants — mic/caps-lock/media (S, high daily visibility)
3. Notification DND scheduling + per-app mute (M)
4. Keybinds editing (M) — makes the existing overlay actually useful
5. The missing bar widgets the user actually wants (pick from the list)
6. Battery protection + Power&Sleep depth (M) — laptop-relevant

Everything else = HOLD until user picks.

## Addendum (from theming/IPC catalogs)
- **⭐ Matugen system-wide theming** (HIGH VALUE): DMS regenerates GTK3/4, Qt5/6ct, KDE, terminals
  (kitty/foot/alacritty/ghostty/wezterm), Firefox/Zen, Discord (vesktop/vencord), Neovim/Emacs/Zed/
  VSCode, dgop — all recolored from the wallpaper/accent via the `matugen` binary + per-app template
  toml/templates, gated by ~24 per-app toggles, live-applied (gsettings/SIGUSR reload). dankc themes
  ONLY itself. This is the single biggest "cohesive desktop" gap. Effort M-L (matugen is external;
  mostly shipping the config/template set + invoking matugen + the settings toggles). RECOMMEND: build
  — makes the whole desktop match, high daily visibility.
- **Richer IPC** (`dms ipc call <domain> <verb>`, ~90 commands): dankc has `dankc ctl` (fewer). Extend
  incrementally as needed. S per command.
- **Per-monitor wallpaper + light/dark wallpaper + wallpaper cycling schedule**: DMS session config. M.
