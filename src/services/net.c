#include "services/net.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* SSID/signal via `nmcli` (a fork+popen, same shell-out style already used by
 * services/audio.c for wpctl and controlcenter.c for rfkill/wpctl/
 * brightnessctl) -- cached briefly since `nmcli dev wifi list` is a slower
 * call than a sysfs read and dc_net_wifi() can be polled every render frame
 * during a popout's entrance animation. */
#define DC_NET_WIFI_CACHE_SECONDS 3

static void refresh_wifi_details(char *ssid, size_t ssid_sz, int *signal_percent)
{
    static char cached_ssid[64];
    static int cached_signal = -1;
    static time_t cache_time;

    time_t now = time(NULL);
    if (now - cache_time >= DC_NET_WIFI_CACHE_SECONDS) {
        cache_time = now;
        cached_ssid[0] = '\0';
        cached_signal = -1;

        FILE *pipe = popen("nmcli -t -f active,ssid,signal dev wifi list --rescan no 2>/dev/null", "r");
        if (pipe) {
            char line[256];
            while (fgets(line, sizeof(line), pipe)) {
                if (strncmp(line, "yes:", 4) != 0)
                    continue;
                char *rest = line + 4;
                char *last_colon = strrchr(rest, ':');
                if (!last_colon)
                    continue;
                *last_colon = '\0';
                cached_signal = atoi(last_colon + 1);
                /* un-escape nmcli's backslash-escaped ':' within the SSID */
                size_t j = 0;
                for (size_t i = 0; rest[i] != '\0' && j < sizeof(cached_ssid) - 1; i++) {
                    if (rest[i] == '\\' && rest[i + 1] == ':')
                        continue;
                    cached_ssid[j++] = rest[i];
                }
                cached_ssid[j] = '\0';
                break;
            }
            pclose(pipe);
        }
    }

    snprintf(ssid, ssid_sz, "%s", cached_ssid);
    *signal_percent = cached_signal;
}

bool dc_net_wifi(dc_net_info *out)
{
    out->has_wifi = false;
    out->connected = false;
    out->ssid[0] = '\0';
    out->signal_percent = -1;

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

    if (out->connected)
        refresh_wifi_details(out->ssid, sizeof(out->ssid), &out->signal_percent);

    return out->has_wifi;
}
