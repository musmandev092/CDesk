/* printers.c — see printers.h for the public contract + design summary.
 *
 * Parsing targets (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.6), CUPS
 * `lpstat`(1) output shapes:
 *
 *   lpstat -p -l
 *     "printer <name> is idle.  enabled since <date>"
 *     "printer <name> now printing <job>.  enabled since <date>"
 *     "printer <name> disabled since <date> - <reason>"
 *     followed by indented continuation lines for that printer, of which
 *     only "\tDescription: ..." and "\tLocation: ..." are used here.
 *
 *   lpstat -d
 *     "system default destination: <name>"
 *     "no system default destination"        (no default set; exit 0)
 *
 *   lpstat -a
 *     "<name> accepting requests since <date>"
 *     "<name> not accepting requests since <date>"
 *
 *   lpstat -o [dest]
 *     "<queue>-<jobid> <user>   <size>   <date>"
 *     (column widths are not a stable contract -- only the first two
 *     whitespace-delimited tokens are parsed structurally, the rest is kept
 *     as one free-form string.)
 *
 * On a machine with no CUPS/no printers configured, `lpstat -p`/`-d`/`-a`/
 * `-o` exit non-zero and print a one-line diagnostic to stderr (verified
 * directly: "lpstat: No destinations added." on this dev machine) --
 * stderr is always redirected to /dev/null and the parser simply finds no
 * matching lines, so every read path here returns 0/"" cleanly with no
 * special-cased error handling needed.
 */
#include "services/printers.h"

#include "core/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* --- availability ---------------------------------------------------------- */

bool dc_printers_available(void)
{
    return system("command -v lpstat >/dev/null 2>&1") == 0;
}

const char *dc_printer_state_name(dc_printer_state s)
{
    switch (s) {
    case DC_PRINTER_STATE_IDLE:
        return "idle";
    case DC_PRINTER_STATE_PRINTING:
        return "printing";
    case DC_PRINTER_STATE_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

/* Strip a trailing \n and/or \r in place, returning the new length. */
static size_t chomp(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    return len;
}

/* --- default destination ---------------------------------------------------- */

static void read_default(char *buf, size_t buf_cap)
{
    if (buf_cap > 0)
        buf[0] = '\0';
    if (!dc_printers_available())
        return;

    FILE *pipe = popen("lpstat -d 2>/dev/null", "r");
    if (!pipe) {
        dc_warn("printers: failed to launch `lpstat -d`");
        return;
    }
    char line[256];
    if (fgets(line, sizeof(line), pipe)) {
        chomp(line);
        const char *marker = "destination:";
        char *at = strstr(line, marker);
        if (at) {
            const char *v = at + strlen(marker);
            while (*v == ' ' || *v == '\t')
                v++;
            snprintf(buf, buf_cap, "%s", v);
        }
        /* else: "no system default destination" -- buf stays "". */
    }
    pclose(pipe);
}

void dc_printers_get_default(char *buf, size_t buf_cap)
{
    read_default(buf, buf_cap);
}

/* --- accepting-requests ------------------------------------------------------ */

static void mark_accepting(dc_printer_info *out, int n)
{
    FILE *pipe = popen("lpstat -a 2>/dev/null", "r");
    if (!pipe)
        return;
    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        chomp(line);
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        size_t nlen = (size_t)(sp - line);
        char name[DC_PRINTER_NAME_MAX];
        if (nlen >= sizeof(name))
            nlen = sizeof(name) - 1;
        memcpy(name, line, nlen);
        name[nlen] = '\0';

        bool accepting = strstr(sp, "not accepting") == NULL;
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].name, name) == 0)
                out[i].accepting = accepting;
        }
    }
    pclose(pipe);
}

/* --- printer list ------------------------------------------------------- */

int dc_printers_list(dc_printer_info out[DC_PRINTERS_MAX])
{
    memset(out, 0, sizeof(*out) * DC_PRINTERS_MAX);
    int n = 0;

    if (!dc_printers_available()) {
        dc_info("printers: lpstat not found on PATH, reporting empty list");
        return 0;
    }

    FILE *pipe = popen("lpstat -p -l 2>/dev/null", "r");
    if (!pipe) {
        dc_warn("printers: failed to launch `lpstat -p -l`");
        return 0;
    }

    int cur = -1; /* index into out[] the following indented lines belong to */
    char line[512];
    while (fgets(line, sizeof(line), pipe)) {
        chomp(line);
        if (line[0] == '\0')
            continue;

        if (strncmp(line, "printer ", 8) == 0) {
            cur = -1;
            if (n >= DC_PRINTERS_MAX)
                continue; /* keep draining the pipe, just stop recording */

            const char *name = line + 8;
            const char *sp = strchr(name, ' ');
            size_t nlen = sp ? (size_t)(sp - name) : strlen(name);
            if (nlen >= sizeof(out[n].name))
                nlen = sizeof(out[n].name) - 1;
            memcpy(out[n].name, name, nlen);
            out[n].name[nlen] = '\0';

            const char *rest = sp ? sp + 1 : "";
            while (*rest == ' ')
                rest++;
            snprintf(out[n].status_text, sizeof(out[n].status_text), "%s", rest);

            if (strstr(rest, "disabled") != NULL)
                out[n].state = DC_PRINTER_STATE_STOPPED;
            else if (strstr(rest, "idle") != NULL)
                out[n].state = DC_PRINTER_STATE_IDLE;
            else if (strstr(rest, "printing") != NULL)
                out[n].state = DC_PRINTER_STATE_PRINTING;
            else
                out[n].state = DC_PRINTER_STATE_UNKNOWN;

            cur = n;
            n++;
            continue;
        }

        if (cur < 0)
            continue;

        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (strncmp(p, "Description:", 12) == 0) {
            const char *v = p + 12;
            while (*v == ' ' || *v == '\t')
                v++;
            snprintf(out[cur].description, sizeof(out[cur].description), "%s", v);
        } else if (strncmp(p, "Location:", 9) == 0) {
            const char *v = p + 9;
            while (*v == ' ' || *v == '\t')
                v++;
            snprintf(out[cur].location, sizeof(out[cur].location), "%s", v);
        }
    }
    pclose(pipe);

    char default_name[DC_PRINTER_NAME_MAX];
    read_default(default_name, sizeof(default_name));
    if (default_name[0]) {
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].name, default_name) == 0)
                out[i].is_default = true;
        }
    }

    mark_accepting(out, n);

    dc_info("printers: %d queue(s) found (default=\"%s\")", n, default_name);
    return n;
}

/* --- writes: fork+execvp, dry-run gated ---------------------------------- */

static bool dryrun_enabled(void)
{
    const char *v = getenv("DANKC_PRINTERS_DRYRUN");
    return v && v[0] == '1';
}

/* argv must be NULL-terminated; argv[0] is the binary to exec (found via
 * $PATH, e.g. "lpoptions"/"lp"/"cancel"). Fire-and-forget, reaped by main's
 * SIGCHLD = SIG_IGN, same shape as display.c's run_niri_output_cmd(). */
static void run_cmd(const char *tag, const char *const argv[], int argc)
{
    if (dryrun_enabled()) {
        char line[1024];
        int off = snprintf(line, sizeof(line), "[dryrun]");
        for (int i = 0; i < argc && argv[i]; i++) {
            off += snprintf(line + off, off < (int)sizeof(line) ? sizeof(line) - (size_t)off : 0,
                    " %s", argv[i]);
        }
        dc_info("printers: %s: %s", tag, line);
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Parent: fire-and-forget. */
}

void dc_printers_set_default(const char *name)
{
    if (!name || !name[0]) {
        dc_warn("printers: set_default called with empty name, ignoring");
        return;
    }
    const char *argv[] = {"lpoptions", "-d", name, NULL};
    run_cmd("set-default", argv, 3);
}

#define DC_PRINTERS_TESTPRINT_FILE "/usr/share/cups/data/testprint"

void dc_printers_test_page(const char *name)
{
    if (!name || !name[0]) {
        dc_warn("printers: test_page called with empty name, ignoring");
        return;
    }
    /* Only check the test file exists when NOT dry-running: dry-run should
     * still log the intended argv even on a machine without CUPS data
     * files installed (e.g. this dev machine, per the task's verification
     * step), rather than silently skipping before the log line fires. */
    if (!dryrun_enabled() && access(DC_PRINTERS_TESTPRINT_FILE, R_OK) != 0) {
        dc_warn("printers: test page source %s not found, skipping",
                DC_PRINTERS_TESTPRINT_FILE);
        return;
    }
    const char *argv[] = {"lp", "-d", name, DC_PRINTERS_TESTPRINT_FILE, NULL};
    run_cmd("test-page", argv, 4);
}

void dc_printers_cancel_job(const char *id)
{
    if (!id || !id[0]) {
        dc_warn("printers: cancel_job called with empty id, ignoring");
        return;
    }
    const char *argv[] = {"cancel", id, NULL};
    run_cmd("cancel-job", argv, 2);
}

/* --- job queue ------------------------------------------------------------ */

int dc_printers_jobs(const char *name, dc_printer_job out[DC_PRINTER_JOBS_MAX])
{
    memset(out, 0, sizeof(*out) * DC_PRINTER_JOBS_MAX);
    int n = 0;

    if (!dc_printers_available())
        return 0;

    /* Always query *all* destinations -- `name` (however it originated) is
     * never interpolated into the shell command line, it's only used to
     * filter parsed results client-side below. */
    FILE *pipe = popen("lpstat -o 2>/dev/null", "r");
    if (!pipe) {
        dc_warn("printers: failed to launch `lpstat -o`");
        return 0;
    }

    size_t name_len = name ? strlen(name) : 0;
    char line[512];
    while (n < DC_PRINTER_JOBS_MAX && fgets(line, sizeof(line), pipe)) {
        chomp(line);
        if (line[0] == '\0')
            continue;

        char *save = NULL;
        char *tok_id = strtok_r(line, " \t", &save);
        if (!tok_id)
            continue;

        if (name_len > 0) {
            size_t idlen = strlen(tok_id);
            if (!(idlen > name_len && strncmp(tok_id, name, name_len) == 0 &&
                        tok_id[name_len] == '-'))
                continue;
        }

        char *tok_user = strtok_r(NULL, " \t", &save);
        const char *rest = save ? save : "";
        while (*rest == ' ' || *rest == '\t')
            rest++;

        snprintf(out[n].id, sizeof(out[n].id), "%s", tok_id);
        snprintf(out[n].user, sizeof(out[n].user), "%s", tok_user ? tok_user : "");
        snprintf(out[n].info, sizeof(out[n].info), "%s", rest);
        n++;
    }
    pclose(pipe);
    return n;
}
