#include "services/apps.h"

#include "core/log.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define DC_APPS_CAP 1024

struct dc_apps {
    dc_app *items;
    int count;
    int cap;
};

/* Lowercase ASCII copy into `dst` (bounded). */
static void str_lower(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < n; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* Strip desktop Exec field codes (%f %F %u %U %i %c %k ...) and surrounding
 * quotes, collapsing whitespace. Result is a plain command line. */
static void clean_exec(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < n; i++) {
        if (src[i] == '%' && src[i + 1]) {
            i++; /* skip the field code letter */
            continue;
        }
        if (src[i] == '"') {
            continue;
        }
        dst[o++] = src[i];
    }
    /* trim trailing space */
    while (o > 0 && (dst[o - 1] == ' ' || dst[o - 1] == '\t'))
        o--;
    dst[o] = '\0';
}

/* Second pass over a .desktop file: fill app->actions[] from `[Desktop
 * Action <id>]` groups whose <id> appears in `actions_raw` (the raw
 * semicolon-separated Actions= value read during the first pass). Actions
 * are kept in Actions='s own order regardless of the groups' order in the
 * file (both are typical, but Actions= is the authoritative order per the
 * desktop-entry spec). */
static void parse_desktop_actions(const char *path, const char *actions_raw, dc_app *app)
{
    char ids[DC_APP_ACTION_MAX][DC_APP_ACTION_NAME];
    int nids = 0;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", actions_raw);
    for (char *tok = strtok(buf, ";"); tok && nids < DC_APP_ACTION_MAX; tok = strtok(NULL, ";")) {
        if (!*tok)
            continue;
        snprintf(ids[nids], sizeof(ids[nids]), "%s", tok);
        nids++;
    }
    if (nids == 0)
        return;

    char tmp_name[DC_APP_ACTION_MAX][DC_APP_ACTION_NAME] = {{0}};
    char tmp_exec[DC_APP_ACTION_MAX][DC_APP_EXEC] = {{0}};

    FILE *f = fopen(path, "r");
    if (!f)
        return;

    int cur_idx = -1; /* which ids[]/tmp_*[] slot the current group targets, or -1 */
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';

        if (line[0] == '[') {
            cur_idx = -1;
            size_t plen = strlen("[Desktop Action ");
            size_t len = strlen(line);
            if (len > plen && line[len - 1] == ']' && strncmp(line, "[Desktop Action ", plen) == 0) {
                size_t idlen = len - plen - 1;
                if (idlen < DC_APP_ACTION_NAME) {
                    char aid[DC_APP_ACTION_NAME];
                    memcpy(aid, line + plen, idlen);
                    aid[idlen] = '\0';
                    for (int i = 0; i < nids; i++)
                        if (strcmp(ids[i], aid) == 0) {
                            cur_idx = i;
                            break;
                        }
                }
            }
            continue;
        }
        if (cur_idx < 0)
            continue;

        if (strncmp(line, "Name=", 5) == 0 && !tmp_name[cur_idx][0]) {
            snprintf(tmp_name[cur_idx], sizeof(tmp_name[cur_idx]), "%.*s", DC_APP_ACTION_NAME - 1,
                     line + 5);
        } else if (strncmp(line, "Exec=", 5) == 0 && !tmp_exec[cur_idx][0]) {
            clean_exec(tmp_exec[cur_idx], sizeof(tmp_exec[cur_idx]), line + 5);
        }
    }
    fclose(f);

    for (int i = 0; i < nids && app->action_count < DC_APP_ACTION_MAX; i++) {
        if (!tmp_name[i][0] || !tmp_exec[i][0])
            continue; /* incomplete action group; skip rather than show a dead row */
        snprintf(app->actions[app->action_count].name, sizeof(app->actions[app->action_count].name),
                 "%.*s", (int)sizeof(app->actions[app->action_count].name) - 1, tmp_name[i]);
        snprintf(app->actions[app->action_count].exec, sizeof(app->actions[app->action_count].exec),
                 "%.*s", (int)sizeof(app->actions[app->action_count].exec) - 1, tmp_exec[i]);
        app->action_count++;
    }
}

/* Parse one .desktop file into `app`. Returns 1 if it's a launchable, visible
 * Application, else 0. `id` is the basename without ".desktop". */
static int parse_desktop(const char *path, const char *id, dc_app *app)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char name[DC_APP_NAME] = {0};
    char exec[DC_APP_EXEC] = {0};
    /* Comment= wins over GenericName= (e.g. "Access the Internet" vs "Web
     * Browser"), matching the reference launcher's row descriptions
     * (docs/13-POPOUTS-SPEC.md sec.6). Both are optional. */
    char comment[DC_APP_DESC] = {0};
    char generic[DC_APP_DESC] = {0};
    char actions_raw[256] = {0}; /* raw Actions= value, e.g. "new-window;new-private-window;" */
    int is_application = 1;      /* assume Application unless Type says otherwise */
    int hidden = 0;
    int in_entry = 0;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';

        if (line[0] == '[') {
            /* Only read the main [Desktop Entry] group; Actions= groups are
             * picked up by a dedicated second pass (parse_desktop_actions),
             * since we don't yet know which action ids matter until we've
             * seen Actions= itself. */
            in_entry = strcmp(line, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_entry)
            continue;

        if (strncmp(line, "Name=", 5) == 0 && !name[0]) {
            snprintf(name, sizeof(name), "%.*s", DC_APP_NAME - 1, line + 5);
        } else if (strncmp(line, "Exec=", 5) == 0 && !exec[0]) {
            clean_exec(exec, sizeof(exec), line + 5);
        } else if (strncmp(line, "Comment=", 8) == 0 && !comment[0]) {
            snprintf(comment, sizeof(comment), "%.*s", DC_APP_DESC - 1, line + 8);
        } else if (strncmp(line, "GenericName=", 12) == 0 && !generic[0]) {
            snprintf(generic, sizeof(generic), "%.*s", DC_APP_DESC - 1, line + 12);
        } else if (strncmp(line, "Actions=", 8) == 0 && !actions_raw[0]) {
            snprintf(actions_raw, sizeof(actions_raw), "%.*s", (int)sizeof(actions_raw) - 1,
                     line + 8);
        } else if (strncmp(line, "Type=", 5) == 0) {
            is_application = strcmp(line + 5, "Application") == 0;
        } else if (strncmp(line, "NoDisplay=", 10) == 0) {
            hidden = hidden || strcmp(line + 10, "true") == 0;
        } else if (strncmp(line, "Hidden=", 7) == 0) {
            hidden = hidden || strcmp(line + 7, "true") == 0;
        }
    }
    fclose(f);

    if (!is_application || hidden || !name[0] || !exec[0])
        return 0;

    snprintf(app->name, sizeof(app->name), "%s", name);
    snprintf(app->exec, sizeof(app->exec), "%s", exec);
    snprintf(app->id, sizeof(app->id), "%s", id);
    snprintf(app->desc, sizeof(app->desc), "%s", comment[0] ? comment : generic);
    app->score = 0;
    app->action_count = 0;
    if (actions_raw[0])
        parse_desktop_actions(path, actions_raw, app);
    return 1;
}

/* True once we've already indexed a desktop id (earlier dirs win, per XDG). */
static int already_have(const dc_apps *apps, const char *id)
{
    for (int i = 0; i < apps->count; i++)
        if (strcmp(apps->items[i].id, id) == 0)
            return 1;
    return 0;
}

static void scan_dir(dc_apps *apps, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && apps->count < apps->cap) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".desktop") != 0)
            continue;

        char id[DC_APP_ID];
        size_t idlen = (size_t)(dot - ent->d_name);
        if (idlen >= sizeof(id))
            continue;
        memcpy(id, ent->d_name, idlen);
        id[idlen] = '\0';

        if (already_have(apps, id))
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%.500s/%.400s", dir, ent->d_name);
        if (parse_desktop(path, id, &apps->items[apps->count]))
            apps->count++;
    }
    closedir(d);
}

static int cmp_name(const void *a, const void *b)
{
    const dc_app *x = a, *y = b;
    return strcasecmp(x->name, y->name);
}

dc_apps *dc_apps_load(void)
{
    dc_apps *apps = calloc(1, sizeof(*apps));
    apps->cap = DC_APPS_CAP;
    apps->items = calloc(apps->cap, sizeof(*apps->items));

    /* XDG order: user data dir first (wins on id conflicts), then XDG_DATA_DIRS. */
    const char *home = getenv("HOME");
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    char user_dir[768];
    if (xdg_data_home && *xdg_data_home)
        snprintf(user_dir, sizeof(user_dir), "%.700s/applications", xdg_data_home);
    else if (home)
        snprintf(user_dir, sizeof(user_dir), "%.700s/.local/share/applications", home);
    else
        user_dir[0] = '\0';
    if (user_dir[0])
        scan_dir(apps, user_dir);

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !*data_dirs)
        data_dirs = "/usr/local/share:/usr/share";
    char *dirs = strdup(data_dirs);
    for (char *tok = strtok(dirs, ":"); tok; tok = strtok(NULL, ":")) {
        char dir[768];
        snprintf(dir, sizeof(dir), "%.700s/applications", tok);
        scan_dir(apps, dir);
    }
    free(dirs);

    qsort(apps->items, apps->count, sizeof(*apps->items), cmp_name);
    dc_info("app launcher indexed %d desktop entries", apps->count);
    return apps;
}

void dc_apps_destroy(dc_apps *apps)
{
    if (!apps)
        return;
    free(apps->items);
    free(apps);
}

int dc_apps_count(const dc_apps *apps)
{
    return apps ? apps->count : 0;
}

/* Score `name` against the lowercased `q`. Higher is better; 0 = no match.
 * Rewards exact prefix and word-start hits, then falls back to subsequence. */
static int score_match(const char *name, const char *q)
{
    if (!q[0])
        return 1;

    char lname[DC_APP_NAME];
    str_lower(lname, sizeof(lname), name);

    /* exact prefix */
    if (strncmp(lname, q, strlen(q)) == 0)
        return 1000 - (int)strlen(lname);

    /* substring, bonus if at a word boundary */
    const char *sub = strstr(lname, q);
    if (sub) {
        int word_start = (sub == lname) || sub[-1] == ' ' || sub[-1] == '-';
        return (word_start ? 600 : 400) - (int)(sub - lname);
    }

    /* subsequence (all query chars appear in order) */
    const char *p = lname;
    for (const char *c = q; *c; c++) {
        p = strchr(p, *c);
        if (!p)
            return 0;
        p++;
    }
    return 100;
}

static int cmp_score(const void *a, const void *b)
{
    const dc_app *const *x = a, *const *y = b;
    if ((*x)->score != (*y)->score)
        return (*y)->score - (*x)->score; /* descending */
    return strcasecmp((*x)->name, (*y)->name);
}

int dc_apps_search(dc_apps *apps, const char *query, const dc_app **out, int max)
{
    if (!apps || max <= 0)
        return 0;

    char q[DC_APP_NAME];
    str_lower(q, sizeof(q), query ? query : "");

    /* Collect matches (score them), then sort and take the top `max`. */
    const dc_app **matches = calloc(apps->count > 0 ? apps->count : 1, sizeof(*matches));
    int m = 0;
    for (int i = 0; i < apps->count; i++) {
        int s = score_match(apps->items[i].name, q);
        if (s > 0) {
            apps->items[i].score = s;
            matches[m++] = &apps->items[i];
        }
    }
    qsort(matches, m, sizeof(*matches), cmp_score);

    int n = m < max ? m : max;
    for (int i = 0; i < n; i++)
        out[i] = matches[i];
    free(matches);
    return n;
}

void dc_app_launch_exec(const char *exec)
{
    if (!exec || !exec[0])
        return;

    pid_t pid = fork();
    if (pid == 0) {
        /* Double-fork so the app reparents to init and never zombies us. */
        setsid();
        if (fork() == 0) {
            execl("/bin/sh", "sh", "-c", exec, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    }
}

void dc_app_launch(const dc_app *app)
{
    if (!app || !app->exec[0])
        return;
    dc_info("launching %s: %s", app->name, app->exec);
    dc_app_launch_exec(app->exec);
}

void dc_app_launch_action(const dc_app *app, int idx)
{
    if (!app || idx < 0 || idx >= app->action_count)
        return;
    const dc_app_action *a = &app->actions[idx];
    dc_info("launching %s action '%s': %s", app->name, a->name, a->exec);
    dc_app_launch_exec(a->exec);
}
