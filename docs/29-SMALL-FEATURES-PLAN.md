# dankc Remaining Small Group-1 Features — Plan (2026-07-04)

Architecture facts: IPC control.c is ONE-WAY (no reply path — new verbs fire-and-forget; "get" verbs
deferred). Verb dispatch main.c control_dispatch() L514 flat strcmp; run_sh() L503 detached; SIGCHLD
SIG_IGN (probe children with kill(pid,0)). dankc does NOT paint wallpaper — dashboard.c wall_apply_
compositor() L1391 pkill+respawn swaybg -m fill -i; dankc only samples for dynamic color/blur/lock.
1Hz clock_tick main.c L231. Bar widgets = static table bar.c find_widget() L1637 {id,measure,draw,has_bg,
custom_hit,region}; users opt-in via widget arrays (invisible until configured). NM D-Bus mature (net.c
1996 lines: ListConnections+GetSettings enum, ActivateConnection/DeactivateConnection, DANKC_NET_DRYRUN);
ZERO VPN today. settings.c immediate-mode helpers; TAB_SYSTEM_UPDATER exists L3733 (Arch checkupdates only).

## Priority: (2)IPC-verbs[S,first,unblocks keybinds] → (5)screen-rec[S-M isolated] ∥ (1)VPN[M-L D-Bus] →
(4)updater[M] → (3)wallpaper[M]. main.c highest contention (all touch it — sequence verb/tick/widget adds).

## 1. VPN (net.c/h + settings.c + bar.c/main.c): dc_net_vpn_list (ListConnections+GetSettings accept
connection.type vpn|wireguard; read NM ActiveConnections for active flag) / _activate(ActivateConnection
path "/" "/") / _deactivate(match ac_path, DeactivateConnection), DANKC_NET_DRYRUN. v1 = SAVED profiles
only (secret-agent DEFERRED — activation-fail surfaces NM error). UI: Settings Network tab VPN section
(clone SAVED-WIFI L2735) + optional `vpn` bar widget (shield, primary when active). CC tile DEFERRED.
Tasks: T1 net.c API, T2 settings VPN section, T3 vpn bar widget.

## 2. Richer IPC (main.c only, ONE batch task): add verbs mapping to existing calls: volume set/up/down/
mute (dc_audio_set_volume + wpctl mute toggle), mic mute, brightness set/up/down (brightnessctl), media
play-pause/next/prev (dc_mpris_*), dnd on/off/toggle (flip dnd_enabled+save+notify), theme dark/light/
auto + dynamic on/off (theme_mode/dynamic_color + reapply+save+notify), wallpaper set <path> (shared
helper), profile performance/balanced/powersaver (dc_power_set_mode), night on/off, record start/region/
stop/toggle (feature 5). prefix parser for arg verbs. Update print_keybinds. OSD pops free via clock_tick
diff. "get" verbs (need reply channel) DEFERRED.

## 3. Wallpaper extras: NEW services/wallpaper.c/h (extract wall_apply_compositor/wall_set_active from
dashboard.c → dc_wallpaper_apply; swaybg supports multi -o/-i for per-monitor). Keys: wallpaperLight/Dark
(re-apply on mode flip via tick + theme_mode edits), wallpaperPerMonitor (JSON object {output:path} — new
tiny parser), wallpaperCycleEnabled/Dir/IntervalSec (countdown in clock_tick, next image → apply +
material_bg invalidate + dynamic recolor, PAUSE while locked; fullscreen-pause + time-of-day DEFERRED).
Settings in tab_personalization. Tasks: T1 extract wallpaper.c refactor, T2 config+light/dark, T3 cycling+
`wallpaper next` verb, T4 per-monitor, T5 settings UI. Risk: swaybg respawn flicker (note); primary output
drives palette.

## 4. Updater: generalize settings.c updater tab (L3733) to multi-backend (checkupdates + flatpak remote-ls
--updates + AUR paru/yay -Qua if installed, per-backend cache files+counts). Keys updateTerminalCmd
(default foot -e sh -c "sudo pacman -Syu;..."), updatesCheckIntervalMin (default 0 manual). Move check/
cache to NEW services/updates.c/h. Bar widget `systemUpdate` (hidden when 0, icon+count, click→open
updater tab via new dc_settings_toggle_tab). Upgrade = spawn terminal (mux_launch_terminal precedent
L3726). Embedded live-log popout DEFERRED. Tasks: T1 settings multi-backend+upgrade buttons, T2 config+
services/updates.c, T3 bar widget+tick.

## 5. Screen recording: NEW services/screenrec.c/h state{recording,started_at,out_path,pid}. start(region):
full=fork+execlp wf-recorder -f path (pid known); region=sh -c 'g=$(slurp)&&exec wf-recorder -g "$g" -f
path' (exec→child pid IS recorder). prefer wl-screenrec if installed, screenRecorderCmd override. stop=
kill(pid,SIGINT) (clean finalize, NEVER SIGKILL). active=kill(pid,0) from clock_tick (SIGCHLD SIG_IGN).
On stop → notification w/ path (in-process notif). Bar widget `screenRecorder` (idle icon / red dot +
M:SS elapsed, click toggle). IPC record start/region/stop/toggle. Keys screenRecorderCmd/Audio. Distinct
from future privacyIndicator. Tasks: T1 screenrec.c/h, T2 main.c verbs+tick+keybinds, T3 bar widget (+
icons.h record glyph), T4 config+settings row. Risk: slurp-cancel = child dies fast (treat <2s no-file as
clean cancel); wf-recorder missing → notify degrade.

## Contention: main.c (ALL — sequence), config.c (3/4/5 append or batch), bar.c (1/4/5 widgets serialize),
settings.c (1 network/3 personalization/4 updater — diff tab fns low-risk but one big file), net.c (1
only), controlcenter.c (untouched — CC tiles deferred), control.c (untouched). DEFERRED overall: NM
SecretAgent, embedded upgrade-log popout, CC tiles, IPC reply channel, time-of-day wallpaper +
fullscreen-pause, third-party privacy-indicator.
