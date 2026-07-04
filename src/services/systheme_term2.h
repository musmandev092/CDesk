/* systheme_term2.h — system-wide theming Task 1 (T1) of the Tier-2 coverage
 * pass (docs/21-THEMING-COVERAGE-PLAN.md): ghostty, wezterm, konsole and
 * urxvt/xterm (via Xresources). Same shape/contract as systheme_term.h's
 * alacritty/kitty/foot emitters: callers (systheme.c's dc_systheme_apply())
 * are expected to have already checked the matching cfg->systheme_<app>
 * toggle and dc_systheme_app_detected("<app>") before calling; these
 * functions do their own, more specific re-check on top of that (e.g.
 * ghostty/wezterm/konsole re-verify their own config dir exists before
 * writing anything, matching systheme_term.c's apply_alacritty()/
 * apply_kitty()/apply_foot() precedent -- dc_systheme_app_detected() can
 * return true purely from a binary being on $PATH, which must never be
 * enough to make dankc *create* a config dir the app itself never made).
 *
 * All four build their per-app palette from dc_theme_current (theme.h) plus
 * dc_dynamic_ansi16() (theme/dynamic.h) for the 16-colour ANSI set, same
 * "background<-surface, foreground<-surface_text, cursor<-primary,
 * selection-background<-primary_container, selection-foreground<-
 * surface_text" role mapping systheme_term.c's alacritty/kitty/foot
 * emitters already use, extended with ANSI-16 in decimal (konsole) as well
 * as hex (ghostty/wezterm/Xresources). See systheme_term2.c's file header
 * for the exact per-app file formats.
 */
#ifndef DC_SERVICES_SYSTHEME_TERM2_H
#define DC_SERVICES_SYSTHEME_TERM2_H

#include <stdbool.h>

/* Writes ~/.config/ghostty/themes/dank-theme (dankc-owned, atomic; creates
 * the themes/ subdirectory if ghostty's own config dir already exists but
 * themes/ doesn't yet) and ensures `theme = dank-theme` in
 * ~/.config/ghostty/config (user-owned, marker+backup). Nudges already-
 * running ghostty instances via `pkill -USR2 ghostty` (SIGCHLD-safe spawn,
 * DRYRUN-gated). */
void dc_systheme_apply_ghostty(bool light);

/* Writes ~/.config/wezterm/colors/DankC.toml (dankc-owned, atomic; creates
 * the colors/ subdirectory if wezterm's own config dir already exists but
 * colors/ doesn't yet). wezterm.lua is Lua, not data -- dankc never edits it;
 * the user must add `color_scheme = "DankC"` once themselves (a settings-UI
 * hint for that is a later task, not this emitter's job). No reload spawn:
 * wezterm scheme-file hot-reload behaviour is unconfirmed, restart is the
 * documented fallback. */
void dc_systheme_apply_wezterm(bool light);

/* Writes ~/.local/share/konsole/DankC.colorscheme (dankc-owned, atomic;
 * KConfig INI, decimal r,g,b triplets from dc_dynamic_ansi16()). Also reads
 * (read-only) ~/.config/konsolerc's `DefaultProfile=` to find the default
 * .profile file under ~/.local/share/konsole/ and, if found, patches its
 * [Appearance] ColorScheme= key (marker+backup); if no DefaultProfile is set
 * or the referenced profile file doesn't exist, the scheme file is still
 * written and this just logs that the profile patch was skipped -- the user
 * can select "DankC" manually in Konsole's profile settings. restart-only,
 * no reload spawn. */
void dc_systheme_apply_konsole(bool light);

/* Writes ~/.config/dank/xresources (dankc-owned, atomic; dankc's own
 * namespace directory, created if missing -- unlike every other emitter in
 * this file, this path isn't inside another app's own config dir, so there's
 * nothing else to avoid creating) and ensures
 * `#include "<abs path>"` in ~/.Xresources (user-owned, marker+backup).
 * Only applies to freshly-started urxvt/xterm instances (X resources are
 * read once at connection time) -- if `xrdb` is on $PATH, nudges the running
 * X server's resource database via `xrdb -merge ~/.Xresources` (SIGCHLD-safe
 * spawn, DRYRUN-gated); otherwise skips the spawn entirely. XWayland-only in
 * a Wayland session. Gated by a default-OFF toggle (systheme_xresources)
 * since it reaches outside dankc's usual $XDG_CONFIG_HOME sandbox into
 * ~/.Xresources directly. */
void dc_systheme_apply_xresources(bool light);

#endif /* DC_SERVICES_SYSTHEME_TERM2_H */
