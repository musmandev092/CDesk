/* updates.c — see updates.h for the public contract + process-model
 * rationale (detached per-backend shell checks polled via cache files,
 * interactive-terminal upgrade spawn).
 */
#include "services/updates.h"

#include "core/config.h"
#include "core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* --- PATH probing (mirrors services/screenrec.c's cmd_exists(), which in
 * turn mirrors ui/dashboard.c's -- duplicated per that file's own rationale:
 * a plain access()-based $PATH lookup, no shell, safe to call anytime
 * including after main.c sets SIGCHLD to SIG_IGN). ------------------------ */

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

/* paru preferred (more actively maintained + faster -Qua); yay fallback.
 * Returns NULL if neither is installed. */
static const char *probe_aur_helper(void)
{
    if (cmd_exists("paru"))
        return "paru";
    if (cmd_exists("yay"))
        return "yay";
    return NULL;
}

/* --- backend table -------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *cache_path;
} backend_def;

/* Fixed pacman/aur/flatpak order, matching dc_update_backend's doc comment. */
static const backend_def BACKENDS[DC_UPDATES_BACKENDS_N] = {
    {"pacman", "/tmp/dankc-updates-pacman.out"},
    {"aur", "/tmp/dankc-updates-aur.out"},
    {"flatpak", "/tmp/dankc-updates-flatpak.out"},
};

static bool backend_available(int idx)
{
    switch (idx) {
    case 0: /* pacman */
        return cmd_exists("checkupdates");
    case 1: /* aur */
        return probe_aur_helper() != NULL;
    case 2: /* flatpak */
        return cmd_exists("flatpak");
    default:
        return false;
    }
}

/* --- detached spawn -------------------------------------------------------- */

/* Run a shell command detached (children auto-reaped via main.c's SIG_IGN on
 * SIGCHLD) -- same shape as services/power.c's/ui/settings.c's run_detached().
 * Unlike settings.c's, this one isn't gated by DANKC_SETTINGS_DRYRUN: these
 * per-backend checks are read-only (never install/upgrade anything), matching
 * the existing checkupdates tab's own unconditional `checkupdates` probing
 * behavior once the button is clicked. */
static void run_detached(const char *cmd)
{
    dc_debug("updates: run: %s", cmd);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* --- check ------------------------------------------------------------- */

/* Builds the read-only "list pending updates, one per line" command for a
 * backend. Returns false if the backend has nothing to run (shouldn't happen
 * for an index already gated by backend_available(), but keeps this
 * defensive). */
static bool backend_query_cmd(int idx, char *out, size_t outsz)
{
    switch (idx) {
    case 0: /* pacman */
        snprintf(out, outsz, "checkupdates 2>/dev/null || true");
        return true;
    case 1: { /* aur */
        const char *helper = probe_aur_helper();
        if (!helper)
            return false;
        snprintf(out, outsz, "%s -Qua 2>/dev/null || true", helper);
        return true;
    }
    case 2: /* flatpak */
        snprintf(out, outsz, "flatpak remote-ls --updates 2>/dev/null || true");
        return true;
    default:
        return false;
    }
}

void dc_updates_check_async(void)
{
    for (int i = 0; i < DC_UPDATES_BACKENDS_N; i++) {
        if (!backend_available(i))
            continue;
        char query[300];
        if (!backend_query_cmd(i, query, sizeof(query)))
            continue;
        char cmd[900];
        snprintf(cmd, sizeof(cmd),
                 "{ %s ; } >%s.tmp 2>/dev/null && mv %s.tmp %s", query, BACKENDS[i].cache_path,
                 BACKENDS[i].cache_path, BACKENDS[i].cache_path);
        run_detached(cmd);
    }
}

/* Reads a backend's cache file's non-empty line count + mtime. Returns false
 * (leaving *count and *mtime untouched) if no check has completed yet. */
static bool read_cache_file(const char *path, int *count, long *mtime)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    *mtime = (long)st.st_mtime;

    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f))
        if (line[0] != '\n')
            n++;
    fclose(f);
    *count = n;
    return true;
}

int dc_updates_read(dc_update_backend *out, int max)
{
    int n = DC_UPDATES_BACKENDS_N;
    if (n > max)
        n = max;

    for (int i = 0; i < n; i++) {
        out[i].name = BACKENDS[i].name;
        out[i].available = backend_available(i);
        out[i].count = -1;
        out[i].mtime = 0;
        int count = 0;
        long mtime = 0;
        if (read_cache_file(BACKENDS[i].cache_path, &count, &mtime)) {
            out[i].count = count;
            out[i].mtime = mtime;
        }
    }
    return n;
}

int dc_updates_total(void)
{
    dc_update_backend rows[DC_UPDATES_BACKENDS_N];
    int n = dc_updates_read(rows, DC_UPDATES_BACKENDS_N);
    int total = 0;
    for (int i = 0; i < n; i++) {
        if (rows[i].available && rows[i].count > 0)
            total += rows[i].count;
    }
    return total;
}

void dc_updates_auto_tick(int interval_min)
{
    if (interval_min <= 0)
        return; /* manual-only, same default as the System Updater tab's own knob */

    static time_t last_check = 0;
    time_t now = time(NULL);
    if (last_check != 0 && now - last_check < interval_min * 60)
        return;
    last_check = now;
    dc_updates_check_async();
}

/* --- upgrade ------------------------------------------------------------ */

#define DC_UPGRADE_CMD_MAX 200

static bool backend_upgrade_cmd(const char *backend, char out[DC_UPGRADE_CMD_MAX])
{
    if (strcmp(backend, "pacman") == 0 || strcmp(backend, "aur") == 0) {
        /* docs/29-SMALL-FEATURES-PLAN.md sec.4's key default -- plain repo
         * sync/upgrade; upgrading AUR packages themselves still needs the
         * user's own AUR-helper invocation from within the spawned terminal. */
        snprintf(out, DC_UPGRADE_CMD_MAX, "sudo pacman -Syu");
        return true;
    }
    if (strcmp(backend, "flatpak") == 0) {
        snprintf(out, DC_UPGRADE_CMD_MAX, "flatpak update");
        return true;
    }
    return false;
}

/* Try a handful of common terminal emulators in order and run `inner_cmd`
 * inside whichever is found first, detached -- same shape as
 * ui/settings.c's mux_launch_terminal(). */
static void auto_probe_terminal(const char inner_cmd[DC_UPGRADE_CMD_MAX], bool dryrun)
{
    char cmd[2 * DC_UPGRADE_CMD_MAX + 220];
    snprintf(cmd, sizeof(cmd),
             "for t in foot kitty alacritty wezterm ghostty xterm; do "
             "command -v \"$t\" >/dev/null 2>&1 || continue; "
             "if [ \"$t\" = wezterm ]; then \"$t\" start -- sh -c '%s' & else "
             "\"$t\" -e sh -c '%s' & fi; exit 0; done",
             inner_cmd, inner_cmd);
    if (dryrun) {
        dc_info("updates: [DANKC_UPDATES_DRYRUN] would auto-probe a terminal and run: %s", cmd);
        return;
    }
    run_detached(cmd);
}

void dc_updates_run_upgrade(const char *backend)
{
    char inner[DC_UPGRADE_CMD_MAX];
    if (!backend || !backend_upgrade_cmd(backend, inner)) {
        dc_warn("updates: run_upgrade: unrecognized backend '%s'", backend ? backend : "(null)");
        return;
    }

    bool dryrun = getenv("DANKC_UPDATES_DRYRUN") != NULL;
    const dc_config *cfg = dc_config_current;

    if (cfg->update_terminal_cmd[0]) {
        if (!strstr(cfg->update_terminal_cmd, "%s")) {
            dc_warn("updates: updateTerminalCmd has no '%%s' placeholder for the upgrade command "
                    "-- ignoring override, falling back to auto-probe");
        } else {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), cfg->update_terminal_cmd, inner);
            if (dryrun) {
                dc_info("updates: [DANKC_UPDATES_DRYRUN] would run (updateTerminalCmd): %s", cmd);
                return;
            }
            run_detached(cmd);
            return;
        }
    }

    auto_probe_terminal(inner, dryrun);
}
