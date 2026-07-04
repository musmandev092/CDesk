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

/* Returns the currently-effective wallpaper path, i.e. what dc_wallpaper_
 * apply() was last told to paint / what dynamic color samples from.
 * Right now this is just dc_config_current->wallpaper verbatim.
 *
 * TODO(wallpaper T2/T4): once wallpaperLight/Dark and wallpaperPerMonitor
 * exist, this becomes the single place that resolves which of those (light/
 * dark mode, per-output override) is "effective" right now, so callers never
 * have to branch on the new config keys themselves. */
const char *dc_wallpaper_effective(void);

#endif /* DC_SERVICES_WALLPAPER_H */
