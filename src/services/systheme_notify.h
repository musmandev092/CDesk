/* systheme_notify.h — Task 3 (wave 2) of system-wide theming: the desktop
 * notification-daemon emitters (mako, dunst, swaync). Same calling
 * convention as systheme_term.h/systheme_apps.h: systheme.c's
 * dc_systheme_apply() has already checked the matching cfg->systheme_<app>
 * toggle and dc_systheme_app_detected("<app>") before calling any of these;
 * each function re-checks its own app's config dir exists (never creates one
 * dankc didn't find) before writing anything -- see systheme.h's safety
 * contract and systheme_notify.c's file header for the exact per-app file
 * formats/caveats.
 */
#ifndef DC_SERVICES_SYSTHEME_NOTIFY_H
#define DC_SERVICES_SYSTHEME_NOTIFY_H

#include <stdbool.h>

/* Writes ~/.config/mako/dank-colors (dankc-owned, atomic: background-color/
 * text-color/border-color/progress-color plus an [urgency=critical]
 * override block) and ensures `include=<abs path>` is the FIRST line of
 * ~/.config/mako/config (user-owned, marker+backup, ensure_line_top --
 * mako has no section-reset, so a later [criteria] block in the user's own
 * config could otherwise re-scope the include; top placement avoids that).
 * Nudges already-running mako via `makoctl reload` (DRYRUN-gated spawn). */
void dc_systheme_apply_mako(bool light);

/* Writes ~/.config/dunst/dunstrc.d/99-dank-colors.conf -- a PURE drop-in
 * (dunst reads every *.conf under dunstrc.d/ alongside dunstrc itself,
 * later files winning key conflicts): dankc never edits the user's own
 * dunstrc. Sets [global] frame_color plus background/foreground/frame_color
 * for [urgency_low]/[urgency_normal]/[urgency_critical] (critical uses the
 * theme's error colour). May create the dunstrc.d/ directory itself -- but
 * ONLY inside an already-existing ~/.config/dunst (this function's own
 * dc_systheme_dir_exists() check on that parent already gated on it being
 * real) -- as a narrow, documented exception to the "dankc never creates an
 * app's config dir" rule; see systheme_notify.c's file header. Nudges
 * already-running dunst via `dunstctl reload` (DRYRUN-gated spawn). */
void dc_systheme_apply_dunst(bool light);

/* Writes ~/.config/swaync/dank-colors.css (dankc-owned, atomic: @define-color
 * noti-bg/cc-bg/noti-fg/noti-border-color plus a best-effort `.critical`
 * override using the theme's error colour). If ~/.config/swaync/style.css
 * already exists, ensures `@import 'dank-colors.css';` is its FIRST line
 * (user-owned, marker+backup, ensure_line_top). If style.css does NOT exist
 * yet, dank-colors.css is still written but the import step is skipped --
 * swaync ships a packaged default style.css baked into its binary that
 * dankc must not clobber by creating a bare one out of nothing (a later
 * settings-UI task hints the user to create style.css themselves). Nudges
 * already-running swaync via `swaync-client --reload-css` (DRYRUN-gated
 * spawn). */
void dc_systheme_apply_swaync(bool light);

#endif /* DC_SERVICES_SYSTHEME_NOTIFY_H */
