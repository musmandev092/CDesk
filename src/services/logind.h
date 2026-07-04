/* logind.h — react to systemd-logind session events.
 *
 * Locks the session before sleep (PrepareForSleep) and on a lock request
 * (loginctl lock-session / Session.Lock). See docs/03-SERVICES. Feeds the lock
 * screen (T22).
 */
#ifndef DC_SERVICES_LOGIND_H
#define DC_SERVICES_LOGIND_H

#include <stdbool.h>

struct dc_dbus;

typedef struct dc_logind dc_logind;

/* Called when logind asks us to lock (pre-sleep or explicit lock). */
typedef void (*dc_logind_lock_cb)(void *user_data);

/* Subscribe on the system bus. NULL if unavailable. */
dc_logind *dc_logind_create(struct dc_dbus *dbus, dc_logind_lock_cb cb, void *user_data);
void dc_logind_destroy(dc_logind *l);

/* --- Idle & Lid (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.9) --------------
 *
 * Read-only view of the EFFECTIVE /etc/systemd/logind.conf settings (main
 * file plus any logind.conf.d drop-in files, later ones overriding earlier
 * keys -- same precedence systemd-logind itself uses),
 * plus an optional write path via a dankc-owned drop-in
 * (/etc/systemd/logind.conf.d/50-dankc.conf, NOT the main file) through
 * pkexec. Values are the raw systemd config strings (e.g. "suspend",
 * "ignore") so this stays a thin passthrough -- no local enum needing to
 * track every value systemd itself supports.
 */
#define DC_LOGIND_VALUE_MAX 32

typedef struct dc_logind_conf_info {
    char idle_action[DC_LOGIND_VALUE_MAX];                   /* default "ignore" */
    int idle_action_sec;                                     /* seconds; default 1800 (30min) */
    char handle_lid_switch[DC_LOGIND_VALUE_MAX];              /* default "suspend" */
    char handle_lid_switch_external_power[DC_LOGIND_VALUE_MAX]; /* default "suspend" */
    bool from_dropin; /* true if any value came from a logind.conf.d drop-in override */
} dc_logind_conf_info;

/* Parses /etc/systemd/logind.conf plus any logind.conf.d drop-in files (in
 * that order, later drop-ins overriding earlier keys, matching systemd's own
 * precedence) and fills `out` with the effective values, falling back to
 * systemd's documented compiled-in defaults for anything never set. Always
 * succeeds (fills defaults even if no file exists) -- there is no
 * "unavailable" state for a read-only status view. `conf_dir_override` is
 * NULL in production; tests pass a scratch directory containing a
 * logind.conf (and optionally a logind.conf.d subdirectory) so this never
 * has to read the real /etc. */
void dc_logind_conf_read(dc_logind_conf_info *out, const char *conf_dir_override);

/* Writes /etc/systemd/logind.conf.d/50-dankc.conf (creating the directory if
 * needed) with exactly the four keys from `cfg`, via `pkexec install`
 * (dankc's own registered polkit agent answers the prompt, same as
 * services/firewall.c). Does NOT edit the main logind.conf and does NOT
 * restart systemd-logind -- the caller is responsible for telling the user a
 * re-login/reboot is needed for the new drop-in to take effect. Gated by
 * $DANKC_LOGIND_DRYRUN=1 (logs the intended file content + pkexec argv
 * instead of writing/spawning anything). Returns false only on a local I/O
 * failure while staging the temp file (the actual privileged write is
 * fire-and-forget, like every other pkexec call in this codebase, so a
 * denied/failed polkit prompt can't be observed here). */
bool dc_logind_conf_write_dropin(const dc_logind_conf_info *cfg);

#endif /* DC_SERVICES_LOGIND_H */
