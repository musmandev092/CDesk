#include "services/logind.h"

#include "core/log.h"
#include "dc.h"
#include "services/dbus.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DC_LOGIND_DEST "org.freedesktop.login1"
#define DC_LOGIND_MANAGER_IFACE "org.freedesktop.login1.Manager"
#define DC_LOGIND_SESSION_IFACE "org.freedesktop.login1.Session"

struct dc_logind {
    sd_bus *bus;
    sd_bus_slot *sleep_slot;
    sd_bus_slot *lock_slot;
    dc_logind_lock_cb cb;
    void *cb_data;
};

/* PrepareForSleep(b): true just before the system suspends -> lock. */
static int on_prepare_for_sleep(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(err);
    dc_logind *l = userdata;
    int start = 0;
    if (sd_bus_message_read(msg, "b", &start) < 0)
        return 0;
    if (start && l->cb) {
        dc_info("logind: locking before sleep");
        l->cb(l->cb_data);
    }
    return 0;
}

/* Session.Lock: an explicit lock request (e.g. loginctl lock-session). */
static int on_session_lock(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(msg);
    DC_UNUSED(err);
    dc_logind *l = userdata;
    if (l->cb) {
        dc_info("logind: lock requested");
        l->cb(l->cb_data);
    }
    return 0;
}

dc_logind *dc_logind_create(struct dc_dbus *dbus, dc_logind_lock_cb cb, void *user_data)
{
    if (!dbus || !dbus->system)
        return NULL;

    dc_logind *l = calloc(1, sizeof(*l));
    l->bus = dbus->system;
    l->cb = cb;
    l->cb_data = user_data;

    sd_bus_match_signal(l->bus, &l->sleep_slot, DC_LOGIND_DEST, NULL, DC_LOGIND_MANAGER_IFACE,
                        "PrepareForSleep", on_prepare_for_sleep, l);
    sd_bus_match_signal(l->bus, &l->lock_slot, DC_LOGIND_DEST, NULL, DC_LOGIND_SESSION_IFACE, "Lock",
                        on_session_lock, l);

    dc_info("logind: watching sleep + lock signals");
    return l;
}

void dc_logind_destroy(dc_logind *l)
{
    if (!l)
        return;
    sd_bus_slot_unref(l->sleep_slot);
    sd_bus_slot_unref(l->lock_slot);
    free(l);
}

/* ====================== Idle & Lid (docs/19 sec.9) ====================== */

static char *lc_trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

/* Parses a systemd time-span value (e.g. "30min", "1h 30min", "90s", "45",
 * bare seconds if no unit) into whole seconds. Supports the unit spellings
 * actually documented in systemd.time(7) that are plausible in this one
 * field; anything unrecognized is treated as seconds. Returns -1 if `s` is
 * empty. */
static int parse_timespan_sec(const char *s)
{
    if (!s || !*s)
        return -1;
    long total = 0;
    const char *p = s;
    bool any = false;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        char *end = NULL;
        double n = strtod(p, &end);
        if (end == p)
            break; /* not a number -- stop, keep whatever we parsed so far */
        p = end;
        while (*p == ' ' || *p == '\t')
            p++;
        char unit[16];
        size_t ulen = 0;
        while (*p && isalpha((unsigned char)*p) && ulen < sizeof(unit) - 1)
            unit[ulen++] = *p++;
        unit[ulen] = '\0';
        double mult = 1.0; /* bare number = seconds, matches systemd.time(7) */
        if (ulen == 0 || strcmp(unit, "s") == 0 || strcmp(unit, "sec") == 0 ||
                strcmp(unit, "second") == 0 || strcmp(unit, "seconds") == 0)
            mult = 1.0;
        else if (strcmp(unit, "ms") == 0)
            mult = 0.001;
        else if (strcmp(unit, "m") == 0 || strcmp(unit, "min") == 0 ||
                strcmp(unit, "minute") == 0 || strcmp(unit, "minutes") == 0)
            mult = 60.0;
        else if (strcmp(unit, "h") == 0 || strcmp(unit, "hr") == 0 || strcmp(unit, "hour") == 0 ||
                strcmp(unit, "hours") == 0)
            mult = 3600.0;
        else if (strcmp(unit, "d") == 0 || strcmp(unit, "day") == 0 || strcmp(unit, "days") == 0)
            mult = 86400.0;
        else if (strcmp(unit, "w") == 0 || strcmp(unit, "week") == 0 ||
                strcmp(unit, "weeks") == 0)
            mult = 604800.0;
        total += (long)(n * mult);
        any = true;
    }
    return any ? (int)total : -1;
}

/* Parses one logind.conf-style file's [Login] section, updating only the
 * four keys this struct tracks (unrecognized keys/sections are ignored --
 * same tolerant philosophy as ui/settings.c's niri Window Rules parser). */
static void parse_logind_file(const char *path, dc_logind_conf_info *out, bool *any_seen)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    bool in_login = false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = lc_trim(line);
        if (!*s || *s == '#' || *s == ';')
            continue;
        if (*s == '[') {
            in_login = strncasecmp(s, "[Login]", 7) == 0;
            continue;
        }
        if (!in_login)
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = lc_trim(s);
        char *val = lc_trim(eq + 1);
        if (!*val)
            continue; /* e.g. "IdleAction=" with nothing after -- leave default */
        if (strcmp(key, "IdleAction") == 0) {
            snprintf(out->idle_action, sizeof(out->idle_action), "%s", val);
            *any_seen = true;
        } else if (strcmp(key, "IdleActionSec") == 0) {
            int sec = parse_timespan_sec(val);
            if (sec >= 0) {
                out->idle_action_sec = sec;
                *any_seen = true;
            }
        } else if (strcmp(key, "HandleLidSwitch") == 0) {
            snprintf(out->handle_lid_switch, sizeof(out->handle_lid_switch), "%s", val);
            *any_seen = true;
        } else if (strcmp(key, "HandleLidSwitchExternalPower") == 0) {
            snprintf(out->handle_lid_switch_external_power,
                    sizeof(out->handle_lid_switch_external_power), "%s", val);
            *any_seen = true;
        }
    }
    fclose(f);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void dc_logind_conf_read(dc_logind_conf_info *out, const char *conf_dir_override)
{
    /* systemd's own compiled-in defaults (logind.conf(5)) -- the base file
     * ships with every key commented out, so these are what's actually in
     * effect on a stock install. */
    memset(out, 0, sizeof(*out));
    snprintf(out->idle_action, sizeof(out->idle_action), "ignore");
    out->idle_action_sec = 1800; /* 30min */
    snprintf(out->handle_lid_switch, sizeof(out->handle_lid_switch), "suspend");
    snprintf(out->handle_lid_switch_external_power,
            sizeof(out->handle_lid_switch_external_power), "suspend");

    char base[256];
    snprintf(base, sizeof(base), "%s", conf_dir_override && conf_dir_override[0]
                                                ? conf_dir_override
                                                : "/etc/systemd");

    char main_path[300];
    snprintf(main_path, sizeof(main_path), "%s/logind.conf", base);
    bool any_seen = false;
    parse_logind_file(main_path, out, &any_seen);

    char dropin_dir[300];
    snprintf(dropin_dir, sizeof(dropin_dir), "%s/logind.conf.d", base);
    DIR *d = opendir(dropin_dir);
    if (d) {
        char *names[256];
        int n = 0;
        struct dirent *e;
        while (n < 256 && (e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len > 5 && strcmp(e->d_name + len - 5, ".conf") == 0)
                names[n++] = strdup(e->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof(names[0]), name_cmp);
        for (int i = 0; i < n; i++) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dropin_dir, names[i]);
            parse_logind_file(path, out, &any_seen);
            free(names[i]);
        }
    }
    out->from_dropin = any_seen;
}

/* --- write: dankc-owned drop-in via pkexec --------------------------------- */

static bool logind_dryrun_enabled(void)
{
    const char *v = getenv("DANKC_LOGIND_DRYRUN");
    return v && v[0] == '1';
}

bool dc_logind_conf_write_dropin(const dc_logind_conf_info *cfg)
{
    char content[512];
    snprintf(content, sizeof(content),
            "# Managed by DankC's Settings > Power tab.\n"
            "# A re-login or `systemctl restart systemd-logind` is needed for\n"
            "# changes here to take effect -- dankc does not restart it for you.\n"
            "[Login]\n"
            "IdleAction=%s\n"
            "IdleActionSec=%dmin\n"
            "HandleLidSwitch=%s\n"
            "HandleLidSwitchExternalPower=%s\n",
            cfg->idle_action, (cfg->idle_action_sec + 59) / 60, cfg->handle_lid_switch,
            cfg->handle_lid_switch_external_power);

    const char *dest = "/etc/systemd/logind.conf.d/50-dankc.conf";

    if (logind_dryrun_enabled()) {
        dc_info("logind: [dryrun] would write %s:", dest);
        const char *p = content;
        char line[128];
        while (*p) {
            size_t n = strcspn(p, "\n");
            if (n >= sizeof(line))
                n = sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = '\0';
            dc_info("logind: [dryrun]   %s", line);
            p += n;
            if (*p == '\n')
                p++;
        }
        dc_info("logind: [dryrun] pkexec install -D -m 0644 <tmpfile> %s", dest);
        return true;
    }

    char tmp_path[] = "/tmp/dankc-logind-XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        dc_warn("logind: mkstemp() failed, cannot stage drop-in write");
        return false;
    }
    ssize_t want = (ssize_t)strlen(content);
    ssize_t wrote = write(fd, content, (size_t)want);
    close(fd);
    if (wrote != want) {
        dc_warn("logind: short write staging drop-in content, aborting");
        unlink(tmp_path);
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("pkexec", "pkexec", "install", "-D", "-m", "0644", tmp_path, dest, (char *)NULL);
        _exit(127);
    }
    /* Parent: fire-and-forget -- the polkit prompt (answered by dankc's own
     * modal) and the actual privileged copy run fully detached. The staged
     * /tmp file is deliberately left behind for pkexec's child to read
     * asynchronously (no waitpid() available under SIGCHLD=SIG_IGN to know
     * when it's safe to unlink -- same tradeoff as every other pkexec call
     * in this codebase); it's a harmless leftover in /tmp, not a secret. */
    dc_info("logind: requested drop-in write to %s (via pkexec install)", dest);
    return true;
}
