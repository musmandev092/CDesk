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
    int percent; /* 0-100 */
    bool charging;
    bool full;

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

#endif /* DC_SERVICES_BATTERY_H */
