/* wallpaper.h — wallpaper service: painting + effective-path resolution.
 *
 * (docs/29-SMALL-FEATURES-PLAN.md sec.3 "Wallpaper extras", T1.)
 *
 * dankc doesn't draw a wallpaper layer of its own (the dynamic-color engine
 * only samples the image file for its palette), so "setting the wallpaper"
 * means handing the image to the compositor the standard niri way: (re)spawn
 * swaybg when it's installed. This is a pure extraction of what was
 * dashboard.c's wall_apply_compositor() — same pkill+respawn `swaybg -m fill
 * -i <path>` behavior, byte-for-byte. dashboard.c's wall_set_active() still
 * owns the config write + dynamic-color reapply + material_bg invalidate
 * around the call; only the actual painting moved here.
 */
#ifndef DC_SERVICES_WALLPAPER_H
#define DC_SERVICES_WALLPAPER_H

#include <stdbool.h>

/* Apply `path` as the compositor wallpaper: pkill any running swaybg, then
 * (re)spawn `swaybg -m fill -i <path>` detached (fire-and-forget, reaped by
 * main's SIGCHLD=SIG_IGN). No-op (logs at info level) if swaybg isn't on
 * $PATH — the config/palette still update regardless via the caller. */
void dc_wallpaper_apply(const char *path);

/* Returns the currently-effective wallpaper path: dc_config_current->
 * wallpaper_light or ->wallpaper_dark when one is set and matches
 * dc_config_light_mode()'s current light/dark mode, else falling back to
 * dc_config_current->wallpaper. Callers should always go through this
 * instead of reading the config wallpaper fields directly, so they never
 * have to re-derive the light/dark precedence themselves.
 *
 * TODO(wallpaper T4): also resolve wallpaperPerMonitor here once that key
 * exists (per-output override; primary output still drives the palette). */
const char *dc_wallpaper_effective(void);

/* Re-derive dc_wallpaper_effective() and, only if it differs from the path
 * last handed to dc_wallpaper_apply() (tracked in a file-local static),
 * respawn swaybg with the new one. Call this after anything that might
 * change the effective path -- config.c's apply_theme() does, since a
 * themeMode flip (or a settings edit to wallpaper/wallpaperLight/Dark) is
 * exactly when the shown wallpaper needs to swap. A no-op the rest of the
 * time (e.g. every dc_config_reapply() from an unrelated settings tweak)
 * keeps this from respawning swaybg on every save. */
void dc_wallpaper_apply_effective(void);

/* Wallpaper cycling (docs/29-SMALL-FEATURES-PLAN.md sec.3, T3): advance
 * through a sorted, image-filtered directory listing on a timer. The
 * directory is dc_config_current->wallpaper_cycle_dir when set, else the
 * same fallback dashboard.c's Wallpapers tab uses (dirname of the configured
 * `wallpaper`, else ~/Pictures/wallpapers, else ~/Pictures) -- mirrored here
 * rather than shared with dashboard.c to keep this a standalone service. */

/* Advance to the next image in the resolved cycle directory (wrapping past
 * the end) and make it the active wallpaper: sets dc_config_mut()->wallpaper,
 * dc_config_reapply()s (stock theme + dynamic-color overlay), invalidates the
 * cached material background, persists via dc_config_save(), and repaints
 * via dc_wallpaper_apply_effective() -- the same shape as dashboard.c's
 * wall_set_active()/main.c's `wallpaper set` control command. Honors
 * DANKC_WALL_DRY like those two (no disk write, no swaybg respawn) for
 * in-place verification. No-op if the resolved directory has no images. */
void dc_wallpaper_cycle_next(void);

/* Call ~once per second (main.c's clock_tick). Advances an internal seconds
 * counter and calls dc_wallpaper_cycle_next() once it reaches
 * dc_config_current->wallpaper_cycle_interval_sec, but only while
 * wallpaper_cycle_enabled is true, interval_sec > 0, and `locked` is false
 * (pass dc_lock_active()'s result -- cycling pauses while the session lock
 * is engaged). The counter resets whenever cycling is disabled/paused or a
 * cycle fires, so re-enabling always waits a full interval before the next
 * advance. */
void dc_wallpaper_cycle_tick(bool locked);

#endif /* DC_SERVICES_WALLPAPER_H */
