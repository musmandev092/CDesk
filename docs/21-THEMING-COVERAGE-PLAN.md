# dankc System-Wide Theming — Full Coverage Plan (2026-07-04)

User order: theme ALL commonly-used Linux apps ("do it properly or not at all").
Engine (already on main): `src/services/systheme.c` + `systheme_internal.h` plumbing
(`dc_systheme_write_owned` atomic tmp+rename; `dc_systheme_ensure_line` marker-inject +
once-only `.bak-<epoch>` backup; `dc_systheme_dir_exists`/`_on_path` detection;
`dc_systheme_spawn` SIGCHLD-safe; `DANKC_THEME_DRYRUN` gate; FNV palette-hash debounce;
never-create-config-dir). v1 emitters done: GTK3/4, alacritty/kitty/foot, VSCode, qt5ct/qt6ct + settings UI.

Palette shorthand: **bg**=surface, **fg**=surface_text, **accent**=primary, **on-accent**=primary_text,
**sel-bg**=primary_container, **panel**=surface_container, **border**=outline, **ANSI[0..15]**=dc_dynamic_ansi16.

## Tier 1 (biggest surface)
- **Kvantum** — `~/.config/Kvantum/DankC/DankC.kvconfig` (+recolored base SVG) + `theme=DankC` in kvantum.kvconfig. INI+SVG. restart-only; `kvantummanager --set DankC` if on PATH. Detect: ~/.config/Kvantum OR kvantummanager. Largest emitter (embed SVG template w/ hex slots).
- **KDE/kdeglobals** — owned `~/.local/share/color-schemes/DankC.colors` (KConfig INI, `r,g,b` triplets: [Colors:Window/View/Button/Selection/Tooltip], [General], [WM]) + `ColorScheme=DankC` in kdeglobals [General]. LIVE (KConfigWatcher); `plasma-apply-colorscheme DankC` if present. Detect: ~/.local/share/color-schemes OR kdeglobals OR kreadconfig6.
- **ghostty** — owned `~/.config/ghostty/themes/dank-theme` (`palette=N=#hex` 0-15, background/foreground/cursor-color/cursor-text/selection-*) + `theme = dank-theme` in config. `pkill -USR2 ghostty`. Detect dir OR ghostty.
- **wezterm** — owned `~/.config/wezterm/colors/DankC.toml` ([colors] ansi[8]/brights[8]/background/foreground/cursor_bg/cursor_fg/selection_*). CANNOT edit wezterm.lua (Lua) → user adds `color_scheme="DankC"` once (UI hint). Detect dir OR wezterm.
- **Zed** — owned `~/.config/zed/themes/DankC.json` (theme-family JSON, dark+light) + set `theme` key in settings.json via cJSON round-trip (owns only `theme` key; skip on parse-fail). settings live; theme-file may need restart. Detect dir OR zed/zeditor.
- **Neovim** — owned `~/.config/nvim/colors/dank.lua` (nvim_set_hl core groups + g:terminal_color_0..15). User sets `colorscheme dank` once (hint). Detect dir OR nvim.
- **rofi** — owned `~/.config/rofi/dank-colors.rasi` (`* { dank-*: #; }`) + `@import` in config.rasi (append OK, later wins). per-invocation. Detect dir OR rofi.
- **fuzzel** — owned `~/.config/fuzzel/dank-colors.ini` ([colors] rrggbbaa no #) + `include=` in fuzzel.ini. per-invocation. Detect dir OR fuzzel.
- **mako** — owned `~/.config/mako/dank-colors` + `include=` at TOP of config (needs ensure_line_top). `makoctl reload`. Detect dir OR mako.
- **dunst** — owned drop-in `~/.config/dunst/dunstrc.d/99-dank-colors.conf` (no user-file edit; may create dunstrc.d inside existing dunst dir). `dunstctl reload`. Detect dir OR dunst.
- **swaync** — owned `~/.config/swaync/dank-colors.css` + `@import` at TOP of style.css (ensure_line_top); only if style.css exists else hint. `swaync-client --reload-css`. Detect dir OR swaync.

## Tier 2
- **Helix** — owned `~/.config/helix/themes/dank.toml` + `theme=` at top of config.toml (ensure_line_top). `pkill -USR1 hx`. Detect dir OR hx.
- **wofi** — owned `~/.config/wofi/dank-colors.css` + `@import` top of style.css (ensure_line_top; create minimal if none). per-invocation. Detect dir OR wofi.
- **tofi** — owned `~/.config/tofi/dank-colors` + `include=` in config. per-invocation. Detect dir OR tofi.
- **btop** — owned `~/.config/btop/themes/dank.theme` + `color_theme="dank"` in btop.conf. `pkill -USR2 btop`. Detect dir OR btop.
- **cava** — patch [color] gradient_color_N in `~/.config/cava/config` (append [color] block, last-wins — verify). `pkill -USR2 cava`. Detect dir OR cava.
- **zathura** — owned `~/.config/zathura/dank-colors` (set default-bg/fg, statusbar, highlight, recolor-*) + `include dank-colors` in zathurarc. restart-only. Detect dir OR zathura.
- **qutebrowser** — owned `~/.config/qutebrowser/dank-colors.py` (c.colors.*) + `config.source('dank-colors.py')` in config.py — ONLY if config.py exists (else hint; creating it disables autoconfig). Detect dir OR qutebrowser.
- **Vim** — owned `~/.vim/colors/dank.vim`. `:colorscheme dank` (hint). Detect ~/.vim OR vim.
- **GTK2** — owned `~/.gtkrc-2.0.dank` + `include` in ~/.gtkrc-2.0. restart-only. Detect ~/.gtkrc-2.0 OR gimp. (GTK1/Qt4 = dead, excluded.)

## Tier 3 (default OFF; external mod/tool or manual)
- **Firefox/Zen/LibreWolf** — per-profile chrome/dank-colors.css + @import in userChrome.css + `toolkit.legacyUserProfileCustomizations.stylesheets` in user.js. restart-only. Chrome-UI only (content needs extension — hint). Parse profiles.ini (Default/Install*). One toggle for all 3.
- **Discord (Vesktop/Vencord)** — owned theme CSS in ~/.config/vesktop/themes or ~/.config/Vencord/themes (:root --background-* overrides). Needs client mod + enable theme once. Detect theme dirs.
- **Spicetify** — owned `~/.config/spicetify/Themes/DankC/color.ini` + `spicetify config current_theme DankC; spicetify apply` (slow, restarts Spotify — apply ONLY on explicit toggle/hash-change, never repeatedly). Needs spicetify CLI. Detect dir OR spicetify.
- **Sublime** — owned Packages/User/DankC.sublime-color-scheme + color_scheme key in Preferences (cJSON round-trip). live. Detect ~/.config/sublime-text/Packages/User.
- **Emacs** — owned ~/.emacs.d/dank-theme.el (custom-theme-set-faces). User (load-theme 'dank) (hint). Detect ~/.emacs.d or ~/.config/emacs OR emacs.
- **Konsole** — owned ~/.local/share/konsole/DankC.colorscheme + ColorScheme= in default .profile (read konsolerc DefaultProfile=). restart-only. Detect dir OR konsole.
- **urxvt/xterm (Xresources)** — owned ~/.config/dank/xresources + `#include` in ~/.Xresources + `xrdb -merge`. new instances only. XWayland-only. Detect (urxvt|xterm) AND xrdb on PATH.
- Excluded: st (recompile), mpv (no palette), gnome-terminal (needs child-stdout UUID lookup — defer), fastfetch (inherits terminal ANSI free), dgop (DMS-only). Optional stretch: tmux/starship (matugen-covered, low effort).

## Caveats
- External mod/tool required: spicetify, discord, firefox(content), qt5ct/qt6ct (needs QT_QPA_PLATFORMTHEME set).
- Manual one-line activation (dankc writes file, user flips): wezterm, neovim, vim, emacs.
- restart-only: GTK3(running), qt/kvantum/kde-partial, foot, konsole, zathura, firefox, gtk2, xresources.
- live/near-live: GTK4, KDE(kdeglobals), alacritty, kitty/ghostty/btop/cava(USR), helix(USR1), VSCode/Sublime/Zed-settings, mako/dunst/swaync(ctl), qutebrowser(:config-source), vencord, all launchers per-invocation.

## Settings UI (Task 8): keep master toggle; group by category (Toolkits/Terminals/Editors/
Launchers/Notifications/Browsers&Chat/Media&Monitors) via collapsible sub-headers with count badge;
detected-first (render detected rows + a "show N undetected" session-local expander per category);
per-row caveat hints; Tier-3 default-off.

## Task breakdown (one Sonnet agent each)
- **T0 (DONE/in-flight)**: config plumbing (~27 toggles + JSON keys + defaults per tier), widen compute_palette_hash to u64, extend dc_systheme_app_detected for all app-ids, add `dc_systheme_ensure_line_top()` prepend helper. No emitters, no settings.c.
- **T1 systheme_term2.c**: ghostty, wezterm, konsole, xresources.
- **T2 systheme_launchers.c**: rofi, wofi, fuzzel, tofi.
- **T3 systheme_notify.c**: mako, dunst, swaync (uses ensure_line_top).
- **T4 systheme_editors.c**: zed, helix, neovim, vim, sublime, emacs (Zed/Sublime reuse VSCode cJSON round-trip; consider promoting to internal helper).
- **T5 systheme_qtkde.c**: kvantum, kde, gtk2 (largest — embed Kvantum SVG template).
- **T6 systheme_browser.c**: firefox (profiles.ini parser, 3 forks one toggle), qutebrowser, discord.
- **T7 systheme_misc.c**: btop, cava, zathura, spicetify (+optional tmux/starship).
- **T8 settings.c (SERIALIZED LAST, only settings.c toucher)**: categorized collapsible UI per above.
Each emitter task also appends its `if(cfg->systheme_X && dc_systheme_app_detected("X")) apply_X(light);`
to dc_systheme_apply() + its file to meson.build (append-only; merge serially → trivial conflicts).

## Risks: wezterm/zed scheme-file hot-reload uncertain (fallback restart hint); cava/btop last-wins
append (verify in scratch XDG); qutebrowser autoconfig disable if config.py created; kdeglobals dup
[General] (KConfig last-wins); Kvantum SVG substitution volume; spicetify fragility (default-off,
apply once per hash); firefox multi-profile (theme all, backup each).
