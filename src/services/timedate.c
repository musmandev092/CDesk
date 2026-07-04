/* timedate.c — see timedate.h for the public contract + design summary. */
#include "services/timedate.h"

#include "core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool dc_timedate_available(void)
{
    /* PATH search via access(X_OK), NOT system("command -v ...") -- see
     * printers.c's dc_printers_available() for why system()/popen-based
     * probes can't be trusted for exit status under SIGCHLD=SIG_IGN. */
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin:/usr/local/bin";
    char buf[512];
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0 && len < sizeof(buf) - 12) {
            snprintf(buf, sizeof(buf), "%.*s/timedatectl", (int)len, p);
            if (access(buf, X_OK) == 0)
                return true;
        }
        if (!sep)
            break;
        p = sep + 1;
    }
    return false;
}

static size_t chomp(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    return len;
}

bool dc_timedate_status(dc_timedate_info *out)
{
    memset(out, 0, sizeof(*out));
    if (!dc_timedate_available())
        return false;

    FILE *pipe = popen("timedatectl show 2>/dev/null", "r");
    if (!pipe) {
        dc_warn("timedate: failed to launch `timedatectl show`");
        return false;
    }
    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        chomp(line);
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "Timezone") == 0)
            snprintf(out->timezone, sizeof(out->timezone), "%s", val);
        else if (strcmp(key, "CanNTP") == 0)
            out->can_ntp = strcmp(val, "yes") == 0;
        else if (strcmp(key, "NTP") == 0)
            out->ntp_enabled = strcmp(val, "yes") == 0;
        else if (strcmp(key, "NTPSynchronized") == 0)
            out->ntp_synchronized = strcmp(val, "yes") == 0;
    }
    pclose(pipe); /* return value unusable under SIGCHLD=SIG_IGN, see file header */

    /* Human-readable "Local time: ..." line from plain `timedatectl`
     * (`show`'s TimeUSec is UTC epoch microseconds, not directly useful for
     * a UI label without redoing timedatectl's own formatting). */
    FILE *p2 = popen("timedatectl 2>/dev/null", "r");
    if (p2) {
        while (fgets(line, sizeof(line), p2)) {
            chomp(line);
            const char *marker = "Local time:";
            char *at = strstr(line, marker);
            if (at) {
                const char *v = at + strlen(marker);
                while (*v == ' ' || *v == '\t')
                    v++;
                snprintf(out->now_local, sizeof(out->now_local), "%s", v);
                break;
            }
        }
        pclose(p2);
    }

    return true;
}

/* --- writes: fork+execvp, dry-run gated (same shape as printers.c's run_cmd) --- */

static bool dryrun_enabled(void)
{
    const char *v = getenv("DANKC_TIMEDATE_DRYRUN");
    return v && v[0] == '1';
}

static void run_cmd(const char *tag, const char *const argv[], int argc)
{
    if (dryrun_enabled()) {
        char line[512];
        int off = snprintf(line, sizeof(line), "[dryrun]");
        for (int i = 0; i < argc && argv[i]; i++)
            off += snprintf(line + off, off < (int)sizeof(line) ? sizeof(line) - (size_t)off : 0,
                    " %s", argv[i]);
        dc_info("timedate: %s: %s", tag, line);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Parent: fire-and-forget, reaped by main's SIGCHLD = SIG_IGN. */
}

void dc_timedate_set_ntp(bool enable)
{
    if (!dryrun_enabled() && !dc_timedate_available()) {
        dc_warn("timedate: set_ntp called but timedatectl not found, ignoring");
        return;
    }
    const char *argv[] = {"timedatectl", "set-ntp", enable ? "true" : "false", NULL};
    run_cmd("set-ntp", argv, 3);
}

void dc_timedate_set_timezone(const char *tz)
{
    if (!tz || !tz[0]) {
        dc_warn("timedate: set_timezone called with empty tz, ignoring");
        return;
    }
    if (!dryrun_enabled() && !dc_timedate_available()) {
        dc_warn("timedate: set_timezone called but timedatectl not found, ignoring");
        return;
    }
    const char *argv[] = {"timedatectl", "set-timezone", tz, NULL};
    run_cmd("set-timezone", argv, 3);
}

int dc_timedate_list_timezones(char out[][DC_TIMEDATE_TZ_MAX], int max)
{
    if (!dc_timedate_available())
        return 0;

    FILE *pipe = popen("timedatectl list-timezones 2>/dev/null", "r");
    if (!pipe) {
        dc_warn("timedate: failed to launch `timedatectl list-timezones`");
        return 0;
    }
    int n = 0;
    char line[256];
    while (n < max && fgets(line, sizeof(line), pipe)) {
        chomp(line);
        if (!line[0])
            continue;
        snprintf(out[n], DC_TIMEDATE_TZ_MAX, "%s", line);
        n++;
    }
    pclose(pipe);
    return n;
}
