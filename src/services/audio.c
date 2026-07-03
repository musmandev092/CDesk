#include "services/audio.h"

#include "core/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* wpctl forks a process, so cache it (docs/POLISH.md P7 item 2). The bar's
 * damage-tracking hash (ui/bar/bar.c bar_compute_signature()) now reads this
 * every ~1Hz tick per bar just to check for a volume change, on top of the
 * 1Hz clock_tick's own OSD-change read and the control-center-pill's draw
 * read — so without a real cache window this would fork on nearly every tick
 * across 2+ bars. Widened to 10s per docs/15-PERF-PLAN.md T2.3 (was 3s).
 * TRADEOFF: dc_audio_set_volume() below still invalidates the cache
 * immediately (line 85), so a user's own slider drag/OSD reflects instantly.
 * The longer window only affects external volume changes (media keys, another
 * app) — detection latency is now up to ~10s instead of ~1s, but saves ~13
 * forks/min (~20 → ~7). External volume changes on the bar are acceptable
 * per docs/POLISH.md P7 ("2-3s worst-case is fine"). */
#define DC_AUDIO_CACHE_SECONDS 10

static dc_audio_info g_cache;
static bool g_cache_ok = false;
static time_t g_cache_time = 0;
static bool g_cache_valid = false;

bool dc_audio_read(dc_audio_info *out)
{
    time_t now = time(NULL);
    if (g_cache_valid && now - g_cache_time < DC_AUDIO_CACHE_SECONDS) {
        *out = g_cache;
        return g_cache_ok;
    }

    out->available = false;
    out->volume = 0;
    out->muted = false;

    dc_debug("audio: forking wpctl get-volume (cache miss)");
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
    g_cache_valid = true;
    return ok;
}

void dc_audio_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "wpctl set-volume @DEFAULT_AUDIO_SINK@ %.2f", percent / 100.0f);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* Invalidate the read cache so a drag's own writes are reflected on the
     * very next dc_audio_read() (the slider's fill needs to track the
     * pointer immediately, not up to DC_AUDIO_CACHE_SECONDS later). */
    g_cache_valid = false;
}
