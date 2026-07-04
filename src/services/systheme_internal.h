/* systheme_internal.h — shared plumbing for the system-theming emitters
 * (src/services/systheme*.c). NOT part of the public API (see systheme.h,
 * which only exposes dc_systheme_apply()/dc_systheme_app_detected() for the
 * rest of dankc). Split out so Task 2+ per-app emitters (Qt, Alacritty,
 * VS Code, kitty, foot) reuse the exact same atomic-write / user-file-backup
 * / marker-injector / dryrun / detection / spawn code instead of
 * re-deriving it per app -- see systheme.c's file header for the safety
 * rules these helpers exist to enforce.
 */
#ifndef DC_SERVICES_SYSTHEME_INTERNAL_H
#define DC_SERVICES_SYSTHEME_INTERNAL_H

#include "theme/theme.h"

#include <stdbool.h>
#include <stddef.h>

#define DC_SYSTHEME_PATH_MAX 512

/* $DANKC_THEME_DRYRUN=1 -- checked once by every helper below so individual
 * emitters never need to check it themselves. */
bool dc_systheme_dryrun(void);

/* "#rrggbb" (7 chars + NUL); `out` must be >= 8 bytes. */
void dc_systheme_hex_rgb(dc_color c, char out[8]);

/* "#aarrggbb" (9 chars + NUL); `out` must be >= 10 bytes. Alpha first,
 * matching Qt/kitty-style ARGB hex conventions (not used by the GTK tier,
 * kept here for the Task 2+ emitters that need it). */
void dc_systheme_hex_argb(dc_color c, char out[10]);

/* True iff `c` reads better with black text on top than white (simple
 * relative-luminance threshold). Used to synthesize an "on_error"-style
 * contrasting foreground for GTK's destructive/error text roles, since
 * dc_theme has a single flat `error` swatch rather than a full M3
 * error/on_error/error_container/on_error_container set (see systheme.c's
 * GTK emitter comment for the concrete discrepancy this causes vs. DMS's
 * matugen-derived reference file). */
bool dc_systheme_prefers_black_text(dc_color c);

/* Atomically write `data` (`len` bytes) to `path`: writes "<path>.tmp" then
 * rename()s over `path`, so a reader never observes a partial file and a
 * write failure never corrupts the previous good copy. Use for files dankc
 * fully owns (safe to overwrite unconditionally, e.g. dank-colors.css).
 * DRYRUN-gated: logs "[DRYRUN] systheme: would write <n> bytes to <path>"
 * and returns true without touching disk. Returns false on a real I/O
 * failure (logged via dc_warn()). */
bool dc_systheme_write_owned(const char *path, const char *data, size_t len);

/* Ensure `needle` appears somewhere in `path`'s contents; if it already
 * does, this is a pure no-op (idempotent -- no backup, no write, safe to
 * call on every dc_systheme_apply()). Otherwise:
 *   - if `path` exists: back it up once to "<path>.bak-<epoch>" (skipped
 *     only if that copy fails, in which case the whole call aborts rather
 *     than risk losing the user's original file), then atomically rewrite
 *     `path` as: original contents + "\n" + `marker_comment` + "\n" +
 *     `line` + "\n".
 *   - if `path` doesn't exist: atomically create it as just
 *     `marker_comment` + "\n" + `line` + "\n" (nothing to back up).
 * `marker_comment` should be a full CSS-style comment line, e.g. "Added by
 * DankC Settings > Theme & Colors" wrapped in comment delimiters by the
 * caller (so a human can find and remove dankc's addition later); `line`
 * is the actual directive, e.g.
 * "@import 'dank-colors.css';". DRYRUN-gated like dc_systheme_write_owned()
 * (the backup step, if any, is also skipped/logged under dryrun). Use for
 * USER-owned files dankc only ever nudges, never fully regenerates. */
bool dc_systheme_ensure_line(const char *path, const char *needle, const char *marker_comment,
                             const char *line);

/* Same idempotent-marker/backup/dryrun contract as dc_systheme_ensure_line()
 * above (idempotency check searches `user_file` for `line` itself, matching
 * how every real caller of dc_systheme_ensure_line() already passes the same
 * string as both the needle and the line -- see systheme.c's GTK emitter),
 * except the marker+line are INSERTED AT THE FILE HEAD instead of appended.
 * Needed by configs where an include/@import directive must precede the
 * rest of the file to take effect (e.g. mako, swaync, wofi, helix). If
 * `user_file` exists, it is backed up once to "<user_file>.bak-<epoch>"
 * before being rewritten as: `marker` + "\n" + `line` + "\n" + original
 * contents. If it doesn't exist yet, it's atomically created as just
 * `marker` + "\n" + `line` + "\n". DRYRUN-gated like dc_systheme_ensure_line(). */
bool dc_systheme_ensure_line_top(const char *user_file, const char *line, const char *marker);

/* True iff `dir` exists (any type) -- the config-dir half of an app's
 * "installed" check. Never creates `dir`. */
bool dc_systheme_dir_exists(const char *dir);

/* True iff `binary` is found on $PATH via access(X_OK) -- the PATH-search
 * half of an app's "installed" check, same style as printers.c's
 * dc_printers_available(). Deliberately never system()/popen(): main.c's
 * process-wide `signal(SIGCHLD, SIG_IGN)` breaks their internal waitpid(). */
bool dc_systheme_on_path(const char *binary);

/* fork()+execvp(argv[0], argv) fire-and-forget for a best-effort "reload"
 * nudge (e.g. `gsettings set org.gnome.desktop.interface color-scheme ...`).
 * Reaped by main's process-wide SIGCHLD=SIG_IGN, same shape as printers.c's
 * run_cmd() -- no waitpid(), the exit status is never needed. `argv` must be
 * NULL-terminated; `argc` excludes the terminating NULL. DRYRUN-gated: logs
 * "systheme: <tag>: would spawn: <argv...>" instead of forking. */
void dc_systheme_spawn(const char *tag, const char *const argv[], int argc);

#endif /* DC_SERVICES_SYSTHEME_INTERNAL_H */
