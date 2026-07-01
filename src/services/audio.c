#include "services/audio.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* wpctl forks a process, so cache within the same second (the bar redraws every
 * second, sometimes for multiple outputs). */
static dc_audio_info g_cache;
static bool g_cache_ok = false;
static time_t g_cache_time = 0;

bool dc_audio_read(dc_audio_info *out)
{
    time_t now = time(NULL);
    if (g_cache_time == now) {
        *out = g_cache;
        return g_cache_ok;
    }

    out->available = false;
    out->volume = 0;
    out->muted = false;

    FILE *pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (!pipe)
        return false;

    char line[128];
    bool ok = false;
    if (fgets(line, sizeof(line), pipe)) {
        float volume = 0.0f;
        if (sscanf(line, "Volume: %f", &volume) == 1) {
            out->volume = (int)(volume * 100.0f + 0.5f);
            out->available = true;
            ok = true;
        }
        if (strstr(line, "MUTED"))
            out->muted = true;
    }
    pclose(pipe);

    g_cache = *out;
    g_cache_ok = ok;
    g_cache_time = now;
    return ok;
}
