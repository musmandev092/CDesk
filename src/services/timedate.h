/* timedate.h — System Date & Time (docs/19-SETTINGS-COMPLETENESS-PLAN.md
 * sec.8): a thin wrapper over `timedatectl`(1). No D-Bus here (unlike
 * services/logind.c) -- timedatectl itself already goes through
 * org.freedesktop.timedate1 and triggers polkit on its own, and dankc
 * registers its own polkit agent (services/polkit.c) so that prompt is
 * answered by dankc's own password modal, same as services/firewall.c's
 * pkexec calls.
 *
 * Reads use popen()/pclose() (same convention as services/printers.c):
 * pclose()'s return value is unusable (main.c's process-wide
 * `signal(SIGCHLD, SIG_IGN)` makes its internal waitpid() fail with ECHILD --
 * verified directly, see printers.c's dc_printers_available() comment for
 * the general problem) but the read side works fine since that only depends
 * on the pipe, not on reaping; every read function here ignores pclose()'s
 * return and only trusts what it actually parsed. Availability is a PATH
 * search via access(X_OK), NOT system("command -v ..."), for the same
 * SIGCHLD reason (see printers.c's dc_printers_available()).
 *
 * Mutating calls (`set-ntp`, `set-timezone`) are fire-and-forget fork+execvp
 * (reaped by main's SIGCHLD=SIG_IGN, same shape as printers.c/display.c),
 * gated by $DANKC_TIMEDATE_DRYRUN=1 (logs the argv instead of forking).
 */
#ifndef DC_SERVICES_TIMEDATE_H
#define DC_SERVICES_TIMEDATE_H

#include <stdbool.h>
#include <stddef.h>

#define DC_TIMEDATE_TZ_MAX 64

typedef struct dc_timedate_info {
    char timezone[DC_TIMEDATE_TZ_MAX]; /* e.g. "Asia/Karachi" */
    bool can_ntp;                      /* an NTP service is available to timedatectl */
    bool ntp_enabled;                  /* systemd-timesyncd (or equivalent) enabled */
    bool ntp_synchronized;             /* clock is currently synced */
    char now_local[64];                /* "Sat 2026-07-04 09:59:36 PKT"-style, for display */
} dc_timedate_info;

/* PATH search for `timedatectl`, access(X_OK)-based (see file header). */
bool dc_timedate_available(void);

/* One-shot blocking read via `timedatectl show` + `timedatectl` (for the
 * human-readable local time line). Returns false (out left zeroed) if
 * timedatectl isn't available. */
bool dc_timedate_status(dc_timedate_info *out);

/* `timedatectl set-ntp true|false`. Fire-and-forget, DANKC_TIMEDATE_DRYRUN
 * gated. No-op (warns) if timedatectl isn't available. */
void dc_timedate_set_ntp(bool enable);

/* `timedatectl set-timezone <tz>`. Fire-and-forget, DANKC_TIMEDATE_DRYRUN
 * gated. No-op (warns) if `tz` is NULL/empty or timedatectl isn't
 * available. */
void dc_timedate_set_timezone(const char *tz);

/* Fills `out` (up to `max` entries, each up to DC_TIMEDATE_TZ_MAX-1 chars)
 * from `timedatectl list-timezones`, returns the count actually filled (may
 * be less than the true total if it exceeds `max`). Blocking; only called
 * once per Settings-tab visit by the caller (cached there), not per-frame. */
int dc_timedate_list_timezones(char out[][DC_TIMEDATE_TZ_MAX], int max);

#endif /* DC_SERVICES_TIMEDATE_H */
