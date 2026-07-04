#include "services/battery_auto.h"

#include "core/config.h"
#include "services/battery.h"
#include "services/notifications.h"
#include "services/power.h"

#include <stdio.h>
#include <time.h>

/* Same cadence as power.c's DC_POWER_REFRESH_SECS -- no automation decision
 * here needs sub-5s resolution, and dc_battery_read() walks
 * /sys/class/power_supply on every call. */
#define DC_BATTERY_AUTO_REFRESH_SECS 5

/* Hysteresis band (percentage points) a one-shot flag must clear by, above
 * its threshold, before it's allowed to fire again -- keeps a battery
 * hovering right at a threshold from spamming a notification every time it
 * ticks up/down by 1%. */
#define DC_BATTERY_AUTO_HYSTERESIS 5

/* charge-limit-reached uses a tighter band: it's comparing against the
 * actual sysfs charge_control_end_threshold, which (unlike the low/critical
 * thresholds) only ever varies by the couple of points a driver's charge
 * controller overshoots/undershoots by around its target. */
#define DC_BATTERY_AUTO_CHARGE_LIMIT_HYSTERESIS 2

void dc_battery_auto_tick(struct dc_notifications *n)
{
    /* AC tri-state: -1 == "no read yet", so the very first successful read
     * only primes this instead of reacting to a fake "edge" at startup. */
    static int last_on_ac = -1;
    static time_t last_read = 0;

    static bool power_saver_active = false;
    static bool notified_low = false;
    static bool notified_critical = false;
    static bool notified_charge_limit = false;

    time_t now = time(NULL);
    if (last_read != 0 && now - last_read < DC_BATTERY_AUTO_REFRESH_SECS)
        return;
    last_read = now;

    dc_battery_info b;
    if (!dc_battery_read(&b) || !b.present)
        return;

    const dc_config *cfg = dc_config_current;
    bool on_ac = b.ac_online;

    /* --- AC edge detect + auto profile switch ------------------------- */
    bool ac_edge = last_on_ac != -1 && (int)on_ac != last_on_ac;
    if (ac_edge && cfg->auto_profile_switch) {
        dc_power_info pw;
        if (dc_power_read(&pw) && pw.available) {
            /* profile_on_ac/profile_on_battery are plain 0/1/2 ints that line
             * up 1:1 with dc_power_mode (see config.h's doc-comment) -- no
             * translation needed, and dc_power_set_mode() itself rejects
             * anything outside POWER_SAVER..PERFORMANCE. */
            dc_power_mode mode =
                on_ac ? (dc_power_mode)cfg->profile_on_ac : (dc_power_mode)cfg->profile_on_battery;
            dc_power_set_mode(mode);
        }
    }
    last_on_ac = on_ac ? 1 : 0;

    /* --- auto power-saver ---------------------------------------------- */
    if (on_ac) {
        power_saver_active = false;
    } else if (cfg->auto_power_saver && !power_saver_active &&
              b.percent_raw <= cfg->low_battery_threshold) {
        dc_power_info pw;
        if (dc_power_read(&pw) && pw.available)
            dc_power_set_mode(DC_POWER_MODE_POWER_SAVER);
        power_saver_active = true;
    }

    /* --- low / critical / charge-limit-reached toasts ------------------- */
    if (!cfg->battery_notifications) {
        notified_low = false;
        notified_critical = false;
        notified_charge_limit = false;
        return;
    }

    bool critical_zone = !on_ac && b.percent_raw <= cfg->critical_battery_threshold;
    bool low_zone = !on_ac && b.percent_raw <= cfg->low_battery_threshold;

    if (critical_zone) {
        if (!notified_critical) {
            char summary[64];
            snprintf(summary, sizeof(summary), "Battery critical (%d%%)", b.percent_raw);
            dc_notifications_post_local(n, "Battery", summary,
                                        "Plug in now to avoid an unexpected shutdown.",
                                        DC_URGENCY_CRITICAL);
            notified_critical = true;
        }
    } else if (on_ac || b.percent_raw > cfg->critical_battery_threshold + DC_BATTERY_AUTO_HYSTERESIS) {
        notified_critical = false;
    }

    /* A critical-zone tick never also posts low -- critical is always a
     * subset of low (critical_battery_threshold < low_battery_threshold), so
     * without this a battery that skips straight past the low crossing (e.g.
     * a fast drop observed only once every ~5s) would fire both in the same
     * tick. */
    if (!critical_zone) {
        if (low_zone) {
            if (!notified_low) {
                char summary[64];
                snprintf(summary, sizeof(summary), "Battery low (%d%%)", b.percent_raw);
                dc_notifications_post_local(n, "Battery", summary, "Consider plugging in soon.",
                                            DC_URGENCY_NORMAL);
                notified_low = true;
            }
        } else if (on_ac || b.percent_raw > cfg->low_battery_threshold + DC_BATTERY_AUTO_HYSTERESIS) {
            notified_low = false;
        }
    }

    if (b.charge_limit_supported && b.charge_limit >= 1 && b.charge_limit < 100) {
        if (on_ac && b.percent_raw >= b.charge_limit) {
            if (!notified_charge_limit) {
                char summary[64];
                snprintf(summary, sizeof(summary), "Charge limit reached (%d%%)", b.percent_raw);
                dc_notifications_post_local(n, "Battery", summary,
                                            "Charging stopped at your configured limit.",
                                            DC_URGENCY_NORMAL);
                notified_charge_limit = true;
            }
        }
        if (!on_ac || b.percent_raw < b.charge_limit - DC_BATTERY_AUTO_CHARGE_LIMIT_HYSTERESIS)
            notified_charge_limit = false;
    } else {
        notified_charge_limit = false;
    }
}
