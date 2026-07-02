/* lock.h — session lock screen (ext-session-lock-v1 + PAM).
 *
 * Locks every output with a full-screen surface showing the clock and a
 * password field; unlocks on a correct PAM check. Matches DMS's lock. Triggered
 * by `dankc ctl lock` or logind (lock-on-sleep). See docs/04-FEATURES.
 *
 * SAFETY: if DANKC_LOCK_ESCAPE=1, F1 force-unlocks without a password (testing
 * only) so a bug can't lock you out.
 */
#ifndef DC_UI_LOCK_H
#define DC_UI_LOCK_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;

typedef struct dc_lock dc_lock;

dc_lock *dc_lock_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render);
void dc_lock_destroy(dc_lock *l);

/* Lock the session (no-op if already locked, or if the compositor lacks
 * ext-session-lock). */
void dc_lock_engage(dc_lock *l);

/* True while the session is locked (input should route here). */
bool dc_lock_active(dc_lock *l);

/* Force-unlock without a password. Testing only — the caller must gate this on
 * DANKC_LOCK_ESCAPE. */
void dc_lock_force_unlock(dc_lock *l);

/* Password entry / navigation while locked. */
void dc_lock_handle_key(dc_lock *l, uint32_t keysym, const char *utf8);

/* Redraw (clock) — call from the periodic tick while locked. */
void dc_lock_tick(dc_lock *l);

#endif /* DC_UI_LOCK_H */
