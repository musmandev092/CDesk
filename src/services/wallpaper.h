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

#endif /* DC_SERVICES_WALLPAPER_H */
