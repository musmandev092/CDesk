/* systheme_qtkde.h — system-wide theming Task T5 (Tier-2 coverage pass, see
 * docs/21-THEMING-COVERAGE-PLAN.md): Kvantum (Qt), the KDE/Plasma
 * kdeglobals colour-scheme, and GTK2. Same shape/contract as
 * systheme_term2.h's ghostty/wezterm/konsole/xresources emitters: callers
 * (systheme.c's dc_systheme_apply()) are expected to have already checked
 * the matching cfg->systheme_<app> toggle and
 * dc_systheme_app_detected("<app>") before calling; these functions do
 * their own, more specific re-check on top of that (e.g. never creating an
 * app's own config dir if it doesn't already exist).
 *
 * See systheme_qtkde.c's file header for the exact per-app file formats,
 * and in particular the Kvantum SVG design-tradeoff writeup (minimal vs.
 * full theme SVG).
 */
#ifndef DC_SERVICES_SYSTHEME_QTKDE_H
#define DC_SERVICES_SYSTHEME_QTKDE_H

#include <stdbool.h>

/* Writes ~/.config/Kvantum/DankC/DankC.kvconfig (dankc-owned INI: a small
 * [%General] behavior block + a [GeneralColors] palette block derived from
 * dc_theme_current) and ~/.config/Kvantum/DankC/DankC.svg (dankc-owned; see
 * systheme_qtkde.c for why this is a deliberately MINIMAL valid Kvantum
 * theme SVG rather than a full per-widget one). Ensures `theme=DankC` under
 * a `[General]` block in ~/.config/Kvantum/kvantum.kvconfig (user-owned,
 * marker+backup). Spawns `kvantummanager --set DankC` if it's on $PATH
 * (DRYRUN-gated, best-effort). restart-only: Kvantum/Qt apps read their
 * style at startup. */
void dc_systheme_apply_kvantum(bool light);

/* Writes ~/.local/share/color-schemes/DankC.colors (dankc-owned KConfig
 * INI: [Colors:Window]/[Colors:View]/[Colors:Button]/[Colors:Selection]/
 * [Colors:Tooltip] + [General] + [WM], decimal "r,g,b" triplets throughout,
 * per KDE's native colour-scheme format). Ensures `ColorScheme=DankC` under
 * a `[General]` block in ~/.config/kdeglobals (user-owned, marker+backup;
 * only if ~/.config or kdeglobals itself already exists -- never manufactures
 * $XDG_CONFIG_HOME out of thin air). Spawns `plasma-apply-colorscheme
 * DankC` if it's on $PATH (DRYRUN-gated, best-effort). KDE/Plasma apps
 * watch kdeglobals live (KConfigWatcher) -- no restart needed once a running
 * app's KConfigWatcher fires. */
void dc_systheme_apply_kde(bool light);

/* Writes ~/.gtkrc-2.0.dank (dankc-owned: a `style "dank" { ... }` block +
 * `class "*" style "dank"`). Ensures `include "<abs path>"` in
 * ~/.gtkrc-2.0 (user-owned, marker+backup; created fresh if GTK2 was only
 * detected via the `gimp` binary and no rc file exists yet, same precedent
 * as the GTK3/4 emitter creating gtk.css on a first run). restart-only:
 * GTK2 reads ~/.gtkrc-2.0 once at process startup. */
void dc_systheme_apply_gtk2(bool light);

#endif /* DC_SERVICES_SYSTHEME_QTKDE_H */
