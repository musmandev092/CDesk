/* systheme_term.c — system-wide theming Task 2: the alacritty/kitty/foot
 * terminal emitters (see systheme_term.h). Reuses every atomic-write /
 * backup+marker / dryrun / detection / spawn primitive from
 * systheme_internal.h (Task 1) instead of re-deriving them; see systheme.c's
 * file header for the full safety contract these helpers enforce (opt-in
 * only, dankc-owned files written atomically, user-owned files only ever
 * nudged with a one-time backup, DANKC_THEME_DRYRUN gates every write/spawn).
 *
 * ANSI-16 palette: dc_dynamic_ansi16() (theme/dynamic.cpp/.h) derives the 16
 * terminal colours from dc_theme_current's primary hue -- see that header's
 * comment for the derivation (canonical hue anchors, slight MCU-style
 * harmonize toward the primary hue, tone/chroma tuned to DMS's own
 * matugen-generated reference terminal palette).
 *
 * Non-ANSI role mapping (background/foreground/cursor/selection) was
 * reverse-engineered against DMS's actual matugen output,
 * ~/.config/alacritty/dank-theme.toml for the stock "green" theme, and
 * matches it exactly, role for role:
 *   background          <- surface
 *   foreground           <- surface_text
 *   cursor "text"/fg      <- surface   (cursor's own text colour)
 *   cursor "cursor"/bg    <- primary   (the cursor block itself)
 *   selection foreground <- surface_text
 *   selection background <- primary_container
 *
 * Known gap (documented, same spirit as systheme.c's GTK on_error caveat):
 * the "ensure it's included" step for alacritty.toml/kitty.conf/foot.ini
 * uses systheme_internal.h's generic marker-line injector, which is a plain
 * text appender, not a TOML/INI-aware array/section merge. If the user's
 * file doesn't yet reference dank-theme.*, dankc appends a *new*
 * `[general]`/`[main]` block with the include -- correct TOML/INI (re-opening
 * a table to add a not-yet-set key is legal in both formats) as long as that
 * table doesn't already declare the same key elsewhere in the file. A config
 * that already has its own `general.import`/`[main] include=` entry (for a
 * *different* file) keeps that entry untouched and gets a second table
 * appended for dankc's own line, which every app tested here parses fine,
 * but a hand-rolled TOML/INI parser more strict than alacritty/foot's own
 * could reject the redefinition -- not reachable by anything in this repo,
 * flagged here for whoever revisits this later.
 */
#include "services/systheme_term.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>

/* --- shared path helpers ------------------------------------------------ */

/* Resolve $XDG_CONFIG_HOME, falling back to "$HOME/.config"; false if
 * neither is set. Mirrors systheme.c's own file-local config_home() (not
 * exported via systheme_internal.h, so each emitter file keeps its own tiny
 * copy -- same precedent as systheme.c's gtk_dir()). */
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

/* --- ANSI-16 hex table --------------------------------------------------- */

static const char *const kAnsiNames[8] = {"black", "red", "green", "yellow",
        "blue", "magenta", "cyan", "white"};

static void hex_ansi16(bool light, char out[16][8])
{
    dc_color ansi[16];
    dc_dynamic_ansi16(dc_theme_current, light, ansi);
    for (int i = 0; i < 16; i++)
        dc_systheme_hex_rgb(ansi[i], out[i]);
}

/* --- alacritty ------------------------------------------------------------ */

#define DC_SYSTHEME_ALACRITTY_MARKER "# Added by DankC Settings > Theme & Colors"

static char *build_alacritty_toml(bool light, size_t *out_len)
{
    char surface[8], surface_text[8], primary[8], primary_container[8];
    char ansi[16][8];

    dc_systheme_hex_rgb(dc_theme_current->surface, surface);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, surface_text);
    dc_systheme_hex_rgb(dc_theme_current->primary, primary);
    dc_systheme_hex_rgb(dc_theme_current->primary_container, primary_container);
    hex_ansi16(light, ansi);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fprintf(f, "[colors.primary]\n");
    fprintf(f, "background = '%s'\n", surface);
    fprintf(f, "foreground = '%s'\n\n", surface_text);

    fprintf(f, "[colors.selection]\n");
    fprintf(f, "text = '%s'\n", surface_text);
    fprintf(f, "background = '%s'\n\n", primary_container);

    fprintf(f, "[colors.cursor]\n");
    fprintf(f, "text = '%s'\n", surface);
    fprintf(f, "cursor = '%s'\n\n", primary);

    fprintf(f, "[colors.normal]\n");
    for (int i = 0; i < 8; i++)
        fprintf(f, "%-7s = '%s'\n", kAnsiNames[i], ansi[i]);
    fprintf(f, "\n[colors.bright]\n");
    for (int i = 0; i < 8; i++)
        fprintf(f, "%-7s = '%s'\n", kAnsiNames[i], ansi[8 + i]);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_alacritty(bool light)
{
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("alacritty", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping alacritty");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping alacritty", dir);
        return;
    }

    size_t len = 0;
    char *toml = build_alacritty_toml(light, &len);
    if (!toml) {
        dc_warn("systheme: failed to build alacritty dank-theme.toml");
        return;
    }

    char theme_path[DC_SYSTHEME_PATH_MAX + 32];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(theme_path, sizeof(theme_path), "%s/dank-theme.toml", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/alacritty.toml", dir);

    dc_systheme_write_owned(theme_path, toml, len);
    free(toml);

    char import_line[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(import_line, sizeof(import_line), "[general]\nimport = [\"%s\"]", theme_path);
    dc_systheme_ensure_line(cfg_path, "dank-theme.toml", DC_SYSTHEME_ALACRITTY_MARKER,
            import_line);

    /* alacritty live-reloads both its own config and any imported files
     * on change -- no reload spawn needed. */
    dc_info("systheme: alacritty theme written to %s", dir);
}

/* --- kitty ------------------------------------------------------------- */

#define DC_SYSTHEME_KITTY_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_SYSTHEME_KITTY_INCLUDE_LINE "include dank-theme.conf"

static char *build_kitty_conf(bool light, size_t *out_len)
{
    char surface[8], surface_text[8], primary[8], primary_container[8];
    char ansi[16][8];

    dc_systheme_hex_rgb(dc_theme_current->surface, surface);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, surface_text);
    dc_systheme_hex_rgb(dc_theme_current->primary, primary);
    dc_systheme_hex_rgb(dc_theme_current->primary_container, primary_container);
    hex_ansi16(light, ansi);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);

    fprintf(f, "foreground %s\n", surface_text);
    fprintf(f, "background %s\n", surface);
    fprintf(f, "cursor %s\n", primary);
    fprintf(f, "cursor_text_color %s\n", surface);
    fprintf(f, "selection_foreground %s\n", surface_text);
    fprintf(f, "selection_background %s\n\n", primary_container);

    for (int i = 0; i < 16; i++)
        fprintf(f, "color%-2d %s\n", i, ansi[i]);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_kitty(bool light)
{
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("kitty", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping kitty");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping kitty", dir);
        return;
    }

    size_t len = 0;
    char *conf = build_kitty_conf(light, &len);
    if (!conf) {
        dc_warn("systheme: failed to build kitty dank-theme.conf");
        return;
    }

    char theme_path[DC_SYSTHEME_PATH_MAX + 32];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(theme_path, sizeof(theme_path), "%s/dank-theme.conf", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/kitty.conf", dir);

    dc_systheme_write_owned(theme_path, conf, len);
    free(conf);

    dc_systheme_ensure_line(cfg_path, DC_SYSTHEME_KITTY_INCLUDE_LINE, DC_SYSTHEME_KITTY_MARKER,
            DC_SYSTHEME_KITTY_INCLUDE_LINE);

    /* Nudge already-running kitty instances (kitty's documented reload
     * mechanism); best-effort, matches gtk-reload's fire-and-forget style. */
    const char *argv[] = {"pkill", "-USR1", "kitty", NULL};
    dc_systheme_spawn("kitty-reload", argv, 3);

    dc_info("systheme: kitty theme written to %s", dir);
}

/* --- foot ---------------------------------------------------------------- */

#define DC_SYSTHEME_FOOT_MARKER "# Added by DankC Settings > Theme & Colors"

/* "rrggbb" (no leading '#'), foot's expected format -- dc_systheme_hex_rgb()
 * always yields "#rrggbb" (7 chars + NUL), so skip its first byte. */
static const char *no_hash(const char hex[8])
{
    return hex + 1;
}

static char *build_foot_ini(bool light, size_t *out_len)
{
    char surface[8], surface_text[8], primary_container[8];
    char ansi[16][8];

    dc_systheme_hex_rgb(dc_theme_current->surface, surface);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, surface_text);
    dc_systheme_hex_rgb(dc_theme_current->primary_container, primary_container);
    hex_ansi16(light, ansi);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fputs("[colors]\n", f);
    fprintf(f, "foreground=%s\n", no_hash(surface_text));
    fprintf(f, "background=%s\n", no_hash(surface));
    fprintf(f, "selection-foreground=%s\n", no_hash(surface_text));
    fprintf(f, "selection-background=%s\n", no_hash(primary_container));
    for (int i = 0; i < 8; i++)
        fprintf(f, "regular%d=%s\n", i, no_hash(ansi[i]));
    for (int i = 0; i < 8; i++)
        fprintf(f, "bright%d=%s\n", i, no_hash(ansi[8 + i]));

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_foot(bool light)
{
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("foot", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping foot");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping foot", dir);
        return;
    }

    size_t len = 0;
    char *ini = build_foot_ini(light, &len);
    if (!ini) {
        dc_warn("systheme: failed to build foot dank-theme.ini");
        return;
    }

    char theme_path[DC_SYSTHEME_PATH_MAX + 32];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(theme_path, sizeof(theme_path), "%s/dank-theme.ini", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/foot.ini", dir);

    dc_systheme_write_owned(theme_path, ini, len);
    free(ini);

    char include_line[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(include_line, sizeof(include_line), "[main]\ninclude=%s", theme_path);
    dc_systheme_ensure_line(cfg_path, "dank-theme.ini", DC_SYSTHEME_FOOT_MARKER, include_line);

    /* foot only reads its config (and includes) at startup -- restart-only,
     * no reload spawn. */
    dc_info("systheme: foot theme written to %s", dir);
}
