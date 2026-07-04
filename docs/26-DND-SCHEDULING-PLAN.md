# dankc Notification DND Scheduling + Per-App Mute — Plan (2026-07-04)

Ground truth: DND gate = one line notifications.c:408 `slot->popup = !dnd_enabled` + sound.c:190; history
never suppressed. 1Hz clock_tick main.c:231 already calls dc_notifications_tick(); notifications_changed()
main.c:373 refreshes toasts+center. now_wall_ms() CLOCK_REALTIME notifications.c:204; time()+localtime_r+
mktime for "next 8am". config camelCase get_*/cJSON_Add*. dc_notification has NO desktop_entry field +
method_notify doesn't parse desktop-entry hint — must add. notifcenter.c has NO keyboard (copy control
center ON_DEMAND pattern controlcenter.c:2259). settings focus_field/edit_buf commit_edit ~L4200, ids
1-12 taken → new start 13.

## DND schedule: keep dnd_enabled as the sole runtime gate (back-compat: old dndEnabled:true=indefinite).
Add int64 dnd_until_epoch (wall sec; 0=indefinite while enabled, >0=auto-resume; stale-past self-clears
on tick) JSON dndUntilEpoch; int dnd_until_hour(default 8) JSON dndUntilHour. API in notifications.h/c:
dc_notif_dnd_start(n,dur_sec) (0=indef), dc_notif_dnd_start_until_hour(n,hour) (localtime+mktime, +1 day
if past, DST-safe), dc_notif_dnd_stop(n), dc_notif_dnd_remaining_sec()→-1 off/0 indef/>0 left. Auto-resume
at top of dc_notifications_tick(): if enabled && until>0 && now>=until → stop, changed=true. Countdown
redraw: tick_ctx gets notif center, refresh ONLY when visible && remaining>0 (1Hz full-surface redraw ok).

## Per-app rule engine: config.h dc_notif_rule{char match[64] (case-insens vs app_name OR desktop_entry);
int action 0=mute/1=ignore/2=popup-only/3=no-history; int urgency -1 keep|0/1/2}; notif_rules[32]+n. Enum
names in notifications.h, config stores ints (like nightlight_schedule_mode). Semantics: mute=recorded but
popup=false+no sound; ignore=never stored (still return fresh id per spec, log debug); popup-only=toast
then DELETE on expiry/dismiss (new no_history+popup_only flags); no-history=toast+Current then delete on
Current→History; urgency override applied in method_notify before lifetime/sound. Eval in method_notify
(notifications.c:284) after hint parse before acquire_slot; first match wins; needs desktop-entry hint
parse → new desktop_entry field. Persist: get_rule_array/add_rule_array, JSON notifAppRules array of
{match,action(str),urgency(str)}. Privacy mode: notif_privacy_mode(JSON notifPrivacyMode) applied at
RENDER in toasts.c:196 (summary→"New notification", skip body; center shows full — DMS behavior).

## UI: notifcenter.c DND chip row (Off|15m|1h|8AM|∞ + countdown label) + History time-filter chips
(All|Today|Yesterday|This week, not persisted, compare created_wall_ms vs local-midnight) + keyboard nav
(ON_DEMAND + wants_keyboard/handle_key, Up/Down over cached rows, Enter expand/act, Del dismiss, Tab
switch, Esc close; renumber NC_HOVER_CARD_BASE→20). settings tab_notifications (L1549): DND presets
segmented + resume-hour stepper + countdown hint; PRIVACY toggle; PER-APP RULES editor (rows: match +
action segmented + urgency segmented + remove; Add via focus_field 13 / commit_edit case 13).

## Tasks (Sonnet each). Serialize: config.c(T1), notifications.c(T2), notifcenter.c(T3→T5), main.c(T3,T5),
settings.c(T4), toasts.c(T4). NO DRYRUN (internal state).
- T1 config.h/c: dnd_until_epoch(int64 — add get_int64), dnd_until_hour(8), notif_privacy_mode, dc_notif_rule
  struct+notif_rules[]+n, get_rule_array/add_rule_array, keys dndUntilEpoch/dndUntilHour/notifPrivacyMode/
  notifAppRules. BLOCKS rest.
- T2 notifications.h/c: desktop_entry field+hint parse; rule enum+match/apply in method_notify (ignore
  short-circuit/mute/urgency/popup_only/no_history); delete-not-archive in tick expiry+resolve_dismiss+
  clear_current; dc_notif_dnd_* + auto-resume in tick. (after T1)
- T3 notifcenter.c+main.c: DND chip row+countdown+click; history filter chips+filter; hover renumber;
  tick_ctx+gated 1Hz refresh. (after T2)
- T4 settings.c+toasts.c: DND presets/resume-hour/countdown hint, PRIVACY toggle, per-app rules editor
  (focus_field 13), toasts.c privacy redaction. (after T2, ‖T3)
- T5 notifcenter.c/h+main.c: ON_DEMAND keyboard + wants_keyboard/handle_key + sel_row+row cache+auto-scroll+
  kbd_ctx. (after T3, same files → serialize)

## Risks: epoch MUST be CLOCK_REALTIME not monotonic (survives suspend); countdown redraw gate visible&&
remaining>0; hover-id renumber lockstep (nc_hittest/draw/click); ignore returns valid id not 0; back-compat
dnd_enabled sole gate + stale past epoch clears; ON_DEMAND not EXCLUSIVE (no focus steal).
