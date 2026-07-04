/* systheme_term.h — Task 2 of system-wide theming: terminal emitters
 * (alacritty, kitty, foot). Entry points called from systheme.c's
 * dc_systheme_apply(), same shape as systheme.c's own apply_gtk(): each
 * function re-checks dc_systheme_dir_exists() on the app's own config dir
 * before writing anything (dc_systheme_app_detected() can return true purely
 * from finding the binary on $PATH, which must NOT be enough to make dankc
 * *create* a config dir the app itself never made -- see systheme.h's safety
 * contract). Callers are expected to have already checked the matching
 * cfg->systheme_<app> toggle and dc_systheme_app_detected("<app>") before
 * calling; these functions don't re-check the toggle (that's per systheme.c's
 * existing apply_gtk() convention, where the toggle gate lives at the call
 * site in dc_systheme_apply()).
 *
 * All three build their per-app palette from dc_theme_current (theme.h) plus
 * dc_dynamic_ansi16() (theme/dynamic.h) for the 16-colour ANSI set. See
 * systheme_term.c's file header for the exact role mapping and the
 * per-app file formats (verified against DMS's own matugen-generated
 * ~/.config/alacritty/dank-theme.toml reference for the stock green theme).
 */
#ifndef DC_SERVICES_SYSTHEME_TERM_H
#define DC_SERVICES_SYSTHEME_TERM_H

#include <stdbool.h>

/* Writes ~/.config/alacritty/dank-theme.toml (dankc-owned, atomic) and
 * ensures it's imported from alacritty.toml (user-owned, marker+backup).
 * alacritty live-reloads both files automatically -- no reload spawn. */
void dc_systheme_apply_alacritty(bool light);

/* Writes ~/.config/kitty/dank-theme.conf (dankc-owned, atomic), ensures
 * `include dank-theme.conf` in kitty.conf (user-owned, marker+backup), and
 * nudges already-running kitty instances to reload via `pkill -USR1 kitty`
 * (SIGCHLD-safe spawn, DRYRUN-gated). */
void dc_systheme_apply_kitty(bool light);

/* Writes ~/.config/foot/dank-theme.ini (dankc-owned, atomic) and ensures an
 * `include=<abs path>` line under foot.ini's [main] section (user-owned,
 * marker+backup). foot only reads its config at startup -- restart-only,
 * no reload spawn. */
void dc_systheme_apply_foot(bool light);

#endif /* DC_SERVICES_SYSTHEME_TERM_H */
