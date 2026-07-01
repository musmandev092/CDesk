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

bool dc_battery_read(dc_battery_info *out)
{
    memset(out, 0, sizeof(*out));

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
        break;
    }

    closedir(dir);
    return found;
}
