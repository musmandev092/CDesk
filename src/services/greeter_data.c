#include "services/greeter_data.h"

#include "core/log.h"

#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- users ---------------------------------------------------------- */

/* True if `s` ends with `suffix` (used to reject system shells like
 * "/usr/sbin/nologin" or "/bin/false" without caring about the shell's full
 * path/prefix). */
static bool ends_with(const char *s, const char *suffix)
{
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen)
        return false;
    return strcmp(s + (slen - suflen), suffix) == 0;
}

static bool is_real_login_shell(const char *shell)
{
    if (!shell || !shell[0])
        return false;
    return !ends_with(shell, "nologin") && !ends_with(shell, "false");
}

/* GECOS is a comma-separated list (full name, room number, work phone, home
 * phone, other); only the first field is a display name. Falls back to `name`
 * if GECOS is absent or its first field is empty. */
static void gecos_display_name(const char *gecos, const char *name, char *out, size_t n)
{
    if (gecos && gecos[0]) {
        const char *comma = strchr(gecos, ',');
        size_t len = comma ? (size_t)(comma - gecos) : strlen(gecos);
        if (len > 0) {
            if (len >= n)
                len = n - 1;
            memcpy(out, gecos, len);
            out[len] = '\0';
            return;
        }
    }
    snprintf(out, n, "%s", name);
}

int dc_greeter_users(dc_greeter_user *out, int max)
{
    if (!out || max <= 0)
        return 0;

    int n = 0;
    setpwent();
    struct passwd *pw;
    while (n < max && (pw = getpwent()) != NULL) {
        if (pw->pw_uid < 1000 || pw->pw_uid >= 60000)
            continue;
        if (!pw->pw_name || !pw->pw_name[0] || strcmp(pw->pw_name, "nobody") == 0)
            continue;
        if (!pw->pw_dir || strcmp(pw->pw_dir, "/var/empty") == 0)
            continue;
        if (!is_real_login_shell(pw->pw_shell))
            continue;

        snprintf(out[n].name, sizeof(out[n].name), "%s", pw->pw_name);
        gecos_display_name(pw->pw_gecos, pw->pw_name, out[n].display, sizeof(out[n].display));
        n++;
    }
    endpwent();

    dc_info("greeter: enumerated %d login user(s)", n);
    return n;
}

/* --- sessions --------------------------------------------------------- */

/* Strip desktop Exec field codes (%f %F %u %U %i %c %k ...) and surrounding
 * quotes, collapsing trailing whitespace. Same convention as
 * services/apps.c's clean_exec() (duplicated here rather than shared, since
 * apps.c's copy is file-static and this module has no other apps.c
 * dependency). */
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

/* Parse one *.desktop file's [Desktop Entry] group into `sess`. Returns true
 * if it's a usable session entry (visible, has Name= and Exec=). */
static bool parse_session_desktop(const char *path, dc_greeter_session *sess)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char name[DC_GREETER_SESSION_NAME] = {0};
    char exec_raw[512] = {0};
    char desktop_names[DC_GREETER_SESSION_DESKTOP_NAMES] = {0};
    bool hidden = false;
    bool in_entry = false;

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
        } else if (strncmp(line, "Exec=", 5) == 0 && !exec_raw[0]) {
            snprintf(exec_raw, sizeof(exec_raw), "%.*s", (int)sizeof(exec_raw) - 1, line + 5);
        } else if (strncmp(line, "DesktopNames=", 13) == 0 && !desktop_names[0]) {
            snprintf(desktop_names, sizeof(desktop_names), "%.*s", (int)sizeof(desktop_names) - 1,
                     line + 13);
        } else if (strncmp(line, "Hidden=", 7) == 0) {
            hidden = hidden || strcmp(line + 7, "true") == 0;
        }
    }
    fclose(f);

    if (hidden || !name[0] || !exec_raw[0])
        return false;

    snprintf(sess->name, sizeof(sess->name), "%s", name);
    clean_exec(sess->exec, sizeof(sess->exec), exec_raw);
    snprintf(sess->desktop_names, sizeof(sess->desktop_names), "%s", desktop_names);
    return sess->exec[0] != '\0';
}

#define DC_GREETER_SESSION_ID 128

/* True once we've already indexed a desktop id (earlier data dirs win, per
 * XDG data-dirs precedence — same rule apps.c applies to application
 * desktop files). */
static bool session_already_have(char ids[][DC_GREETER_SESSION_ID], int nids, const char *id)
{
    for (int i = 0; i < nids; i++)
        if (strcmp(ids[i], id) == 0)
            return true;
    return false;
}

/* Scan one subdirectory (a "wayland-sessions" or "xsessions" under some XDG
 * data dir) for *.desktop files, appending usable ones to `out`. */
static void scan_session_dir(const char *dir, bool is_x11, dc_greeter_session *out, int *n,
                              int max, char ids[][DC_GREETER_SESSION_ID], int *nids, int max_ids)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while (*n < max && (ent = readdir(d)) != NULL) {
        const char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".desktop") != 0)
            continue;

        char id[DC_GREETER_SESSION_ID];
        size_t idlen = (size_t)(dot - ent->d_name);
        if (idlen >= sizeof(id))
            continue;
        memcpy(id, ent->d_name, idlen);
        id[idlen] = '\0';

        if (session_already_have(ids, *nids, id))
            continue;

        char path[1024];
        snprintf(path, sizeof(path), "%.500s/%.400s", dir, ent->d_name);
        if (parse_session_desktop(path, &out[*n])) {
            out[*n].is_x11 = is_x11;
            (*n)++;
            if (*nids < max_ids) {
                snprintf(ids[*nids], DC_GREETER_SESSION_ID, "%s", id);
                (*nids)++;
            }
        }
    }
    closedir(d);
}

int dc_greeter_sessions(dc_greeter_session *out, int max)
{
    if (!out || max <= 0)
        return 0;

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !*data_dirs)
        data_dirs = "/usr/local/share:/usr/share";

    /* Track seen ids across both passes so a stray same-name file in both
     * wayland-sessions/ and xsessions/ (or duplicated across data dirs)
     * doesn't produce two rows. */
    char (*ids)[DC_GREETER_SESSION_ID] = calloc((size_t)max, DC_GREETER_SESSION_ID);
    int nids = 0;
    int n = 0;

    if (ids) {
        static const struct {
            const char *subdir;
            bool is_x11;
        } kinds[] = {
            {"wayland-sessions", false},
            {"xsessions", true},
        };

        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]) && n < max; k++) {
            /* strtok mutates its input, so re-copy the dirs list for each pass. */
            char *pass = strdup(data_dirs);
            if (!pass)
                break;
            for (char *tok = strtok(pass, ":"); tok && n < max; tok = strtok(NULL, ":")) {
                char subdir[768];
                snprintf(subdir, sizeof(subdir), "%.700s/%s", tok, kinds[k].subdir);
                scan_session_dir(subdir, kinds[k].is_x11, out, &n, max, ids, &nids, max);
            }
            free(pass);
        }
        free(ids);
    }

    dc_info("greeter: enumerated %d session(s)", n);
    return n;
}

/* --- last-user/last-session memory ------------------------------------ */

static bool state_path(const char *fname, char *out, size_t n)
{
    const char *dir = getenv("DANKC_GREETER_STATE_DIR");
    if (!dir || !*dir)
        return false;
    snprintf(out, n, "%.500s/%s", dir, fname);
    return true;
}

static void write_state_file(const char *fname, const char *value)
{
    if (!value)
        return;
    char path[768];
    if (!state_path(fname, path, sizeof(path)))
        return;

    FILE *f = fopen(path, "w");
    if (!f)
        return; /* unwritable state dir: tolerate, no-op */
    fputs(value, f);
    fputc('\n', f);
    fclose(f);
}

static bool read_state_file(const char *fname, char *out, size_t n)
{
    char path[768];
    if (!state_path(fname, path, sizeof(path)))
        return false;

    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok)
        return false;

    char *nl = strpbrk(out, "\r\n");
    if (nl)
        *nl = '\0';
    return out[0] != '\0';
}

void dc_greeter_remember(const char *user, const char *session_name)
{
    write_state_file("last_user", user);
    write_state_file("last_session", session_name);
}

bool dc_greeter_last_user(char *out, size_t n)
{
    return read_state_file("last_user", out, n);
}

bool dc_greeter_last_session(char *out, size_t n)
{
    return read_state_file("last_session", out, n);
}
