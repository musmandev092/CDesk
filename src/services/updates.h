/* updates.h — system update checker (docs/29-SMALL-FEATURES-PLAN.md sec.4):
 * multi-backend read-only "how many updates are pending" checker, matching
 * the process model ui/settings.c's existing (Arch-only) checkupdates tab
 * pioneered -- see settings.c's TAB_SYSTEM_UPDATER section (updates_check_
 * now()/updates_read_cache()) for the pattern this generalizes:
 *
 *   - Checking for updates can be slow (checkupdates/AUR helpers sync a temp
 *     package DB or hit the network) and must never block dankc's single-
 *     threaded event loop, so a check is a detached, fire-and-forget shell
 *     command per backend (fork+setsid+exec /bin/sh -c, no waitpid --
 *     main.c sets SIGCHLD to SIG_IGN process-wide, so there is nothing to
 *     reap here).
 *   - Results are polled from a small per-backend cache file (one pending-
 *     update name per line, written to a .tmp path then renamed into place so
 *     a reader never sees a half-written file), not from the child's exit
 *     status or a pipe.
 *
 * Three backends, each independently probed + checked:
 *   - "pacman": `checkupdates` (pacman-contrib) -- official repos, read-only
 *     (syncs its own temp copy of the sync DB, never touches the real pacman
 *     DB or installs anything).
 *   - "aur": `paru -Qua` (preferred) or `yay -Qua` -- whichever AUR helper is
 *     installed; also read-only.
 *   - "flatpak": `flatpak remote-ls --updates` -- pending flatpak ref
 *     updates.
 *
 * A backend whose CLI tool isn't on PATH is simply reported unavailable
 * (available=false, count=-1) -- this file never tries to install anything,
 * and the caller (a later settings-tab task) is expected to degrade cleanly
 * (hide/gray out that backend's row) rather than error.
 *
 * Upgrading is a separate, explicit action (dc_updates_run_upgrade()): it
 * spawns an interactive terminal running the real upgrade command, so the
 * user sees pacman's/flatpak's own prompts (sudo password, confirmation,
 * progress) rather than dankc silently running a privileged command in the
 * background. `updateTerminalCmd` (dc_config.update_terminal_cmd) lets a user
 * override which terminal/command runs it -- see dc_updates_run_upgrade()'s
 * doc comment for the exact contract.
 */
#ifndef DC_SERVICES_UPDATES_H
#define DC_SERVICES_UPDATES_H

#include <stdbool.h>

#define DC_UPDATES_BACKENDS_N 3

/* One row per backend, in a fixed pacman/aur/flatpak order (matches
 * dc_updates_read()'s fill order). `count` is -1 when no completed check
 * result is available yet (either never checked this session, or the
 * backend isn't installed); a count >= 0 is the last completed check's
 * pending-update count (0 = checked, nothing pending). `mtime` is the cache
 * file's mtime (unix seconds) as a "last checked" timestamp, 0 if never
 * checked. */
typedef struct dc_update_backend {
    const char *name; /* "pacman" | "aur" | "flatpak" */
    bool available;   /* backend's CLI tool found on PATH */
    int count;
    long mtime;
} dc_update_backend;

/* Kicks off a detached, non-blocking check for every backend currently
 * available (per PATH probing) -- each backend's check runs as its own
 * independent detached shell writing to its own cache file; backends whose
 * tool isn't installed are skipped entirely (no cache file touched). Safe to
 * call repeatedly, e.g. from a periodic timer driven by
 * `updatesCheckIntervalMin` (dc_config.updates_check_interval_min) in a later
 * task -- a check already in flight for a backend is just superseded by
 * whichever run finishes writing its cache file last, same fire-and-forget
 * contract as the existing checkupdates tab (no in-flight tracking, no
 * cancellation). */
void dc_updates_check_async(void);

/* Fills out[0 .. n) (n = min(DC_UPDATES_BACKENDS_N, max)) with each backend's
 * current availability (probed live) and last completed check's count/mtime
 * (read fresh from its cache file, if any). Returns n. Cheap enough to call
 * on every render (stats a handful of small files, no forking). */
int dc_updates_read(dc_update_backend *out, int max);

/* Sum of `count` across every backend with a completed check (count >= 0);
 * unavailable or never-checked backends contribute 0. Convenience for a
 * bar-widget badge in a later task. */
int dc_updates_total(void);

/* Spawns an interactive terminal running the real upgrade command for
 * `backend`:
 *   - "pacman" or "aur" -> "sudo pacman -Syu" (docs/29 sec.4's key default;
 *     the "aur" backend intentionally reuses the plain repo sync/upgrade
 *     here -- actually upgrading AUR packages themselves still requires the
 *     user's own AUR-helper invocation, out of scope for this fire-and-
 *     forget spawn).
 *   - "flatpak" -> "flatpak update".
 *
 * Uses dc_config.update_terminal_cmd if non-empty: a printf-style template
 * containing exactly one literal "%s", substituted with the upgrade command
 * above -- e.g. "foot -e sh -c '%s; read -p \"[enter to close]\" x'". A
 * configured template missing the "%s" is rejected (logged, falls back to
 * auto-probing) rather than silently dropping the upgrade command.
 *
 * With no override, auto-probes foot/kitty/alacritty/wezterm/ghostty/xterm
 * on PATH and runs the command in whichever is found first (same shape as
 * ui/settings.c's mux_launch_terminal()).
 *
 * Detached fire-and-forget (fork+setsid+exec /bin/sh -c, no waitpid).
 * $DANKC_UPDATES_DRYRUN set to any value logs the resolved command instead
 * of spawning it, for offline verification. A `backend` that isn't one of
 * the three names above is a no-op (logged as a warning). */
void dc_updates_run_upgrade(const char *backend);

#endif /* DC_SERVICES_UPDATES_H */
