/* battery.h — battery state read directly from sysfs (no D-Bus).
 *
 * A light stand-in until the UPower sd-bus service lands (M3); good enough for
 * a bar indicator and dependency-free.
 */
#ifndef DC_SERVICES_BATTERY_H
#define DC_SERVICES_BATTERY_H

#include <stdbool.h>

typedef struct dc_battery_info {
    bool present;
    int percent; /* 0-100, RESCALED against charge_limit when supported (see
                  * dc_battery_read in battery.c) -- this is what bar.c /
                  * battery_popout.c should display so "100%" is reachable
                  * even when the charge limit caps the raw sysfs capacity. */
    bool charging;
    bool full;

    /* AC/USB power-supply "online" state (docs/12-BAR-SPEC.md sec.4/6 battery
     * item 5), hoisted from bar.c's former bar_ac_online(): true if any Mains
     * or USB power-supply under /sys/class/power_supply reports online=1.
     * `charging` is a strict sysfs status=="Charging" check, but plenty of
     * laptops report "Not charging" once a charge threshold is hit while AC
     * stays connected -- callers wanting "is AC plugged in" should use
     * ac_online, not charging. */
    bool ac_online;

    /* Raw (un-rescaled) sysfs capacity, 0-100. Automation (battery_auto.c,
     * low/critical thresholds) must compare against this, not `percent` --
     * `percent` is rescaled to make "100%" reachable under a charge limit,
     * which would otherwise make low-battery thresholds fire late. */
    int percent_raw;

    /* Charge-limit support (charge_control_end_threshold sysfs attribute).
     * Not all drivers expose this (ThinkPad/ASUS/LG EC-dependent); when
     * false, charge_limit is -1 and percent == percent_raw. */
    bool charge_limit_supported;
    int charge_limit; /* 1-100, -1 if unsupported */

    /* sysfs dir basename of the battery this info was read from (e.g. "BAT0"),
     * for the charge-limit setter (dc_battery_set_charge_limit, added in T3).
     * Empty string if no battery found. */
    char batt_dir[64];

    /* Battery popout stat cards (docs/13-POPOUTS-SPEC.md sec.2). -1 when the
     * kernel driver doesn't expose the underlying sysfs attribute at all
     * (some drivers report neither energy_full* nor charge_full*). Preferred
     * source is energy_full[_design] (µWh); charge_full[_design] (µAh) *
     * voltage_now (µV) is used as a fallback for the Wh figures, and the
     * charge_full/charge_full_design ratio alone (voltage cancels out) as a
     * fallback for health_percent. */
    double energy_full_wh;        /* "Capacity" card: energy_full / 1e6 */
    double energy_full_design_wh; /* design capacity, same units */
    int health_percent;           /* "Health" card: energy_full/energy_full_design * 100 */
} dc_battery_info;

/* Fill `out` from the first battery under /sys/class/power_supply. Returns true
 * if a battery was found. */
bool dc_battery_read(dc_battery_info *out);

/* Write `pct` (50-100; 100 disables the limit) to `batt_dir`'s
 * charge_control_end_threshold sysfs attribute, via
 * `pkexec tee /sys/class/power_supply/<batt_dir>/charge_control_end_threshold`
 * (tee, not `install`, since sysfs attributes are magic files, not regular
 * ones -- an argv-exec straight into tee avoids any shell-quoting concerns
 * too). `batt_dir` should come from dc_battery_info.batt_dir.
 *
 * Fire-and-forget (fork+pipe, reaped by main's SIGCHLD=SIG_IGN, same shape
 * as every other pkexec call in this codebase): returns true once the
 * pkexec request has been launched, NOT once the write has actually
 * succeeded -- there is no success signal. pkexec will prompt for
 * authentication via dankc's own polkit agent (see polkit.c). Callers that
 * need to know the outcome should re-read sysfs afterwards (dc_battery_read)
 * and compare charge_limit against what was requested.
 *
 * DANKC_BATTERY_DRYRUN=1 logs the would-be write instead of forking --
 * verification harness for this task uses it to confirm call shape without
 * touching a live session (mirrors DANKC_LOGIND_DRYRUN in logind.c).
 *
 * The kernel resets charge_control_end_threshold on every reboot, so this
 * function is never called automatically at startup to "reapply" a saved
 * limit -- that would mean an unsolicited pkexec prompt at login. Re-applying
 * a remembered limit (if desired) is a settings/UI concern, driven by an
 * explicit user action, not something battery.c does on its own.
 *
 * Returns false without forking anything if `pct` is out of range or
 * `batt_dir` is empty/contains a '/' (defensive -- batt_dir nominally comes
 * from a readdir() basename, but this is the boundary where a bad value
 * would otherwise be concatenated straight into a filesystem path). */
bool dc_battery_set_charge_limit(const char *batt_dir, int pct);

#endif /* DC_SERVICES_BATTERY_H */
