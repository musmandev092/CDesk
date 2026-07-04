/* battery_auto.h — battery-protection automation (docs/24-BATTERY-POWER-PLAN.md
 * sec.2 "Automation"). Ticked once per second from main.c's clock_tick, but
 * self-limits its own dc_battery_read() to roughly every 5s (sysfs reads
 * aren't free and nothing here needs sub-5s resolution) -- same cadence as
 * services/power.c's own DC_POWER_REFRESH_SECS cache.
 *
 * Drives, off dc_config_current + dc_battery_read()/dc_power_read():
 *   - auto profile switch on an observed AC-plug/unplug edge (never at
 *     startup -- there is no "edge" to react to on the very first read).
 *   - auto power-saver while on battery at/under the low threshold.
 *   - low/critical/charge-limit-reached toasts via
 *     dc_notifications_post_local(), one-shot per crossing with hysteresis.
 *
 * All state (the AC tri-state and every one-shot notification/power-saver
 * flag) is private `static` storage inside battery_auto.c -- there is
 * nothing for a caller to allocate or own; just call dc_battery_auto_tick()
 * once per second and forget about it.
 */
#ifndef DC_SERVICES_BATTERY_AUTO_H
#define DC_SERVICES_BATTERY_AUTO_H

struct dc_notifications;

/* Call once per second (from main.c's clock_tick). No-op on ticks where the
 * internal ~5s self-limit hasn't elapsed yet, and on any tick where no
 * battery is present. `n` may be NULL (dc_notifications_post_local() itself
 * tolerates that), though in practice main.c always has one. */
void dc_battery_auto_tick(struct dc_notifications *n);

#endif /* DC_SERVICES_BATTERY_AUTO_H */
