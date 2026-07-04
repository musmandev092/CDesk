/* wallpaper.c — see wallpaper.h for the public contract + design summary.
 *
 * Pure extraction from src/ui/dashboard.c's former wall_apply_compositor() +
 * cmd_exists() helper: no behavior change, just a new home.
 */
#include "services/wallpaper.h"

#include "core/config.h"
#include "core/log.h"
#include "ui/material_bg.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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

/* --- Wallpaper cycling (docs/29-SMALL-FEATURES-PLAN.md sec.3, T3) -------- */

/* Same extension allow-list as dashboard.c's wall_ext_ok(), plus .webp: the
 * dashboard excludes .webp only because its *thumbnail* decoder (stb_image)
 * can't read it, but swaybg itself handles it fine, and cycling never
 * decodes the image locally -- it just hands the path to dc_wallpaper_apply()
 * -- so there's no reason to exclude it here. */
static bool cycle_ext_ok(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0 ||
           strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".bmp") == 0 ||
           strcasecmp(dot, ".gif") == 0 || strcasecmp(dot, ".webp") == 0;
}

/* Resolve the cycle directory: dc_config_current->wallpaper_cycle_dir when
 * set, else the same fallback dashboard.c's wall_pick_dir() uses (dirname of
 * the configured `wallpaper`, else ~/Pictures/wallpapers, else ~/Pictures).
 * Mirrored rather than shared so this service doesn't need to depend on
 * ui/dashboard.h for one helper. */
static void cycle_pick_dir(char *out, size_t sz)
{
    const dc_config *cfg = dc_config_current;
    if (cfg->wallpaper_cycle_dir[0]) {
        snprintf(out, sz, "%s", cfg->wallpaper_cycle_dir);
        return;
    }
    if (cfg->wallpaper[0] && cfg->wallpaper[0] != '#') {
        snprintf(out, sz, "%s", cfg->wallpaper);
        char *slash = strrchr(out, '/');
        if (slash && slash != out) {
            *slash = '\0';
            return;
        }
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        snprintf(out, sz, "/usr/share/backgrounds");
        return;
    }
    snprintf(out, sz, "%s/Pictures/wallpapers", home);
    struct stat st;
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
        return;
    snprintf(out, sz, "%s/Pictures", home);
}

#define DC_WALLPAPER_CYCLE_MAX 512

static int cycle_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* Cached listing: rescanned only when the resolved directory changes, so a
 * cycle_next() call every N seconds doesn't readdir() every time. */
static char cycle_dir[DC_CONFIG_PATH_MAX];
static char cycle_list[DC_WALLPAPER_CYCLE_MAX][DC_CONFIG_PATH_MAX];
static int cycle_count;
static bool cycle_scanned;

static void cycle_rescan(const char *dir)
{
    snprintf(cycle_dir, sizeof(cycle_dir), "%s", dir);
    cycle_scanned = true;
    cycle_count = 0;

    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && cycle_count < DC_WALLPAPER_CYCLE_MAX) {
        if (ent->d_name[0] == '.' || !cycle_ext_ok(ent->d_name))
            continue;
        char full[DC_CONFIG_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n < 0 || n >= (int)sizeof(full))
            continue; /* path too long to store faithfully: skip */
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        snprintf(cycle_list[cycle_count], sizeof(cycle_list[cycle_count]), "%s", full);
        cycle_count++;
    }
    closedir(d);
    qsort(cycle_list, (size_t)cycle_count, sizeof(cycle_list[0]), cycle_cmp);
    dc_debug("wallpaper cycle: %d image(s) in %s", cycle_count, dir);
}

void dc_wallpaper_cycle_next(void)
{
    char dir[DC_CONFIG_PATH_MAX];
    cycle_pick_dir(dir, sizeof(dir));
    if (!cycle_scanned || strcmp(dir, cycle_dir) != 0)
        cycle_rescan(dir);
    if (cycle_count == 0)
        return;

    dc_config *cfg = dc_config_mut();
    int cur = -1;
    for (int i = 0; i < cycle_count; i++) {
        if (strcmp(cycle_list[i], cfg->wallpaper) == 0) {
            cur = i;
            break;
        }
    }
    const char *next = cycle_list[(cur + 1) % cycle_count];
    if (strcmp(cfg->wallpaper, next) == 0)
        return; /* only image in the dir already active: nothing to do */

    snprintf(cfg->wallpaper, sizeof(cfg->wallpaper), "%s", next);
    dc_config_reapply(); /* stock theme + dynamic-color overlay when enabled */
    dc_material_bg_invalidate(); /* new wallpaper -> panels' blurred bg regenerates lazily */
    if (!getenv("DANKC_WALL_DRY")) {
        dc_config_save();
        dc_wallpaper_apply_effective(); /* respects wallpaperLight/Dark if the mode has one set */
    }
    dc_config_notify_changed(); /* bars pick up the (possibly) new palette */
    dc_info("wallpaper cycle: %s%s", next, getenv("DANKC_WALL_DRY") ? " (dry)" : "");
}

void dc_wallpaper_cycle_tick(bool locked)
{
    static int elapsed_sec;
    const dc_config *cfg = dc_config_current;
    if (!cfg->wallpaper_cycle_enabled || cfg->wallpaper_cycle_interval_sec <= 0 || locked) {
        elapsed_sec = 0;
        return;
    }
    elapsed_sec++;
    if (elapsed_sec >= cfg->wallpaper_cycle_interval_sec) {
        elapsed_sec = 0;
        dc_wallpaper_cycle_next();
    }
}
