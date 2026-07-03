/* autostart.h — XDG session autostart.
 *
 * Launches `.desktop` entries from `$XDG_CONFIG_HOME/autostart` (default
 * ~/.config/autostart) and each `$XDG_CONFIG_DIRS/autostart` dir (default
 * /etc/xdg/autostart), following the freedesktop.org Desktop Application
 * Autostart Specification: skips `Hidden=true`, `X-GNOME-Autostart-
 * enabled=false`, entries whose `OnlyShowIn`/`NotShowIn` excludes
 * XDG_CURRENT_DESKTOP, and `TryExec=` binaries missing from PATH. A user-dir
 * entry always wins over a same-named system entry (even a skip, so a user
 * `Hidden=true` override correctly suppresses the system one instead of both
 * firing). See docs/14-COMPLETION-PLAN.md W1.2.
 */
#ifndef DC_SERVICES_AUTOSTART_H
#define DC_SERVICES_AUTOSTART_H

/* Scan both autostart directory sets and spawn every entry that passes its
 * checks, detached (reuses dc_app_launch_exec(), services/apps.h) -- fire
 * and forget, one shot. No-op (logs one line) if
 * dc_config_current->autostart_enabled is false. Ensures XDG_CURRENT_DESKTOP
 * is set (defaults to "niri" if unset) before evaluating OnlyShowIn/
 * NotShowIn, and exports it via setenv() so autostart children and anything
 * else dankc spawns see the same value.
 *
 * Call exactly once at startup (src/main.c, after the Wayland connection +
 * niri IPC are up) -- never from a render/tick path, or entries would
 * relaunch every frame. */
void dc_autostart_run(void);

#endif /* DC_SERVICES_AUTOSTART_H */
