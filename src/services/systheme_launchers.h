/* systheme_launchers.h — Task 2 (T2) of system-wide theming: application
 * launcher / menu emitters (rofi, wofi, fuzzel, tofi). Same shape/contract as
 * systheme_term.h's terminal emitters and systheme_apps.h's VS Code/Qt
 * emitters: callers (systheme.c's dc_systheme_apply()) are expected to have
 * already checked the matching cfg->systheme_<app> toggle and
 * dc_systheme_app_detected("<app>") before calling; each function re-checks
 * dc_systheme_dir_exists() on the app's own config dir before writing
 * anything, so dc_systheme_app_detected() returning true purely from finding
 * the binary on $PATH never causes dankc to *create* a config dir the app
 * itself never made (systheme.h's safety contract).
 *
 * All four build their per-app palette from dc_theme_current (theme.h) +
 * dc_config_light_mode(). None of them live-reload -- launchers read their
 * config fresh on every invocation, so there's no reload spawn for any of
 * them (see systheme_launchers.c's file header for the exact per-app file
 * formats and role mapping).
 */
#ifndef DC_SERVICES_SYSTHEME_LAUNCHERS_H
#define DC_SERVICES_SYSTHEME_LAUNCHERS_H

#include <stdbool.h>

/* Writes ~/.config/rofi/dank-colors.rasi (dankc-owned, atomic) and ensures
 * `@import "dank-colors.rasi"` is present somewhere in config.rasi
 * (user-owned, marker+backup, appended -- rofi's rasi cascade lets a later
 * `@import` override earlier property definitions, so append-only is
 * sufficient). Per-invocation reload -- no reload spawn. */
void dc_systheme_apply_rofi(bool light);

/* Writes ~/.config/wofi/dank-colors.css (dankc-owned, atomic) and ensures
 * `@import 'dank-colors.css';` is prepended at the TOP of style.css
 * (user-owned, marker+backup via dc_systheme_ensure_line_top() -- GTK CSS
 * @import rules must precede other statements). If style.css doesn't exist
 * yet, it's created containing just the import plus a handful of minimal
 * selectors wired to the dank palette. Per-invocation reload -- no reload
 * spawn. */
void dc_systheme_apply_wofi(bool light);

/* Writes ~/.config/fuzzel/dank-colors.ini (dankc-owned, atomic) and ensures
 * an `include=<abs path>` line is present in fuzzel.ini (user-owned,
 * marker+backup, appended -- fuzzel's ini parser applies includes in
 * document order and later key assignments win, so append-only is
 * sufficient. Colours are 8-digit rrggbbaa with no leading '#', fuzzel's
 * required format). Per-invocation reload -- no reload spawn. */
void dc_systheme_apply_fuzzel(bool light);

/* Writes ~/.config/tofi/dank-colors (dankc-owned, atomic) and ensures an
 * `include=<abs path>` line is present in tofi's config (user-owned,
 * marker+backup, appended -- tofi applies keys in document order, later
 * wins). Colours are "#RRGGBB", tofi's expected format. Per-invocation
 * reload -- no reload spawn. */
void dc_systheme_apply_tofi(bool light);

#endif /* DC_SERVICES_SYSTHEME_LAUNCHERS_H */
