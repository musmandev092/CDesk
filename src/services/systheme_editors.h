/* systheme_editors.h — system-wide theming Task T4 (Tier-2 coverage pass,
 * see docs/21-THEMING-COVERAGE-PLAN.md): text editors -- Zed, Helix, Neovim,
 * Vim, Sublime Text, Emacs. Same shape/contract as systheme_term2.h /
 * systheme_launchers.h: callers (systheme.c's dc_systheme_apply()) are
 * expected to have already checked the matching cfg->systheme_<app> toggle
 * and dc_systheme_app_detected("<app>") before calling; each function
 * re-checks its own app's config dir (or, for Neovim/Vim/Emacs, creates only
 * the one dankc-owned subdirectory *inside* an already-existing parent it
 * never invents itself -- see systheme_editors.c's file header) before
 * writing anything.
 *
 * All six build their palette from dc_theme_current (theme.h) +
 * dc_dynamic_ansi16() (theme/dynamic.h), same "background<-surface,
 * foreground<-surface_text, accent<-primary" role mapping every other
 * systheme_*.c emitter uses, extended with a shared syntax-highlighting
 * mapping onto the ANSI-16 set (comment/string/keyword/function/type/
 * constant/tag) so every editor's syntax colors agree with each other and
 * with the terminal emitters. See systheme_editors.c's file header for the
 * exact per-app file formats, that shared syntax mapping, and each app's
 * activation/reload story.
 */
#ifndef DC_SERVICES_SYSTHEME_EDITORS_H
#define DC_SERVICES_SYSTHEME_EDITORS_H

#include <stdbool.h>

/* Writes ~/.config/zed/themes/DankC.json (dankc-owned, atomic; a Zed
 * "theme family" with a "DankC Dark" and a "DankC Light" member, both
 * present so the family JSON is always structurally complete -- see this
 * file's .c header for the caveat about which one has live colors) and, via
 * a cJSON parse/patch/reserialize round-trip identical in spirit to
 * systheme_apps.c's VS Code emitter, sets the top-level "theme" string key
 * in ~/.config/zed/settings.json to whichever family member matches the
 * current light/dark mode -- every other settings.json key is preserved
 * byte-for-byte. Skips (with a warning, touching nothing) if settings.json
 * exists but fails to parse as strict JSON. Zed applies settings.json
 * changes live; the theme *file* may need a restart to pick up on some Zed
 * versions. */
void dc_systheme_apply_zed(bool light);

/* Writes ~/.config/helix/themes/dank.toml (dankc-owned, atomic) and ensures
 * `theme = "dank"` at the very TOP of ~/.config/helix/config.toml (user-
 * owned, marker+backup via dc_systheme_ensure_line_top() -- a bare
 * top-level TOML key must precede any `[section]` table header to parse).
 * Nudges already-running `hx` instances via `pkill -USR1 hx` (helix's
 * documented config-reload signal; SIGCHLD-safe spawn, DRYRUN-gated). */
void dc_systheme_apply_helix(bool light);

/* Writes ~/.config/nvim/colors/dank.lua (dankc-owned, atomic): a Lua
 * colorscheme script that clears any existing highlighting, sets
 * `vim.g.colors_name`, applies core highlight groups via
 * `vim.api.nvim_set_hl()`, and sets `vim.g.terminal_color_0`..`_15` for
 * :terminal. Write-only -- Neovim only loads a colorscheme when the user
 * runs `:colorscheme dank` (or configures it in their own init.lua); no
 * reload spawn, no config.toml/init.lua edit (Task 8's settings UI is
 * expected to surface that activation step as a hint, not this emitter). */
void dc_systheme_apply_neovim(bool light);

/* Writes ~/.vim/colors/dank.vim (dankc-owned, atomic): a classic Vimscript
 * colorscheme (`hi` highlight-group commands + `g:terminal_ansi_colors`).
 * Write-only, same activation story as Neovim above (`:colorscheme dank`,
 * one time, by the user) -- no reload spawn. */
void dc_systheme_apply_vim(bool light);

/* Writes ~/.config/sublime-text/Packages/User/DankC.sublime-color-scheme
 * (dankc-owned, atomic; Sublime's native JSON color-scheme format: a
 * "variables" palette, "globals" for chrome, and scope "rules" for syntax
 * highlighting) and, via the same cJSON round-trip approach as Zed/VS Code,
 * sets "color_scheme" in
 * ~/.config/sublime-text/Packages/User/Preferences.sublime-settings --
 * every other key preserved, skipped (with a warning) on parse failure.
 * Sublime applies both files live, no reload spawn. */
void dc_systheme_apply_sublime(bool light);

/* Writes ~/.emacs.d/dank-theme.el (dankc-owned, atomic): a `deftheme` +
 * `custom-theme-set-faces` Emacs Lisp theme covering the core faces
 * (default/cursor/region/mode-line/font-lock-*). Write-only -- Emacs themes
 * require the user to add dank-theme.el's directory to
 * `custom-theme-load-path` and call `(load-theme 'dank t)` themselves
 * (a one-time hint, Task 8's job); no reload spawn. */
void dc_systheme_apply_emacs(bool light);

#endif /* DC_SERVICES_SYSTHEME_EDITORS_H */
