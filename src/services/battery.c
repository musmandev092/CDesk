#include "services/battery.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_POWER_SUPPLY_DIR "/sys/class/power_supply"

static bool read_line(const char *path, char *buf, size_t cap)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return false;
    bool ok = fgets(buf, (int)cap, file) != NULL;
    fclose(file);
    if (!ok)
        return false;
    buf[strcspn(buf, "\n")] = '\0';
    return true;
}

/* Read a sysfs attribute as a long. Returns -1 if absent/unparseable (every
 * value this is used for -- energy/charge/voltage -- is non-negative, so -1
 * is an unambiguous "missing" sentinel). */
static long read_attr(const char *dir_name, const char *attr)
{
    char path[512];
    char value[32];
    snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/%s", dir_name, attr);
    if (!read_line(path, value, sizeof(value)))
        return -1;
    char *end = NULL;
    long v = strtol(value, &end, 10);
    return (end != value && v >= 0) ? v : -1;
}

bool dc_battery_read(dc_battery_info *out)
{
    memset(out, 0, sizeof(*out));
    out->energy_full_wh = -1.0;
    out->energy_full_design_wh = -1.0;
    out->health_percent = -1;

    DIR *dir = opendir(DC_POWER_SUPPLY_DIR);
    if (!dir)
        return false;

    bool found = false;
    struct dirent *entry;
    char path[512];
    char value[32];

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/type", entry->d_name);
        if (!read_line(path, value, sizeof(value)) || strcmp(value, "Battery") != 0)
            continue;

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/capacity", entry->d_name);
        if (!read_line(path, value, sizeof(value)))
            continue;
        out->percent = atoi(value);

        snprintf(path, sizeof(path), DC_POWER_SUPPLY_DIR "/%s/status", entry->d_name);
        if (read_line(path, value, sizeof(value))) {
            out->charging = strcmp(value, "Charging") == 0;
            out->full = strcmp(value, "Full") == 0;
        }
        out->present = true;
        found = true;

        /* Battery popout "Health"/"Capacity" cards (docs/13-POPOUTS-SPEC.md
         * sec.2): energy_full[_design] (µWh) preferred, charge_full[_design]
         * (µAh) * voltage_now (µV) as a fallback -- some drivers (notably
         * some ThinkPads/older EC firmware) only expose the charge_* family. */
        long energy_full = read_attr(entry->d_name, "energy_full");
        long energy_full_design = read_attr(entry->d_name, "energy_full_design");
        long charge_full = read_attr(entry->d_name, "charge_full");
        long charge_full_design = read_attr(entry->d_name, "charge_full_design");
        long voltage_now = read_attr(entry->d_name, "voltage_now");

        if (energy_full > 0)
            out->energy_full_wh = (double)energy_full / 1e6;
        else if (charge_full > 0 && voltage_now > 0)
            out->energy_full_wh = (double)charge_full * (double)voltage_now / 1e12;

        if (energy_full_design > 0)
            out->energy_full_design_wh = (double)energy_full_design / 1e6;
        else if (charge_full_design > 0 && voltage_now > 0)
            out->energy_full_design_wh = (double)charge_full_design * (double)voltage_now / 1e12;

        if (energy_full > 0 && energy_full_design > 0)
            out->health_percent =
                (int)((double)energy_full / (double)energy_full_design * 100.0 + 0.5);
        else if (charge_full > 0 && charge_full_design > 0)
            out->health_percent =
                (int)((double)charge_full / (double)charge_full_design * 100.0 + 0.5);

        break;
    }

    closedir(dir);
    return found;
}
