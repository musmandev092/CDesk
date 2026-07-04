/* wallpaper.c — see wallpaper.h for the public contract + design summary.
 *
 * Pure extraction from src/ui/dashboard.c's former wall_apply_compositor() +
 * cmd_exists() helper: no behavior change, just a new home.
 */
#include "services/wallpaper.h"

#include "core/config.h"
#include "core/log.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Best-effort $PATH lookup (no shell): true if `name` is executable. */
static bool cmd_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path)
        return false;
    while (*path) {
        const char *colon = strchr(path, ':');
        size_t len = colon ? (size_t)(colon - path) : strlen(path);
        if (len > 0 && len < 400) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%.*s/%.100s", (int)len, path, name);
            if (access(buf, X_OK) == 0)
                return true;
        }
        path += len;
        if (*path == ':')
            path++;
    }
    return false;
}

void dc_wallpaper_apply(const char *path)
{
    if (!cmd_exists("swaybg")) {
        dc_info("wallpaper: swaybg not installed; config/palette updated only");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        /* Replace pattern: retire the previous instance, then take over. */
        pid_t k = fork();
        if (k == 0) {
            execlp("pkill", "pkill", "-x", "swaybg", (char *)NULL);
            _exit(127);
        }
        usleep(100 * 1000); /* let the old instance exit */
        execlp("swaybg", "swaybg", "-m", "fill", "-i", path, (char *)NULL);
        _exit(127);
    }
}

const char *dc_wallpaper_effective(void)
{
    const dc_config *cfg = dc_config_current;
    bool light = dc_config_light_mode();
    if (light && cfg->wallpaper_light[0])
        return cfg->wallpaper_light;
    if (!light && cfg->wallpaper_dark[0])
        return cfg->wallpaper_dark;
    /* TODO(wallpaper T4): resolve wallpaperPerMonitor here once that key
     * exists (per-output override, primary output drives the palette). */
    return cfg->wallpaper;
}

/* Last path actually handed to dc_wallpaper_apply() by _apply_effective(),
 * so repeat calls (e.g. every dc_config_reapply() after an unrelated
 * settings edit) don't respawn swaybg when nothing actually changed.
 * Starts empty each process run -- the first call after startup always
 * "changes" (empty -> whatever's effective) if a wallpaper is configured. */
static char g_last_applied[DC_CONFIG_PATH_MAX];

void dc_wallpaper_apply_effective(void)
{
    const char *effective = dc_wallpaper_effective();
    if (strncmp(effective, g_last_applied, sizeof(g_last_applied)) == 0)
        return;
    snprintf(g_last_applied, sizeof(g_last_applied), "%s", effective);
    if (effective[0])
        dc_wallpaper_apply(effective);
}
