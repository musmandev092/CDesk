/* systheme_browser.h — system-wide theming Task T6 (Tier-3 coverage pass,
 * see docs/21-THEMING-COVERAGE-PLAN.md): Firefox/Zen/LibreWolf, qutebrowser,
 * Discord (Vesktop/Vencord). Same shape/contract as every other
 * systheme_*.h: callers (systheme.c's dc_systheme_apply()) are expected to
 * have already checked the matching cfg->systheme_<app> toggle and
 * dc_systheme_app_detected("<app>") before calling; each function re-checks
 * its own app's actual on-disk state (profile dirs / config dir / themes
 * dir) before writing anything, and never creates an app's own top-level
 * config directory (systheme.h's safety contract) -- see
 * systheme_browser.c's file header for the one narrow, documented exception
 * (Firefox's per-profile chrome/ subdirectory, exactly analogous to
 * systheme_apps.c's qt5ct/qt6ct colors/ subdirectory and systheme_notify.c's
 * dunst dunstrc.d/ subdirectory precedents).
 *
 * All three are Tier 3 ("default OFF; external mod/tool or manual" per the
 * plan doc): cfg->systheme_firefox and cfg->systheme_discord both default to
 * false (core/config.c) since both need an out-of-band step (a browser
 * restart + content still needs an extension for Firefox; a Vesktop/Vencord
 * client mod plus enabling the theme once for Discord) before they visibly
 * do anything; cfg->systheme_qutebrowser defaults true since qutebrowser
 * reads its whole config directory (including any stray *.py dankc drops in)
 * on every launch with no extra activation step, once config.source() (or a
 * user's own import) actually pulls dank-colors.py in.
 */
#ifndef DC_SERVICES_SYSTHEME_BROWSER_H
#define DC_SERVICES_SYSTHEME_BROWSER_H

#include <stdbool.h>

/* For every profile found by parsing profiles.ini under ~/.mozilla/firefox,
 * ~/.zen, and ~/.librewolf (one dankc toggle covers all three forks --
 * they share Firefox's profile-manager INI format and chrome/ layout):
 * writes a dankc-owned <profile>/chrome/dank-colors.css (:root custom
 * properties -- --lwt-accent-color/--toolbar-bgcolor/tab colors -- from
 * primary/surface_container/surface_text/surface), creating the chrome/
 * subdirectory only inside a profile directory that profiles.ini itself
 * pointed at (never inventing a profile), ensures
 * `@import "dank-colors.css";` is present in <profile>/chrome/userChrome.css
 * (marker+backup, created if absent since without it the CSS above never
 * loads), and ensures
 * `user_pref("toolkit.legacyUserProfileCustomizations.stylesheets", true);`
 * is present in <profile>/user.js (marker+backup, created if absent --
 * required for Firefox to read userChrome.css/userContent.css at all).
 * Chrome (browser UI) only -- page CONTENT still needs a separate userstyles
 * extension, out of scope here. Restart-only: Firefox reads chrome/ and
 * user.js only at startup. profiles.ini itself is read-only, never written. */
void dc_systheme_apply_firefox(bool light);

/* Writes ~/.config/qutebrowser/dank-colors.py (dankc-owned, atomic:
 * c.colors.* assignments for completion/statusbar/tabs/hints/messages) and,
 * ONLY if ~/.config/qutebrowser/config.py already exists (creating one from
 * scratch disables qutebrowser's autoconfig.yml-based settings entirely --
 * a much bigger behavior change than this emitter is willing to make on the
 * user's behalf), ensures `config.source('dank-colors.py')` is present in it
 * (marker+backup). If config.py doesn't exist yet, dank-colors.py is still
 * written (harmless sitting unused on disk) but the source-injection step is
 * silently skipped -- Task 8's settings UI is expected to hint the one-line
 * manual fix. Per-invocation reload (qutebrowser reads its config fresh on
 * each launch) -- no reload spawn, no runtime :config-source. */
void dc_systheme_apply_qutebrowser(bool light);

/* Writes a dankc-owned BetterDiscord-style theme,
 * <dir>/DankC.theme.css, into whichever of ~/.config/vesktop/themes or
 * ~/.config/Vencord/themes already exists (vesktop checked first; neither
 * dir is ever created -- if a user has neither client mod's themes/
 * directory yet, this is a silent no-op). Sets :root custom properties
 * (--background-*, --text-*, --interactive-*, --brand-experiment,
 * --status-danger) from surface/surface_container/surface_text/primary/error.
 * Requires the client mod (stock Discord ignores this file entirely) and
 * the user enabling "DankC" once in Vencord/Vesktop's theme list -- no
 * reload spawn, no other file touched. */
void dc_systheme_apply_discord(bool light);

#endif /* DC_SERVICES_SYSTHEME_BROWSER_H */
