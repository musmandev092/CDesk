/* printers.h — CUPS printer service: list/default/test-page/job-queue over
 * the CUPS CLI (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.6 "Printers").
 *
 * Shells out to `lpstat`/`lpoptions`/`lp`/`cancel` -- no libcups linkage,
 * matching the project's existing "shell out to CLI, don't link heavy
 * client libs" pattern used everywhere else in `services/` (power.c,
 * display.c, nightlight.c, ...). Confirmed via docs/19's research: every
 * operation here is per-user, no root:
 *   - `lpoptions -d <name>` writes ~/.cups/lpoptions as the invoking user.
 *   - `lp -d <name> <file>` spools a job as the invoking user.
 *   - `cancel <job-id>` is per-user for the job's own owner.
 * Adding/removing printer queues (`lpadmin`) needs root or `lpadmin` group
 * membership and is deliberately out of scope -- punt to system-config-printer
 * or the CUPS web UI (http://localhost:631), same as the old read-only
 * `tab_printer` comment already said. Ink/toner levels are not exposed by
 * CUPS at all (vendor/driver-specific) -- correctly out of scope too.
 *
 * Degrades gracefully everywhere: no `lpstat` on PATH, no CUPS daemon, or
 * zero configured printers all produce an empty list / false / no-op,
 * never a crash or a blocking hang beyond the CLI's own runtime.
 */
#ifndef DC_SERVICES_PRINTERS_H
#define DC_SERVICES_PRINTERS_H

#include <stdbool.h>
#include <stddef.h>

#define DC_PRINTERS_MAX 16
#define DC_PRINTER_NAME_MAX 128
#define DC_PRINTER_TEXT_MAX 256
#define DC_PRINTER_JOBS_MAX 32

typedef enum {
    DC_PRINTER_STATE_UNKNOWN = 0,
    DC_PRINTER_STATE_IDLE,
    DC_PRINTER_STATE_PRINTING,
    DC_PRINTER_STATE_STOPPED,
} dc_printer_state;

/* One configured CUPS queue, as reported by `lpstat -p -l`. */
typedef struct dc_printer_info {
    char name[DC_PRINTER_NAME_MAX];

    dc_printer_state state;
    /* Raw remainder of the "printer <name> ..." line, e.g.
     * "is idle.  enabled since Mon 01 Jul 2026 03:12:00 PM PKT" -- kept
     * verbatim as a human-readable fallback alongside the parsed `state`. */
    char status_text[DC_PRINTER_TEXT_MAX];

    /* From `lpstat -p -l`'s indented "Description:"/"Location:" lines.
     * Empty if CUPS reports none (both are optional per-queue fields). */
    char description[DC_PRINTER_TEXT_MAX];
    char location[DC_PRINTER_TEXT_MAX];

    bool is_default; /* matches `lpstat -d`'s system default destination */
    bool accepting;  /* from `lpstat -a` -- accepting new jobs */
} dc_printer_info;

/* One queued/printing job, as reported by `lpstat -o`. */
typedef struct dc_printer_job {
    /* Full CUPS job id exactly as lpstat prints it (e.g. "queue-23") --
     * pass verbatim to dc_printers_cancel_job(). */
    char id[DC_PRINTER_NAME_MAX];
    char user[64];
    /* Free-form remainder (size + submission date), kept as one string
     * rather than split further -- lpstat's column widths aren't a stable
     * contract worth over-parsing. */
    char info[DC_PRINTER_TEXT_MAX];
} dc_printer_job;

/* True if `lpstat` is on PATH (proxy for "CUPS client tools installed").
 * Every function below already checks this internally and degrades to an
 * empty/no-op result when false -- exposed separately so a future UI can
 * decide whether to show the whole Printers tab at all (matches
 * tab_printer's existing "hide/empty-state when no backend" pattern). */
bool dc_printers_available(void);

/* One-shot read: shells out to `lpstat -p -l` (list + state + description/
 * location), `lpstat -d` (default), and `lpstat -a` (accepting). Fills
 * out[0..DC_PRINTERS_MAX), returns the count -- 0 is not an error, it just
 * means no printers are configured (or lpstat isn't installed). Blocking/
 * synchronous (bounded by three quick popen()s, same contract as
 * dc_display_list()) -- don't call from a latency-sensitive render path. */
int dc_printers_list(dc_printer_info out[DC_PRINTERS_MAX]);

/* Convenience: current system default queue name from `lpstat -d` alone,
 * without the full list. Writes "" if there is none / CUPS is unavailable.
 * `buf_cap` must be >= DC_PRINTER_NAME_MAX. */
void dc_printers_get_default(char *buf, size_t buf_cap);

/* --- writes ---------------------------------------------------------------
 *
 * Both per-user, no root (see file header). $DANKC_PRINTERS_DRYRUN=1 logs
 * the exact argv instead of forking/executing anything -- same convention
 * as $DANKC_DISPLAY_DRYRUN / $DANKC_NET_DRYRUN, checked at the call site so
 * verification can never touch a live default printer or spool a real job.
 * No-op (logged) if `name` is NULL/empty.
 */

/* `lpoptions -d <name>`. */
void dc_printers_set_default(const char *name);

/* `lp -d <name> /usr/share/cups/data/testprint`. No-ops (logged) if that
 * CUPS-shipped test file isn't present on this machine, rather than
 * silently spooling something else in its place. */
void dc_printers_test_page(const char *name);

/* Job queue: `lpstat -o` (always queries *all* destinations, never
 * interpolates `name` into the shell command) filtered client-side to jobs
 * whose id starts with "<name>-" when `name` is non-NULL/non-empty, or all
 * jobs when NULL/"". Read-only, same blocking contract as
 * dc_printers_list(). Returns the count written to `out`. */
int dc_printers_jobs(const char *name, dc_printer_job out[DC_PRINTER_JOBS_MAX]);

/* `cancel <id>` (id exactly as reported in dc_printer_job.id). Dry-run
 * gated the same as the writes above. No-op (logged) if `id` is empty. */
void dc_printers_cancel_job(const char *id);

/* "idle"/"printing"/"stopped"/"unknown" -- UI-friendly state name. */
const char *dc_printer_state_name(dc_printer_state s);

#endif /* DC_SERVICES_PRINTERS_H */
