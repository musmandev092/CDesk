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
    /* TODO(wallpaper T2/T4): resolve wallpaperLight/Dark + wallpaperPerMonitor
     * here once those keys exist; for now the config's single `wallpaper`
     * field is the only source of truth. */
    return dc_config_current->wallpaper;
}
