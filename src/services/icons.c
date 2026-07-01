#include "services/icons.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Icon-theme sizes to try, bar-friendly sizes first. */
static const char *const ICON_SIZES[] = {"24x24",   "32x32",   "48x48", "16x16",
                                         "64x64",   "128x128", "256x256"};

static char *dup_if_readable(const char *path)
{
    return access(path, R_OK) == 0 ? strdup(path) : NULL;
}

/* Read the Icon= value from the app's .desktop file. malloc'd or NULL. */
static char *desktop_icon_name(const char *app_id)
{
    char dirs[10][512];
    int n = 0;

    const char *home = getenv("HOME");
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home && *data_home)
        snprintf(dirs[n++], sizeof(dirs[0]), "%s/applications", data_home);
    else if (home)
        snprintf(dirs[n++], sizeof(dirs[0]), "%s/.local/share/applications", home);

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !*data_dirs)
        data_dirs = "/usr/local/share:/usr/share";
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", data_dirs);
    for (char *tok = strtok(buf, ":"); tok && n < 10; tok = strtok(NULL, ":"))
        snprintf(dirs[n++], sizeof(dirs[0]), "%s/applications", tok);

    for (int i = 0; i < n; i++) {
        char path[1200];
        snprintf(path, sizeof(path), "%.900s/%.150s.desktop", dirs[i], app_id);
        FILE *file = fopen(path, "r");
        if (!file)
            continue;
        char line[512];
        char *icon = NULL;
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "Icon=", 5) == 0) {
                line[strcspn(line, "\r\n")] = '\0';
                icon = strdup(line + 5);
                break;
            }
        }
        fclose(file);
        if (icon)
            return icon;
    }
    return NULL;
}

/* Search icon-theme dirs, hicolor and pixmaps for <name>.(png|svg). */
static char *find_icon_file(const char *name, int svg_ok)
{
    if (!name || !*name)
        return NULL;
    if (name[0] == '/')
        return dup_if_readable(name);

    char path[1200];
    char *hit;

    snprintf(path, sizeof(path), "/usr/share/pixmaps/%.256s.png", name);
    if ((hit = dup_if_readable(path)))
        return hit;

    char bases[2][512];
    int nb = 0;
    const char *home = getenv("HOME");
    if (home)
        snprintf(bases[nb++], sizeof(bases[0]), "%s/.local/share/icons", home);
    snprintf(bases[nb++], sizeof(bases[0]), "/usr/share/icons");

    /* PNG first (nanovg/stb_image decodes it). */
    for (int b = 0; b < nb; b++) {
        DIR *dir = opendir(bases[b]);
        if (!dir)
            continue;
        struct dirent *ent;
        while ((ent = readdir(dir))) {
            if (ent->d_name[0] == '.')
                continue;
            for (size_t s = 0; s < sizeof(ICON_SIZES) / sizeof(ICON_SIZES[0]); s++) {
                snprintf(path, sizeof(path), "%s/%.200s/%s/apps/%.200s.png", bases[b], ent->d_name,
                         ICON_SIZES[s], name);
                if ((hit = dup_if_readable(path))) {
                    closedir(dir);
                    return hit;
                }
            }
        }
        closedir(dir);
    }

    if (svg_ok) {
        snprintf(path, sizeof(path), "/usr/share/pixmaps/%.256s.svg", name);
        if ((hit = dup_if_readable(path)))
            return hit;
        for (int b = 0; b < nb; b++) {
            DIR *dir = opendir(bases[b]);
            if (!dir)
                continue;
            struct dirent *ent;
            while ((ent = readdir(dir))) {
                if (ent->d_name[0] == '.')
                    continue;
                snprintf(path, sizeof(path), "%s/%.200s/scalable/apps/%.200s.svg", bases[b], ent->d_name,
                         name);
                if ((hit = dup_if_readable(path))) {
                    closedir(dir);
                    return hit;
                }
            }
            closedir(dir);
        }
    }
    return NULL;
}

char *dc_icon_resolve(const char *app_id, int size_hint, int svg_ok)
{
    (void)size_hint;
    if (!app_id || !*app_id)
        return NULL;

    char *icon_name = desktop_icon_name(app_id);
    char *path = find_icon_file(icon_name ? icon_name : app_id, svg_ok);
    free(icon_name);

    if (!path) {
        char lower[128];
        snprintf(lower, sizeof(lower), "%s", app_id);
        for (char *p = lower; *p; p++)
            *p = (char)tolower((unsigned char)*p);
        path = find_icon_file(lower, svg_ok);
    }
    return path;
}
