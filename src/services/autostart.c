#include "services/autostart.h"

#include "core/config.h"
#include "core/log.h"
#include "services/apps.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DC_AUTOSTART_MAX 256
#define DC_AUTOSTART_ID_MAX 128
#define DC_AUTOSTART_NAME_MAX 128
#define DC_AUTOSTART_EXEC_MAX 320

/* Basenames (without ".desktop") already decided, across both directory
 * sets -- the user dir is always scanned first, so a same-named system
 * entry never fires (or double-fires) once the user dir has already made a
 * call on that id, matching how real DEs let ~/.config/autostart override
 * /etc/xdg/autostart. */
static char g_seen[DC_AUTOSTART_MAX][DC_AUTOSTART_ID_MAX];
static int g_seen_count;

static int already_seen(const char *id)
{
    for (int i = 0; i < g_seen_count; i++)
        if (strcmp(g_seen[i], id) == 0)
            return 1;
    return 0;
}

static void mark_seen(const char *id)
{
    if (g_seen_count >= DC_AUTOSTART_MAX)
        return;
    snprintf(g_seen[g_seen_count++], DC_AUTOSTART_ID_MAX, "%s", id);
}

/* Strip desktop Exec field codes (%f %F %u %U %i %c %k ...) and surrounding
 * quotes -- same convention (and duplicated for the same reason) as
 * services/apps.c's static clean_exec(): each scanner stays self-contained
 * rather than sharing private helpers across service modules. */
static void clean_exec(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < n; i++) {
        if (src[i] == '%' && src[i + 1]) {
            i++; /* skip the field code letter */
            continue;
        }
        if (src[i] == '"')
            continue;
        dst[o++] = src[i];
    }
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '\t'))
        o--;
    dst[o] = '\0';
}

/* True if `desktop` (XDG_CURRENT_DESKTOP, ':'-separated per spec) contains
 * `token`, case-insensitively. */
static int desktop_contains(const char *desktop, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = desktop;
    while (p && *p) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len == tlen && strncasecmp(p, token, tlen) == 0)
            return 1;
        if (!colon)
            break;
        p = colon + 1;
    }
    return 0;
}

/* True if any ';'-separated token in `list` (an OnlyShowIn=/NotShowIn=
 * value) appears in `desktop`. */
static int any_token_in_desktop(const char *list, const char *desktop)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s", (int)sizeof(buf) - 1, list);
    for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
        if (desktop_contains(desktop, tok))
            return 1;
    }
    return 0;
}

/* TryExec= check: true if `bin` resolves to an executable file, either as an
 * absolute path or somewhere on $PATH. An empty `bin` means "no TryExec
 * constraint" (always true). */
static int exists_on_path(const char *bin)
{
    if (!bin || !bin[0])
        return 1;
    if (bin[0] == '/')
        return access(bin, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path || !*path)
        path = "/usr/local/bin:/usr/bin:/bin";
    char *dirs = strdup(path);
    if (!dirs)
        return 1; /* fail open: don't block launch on an allocation hiccup */

    int found = 0;
    for (char *tok = strtok(dirs, ":"); tok; tok = strtok(NULL, ":")) {
        char full[1024];
        snprintf(full, sizeof(full), "%.900s/%.100s", tok, bin);
        if (access(full, X_OK) == 0) {
            found = 1;
            break;
        }
    }
    free(dirs);
    return found;
}

typedef enum {
    AS_LAUNCH = 0,
    AS_SKIP_NOT_APPLICATION,
    AS_SKIP_NO_EXEC,
    AS_SKIP_HIDDEN,
    AS_SKIP_GNOME_DISABLED,
    AS_SKIP_DESKTOP_MISMATCH,
    AS_SKIP_TRYEXEC_MISSING,
} as_verdict;

typedef struct {
    char name[DC_AUTOSTART_NAME_MAX];
    char exec[DC_AUTOSTART_EXEC_MAX];
} parsed_entry;

/* Parse one .desktop file's [Desktop Entry] group and decide whether it
 * should launch under `desktop` (the XDG_CURRENT_DESKTOP value). Mirrors
 * services/apps.c's parse_desktop() line-scanning style. */
static as_verdict parse_autostart_desktop(const char *path, const char *desktop,
                                           parsed_entry *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return AS_SKIP_NO_EXEC;

    char name[DC_AUTOSTART_NAME_MAX] = {0};
    char exec[DC_AUTOSTART_EXEC_MAX] = {0};
    char tryexec[256] = {0};
    char only_show_in[256] = {0};
    char not_show_in[256] = {0};
    int is_application = 1; /* assume Application unless Type= says otherwise */
    int hidden = 0;
    int gnome_enabled = 1;
    int in_entry = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';

        if (line[0] == '[') {
            in_entry = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_entry)
            continue;

        if (strncmp(line, "Name=", 5) == 0 && !name[0]) {
            snprintf(name, sizeof(name), "%.*s", (int)sizeof(name) - 1, line + 5);
        } else if (strncmp(line, "Exec=", 5) == 0 && !exec[0]) {
            clean_exec(exec, sizeof(exec), line + 5);
        } else if (strncmp(line, "TryExec=", 8) == 0 && !tryexec[0]) {
            snprintf(tryexec, sizeof(tryexec), "%.*s", (int)sizeof(tryexec) - 1, line + 8);
        } else if (strncmp(line, "Type=", 5) == 0) {
            is_application = strcmp(line + 5, "Application") == 0;
        } else if (strncmp(line, "Hidden=", 7) == 0) {
            hidden = strcmp(line + 7, "true") == 0;
        } else if (strncmp(line, "X-GNOME-Autostart-enabled=", 26) == 0) {
            gnome_enabled = strcmp(line + 26, "false") != 0;
        } else if (strncmp(line, "OnlyShowIn=", 11) == 0 && !only_show_in[0]) {
            snprintf(only_show_in, sizeof(only_show_in), "%.*s", (int)sizeof(only_show_in) - 1,
                     line + 11);
        } else if (strncmp(line, "NotShowIn=", 10) == 0 && !not_show_in[0]) {
            snprintf(not_show_in, sizeof(not_show_in), "%.*s", (int)sizeof(not_show_in) - 1,
                     line + 10);
        }
    }
    fclose(f);

    snprintf(out->name, sizeof(out->name), "%s", name[0] ? name : "(unnamed)");
    snprintf(out->exec, sizeof(out->exec), "%s", exec);

    if (!is_application)
        return AS_SKIP_NOT_APPLICATION;
    if (!exec[0])
        return AS_SKIP_NO_EXEC;
    if (hidden)
        return AS_SKIP_HIDDEN;
    if (!gnome_enabled)
        return AS_SKIP_GNOME_DISABLED;
    if (only_show_in[0] && !any_token_in_desktop(only_show_in, desktop))
        return AS_SKIP_DESKTOP_MISMATCH;
    if (not_show_in[0] && any_token_in_desktop(not_show_in, desktop))
        return AS_SKIP_DESKTOP_MISMATCH;
    if (tryexec[0] && !exists_on_path(tryexec))
        return AS_SKIP_TRYEXEC_MISSING;
    return AS_LAUNCH;
}

static const char *verdict_reason(as_verdict v)
{
    switch (v) {
    case AS_SKIP_NOT_APPLICATION:
        return "Type is not Application";
    case AS_SKIP_NO_EXEC:
        return "missing/unreadable Exec=";
    case AS_SKIP_HIDDEN:
        return "Hidden=true";
    case AS_SKIP_GNOME_DISABLED:
        return "X-GNOME-Autostart-enabled=false";
    case AS_SKIP_DESKTOP_MISMATCH:
        return "OnlyShowIn/NotShowIn excludes this desktop";
    case AS_SKIP_TRYEXEC_MISSING:
        return "TryExec binary not found on PATH";
    case AS_LAUNCH:
    default:
        return "";
    }
}

static void scan_dir(const char *dir, const char *desktop)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".desktop") != 0)
            continue;

        char id[DC_AUTOSTART_ID_MAX];
        size_t idlen = (size_t)(dot - ent->d_name);
        if (idlen >= sizeof(id))
            continue;
        memcpy(id, ent->d_name, idlen);
        id[idlen] = '\0';

        /* A higher-priority dir (scanned earlier) already decided this id --
         * per spec, don't let a lower-priority dir's copy fire (or re-skip)
         * it a second time. */
        if (already_seen(id))
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%.500s/%.400s", dir, ent->d_name);

        parsed_entry pe = {0};
        as_verdict v = parse_autostart_desktop(path, desktop, &pe);
        mark_seen(id);

        if (v == AS_LAUNCH) {
            dc_info("autostart: launching %s (\"%s\"): %s", id, pe.name, pe.exec);
            dc_app_launch_exec(pe.exec);
        } else {
            dc_info("autostart: skipping %s (\"%s\") -- %s", id, pe.name, verdict_reason(v));
        }
    }
    closedir(d);
}

void dc_autostart_run(void)
{
    g_seen_count = 0;

    if (!dc_config_current->autostart_enabled) {
        dc_info("autostart: disabled via config (autostartEnabled=false); skipping");
        return;
    }

    /* niri's own default config exports XDG_CURRENT_DESKTOP=niri (embedded
     * niri.kdl, matches DMS's deployer), but a hand-rolled or third-party
     * niri config might not set it -- default it ourselves so OnlyShowIn/
     * NotShowIn evaluate sanely, and export it so autostart children (and
     * anything else spawned this session) see the same identity a full DE
     * would present. */
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !*desktop) {
        setenv("XDG_CURRENT_DESKTOP", "niri", 1);
        desktop = "niri";
        dc_info("autostart: XDG_CURRENT_DESKTOP was unset; defaulting to \"niri\"");
    }

    const char *home = getenv("HOME");
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    char user_dir[768];
    if (xdg_config_home && *xdg_config_home)
        snprintf(user_dir, sizeof(user_dir), "%.700s/autostart", xdg_config_home);
    else if (home)
        snprintf(user_dir, sizeof(user_dir), "%.700s/.config/autostart", home);
    else
        user_dir[0] = '\0';
    if (user_dir[0])
        scan_dir(user_dir, desktop);

    const char *config_dirs = getenv("XDG_CONFIG_DIRS");
    if (!config_dirs || !*config_dirs)
        config_dirs = "/etc/xdg";
    char *dirs = strdup(config_dirs);
    if (dirs) {
        for (char *tok = strtok(dirs, ":"); tok; tok = strtok(NULL, ":")) {
            char dir[768];
            snprintf(dir, sizeof(dir), "%.700s/autostart", tok);
            scan_dir(dir, desktop);
        }
        free(dirs);
    }

    dc_info("autostart: scan complete (%d entr%s seen)", g_seen_count,
             g_seen_count == 1 ? "y" : "ies");
}
