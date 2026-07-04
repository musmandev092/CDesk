/* systheme_misc.h — system-wide theming Task T7 (see
 * docs/21-THEMING-COVERAGE-PLAN.md): btop, cava, zathura, spicetify (Spotify).
 * Same shape/contract as systheme_editors.h / systheme_launchers.h: callers
 * (systheme.c's dc_systheme_apply()) are expected to have already checked the
 * matching cfg->systheme_<app> toggle and dc_systheme_app_detected("<app>")
 * before calling; each function re-checks its own app's config dir before
 * writing anything and never creates that top-level dir itself (only
 * conventional subdirectories inside an already-existing one, e.g. btop's
 * themes/). See systheme_misc.c's file header for the exact per-app file
 * formats, reload story, and (for spicetify) the extra apply-once-per-hash
 * guard.
 */
#ifndef DC_SERVICES_SYSTHEME_MISC_H
#define DC_SERVICES_SYSTHEME_MISC_H

#include <stdbool.h>

/* Writes ~/.config/btop/themes/dank.theme (dankc-owned, atomic; btop's
 * `theme[key]="#hexvalue"` format): core chrome (main_bg/main_fg/title/
 * hi_fg/selected_bg/selected_fg/inactive_fg/graph_text/div_line/cpu_box/
 * mem_box/net_box/proc_box) plus
 * three-stop start/mid/end gradients for the cpu, mem (used/free/cached/
 * available) and net (download/upload) graph widgets, drawn from
 * primary/success/warning/error. Ensures `color_theme = "dank"` in
 * ~/.config/btop/btop.conf (user-owned; appended if not already present --
 * btop.conf is last-key-wins, so a plain append is sufficient and never
 * needs to hunt down/replace a prior value). Nudges already-running btop via
 * `pkill -USR2 btop` (its documented config-reload signal). */
void dc_systheme_apply_btop(bool light);

/* Appends a `[color]` block (gradient=1 + gradient_color_1..8, an 8-stop
 * primary->secondary ramp) to ~/.config/cava/config (user-owned; cava has no
 * @include directive, so the block is embedded directly, unlike every other
 * emitter in this codebase). Custom marker-based replace-in-place (not
 * dc_systheme_ensure_line(), whose "idempotent iff the exact line already
 * matches" contract would either grow the file forever across palette
 * changes or freeze stale colors in place -- see this file's cava section
 * for why): a prior dankc block is located by its marker comment and
 * replaced with the freshly rendered one; the file is backed up once, the
 * very first time dankc ever touches it. Nudges already-running cava via
 * `pkill -USR2 cava` (its documented reload signal). */
void dc_systheme_apply_cava(bool light);

/* Writes ~/.config/zathura/dank-colors (dankc-owned, atomic; zathurarc `set
 * key value` syntax): default-bg/fg, statusbar-*, inputbar-*, notification-*,
 * completion-*, highlight-color/highlight-active-color, and the
 * recolor-lightcolor/recolor-darkcolor pair (so zathura's "invert PDF
 * colors" mode also matches, if the user has it on). Ensures `include
 * dank-colors` in ~/.config/zathura/zathurarc (user-owned, appended once via
 * dc_systheme_ensure_line() -- the include line itself never changes across
 * palette updates, unlike cava's embedded block, so the generic helper's
 * exact-match idempotency is exactly right here). Restart-only: zathura has
 * no live-reload/reload-signal story, so no spawn. */
void dc_systheme_apply_zathura(bool light);

/* Writes ~/.config/spicetify/Themes/DankC/color.ini (dankc-owned, atomic;
 * spicetify's `[Base]` INI section, hex values WITHOUT a leading '#' per its
 * own convention): text/subtext/main/sidebar/player/card/button/
 * button-active/notification (+ a couple of directly-analogous extras --
 * notification-error/misc -- sourced from the same palette). Only if
 * cfg->systheme_spicetify is explicitly on (default off) AND the rendered
 * color.ini actually differs from the last one this process wrote (an
 * internal per-process hash guard, on top of dc_systheme_apply()'s own
 * whole-palette debounce, so this function's own contract holds even if it
 * were ever called directly): spawns `spicetify config current_theme DankC`
 * then `spicetify apply` -- disruptive (restarts the Spotify client), so
 * this is the only emitter in the whole systheme_* family that ever runs a
 * second, mutating command beyond writing its own file. Needs the spicetify
 * CLI on $PATH and its one-time `spicetify backup apply` already done by the
 * user; dankc never runs that setup step itself. */
void dc_systheme_apply_spicetify(bool light);

#endif /* DC_SERVICES_SYSTHEME_MISC_H */
