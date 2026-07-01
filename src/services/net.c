#include "services/net.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

bool dc_net_wifi(dc_net_info *out)
{
    out->has_wifi = false;
    out->connected = false;

    DIR *dir = opendir("/sys/class/net");
    if (!dir)
        return false;

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        /* Wi-Fi interfaces are named wlan0, wlp*, etc. */
        if (strncmp(ent->d_name, "wl", 2) != 0)
            continue;
        out->has_wifi = true;

        char path[300];
        snprintf(path, sizeof(path), "/sys/class/net/%.200s/operstate", ent->d_name);
        FILE *file = fopen(path, "r");
        if (!file)
            continue;
        char state[32];
        if (fgets(state, sizeof(state), file)) {
            state[strcspn(state, "\r\n")] = '\0';
            if (strcmp(state, "up") == 0)
                out->connected = true;
        }
        fclose(file);
    }
    closedir(dir);
    return out->has_wifi;
}
