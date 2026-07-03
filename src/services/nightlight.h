/* nightlight.h — Night Light service (docs/19-SETTINGS-COMPLETENESS-PLAN.md
 * sec.4 "Night Light"): color-temperature control + on/off + schedule,
 * replacing the old duplicated `pgrep gammastep` / `pkill gammastep -O 4000 &`
 * one-shot toggle that used to live directly in main.c and settings.c.
 *
 * Backend: prefers **wlsunset** (responds to SIGUSR1 for a live day/night/
 * auto cycle without a restart) if installed, else falls back to
 * **gammastep** (no live-toggle signal -- manual overrides are always a
 * kill-and-relaunch with new flags). Probed once, lazily, and cached --
 * matches services/power.c's probe_backend() pattern. See nightlight.c's
 * top-of-file comment for exactly which flags/config-file keys are used per
 * backend and schedule mode, and docs/19 sec.4 for the mechanism research
 * this is built from.
 *
 * Ownership model: this service is the single owner of the spawned child's
 * pid (no `pgrep -x` round trips like the old code) and of persistence --
 * every setter here mutates `dc_config_mut()` and calls `dc_config_save()`
 * itself, so the future Settings UI slider/schedule tab can just call these
 * functions directly without touching config.c. Call dc_nightlight_init()
 * once at startup (after dc_config_load()) to re-apply a persisted "was
 * enabled" state; call dc_nightlight_shutdown() at shell exit so the user's
 * screen isn't left tinted after `dankc` quits.
 */
#ifndef DC_SERVICES_NIGHTLIGHT_H
#define DC_SERVICES_NIGHTLIGHT_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    DC_NIGHTLIGHT_BACKEND_NONE = 0, /* neither wlsunset nor gammastep installed */
    DC_NIGHTLIGHT_BACKEND_WLSUNSET,
    DC_NIGHTLIGHT_BACKEND_GAMMASTEP,
} dc_nightlight_backend;

typedef enum {
    /* Fixed forced temperature, always on while enabled -- no time-based
     * automation. The "Manual" segmented option in docs/19's panel design. */
    DC_NIGHTLIGHT_SCHED_MANUAL = 0,
    /* Sunset-to-sunrise, computed from lat/lon (reuses the weather widget's
     * already-configured location, docs/19 sec.4). */
    DC_NIGHTLIGHT_SCHED_SUNSET = 1,
    /* Fixed daily HH:MM-to-HH:MM window (nightlight_from -> nightlight_to). */
    DC_NIGHTLIGHT_SCHED_TIMES = 2,
} dc_nightlight_schedule;

/* Backend actually available on this machine (probed once, lazily, cached).
 * NONE means every setter below is a harmless no-op (logged once). */
dc_nightlight_backend dc_nightlight_backend_get(void);
const char *dc_nightlight_backend_name(dc_nightlight_backend be); /* "wlsunset"/"gammastep"/"none" */

/* Re-apply the persisted config (nightlight_enabled + temp + schedule) --
 * call once at startup, after dc_config_load(). No-op if nightlight_enabled
 * is false in the loaded config. */
void dc_nightlight_init(void);

/* Kill any owned backend child and, if it was on, restore neutral gamma.
 * Call once at shell shutdown so quitting dankc doesn't leave the screen
 * tinted. Does NOT persist a change to nightlight_enabled -- next launch
 * still re-applies whatever was last saved (matches "kill only your own
 * child" semantics: a foreign gammastep/wlsunset the user started by hand is
 * never touched). */
void dc_nightlight_shutdown(void);

/* --- state (read-only) ----------------------------------------------------- */

/* True if dankc currently owns a live backend child applying an adjustment
 * (i.e. nightlight is effectively on right now). */
bool dc_nightlight_active(void);

/* Configured temperature in Kelvin -- the forced temp in MANUAL mode, or the
 * night-side temp in SUNSET/TIMES mode (daytime side is always a fixed
 * neutral 6500K in both of those modes; only the night side is adjustable
 * from the UI, matching docs/19's "one slider" design). Range ~2500-6500. */
int dc_nightlight_get_temp(void);

dc_nightlight_schedule dc_nightlight_get_schedule(void);

/* Fills `from`/`to` with the persisted "HH:MM" strings for
 * DC_NIGHTLIGHT_SCHED_TIMES (empty strings if never set). Buffers should be
 * at least 6 bytes ("HH:MM\0"). */
void dc_nightlight_get_times(char *from, size_t from_n, char *to, size_t to_n);

/* --- mutators (persist to config.json + apply immediately) ----------------- */

/* Master on/off switch. Applies the currently configured temp/schedule when
 * turning on; kills the owned child (restoring neutral) when turning off.
 * This is what `dankc ctl night` now routes through (see main.c). */
void dc_nightlight_enable(bool on);

/* Convenience flip of dc_nightlight_enable() around the current state --
 * exactly what the old `pgrep|pkill` toggle did, just via the owned pid
 * instead of a process-table scan. */
void dc_nightlight_toggle(void);

/* Live-adjust the (night-side) color temperature. Clamped to [2500,6500].
 * If nightlight is currently active, relaunches the backend with the new
 * value (debounce on slider-release is the *caller's* job -- e.g. the future
 * settings.c slider -- this function itself does one immediate
 * kill+relaunch per call, matching gammastep's kill-and-relaunch-only
 * story; wlsunset callers that only ever run in MANUAL mode get the same
 * behavior here, SIGUSR1 is only used for the on/off/auto tri-state, see
 * nightlight.c). Persists regardless of whether nightlight is on. */
void dc_nightlight_set_temp(int kelvin);

/* Change schedule mode (and, for TIMES, the HH:MM window). `from_hhmm`/
 * `to_hhmm` are ignored (may be NULL) unless mode == DC_NIGHTLIGHT_SCHED_TIMES.
 * Persists and, if nightlight is currently enabled, relaunches the backend
 * under the new schedule immediately. */
void dc_nightlight_set_schedule(dc_nightlight_schedule mode, const char *from_hhmm,
                                const char *to_hhmm);

#endif /* DC_SERVICES_NIGHTLIGHT_H */
