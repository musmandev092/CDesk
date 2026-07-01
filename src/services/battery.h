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
} dc_battery_info;

/* Fill `out` from the first battery under /sys/class/power_supply. Returns true
 * if a battery was found. */
bool dc_battery_read(dc_battery_info *out);

#endif /* DC_SERVICES_BATTERY_H */
