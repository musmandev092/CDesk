/* systheme_apps.h — Task 3 of system-wide theming: VS Code + Qt (qt5ct/
 * qt6ct) emitters. Same shape/contract as systheme_term.h's terminal
 * emitters: callers (systheme.c's dc_systheme_apply()) are expected to have
 * already checked the matching cfg->systheme_<app> toggle and
 * dc_systheme_app_detected("<app>") before calling; these functions do their
 * OWN, more specific detection on top of that (VS Code: which of
 * Code - OSS/Code/VSCodium actually has a settings.json; Qt: which of
 * qt5ct/qt6ct actually has a config dir) and silently no-op if their more
 * specific check comes up empty, same precedent as systheme.c's
 * apply_gtk_variant() re-checking gtk-N.0 per variant. See systheme_apps.c's
 * file header for the exact file formats and the JSON-parse-failure safety
 * rule for VS Code.
 */
#ifndef DC_SERVICES_SYSTHEME_APPS_H
#define DC_SERVICES_SYSTHEME_APPS_H

#include <stdbool.h>

/* Rewrites the top-level "workbench.colorCustomizations" object in whichever
 * of ~/.config/{Code - OSS,Code,VSCodium}/User/settings.json is found first
 * (in that order), leaving every other key untouched. If the file exists but
 * fails to parse as strict JSON (VS Code's settings.json legally allows
 * JSONC comments/trailing commas, which cJSON rejects), this logs a warning
 * and skips VS Code entirely rather than risk clobbering a file it can't
 * fully understand. No reload spawn -- VS Code applies settings.json live. */
void dc_systheme_apply_vscode(bool light);

/* Writes a 21-role Qt QPalette [ColorScheme] to
 * ~/.config/qt{5,6}ct/colors/dankc.conf (dankc-owned, atomic) for whichever
 * of qt5ct/qt6ct has an existing config dir, and patches that app's
 * qt{5,6}ct.conf [Appearance] section (custom_palette=true,
 * color_scheme_path=<abs path>) via the marker+backup line-patcher. Restart
 * required -- qt5ct/qt6ct don't live-reload their palette, no reload spawn. */
void dc_systheme_apply_qt(bool light);

#endif /* DC_SERVICES_SYSTHEME_APPS_H */
