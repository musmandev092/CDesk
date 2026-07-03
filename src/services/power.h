/* power.h — power-profile service (docs/13-POPOUTS-SPEC.md sec.2).
 *
 * The user runs `tuned`, not `power-profiles-daemon`. Rather than hardcode
 * against one backend, this module probes the system bus once (lazily, on
 * first read) and picks the best available:
 *
 *   1. org.freedesktop.UPower.PowerProfiles -- the standard freedesktop
 *      interface (power-profiles-daemon, or tuned's own `tuned-ppd` bridge
 *      service, which speaks the same interface backed by tuned profiles).
 *   2. com.redhat.tuned -- tuned's native D-Bus control interface
 *      (/Tuned, com.redhat.tuned.control), used directly if the PPD bridge
 *      isn't running.
 *   3. `tuned-adm` CLI -- last resort if tuned has no D-Bus service enabled
 *      at all (parses `tuned-adm active` / `list`, sets via `tuned-adm
 *      profile <name>` spawned detached).
 *
 * All three are folded onto the same 3-mode view DMS uses (power-saver /
 * balanced / performance) so the UI never needs to know which backend is
 * live. The raw backend profile name is also exposed for display, since a
 * tuned profile like "throughput-performance" doesn't exactly match any of
 * the 3 mode slugs even though it maps onto "performance".
 */
#ifndef DC_SERVICES_POWER_H
#define DC_SERVICES_POWER_H

#include <stdbool.h>

struct dc_dbus;

typedef enum {
    DC_POWER_MODE_POWER_SAVER = 0,
    DC_POWER_MODE_BALANCED = 1,
    DC_POWER_MODE_PERFORMANCE = 2,
    DC_POWER_MODE_UNKNOWN = -1,
} dc_power_mode;

typedef enum {
    DC_POWER_BACKEND_NONE = 0,       /* nothing found -- section stays dimmed */
    DC_POWER_BACKEND_PPD,            /* org.freedesktop.UPower.PowerProfiles */
    DC_POWER_BACKEND_TUNED_DBUS,     /* com.redhat.tuned, direct */
    DC_POWER_BACKEND_TUNED_CLI,      /* tuned-adm CLI fallback */
} dc_power_backend;

typedef struct dc_power_info {
    bool available;             /* a backend answered */
    dc_power_backend backend;
    dc_power_mode active_mode;  /* -1 if the raw profile doesn't map onto a mode */
    char active_profile[64];    /* raw backend profile name, e.g. "throughput-performance" */
    bool has_performance_mode;  /* whether a distinguishable performance profile exists */
} dc_power_info;

/* Bind the system bus (from dc_dbus). Call once at startup; backend
 * detection itself is deferred to the first dc_power_read() (lazy, like
 * mpris.c's find_player()). */
void dc_power_init(struct dc_dbus *dbus);

/* Read cached power-profile state (refreshed at most every few seconds).
 * Returns true if a backend is available. */
bool dc_power_read(dc_power_info *out);

/* Switch to one of the 3 DMS modes. For tuned backends this maps onto the
 * best-matching real tuned profile (see power.c's pick_profile_for_mode).
 * Fire-and-forget for the CLI backend; returns false immediately if no
 * backend or no matching profile exists. Forces the next dc_power_read() to
 * refresh rather than serve the cache. */
bool dc_power_set_mode(dc_power_mode mode);

/* "Power Saver" / "Balanced" / "Performance" / "Unknown". */
const char *dc_power_mode_label(dc_power_mode mode);

#endif /* DC_SERVICES_POWER_H */
