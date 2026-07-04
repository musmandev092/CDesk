/* systheme_notify.c — system-wide theming Task 3 (wave 2): the desktop
 * notification-daemon emitters -- mako, dunst, swaync (see
 * systheme_notify.h). Reuses systheme_internal.h's atomic-write /
 * marker+backup / ensure_line_top / dryrun / dir-exists / spawn primitives
 * exactly like Task 1/2's systheme_term.c and systheme_apps.c; see
 * systheme.c's file header for the full safety contract (opt-in only,
 * dankc-owned files written atomically, user-owned files only ever nudged
 * with a one-time backup, DANKC_THEME_DRYRUN gates every write/spawn, an
 * app's config dir is never created by dankc -- dunst's drop-in subdir is
 * the one narrow, documented exception below).
 *
 * Palette mapping (shorthand per docs/21-THEMING-COVERAGE-PLAN.md):
 *   panel  <- surface_container
 *   fg     <- surface_text
 *   accent <- primary
 *   error  <- error (+ a synthesized contrasting on-error text/foreground,
 *             same black/white relative-luminance pick systheme.c's GTK
 *             emitter already uses for its own destructive/error text role
 *             -- dc_theme has no dedicated on_error swatch to read instead)
 *
 * --- mako ----------------------------------------------------------------
 *
 * mako's config format is flat `key=value` lines with optional `[criteria]`
 * blocks that override those keys when a notification matches the criteria
 * (e.g. `[urgency=critical]`). dankc owns a whole separate file,
 * ~/.config/mako/dank-colors, and gets it read via a top-level `include=`
 * directive rather than trying to merge into the user's own config: mako
 * has no "reset to defaults" between criteria blocks, so if the include
 * line landed *after* some user-authored `[criteria]` block, dank-colors'
 * top-level keys would end up scoped to (and overridden by) that block
 * instead of applying globally, and its own `[urgency=critical]` block
 * could end up nested inside an unrelated criteria block instead of being
 * its own top-level section. Putting the `include=` as the very FIRST line
 * (dc_systheme_ensure_line_top()) sidesteps both failure modes since
 * everything dankc defines is fully parsed and top-level before the user's
 * own config contributes a single byte.
 *
 * --- dunst -----------------------------------------------------------------
 *
 * dunst (post-1.5, libconfuse-based dunstrc) supports a `dunstrc.d/`
 * drop-in directory next to dunstrc: every `*.conf` inside it is parsed
 * after the main file, later files winning on any repeated key. That makes
 * a pure drop-in strictly better than the marker-injector approach used
 * everywhere else in systheme*.c: dankc never has to read, parse, or back
 * up the user's own dunstrc at all, so a hand-tuned dunstrc.d/00-*.conf
 * cannot be clobbered and there's zero risk of misplacing dankc's own
 * `[urgency_*]` overrides inside the wrong criteria/rule scope (dunst's
 * urgency sections aren't nestable the way mako's criteria are, so this
 * risk doesn't actually apply here, but the drop-in is still simpler and
 * matches the plan doc's explicit instruction). This is the ONE exception
 * in all of systheme*.c to "never create a directory the app itself didn't
 * already create": dc_systheme_apply_dunst() below only ever mkdir()s
 * dunstrc.d/ *inside* an ~/.config/dunst that dc_systheme_dir_exists()
 * already confirmed is real (i.e. dunst is genuinely installed/configured
 * here) -- it never creates ~/.config/dunst itself.
 *
 * --- swaync ----------------------------------------------------------------
 *
 * swaync's own packaged binary ships a full default style.css compiled in
 * and only reads a user ~/.config/swaync/style.css if one exists on disk at
 * all -- so, unlike every other "ensure an @import" case in systheme*.c,
 * dankc creating a *bare* style.css out of nothing would silently replace
 * swaync's entire built-in stylesheet with just dankc's one @import,
 * losing all of swaync's own default layout/spacing/animation rules (not a
 * risk for e.g. wofi/rofi, whose @import targets are additive on top of an
 * otherwise-empty default). So dank-colors.css is always written
 * unconditionally (it's fully dankc-owned and harmless sitting unused on
 * disk), but the `@import` is only ensured at the top of style.css when
 * that file already exists; if it doesn't, this is a silent no-op for the
 * import step (a later settings-UI task hints the user to create style.css
 * once, matching wezterm/neovim's own "manual one-line activation"
 * caveat in the coverage plan doc).
 */
#include "services/systheme_notify.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/theme.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* --- shared path helper --------------------------------------------------
 * Resolve $XDG_CONFIG_HOME, falling back to "$HOME/.config"; false if
 * neither is set. Same tiny file-local copy every systheme_*.c Task file
 * keeps (see systheme_term.c's own config_home() comment for why it isn't
 * hoisted into systheme_internal.h). */
static bool config_home(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, cap, "%s", xdg);
        return true;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, cap, "%s/.config", home);
        return true;
    }
    return false;
}

static bool app_dir(const char *name, char *out, size_t cap)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base)))
        return false;
    snprintf(out, cap, "%s/%s", base, name);
    return true;
}

/* --- mako ----------------------------------------------------------------- */

#define DC_SYSTHEME_MAKO_MARKER "# Added by DankC Settings > Theme & Colors"

static char *build_mako_colors(size_t *out_len)
{
    char panel[8], fg[8], accent[8], err[8];
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->error, err);
    const char *err_fg = dc_systheme_prefers_black_text(dc_theme_current->error) ? "#000000"
                                                                                  : "#ffffff";

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fprintf(f, "background-color=%s\n", panel);
    fprintf(f, "text-color=%s\n", fg);
    fprintf(f, "border-color=%s\n", accent);
    fprintf(f, "progress-color=%s\n\n", accent);

    fputs("[urgency=critical]\n", f);
    fprintf(f, "background-color=%s\n", err);
    fprintf(f, "text-color=%s\n", err_fg);
    fprintf(f, "border-color=%s\n", err);
    fprintf(f, "progress-color=%s\n", err);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_mako(bool light)
{
    (void)light; /* mako's palette here has no distinct light-mode role mapping
                  * of its own -- dc_theme_current already reflects the active
                  * light/dark palette by the time dc_systheme_apply() calls
                  * this (see systheme.h's dc_systheme_apply() contract). */
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("mako", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping mako");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping mako", dir);
        return;
    }

    size_t len = 0;
    char *colors = build_mako_colors(&len);
    if (!colors) {
        dc_warn("systheme: failed to build mako dank-colors");
        return;
    }

    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/config", dir);

    dc_systheme_write_owned(colors_path, colors, len);
    free(colors);

    char include_line[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(include_line, sizeof(include_line), "include=%s", colors_path);
    dc_systheme_ensure_line_top(cfg_path, include_line, DC_SYSTHEME_MAKO_MARKER);

    const char *argv[] = {"makoctl", "reload", NULL};
    dc_systheme_spawn("mako-reload", argv, 2);

    dc_info("systheme: mako theme written to %s", dir);
}

/* --- dunst ----------------------------------------------------------------- */

#define DC_SYSTHEME_DUNST_DROPIN_NAME "99-dank-colors.conf"

static char *build_dunst_dropin(size_t *out_len)
{
    char panel[8], fg[8], accent[8], err[8];
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->error, err);
    const char *err_fg = dc_systheme_prefers_black_text(dc_theme_current->error) ? "#000000"
                                                                                  : "#ffffff";

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n", f);
    fputs("# Pure drop-in: dunst reads every *.conf under dunstrc.d/ after\n", f);
    fputs("# dunstrc itself, later files winning key conflicts -- dankc never\n", f);
    fputs("# edits dunstrc directly, so nothing here needs a backup.\n\n", f);

    fputs("[global]\n", f);
    fprintf(f, "    frame_color = \"%s\"\n\n", accent);

    fputs("[urgency_low]\n", f);
    fprintf(f, "    background = \"%s\"\n", panel);
    fprintf(f, "    foreground = \"%s\"\n", fg);
    fprintf(f, "    frame_color = \"%s\"\n\n", accent);

    fputs("[urgency_normal]\n", f);
    fprintf(f, "    background = \"%s\"\n", panel);
    fprintf(f, "    foreground = \"%s\"\n", fg);
    fprintf(f, "    frame_color = \"%s\"\n\n", accent);

    fputs("[urgency_critical]\n", f);
    fprintf(f, "    background = \"%s\"\n", err);
    fprintf(f, "    foreground = \"%s\"\n", err_fg);
    fprintf(f, "    frame_color = \"%s\"\n", err);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_dunst(bool light)
{
    (void)light; /* see dc_systheme_apply_mako()'s identical comment. */
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("dunst", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping dunst");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping dunst", dir);
        return;
    }

    /* Narrow, documented exception to "dankc never creates an app's config
     * dir": dunstrc.d/ is created ONLY inside the ~/.config/dunst just
     * confirmed to exist above -- never ~/.config/dunst itself. dunst has
     * no include directive dankc could inject into a user-owned dunstrc, so
     * the drop-in dir is the only way to deliver a pure, ownership-clean
     * theme file (see this file's header comment). */
    char dropin_dir[DC_SYSTHEME_PATH_MAX + 16];
    snprintf(dropin_dir, sizeof(dropin_dir), "%s/dunstrc.d", dir);
    if (!dc_systheme_dir_exists(dropin_dir)) {
        if (dc_systheme_dryrun()) {
            dc_info("[DRYRUN] systheme: would mkdir %s", dropin_dir);
        } else if (mkdir(dropin_dir, 0755) != 0 && errno != EEXIST) {
            dc_warn("systheme: could not create %s, skipping dunst", dropin_dir);
            return;
        }
    }

    size_t len = 0;
    char *conf = build_dunst_dropin(&len);
    if (!conf) {
        dc_warn("systheme: failed to build dunst drop-in");
        return;
    }

    char dropin_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(dropin_path, sizeof(dropin_path), "%s/" DC_SYSTHEME_DUNST_DROPIN_NAME, dropin_dir);
    dc_systheme_write_owned(dropin_path, conf, len);
    free(conf);

    const char *argv[] = {"dunstctl", "reload", NULL};
    dc_systheme_spawn("dunst-reload", argv, 2);

    dc_info("systheme: dunst theme written to %s", dropin_path);
}

/* --- swaync ----------------------------------------------------------------- */

#define DC_SYSTHEME_SWAYNC_MARKER "/* Added by DankC Settings > Theme & Colors */"

static char *build_swaync_css(size_t *out_len)
{
    char panel[8], fg[8], accent[8], err[8];
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->error, err);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("/* Generated by DankC's system theming engine (Settings > Theme & Colors) */\n\n", f);
    fprintf(f, "@define-color noti-bg %s;\n", panel);
    fprintf(f, "@define-color cc-bg %s;\n", panel);
    fprintf(f, "@define-color noti-fg %s;\n", fg);
    fprintf(f, "@define-color noti-border-color %s;\n\n", accent);

    fputs("/* Best-effort critical-urgency override: swaync's own default\n", f);
    fputs(" * stylesheet doesn't expose a separate @define-color for critical\n", f);
    fputs(" * notifications across all shipped versions, so this targets its\n", f);
    fputs(" * `.critical` class directly instead of defining an unused variable. */\n", f);
    fprintf(f, ".notification.critical, .notification-row .critical {\n"
               "    background: %s;\n"
               "    border-color: %s;\n"
               "}\n",
            err, err);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_swaync(bool light)
{
    (void)light; /* see dc_systheme_apply_mako()'s identical comment. */
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("swaync", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping swaync");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping swaync", dir);
        return;
    }

    size_t len = 0;
    char *css = build_swaync_css(&len);
    if (!css) {
        dc_warn("systheme: failed to build swaync dank-colors.css");
        return;
    }

    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    char style_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.css", dir);
    snprintf(style_path, sizeof(style_path), "%s/style.css", dir);

    dc_systheme_write_owned(colors_path, css, len);
    free(css);

    /* Only nudge style.css if the user already has one -- see this file's
     * header comment on why dankc must never create a bare style.css out of
     * nothing (that would drop swaync's packaged default stylesheet). */
    if (dc_systheme_dir_exists(style_path)) {
        dc_systheme_ensure_line_top(style_path, "@import 'dank-colors.css';",
                DC_SYSTHEME_SWAYNC_MARKER);
    } else {
        dc_info("systheme: %s has no style.css yet, wrote dank-colors.css only "
                "(add \"@import 'dank-colors.css';\" to style.css to enable it)",
                dir);
    }

    const char *argv[] = {"swaync-client", "--reload-css", NULL};
    dc_systheme_spawn("swaync-reload", argv, 2);

    dc_info("systheme: swaync theme written to %s", dir);
}
