/* firewall.h — firewall service: ufw + firewalld dual backend
 * (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.5 "Firewall").
 *
 * Same shape as services/power.c's PPD/tuned dual-backend pattern: probe
 * once lazily, cache the result, fold both backends onto one UI-facing
 * struct so a future Settings tab never branches on backend.
 *
 * Detection order (docs sec.5):
 *   1. `systemctl is-active firewalld` (no root) -> if active, FIREWALLD.
 *   2. else `command -v ufw` exists -> UFW (ufw itself isn't a daemon, so
 *      presence alone is the signal, same as power.c's `tuned-adm` CLI
 *      fallback check).
 *   3. else `command -v firewall-cmd` exists -> FIREWALLD, installed but
 *      not currently running (status will read disabled/unknown).
 *   4. else NONE -- a future Settings tab hides itself entirely, matching
 *      tab_printer's existing "empty state, no backend" pattern.
 *
 * Status reads are root-free wherever possible:
 *   - ufw: `/etc/ufw/ufw.conf`'s world-readable `ENABLED=yes|no` line (the
 *     same file `ufw enable`/`disable` itself writes back to) plus
 *     `/etc/default/ufw`'s `DEFAULT_{INPUT,OUTPUT,FORWARD}_POLICY` lines
 *     for the default-policy summary. `ufw status` itself needs root; it is
 *     tried as a best-effort upgrade (overrides the file-based read if it
 *     succeeds, e.g. when dankc is somehow running as root) but a normal
 *     unprivileged run degrades cleanly to the file-based reading rather
 *     than reporting an error.
 *   - firewalld: `systemctl is-active firewalld` for enabled state (no
 *     root), `firewall-cmd --get-active-zones` for the active zone
 *     (typically allowed unprivileged by firewalld's own D-Bus policy;
 *     left empty if it fails).
 *
 * Every mutating call (enable/disable, allow/deny a service) shells out via
 * `pkexec`. Because dankc registers its own polkit authentication agent
 * (services/polkit.c) for the session, that pkexec's authorization prompt
 * is answered by dankc's OWN password modal (ui/polkit_modal.c), not a
 * foreign GTK/KDE dialog -- no new agent-side code needed, only these call
 * sites. `$DANKC_FIREWALL_DRYRUN=1` logs the exact argv instead of forking
 * pkexec at all, so verification/testing can never actually flip the
 * user's live firewall state or rules (same convention as
 * `$DANKC_PRINTERS_DRYRUN` / `$DANKC_DISPLAY_DRYRUN`).
 */
#ifndef DC_SERVICES_FIREWALL_H
#define DC_SERVICES_FIREWALL_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DC_FIREWALL_BACKEND_NONE = 0, /* nothing found -- a future tab hides itself */
    DC_FIREWALL_BACKEND_UFW,
    DC_FIREWALL_BACKEND_FIREWALLD,
} dc_firewall_backend;

typedef struct dc_firewall_info {
    bool available;              /* a backend was found (backend != NONE) */
    dc_firewall_backend backend;

    bool enabled_known;          /* could we determine on/off with confidence? */
    bool enabled;                /* firewall currently active (only meaningful if enabled_known) */

    /* ufw only: human-readable default-policy summary built from
     * /etc/default/ufw, e.g. "incoming: deny, outgoing: allow, routed: deny".
     * Empty string if unavailable or backend != UFW. */
    char default_policy[96];

    /* firewalld only: first active zone from `firewall-cmd
     * --get-active-zones`. Empty string if unavailable or backend != FIREWALLD. */
    char active_zone[64];
} dc_firewall_info;

/* A short fixed shortlist of common services a Settings UI can offer as
 * one-click allow/deny toggles (docs sec.5's "SSH, HTTP, HTTPS, Samba,
 * mDNS"). Names are valid both as ufw's own service-name argument (from
 * /etc/services, matched by ufw's `applications.d`-free builtin lookup)
 * and firewalld's `--add-service=`/`--remove-service=` service ids. */
#define DC_FIREWALL_COMMON_SERVICE_COUNT 5
extern const char *const dc_firewall_common_services[DC_FIREWALL_COMMON_SERVICE_COUNT];

/* Lazily probes and caches which backend is present (see detection order
 * above). Safe to call repeatedly; only probes once per process lifetime,
 * same contract as power.c's dc_power_read() backend probe. */
dc_firewall_backend dc_firewall_backend_get(void);

/* "ufw" / "firewalld" / "none" -- for logging/UI display. */
const char *dc_firewall_backend_name(dc_firewall_backend b);

/* One-shot, root-free-where-possible status read (see file header for the
 * exact mechanism per backend). Fills `out`, returns `out->available`.
 * Blocking/synchronous (bounded by a couple of quick file reads / popen()s
 * for firewalld's zone query) -- don't call from a latency-sensitive render
 * path, same contract as dc_printers_list(). */
bool dc_firewall_status(dc_firewall_info *out);

/* Enable/disable the detected firewall:
 *   ufw:       pkexec ufw enable | disable
 *   firewalld: pkexec systemctl enable --now firewalld | disable --now firewalld
 * Fire-and-forget (fork+execvp, reaped by main's SIGCHLD=SIG_IGN) -- the
 * pkexec authorization prompt is answered by dankc's own registered polkit
 * agent. Returns false immediately (no-op, warns) if no backend is
 * detected; true otherwise (command was fired or, under
 * $DANKC_FIREWALL_DRYRUN=1, logged instead of fired). Does not itself
 * refresh any cached status -- callers should dc_firewall_status() again
 * after giving the user time to authenticate. */
bool dc_firewall_set_enabled(bool enable);

/* Allow/deny one service by name (see dc_firewall_common_services above for
 * a ready-made shortlist, but any valid ufw/firewalld service name works):
 *   ufw:       pkexec ufw allow <service> | deny <service>
 *   firewalld: pkexec firewall-cmd --zone=<active> --add-service=<service> --permanent
 *              (then) pkexec firewall-cmd --reload
 *              (falls back to zone "public" if the active zone couldn't be
 *              determined -- firewalld's own default zone)
 * Same dry-run/fire-and-forget/no-backend contract as dc_firewall_set_enabled().
 * No-op (warns, returns false) if `service` is NULL/empty. */
bool dc_firewall_allow(const char *service, bool allow);

/* TEST-ONLY: forces the cached backend to `b`, bypassing dc_firewall_backend_get()'s
 * probe. Exists solely so $DANKC_FIREWALL_TEST can exercise (dry-run only,
 * see above) the command-building for a backend that isn't actually
 * installed on the current dev machine -- e.g. verifying the firewalld
 * pkexec argv shapes on a box that only has ufw. Never call this from
 * production code paths (settings UI, startup, etc.) -- only from a
 * DANKC_FIREWALL_TEST-gated block. */
void dc_firewall_debug_force_backend(dc_firewall_backend b);

#endif /* DC_SERVICES_FIREWALL_H */
