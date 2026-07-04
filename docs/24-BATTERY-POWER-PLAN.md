# dankc Battery Protection + Power & Sleep Depth — Plan (2026-07-04)

Ground truth: battery.c reads SYSFS directly (not UPower); no AC field, no thresholds, no notifications
(DANKC_FAKE_BATTERY[_CHARGING] exist). AC-online logic currently in bar.c:1341 bar_ac_online() → hoist
to battery.c. 1Hz clock_tick() in main.c:231 (tick_ctx has dc_notifications*) — automation hooks here.
power.c: dc_power_set_mode() ready. pkexec drop-in pattern = logind.c:254 dc_logind_conf_write_dropin
(but charge-limit needs `pkexec tee <path>` w/ value on stdin, NOT install). NO idle mechanism (no
ext-idle-notify client, no swayidle); logind IdleAction/Sec is single-valued (can't do per-AC/battery
idle) — only HandleLidSwitch/HandleLidSwitchExternalPower are per-source (logind.c already parses+writes
both; settings.c only exposes the first). notifications.c has NO local-post API — must add one.

## Key decisions
- Charge-limit: read charge_control_end_threshold (1-100) in dc_battery_read; new fields ac_online,
  percent_raw, charge_limit_supported, charge_limit, batt_dir. UI RESCALE: percent=min(100,(raw*100+
  limit/2)/limit); automation thresholds use percent_raw (NOT rescaled). Setter dc_battery_set_charge_limit
  = fork+pipe+`execlp pkexec tee <path>` w/ "%d\n" on stdin, DANKC_BATTERY_DRYRUN gate. sysfs resets on
  reboot → do NOT auto-reapply at startup (would pkexec-prompt at login); store desired in config, hint
  "Apply" when sysfs!=config. UI reflects sysfs (poll-back), not the wish.
- Automation (new module src/services/battery_auto.c/.h, ticked from clock_tick, self-limit ~5s): AC-edge
  detect; auto profile switch on AC edge (profileOnAC/Battery, only on observed edges never startup);
  auto power-saver when !AC && raw<=low; low notif (normal) + critical notif (critical urgency), one-shot
  flags w/ +5 hysteresis reset, reset on AC; charge-limit-reached notif. DND handled by existing path.
  Add DANKC_FAKE_BATTERY_AC=0|1.
- Notifications local-post (notifications.c): dc_notifications_post_local(n,app,summary,body,urgency) —
  acquire_slot()+fill like seed_demo w/ now-ts+popup, dc_sound_notify+changed_cb like method_notify.
- Power&Sleep depth NOW (logind-only, cheap): expose HandleLidSwitchExternalPower ("Lid close on AC")
  row (writer already handles it), add Hibernate/suspend-then-hibernate options to idle+lid action lists.
  Per-AC/battery idle timeouts NOT doable via logind → hint says needs idle service.
- STRETCH T7 (separately approvable): src/services/idle.c ext-idle-notify-v1 client (niri supports).
  protocol/ext-idle-notify-v1.xml + codegen. 4-stage timers (fade-to-lock via ui/lock.c, monitor-off via
  `niri msg action power-off-monitors`, suspend/hibernate via logind D-Bus Manager.Suspend/Hibernate —
  NO pkexec, polkit allows active session; gate DANKC_POWER_DRYRUN like powermenu). AC/battery timeout sets.

## Task breakdown (Sonnet each). Serialized: battery.c(T1→T3), config.c(T2), settings.c(T6→T7), main.c(T5→T7), notifications.c(T4).
- T1 battery.c: ac_online (hoist bar_ac_online), percent_raw, charge_limit read, rescale, batt_dir, +DANKC_FAKE_BATTERY_AC. (also edits bar.c to use battery.c's ac_online). DISJOINT — do first.
- T2 config.c/.h: add all keys once: chargeLimit(100), batteryNotifications(true), lowBatteryThreshold(20),
  criticalBatteryThreshold(10), autoPowerSaver(false), autoProfileSwitch(false), profileOnAC(1),
  profileOnBattery(0), + T7 keys idleTimeoutsEnabled(false) + 8 ints lock/monitorOff/suspend/hibernate
  ×Ac/Battery TimeoutMin(0). get_int pattern config.c:389.
- T3 battery.c dc_battery_set_charge_limit (after T1): pkexec tee + stdin pipe + DANKC_BATTERY_DRYRUN.
- T4 notifications.c dc_notifications_post_local (parallel).
- T5 battery_auto.c/.h + main.c clock_tick hookup + build files (after T1-T4).
- T6 settings.c tab_power (after T1-T3,T2): BATTERY section (charge slider when supported + Apply hint +
  reboot hint), notif/threshold/autoPowerSaver toggles, AUTOMATION (autoProfileSwitch + 2 profile pickers),
  IDLE&LID add lid-on-AC row + Hibernate option + idle-timeouts hint.
- T7 (stretch) idle service per above.

## Risks: sysfs charge threshold driver variance (ThinkPad/ASUS/LG; some clamp/fixed values; multi-batt
→ v1 first battery only; UI reflects sysfs via poll-back); pkexec fire-and-forget (no success signal,
debounce slider write-on-release); notif spam (hysteresis+one-shot); idle gap (logind IdleAction may be
inert on niri since niri may not set session IdleHint — T7 idle client is the only robust path, don't
overpromise in hints).
