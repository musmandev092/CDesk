/* systheme_qtkde.c — system-wide theming Task T5 (Tier-2 coverage pass, see
 * docs/21-THEMING-COVERAGE-PLAN.md): Kvantum, the KDE/Plasma kdeglobals
 * colour scheme, and GTK2. Reuses every atomic-write / marker+backup /
 * dryrun / detection / spawn primitive from systheme_internal.h exactly
 * like every other systheme_*.c (see systheme.c's file header for the full
 * safety contract: opt-in only, dankc-owned files written atomically,
 * user-owned files only ever nudged with a one-time backup, DANKC_THEME_DRYRUN
 * gates every write/spawn, an app's own config dir is never created by
 * dankc).
 *
 * --- Kvantum -----------------------------------------------------------
 *
 * Kvantum is an SVG-based Qt5/Qt6 style engine. A theme lives in its own
 * directory, ~/.config/Kvantum/<Name>/, and is made of two dankc-owned
 * files: <Name>.kvconfig (a QSettings-style INI: `[%General]` for a small,
 * safe subset of widget-behavior knobs, `[GeneralColors]` for the actual
 * palette -- window.color/base.color/button.color/highlight.color/
 * text.color/etc., all `#rrggbb`) and <Name>.svg (the theme's drawable
 * elements).
 *
 * *** Kvantum SVG design decision: MINIMAL, not full. ***
 * A real, hand-authored Kvantum theme SVG defines dozens of per-widget
 * group ids (PanelButtonCommand, LineEdit, Tab, MenuItem, Slider,
 * ScrollBar, CheckBox, RadioButton, ProgressBar, ...), each with several
 * sub-states (normal/toggled/pressed/disabled/focused) -- hundreds of
 * individually-drawn elements, verified against Kvantum's own internal
 * naming/fallback rules. Hand-rolling that whole vocabulary here risks
 * shipping a *subtly wrong* theme SVG (a misnamed group silently never
 * matches anything) which is worse than shipping none, and there is no
 * cheap way to validate correctness against Kvantum's actual renderer from
 * this codebase. Kvantum is documented to degrade gracefully when it can't
 * find some requested widget-state element in the theme SVG: colors "will
 * be taken from the currently used color palette" (i.e. [GeneralColors] in
 * the companion .kvconfig) rather than the theme failing to load or
 * rendering blank/broken. So DankC.svg here is a deliberately MINIMAL, but
 * fully well-formed, Kvantum theme SVG with no widget-specific drawable
 * groups at all -- Kvantum ends up flat-rendering every widget straight
 * from DankC.kvconfig's [GeneralColors], which is exactly where this
 * emitter's whole palette already lives. Trade: no hand-tuned per-widget
 * bevels/gradients/rounding a bespoke theme SVG would give; win:
 * guaranteed-parseable, guaranteed-correct color application with zero risk
 * of a malformed or mismatched full SVG. Correctness over completeness, per
 * this task's brief.
 *
 * `theme=DankC` is then ensured under a `[General]` block in the *global*
 * ~/.config/Kvantum/kvantum.kvconfig (user-owned, marker+backup, same
 * repeated-INI-section tolerance as every other emitter's ensure_line()
 * patch -- QSettings' ini backend merges same-named sections across the
 * file, last key wins). `kvantummanager --set DankC` is spawned if it's on
 * $PATH (best-effort; it flips the same theme= pointer via Kvantum's own
 * tool, which some desktop integrations additionally hook to notify running
 * apps -- not relied upon here). restart-only: Kvantum/Qt apps read their
 * style once at startup.
 *
 * --- KDE / Plasma color scheme ------------------------------------------
 *
 * ~/.local/share/color-schemes/DankC.colors is a dankc-owned KConfig INI
 * in KDE's native colour-scheme format: `[Colors:Window]`/`[Colors:View]`/
 * `[Colors:Button]`/`[Colors:Selection]`/`[Colors:Tooltip]`, each with
 * BackgroundNormal/ForegroundNormal/DecorationFocus/DecorationHover (plus
 * ForegroundNegative/ForegroundPositive/ForegroundNeutral mapped from
 * dc_theme's error/success/warning), all as decimal `r,g,b` triplets (KDE's
 * native format -- not hex). Plus a `[General]` block (Name=/ColorScheme=)
 * and a `[WM]` block (window-decoration active/inactive backgrounds).
 *
 * `ColorScheme=DankC` is then nudged under a `[General]` block of
 * ~/.config/kdeglobals (user-owned, marker+backup) -- ONLY if
 * $XDG_CONFIG_HOME (or kdeglobals itself) already exists, never
 * manufactured from nothing (this emitter's one deviation from "just call
 * ensure_line() unconditionally" every other emitter here uses, because
 * kdeglobals is a much more central/sensitive file than e.g. a per-app rc).
 * KConfig's ini parser tolerates a re-opened `[General]` section the same
 * way QSettings does (last occurrence of a duplicate key wins) -- documented
 * here rather than trying to parse/merge the existing [General] block in
 * place, which would require a real KConfig-aware INI merge this emitter
 * deliberately doesn't attempt (same "textual append, not structural edit"
 * contract as every other ensure_line() user). `plasma-apply-colorscheme
 * DankC` is spawned if present (best-effort). KDE/Plasma apps watch
 * kdeglobals live via KConfigWatcher -- no restart needed for already-running
 * apps once the on-disk change lands.
 *
 * --- GTK2 -----------------------------------------------------------------
 *
 * ~/.gtkrc-2.0.dank is a dankc-owned GTK2 rc fragment: a `style "dank" {
 * ... }` block (bg/base/fg/text for NORMAL, plus bg[SELECTED]/
 * base[SELECTED] <- accent and fg[SELECTED]/text[SELECTED] <- the
 * contrasting text-on-accent tone) followed by `class "*" style "dank"` so
 * every widget picks it up. `include "<abs path>"` is then ensured in
 * ~/.gtkrc-2.0 (user-owned, marker+backup; created fresh if this machine
 * only has GTK2 detected via the `gimp` binary and no rc file exists yet --
 * same precedent as the GTK3/4 emitter creating gtk.css on a first run,
 * systheme.c's apply_gtk_variant()). restart-only: GTK2 has no live
 * theme-watch mechanism at all (unlike GTK3/4), so this never touches
 * already-running GTK2 apps.
 */
#include "services/systheme_qtkde.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- shared path helpers (each systheme_*.c keeps its own tiny copy, same
 * precedent as systheme_term.c/systheme_term2.c/systheme_apps.c) ----------- */

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

static bool data_home(char *out, size_t cap)
{
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, cap, "%s", xdg);
        return true;
    }
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, cap, "%s/.local/share", home);
        return true;
    }
    return false;
}

static void app_dir(const char *base, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", base, name);
}

/* mkdir() a single directory (no-op if it already exists); DRYRUN-gated.
 * Same shape as systheme_apps.c's/systheme_term2.c's ensure_dir() -- a real
 * failure just surfaces as a write failure from dc_systheme_write_owned()
 * right after, which already logs. */
static void ensure_dir(const char *dir)
{
    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would mkdir -p %s", dir);
        return;
    }
    mkdir(dir, 0755);
}

/* --- Kvantum ---------------------------------------------------------- */

#define DC_KVANTUM_MARKER "; Added by DankC Settings > Theme & Colors"

/* Deliberately minimal, well-formed Kvantum theme SVG: no widget-specific
 * drawable groups. See this file's header for why -- Kvantum falls back to
 * flat-rendering every widget from the companion .kvconfig's
 * [GeneralColors] palette when it can't find a requested element, so this
 * is a correct (if visually plain) theme rather than a risky hand-rolled
 * approximation of Kvantum's full per-widget vocabulary. Palette-
 * independent (no hex substitution needed) since it carries no color
 * information of its own. */
static const char DC_KVANTUM_SVG[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
        "<!--\n"
        "  DankC (Kvantum theme): generated by DankC's system theming engine.\n"
        "\n"
        "  This is a deliberately MINIMAL Kvantum theme SVG (no per-widget\n"
        "  drawable groups): Kvantum falls back to a flat rendering sourced\n"
        "  from this theme's DankC.kvconfig [GeneralColors] palette whenever a\n"
        "  requested widget-state element isn't found here, rather than\n"
        "  failing to load or rendering blank. See systheme_qtkde.c's file\n"
        "  header for the full design writeup.\n"
        "-->\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"1000\" "
        "viewBox=\"0 0 1000 1000\"></svg>\n";

/* [%General]: a small, conservative subset of Kvantum's widget-behavior
 * knobs (every key not set here just falls back to Kvantum's own built-in
 * default, same as any minimal community theme's kvconfig). [GeneralColors]
 * is the actual palette, built from dc_theme_current with the same
 * bg/fg/accent/sel-bg/panel/border role vocabulary as this task's brief,
 * widened where dc_theme has a more specific field to draw from (e.g.
 * primary_text for the contrasting text atop an accent-colored surface). */
static char *build_kvantum_kvconfig(size_t *out_len)
{
    char bg[8], fg[8], accent[8], accent_text[8], sel_bg[8];
    char panel[8], panel_low[8], panel_high[8], border[8], variant_text[8];

    dc_systheme_hex_rgb(dc_theme_current->surface, bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, accent_text);
    dc_systheme_hex_rgb(dc_theme_current->primary_container, sel_bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_container_low, panel_low);
    dc_systheme_hex_rgb(dc_theme_current->surface_container_high, panel_high);
    dc_systheme_hex_rgb(dc_theme_current->outline, border);
    dc_systheme_hex_rgb(dc_theme_current->surface_variant_text, variant_text);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("; Generated by DankC's system theming engine (Settings > Theme & Colors)\n", f);
    fputs("[%General]\n", f);
    fputs("author=DankC\n", f);
    fputs("comment=Generated from DankC's live palette\n", f);
    fputs("x11drag=all\n", f);
    fputs("alt_mnemonic=true\n", f);
    fputs("left_tabs=false\n", f);
    fputs("translucent_windows=false\n", f);
    fputs("blurring=false\n", f);
    fputs("animate_states=false\n", f);
    fputs("composite=true\n", f);
    fputs("\n[GeneralColors]\n", f);
    fprintf(f, "window.color=%s\n", bg);
    fprintf(f, "inactive.window.color=%s\n", bg);
    fprintf(f, "window.text.color=%s\n", fg);
    fprintf(f, "base.color=%s\n", bg);
    fprintf(f, "inactive.base.color=%s\n", bg);
    fprintf(f, "alt.base.color=%s\n", panel_low);
    fprintf(f, "button.color=%s\n", panel);
    fprintf(f, "button.text.color=%s\n", fg);
    fprintf(f, "light.color=%s\n", panel_high);
    fprintf(f, "mid.light.color=%s\n", panel);
    fprintf(f, "mid.color=%s\n", border);
    fprintf(f, "dark.color=%s\n", border);
    fputs("shadow.color=black\n", f);
    fprintf(f, "highlight.color=%s\n", sel_bg);
    fprintf(f, "inactive.highlight.color=%s\n", sel_bg);
    fprintf(f, "highlight.text.color=%s\n", accent_text);
    fprintf(f, "tooltip.base.color=%s\n", panel_high);
    fprintf(f, "tooltip.text.color=%s\n", fg);
    fprintf(f, "disabled.text.color=%s\n", variant_text);
    fprintf(f, "link.color=%s\n", accent);
    fprintf(f, "link.visited.color=%s\n", accent);
    fprintf(f, "progress.indicator.text.color=%s\n", accent_text);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_kvantum(bool light)
{
    (void)light; /* Kvantum's palette has no distinct light/dark tuning beyond
                   * what's already baked into dc_theme_current -- matches
                   * the qt5ct/qt6ct emitter (systheme_apps.c). */
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping kvantum");
        return;
    }

    char kv_dir[DC_SYSTHEME_PATH_MAX + 32];
    app_dir(base, "Kvantum", kv_dir, sizeof(kv_dir));
    if (!dc_systheme_dir_exists(kv_dir)) {
        /* Never create Kvantum's own top-level config dir -- if it doesn't
         * exist, Kvantum itself just isn't in use on this machine, same
         * contract as every other emitter's "app dir doesn't exist" skip. */
        dc_info("systheme: %s does not exist, skipping kvantum", kv_dir);
        return;
    }

    char theme_dir[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(theme_dir, sizeof(theme_dir), "%s/DankC", kv_dir);
    /* DankC/ is dankc's own per-theme subdirectory of Kvantum's own config
     * dir (just confirmed to exist above) -- not "creating Kvantum's config
     * dir", same precedent as qt5ct/qt6ct's colors/ subdirectory. */
    ensure_dir(theme_dir);

    size_t len = 0;
    char *kvconfig = build_kvantum_kvconfig(&len);
    if (!kvconfig) {
        dc_warn("systheme: failed to build DankC.kvconfig");
        return;
    }
    char kvconfig_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(kvconfig_path, sizeof(kvconfig_path), "%s/DankC.kvconfig", theme_dir);
    dc_systheme_write_owned(kvconfig_path, kvconfig, len);
    free(kvconfig);

    char svg_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(svg_path, sizeof(svg_path), "%s/DankC.svg", theme_dir);
    dc_systheme_write_owned(svg_path, DC_KVANTUM_SVG, strlen(DC_KVANTUM_SVG));

    char kvantum_kvconfig[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(kvantum_kvconfig, sizeof(kvantum_kvconfig), "%s/kvantum.kvconfig", kv_dir);
    dc_systheme_ensure_line(kvantum_kvconfig, "theme=DankC", DC_KVANTUM_MARKER,
            "[General]\ntheme=DankC");

    if (dc_systheme_on_path("kvantummanager")) {
        const char *argv[] = {"kvantummanager", "--set", "DankC", NULL};
        dc_systheme_spawn("kvantum-set", argv, 3);
    }

    /* Kvantum/Qt apps read their style once at startup -- restart-only. */
    dc_info("systheme: kvantum theme written to %s", theme_dir);
}

/* --- KDE / Plasma color scheme ------------------------------------------ */

#define DC_KDEGLOBALS_MARKER "# Added by DankC Settings > Theme & Colors"

/* "r,g,b" decimal triplet, KDE's native color-scheme format (unlike every
 * hex-based emitter elsewhere in the systheme_* family). */
static void decimal_rgb(dc_color c, char out[16])
{
    snprintf(out, 16, "%d,%d,%d", c.r, c.g, c.b);
}

static char *build_kde_colors(size_t *out_len)
{
    char win_bg[16], win_fg[16];
    char view_bg[16], view_fg[16];
    char btn_bg[16], btn_fg[16];
    char sel_bg[16], sel_fg[16];
    char tip_bg[16], tip_fg[16];
    char accent[16];
    char err[16], ok[16], warn[16];
    char variant_fg[16];

    decimal_rgb(dc_theme_current->surface, win_bg);
    decimal_rgb(dc_theme_current->surface_text, win_fg);
    decimal_rgb(dc_theme_current->surface_container_low, view_bg);
    decimal_rgb(dc_theme_current->surface_text, view_fg);
    decimal_rgb(dc_theme_current->surface_container, btn_bg);
    decimal_rgb(dc_theme_current->surface_text, btn_fg);
    decimal_rgb(dc_theme_current->primary, sel_bg);
    decimal_rgb(dc_theme_current->primary_text, sel_fg);
    decimal_rgb(dc_theme_current->surface_container_high, tip_bg);
    decimal_rgb(dc_theme_current->surface_text, tip_fg);
    decimal_rgb(dc_theme_current->primary, accent);
    decimal_rgb(dc_theme_current->error, err);
    decimal_rgb(dc_theme_current->success, ok);
    decimal_rgb(dc_theme_current->warning, warn);
    decimal_rgb(dc_theme_current->surface_variant_text, variant_fg);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n", f);

    fputs("\n[Colors:Window]\n", f);
    fprintf(f, "BackgroundNormal=%s\n", win_bg);
    fprintf(f, "ForegroundNormal=%s\n", win_fg);
    fprintf(f, "DecorationFocus=%s\n", accent);
    fprintf(f, "DecorationHover=%s\n", accent);
    fprintf(f, "ForegroundNegative=%s\n", err);
    fprintf(f, "ForegroundPositive=%s\n", ok);
    fprintf(f, "ForegroundNeutral=%s\n", warn);

    fputs("\n[Colors:View]\n", f);
    fprintf(f, "BackgroundNormal=%s\n", view_bg);
    fprintf(f, "ForegroundNormal=%s\n", view_fg);
    fprintf(f, "DecorationFocus=%s\n", accent);
    fprintf(f, "DecorationHover=%s\n", accent);
    fprintf(f, "ForegroundNegative=%s\n", err);
    fprintf(f, "ForegroundPositive=%s\n", ok);
    fprintf(f, "ForegroundNeutral=%s\n", warn);

    fputs("\n[Colors:Button]\n", f);
    fprintf(f, "BackgroundNormal=%s\n", btn_bg);
    fprintf(f, "ForegroundNormal=%s\n", btn_fg);
    fprintf(f, "DecorationFocus=%s\n", accent);
    fprintf(f, "DecorationHover=%s\n", accent);
    fprintf(f, "ForegroundNegative=%s\n", err);
    fprintf(f, "ForegroundPositive=%s\n", ok);
    fprintf(f, "ForegroundNeutral=%s\n", warn);

    fputs("\n[Colors:Selection]\n", f);
    fprintf(f, "BackgroundNormal=%s\n", sel_bg);
    fprintf(f, "ForegroundNormal=%s\n", sel_fg);
    fprintf(f, "DecorationFocus=%s\n", accent);
    fprintf(f, "DecorationHover=%s\n", accent);
    fprintf(f, "ForegroundNegative=%s\n", err);
    fprintf(f, "ForegroundPositive=%s\n", ok);
    fprintf(f, "ForegroundNeutral=%s\n", warn);

    fputs("\n[Colors:Tooltip]\n", f);
    fprintf(f, "BackgroundNormal=%s\n", tip_bg);
    fprintf(f, "ForegroundNormal=%s\n", tip_fg);
    fprintf(f, "DecorationFocus=%s\n", accent);
    fprintf(f, "DecorationHover=%s\n", accent);
    fprintf(f, "ForegroundNegative=%s\n", err);
    fprintf(f, "ForegroundPositive=%s\n", ok);
    fprintf(f, "ForegroundNeutral=%s\n", warn);

    fputs("\n[General]\n", f);
    fputs("Name=DankC\n", f);
    fputs("ColorScheme=DankC\n", f);

    fputs("\n[WM]\n", f);
    fprintf(f, "activeBackground=%s\n", win_bg);
    fprintf(f, "activeForeground=%s\n", win_fg);
    fprintf(f, "inactiveBackground=%s\n", win_bg);
    fprintf(f, "inactiveForeground=%s\n", variant_fg);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_kde(bool light)
{
    (void)light; /* No separate light/dark tuning beyond dc_theme_current
                   * itself, matching Kvantum/Qt above. */
    char data[DC_SYSTHEME_PATH_MAX];
    if (!data_home(data, sizeof(data))) {
        dc_warn("systheme: no $XDG_DATA_HOME/$HOME, skipping kde");
        return;
    }

    char schemes_dir[DC_SYSTHEME_PATH_MAX + 48];
    app_dir(data, "color-schemes", schemes_dir, sizeof(schemes_dir));
    /* Unlike most emitters, color-schemes/ isn't gated on some OTHER app's
     * own dir already existing first -- it's a standard XDG data
     * subdirectory any KDE/Plasma install expects to find its user color
     * schemes in, so it's created outright if $XDG_DATA_HOME/~/.local/share
     * itself resolves (mirrors dc_systheme_app_detected("kde") treating "the
     * color-schemes dir exists" as just one of three independent detection
     * signals, not a prerequisite gate). */
    ensure_dir(schemes_dir);

    size_t len = 0;
    char *colors = build_kde_colors(&len);
    if (!colors) {
        dc_warn("systheme: failed to build DankC.colors");
        return;
    }
    char colors_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(colors_path, sizeof(colors_path), "%s/DankC.colors", schemes_dir);
    dc_systheme_write_owned(colors_path, colors, len);
    free(colors);
    dc_info("systheme: kde color scheme written to %s", colors_path);

    char cfg_base[DC_SYSTHEME_PATH_MAX];
    char kdeglobals[DC_SYSTHEME_PATH_MAX + 32];
    if (!config_home(cfg_base, sizeof(cfg_base))) {
        dc_info("systheme: no $XDG_CONFIG_HOME/$HOME, wrote color scheme only -- select "
                "\"DankC\" manually in System Settings > Colors");
        return;
    }
    snprintf(kdeglobals, sizeof(kdeglobals), "%s/kdeglobals", cfg_base);

    /* Never manufacture $XDG_CONFIG_HOME out of thin air just to write
     * kdeglobals -- only nudge it if the config dir or kdeglobals itself
     * already exists (this emitter's one deviation from every other
     * emitter's unconditional ensure_line() call, since kdeglobals is a
     * much more central/sensitive file than a per-app rc). */
    if (!dc_systheme_dir_exists(cfg_base) && !dc_systheme_dir_exists(kdeglobals)) {
        dc_info("systheme: %s does not exist, wrote color scheme only -- select \"DankC\" "
                "manually once KDE/Plasma is configured",
                cfg_base);
        return;
    }

    dc_systheme_ensure_line(kdeglobals, "ColorScheme=DankC", DC_KDEGLOBALS_MARKER,
            "[General]\nColorScheme=DankC");

    if (dc_systheme_on_path("plasma-apply-colorscheme")) {
        const char *argv[] = {"plasma-apply-colorscheme", "DankC", NULL};
        dc_systheme_spawn("kde-colorscheme", argv, 2);
    }

    /* KDE/Plasma apps watch kdeglobals live via KConfigWatcher -- no
     * restart needed for already-running apps once the on-disk change
     * lands (plasma-apply-colorscheme, when present, is a faster/more
     * direct nudge than waiting on the watcher alone). */
    dc_info("systheme: kdeglobals nudged to use DankC");
}

/* --- GTK2 --------------------------------------------------------------- */

#define DC_GTK2_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_GTK2_RC_FILENAME ".gtkrc-2.0"
#define DC_GTK2_DANK_FILENAME ".gtkrc-2.0.dank"

static char *build_gtk2_rc(size_t *out_len)
{
    char bg[8], fg[8], accent[8], accent_text[8];

    dc_systheme_hex_rgb(dc_theme_current->surface, bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, accent_text);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fputs("style \"dank\"\n{\n", f);
    fprintf(f, "  bg[NORMAL]     = \"%s\"\n", bg);
    fprintf(f, "  bg[SELECTED]   = \"%s\"\n", accent);
    fprintf(f, "  base[NORMAL]   = \"%s\"\n", bg);
    fprintf(f, "  base[SELECTED] = \"%s\"\n", accent);
    fprintf(f, "  fg[NORMAL]     = \"%s\"\n", fg);
    fprintf(f, "  fg[SELECTED]   = \"%s\"\n", accent_text);
    fprintf(f, "  text[NORMAL]   = \"%s\"\n", fg);
    fprintf(f, "  text[SELECTED] = \"%s\"\n", accent_text);
    fputs("}\n\n", f);
    fputs("class \"*\" style \"dank\"\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_gtk2(bool light)
{
    (void)light; /* No separate light/dark tuning beyond dc_theme_current
                   * itself, matching the GTK3/4 emitter's own palette (only
                   * its gsettings reload-nudge cares about `light`). */
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("systheme: no $HOME, skipping gtk2");
        return;
    }

    size_t len = 0;
    char *rc = build_gtk2_rc(&len);
    if (!rc) {
        dc_warn("systheme: failed to build ~/.gtkrc-2.0.dank");
        return;
    }
    char dank_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(dank_path, sizeof(dank_path), "%s/%s", home, DC_GTK2_DANK_FILENAME);
    dc_systheme_write_owned(dank_path, rc, len);
    free(rc);

    char rc_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(rc_path, sizeof(rc_path), "%s/%s", home, DC_GTK2_RC_FILENAME);

    char include_line[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(include_line, sizeof(include_line), "include \"%s\"", dank_path);
    /* Needle = the absolute .gtkrc-2.0.dank path itself, same
     * "needle==the unique thing we just wrote" precedent as the xresources
     * emitter's #include patch (systheme_term2.c). */
    dc_systheme_ensure_line(rc_path, dank_path, DC_GTK2_MARKER, include_line);

    /* GTK2 has no live theme-watch mechanism at all (unlike GTK3/4) --
     * restart-only, no reload spawn. */
    dc_info("systheme: gtk2 theme written to %s", dank_path);
}
