/* systheme.h — system-wide theming: dankc writes each supported desktop
 * app's *native* theme file directly from its own live palette
 * (dc_theme_current, see src/theme/theme.h) plus dc_config_light_mode().
 * There is no external `matugen` invocation anywhere in this path -- dankc
 * IS the color engine; this module just emits the file formats other apps
 * already know how to read.
 *
 * Task 1 implements the GTK3/GTK4 tier (see systheme.c). Later tasks add
 * Qt/Alacritty/VS Code/kitty/foot emitters alongside it, reusing the shared
 * atomic-write/backup/marker-injector/dryrun/spawn helpers declared in
 * systheme_internal.h.
 *
 * Safety contract (non-negotiable, see systheme.c's file header for the
 * full rationale): opt-in only (dc_config.systheme_enabled + the app's own
 * toggle + the app must be detected), dankc-owned files are written
 * atomically, user-owned files dankc only *nudges* (e.g. gtk.css's
 * @import) are backed up once before the first touch and never duplicate
 * their marker line on repeat calls. Disabling a toggle never deletes
 * anything dankc previously wrote.
 */
#ifndef DC_SERVICES_SYSTHEME_H
#define DC_SERVICES_SYSTHEME_H

#include <stdbool.h>

struct dc_config;

/* (Re)apply system theming for every enabled+detected app tier, from the
 * CURRENTLY active palette (dc_theme_current) and dc_config_light_mode().
 * Call after dc_theme_set()/dc_theme_set_custom() have finalized the
 * palette for this config generation (config.c's apply_theme() does this).
 *
 * Cheap to call often: an FNV hash over the palette bytes + light flag +
 * per-app toggle bitmask is computed first, and the whole call is a no-op
 * (zero disk I/O) if nothing has changed since the last call -- so repeated
 * dc_config_reapply() from the settings UI dragging a slider costs nothing
 * once the palette settles. Also an instant no-op, before any hashing, when
 * cfg->systheme_enabled is false.
 *
 * $DANKC_THEME_DRYRUN=1 makes every write and every reload-nudge spawn log
 * what it *would* do instead of touching disk/exec'ing -- see
 * systheme_internal.h's dc_systheme_dryrun(). Always export this while
 * testing; this module must never be exercised against a real
 * ~/.config/gtk-*.0 outside of a deliberate, reviewed non-dryrun run. */
void dc_systheme_apply(const struct dc_config *cfg);

/* True iff `app` looks installed on this machine: its config dir exists
 * under $HOME, or its binary is found on $PATH (access() based, never
 * system()/popen() -- see printers.c's dc_printers_available() comment for
 * why). Never creates anything. For the settings UI (a later task) to grey
 * out / hide toggles for apps that aren't present. Recognized ids: "gtk",
 * "qt", "alacritty", "vscode", "kitty", "foot", "kvantum", "kde", "ghostty",
 * "wezterm", "konsole", "xresources", "zed", "helix", "neovim", "vim",
 * "sublime", "emacs", "rofi", "wofi", "fuzzel", "tofi", "mako", "dunst",
 * "swaync", "btop", "cava", "zathura", "qutebrowser", "spicetify", "gtk2",
 * "firefox", "discord". Unknown ids return false. */
bool dc_systheme_app_detected(const char *app);

#endif /* DC_SERVICES_SYSTHEME_H */
