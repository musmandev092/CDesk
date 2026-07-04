/* systheme_launchers.c — system-wide theming Task 2 (T2): the application
 * launcher / menu emitters (see systheme_launchers.h). Reuses
 * systheme_internal.h's atomic-write / marker+backup(-top) / dryrun /
 * dir-exists / detection primitives exactly like systheme_term.c's terminal
 * emitters and systheme_apps.c's VS Code/Qt emitters; see systheme.c's file
 * header for the full safety contract (opt-in only, dankc-owned files
 * written atomically, user-owned files only ever nudged with a one-time
 * backup, DANKC_THEME_DRYRUN gates every write, an app's config dir is never
 * created by dankc).
 *
 * All four launchers here re-read their config on every invocation (no
 * daemon to live-reload), so none of these functions ever call
 * dc_systheme_spawn() -- the next time the user opens the launcher, it just
 * picks up the new files.
 *
 * Palette mapping (shared informal vocabulary, same shorthand as
 * docs/21-THEMING-COVERAGE-PLAN.md): bg=surface, fg=surface_text,
 * accent=primary, on-accent=primary_text, sel-bg=primary_container,
 * panel=surface_container, border=outline, error=error.
 *
 * --- rofi -----------------------------------------------------------------
 * Owned dank-colors.rasi defines `dank-*` rasi variables plus sensible
 * top-level background-color/text-color defaults; config.rasi gets a plain
 * `@import "dank-colors.rasi"` appended (rofi's rasi cascade lets a later
 * `@import`'s property definitions win over earlier ones, so append-only,
 * same as dc_systheme_ensure_line(), is sufficient -- no need for
 * ensure_line_top here).
 *
 * --- wofi -----------------------------------------------------------------
 * Owned dank-colors.css is plain GTK CSS (window/#input/#entry/#text/
 * #entry:selected). style.css needs the `@import` to precede every other
 * rule (GTK CSS requires @import statements first), so this uses
 * dc_systheme_ensure_line_top() when style.css already exists. If it
 * doesn't exist yet (fresh wofi install with no user style), this writes a
 * *new* style.css directly via dc_systheme_write_owned() containing the
 * import plus a handful of minimal selectors wired to the dank palette --
 * ensure_line_top() alone would only produce the two-line
 * marker+import stub, leaving wofi's own chrome unstyled by default.
 *
 * --- fuzzel ---------------------------------------------------------------
 * Owned dank-colors.ini's [colors] values are 8-digit "rrggbbaa" with no
 * leading '#' (fuzzel's required format, alpha last) -- NOT the same as
 * dc_systheme_hex_argb()'s "#aarrggbb" (alpha first, Qt/kitty convention),
 * so this file has its own tiny rrggbbaa formatter. fuzzel.ini gets a plain
 * `include=<abs path>` line appended (fuzzel applies keys in document
 * order, later wins, so append-only is sufficient).
 *
 * --- tofi -----------------------------------------------------------------
 * Owned dank-colors uses tofi's plain "key = #RRGGBB" syntax. tofi's config
 * gets a plain `include=<abs path>` line appended (same later-wins,
 * append-only reasoning as fuzzel).
 */
#include "services/systheme_launchers.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- shared path helpers (each systheme_*.c keeps its own tiny copy, same
 * precedent as systheme_term.c's config_home()/app_dir() and
 * systheme_apps.c's config_home()) ------------------------------------------ */

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

static bool file_exists(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* --- rofi ------------------------------------------------------------------ */

#define DC_SYSTHEME_ROFI_MARKER "// Added by DankC Settings > Theme & Colors"
#define DC_SYSTHEME_ROFI_IMPORT_LINE "@import \"dank-colors.rasi\""

static char *build_rofi_rasi(size_t *out_len)
{
    char bg[8], fg[8], accent[8], sel_bg[8], panel[8], border[8], error_hex[8];

    dc_systheme_hex_rgb(dc_theme_current->surface, bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->primary_container, sel_bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->outline, border);
    dc_systheme_hex_rgb(dc_theme_current->error, error_hex);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("/*\n"
          " * Dank Colors\n"
          " * Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          " */\n\n",
            f);
    fputs("* {\n", f);
    fprintf(f, "    dank-bg:     %s;\n", bg);
    fprintf(f, "    dank-fg:     %s;\n", fg);
    fprintf(f, "    dank-accent: %s;\n", accent);
    fprintf(f, "    dank-sel-bg: %s;\n", sel_bg);
    fprintf(f, "    dank-panel:  %s;\n", panel);
    fprintf(f, "    dank-border: %s;\n", border);
    fprintf(f, "    dank-error:  %s;\n\n", error_hex);
    fputs("    background-color: @dank-bg;\n", f);
    fputs("    text-color:       @dank-fg;\n", f);
    fputs("}\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_rofi(bool light)
{
    (void)light; /* rofi has no distinct light/dark tuning beyond what's
                   * already baked into dc_theme_current, same as the Qt
                   * emitter. */
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("rofi", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping rofi");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping rofi", dir);
        return;
    }

    size_t len = 0;
    char *rasi = build_rofi_rasi(&len);
    if (!rasi) {
        dc_warn("systheme: failed to build rofi dank-colors.rasi");
        return;
    }

    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.rasi", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/config.rasi", dir);

    dc_systheme_write_owned(colors_path, rasi, len);
    free(rasi);

    dc_systheme_ensure_line(cfg_path, "dank-colors.rasi", DC_SYSTHEME_ROFI_MARKER,
            DC_SYSTHEME_ROFI_IMPORT_LINE);

    /* rofi re-reads its config on every invocation -- no reload spawn. */
    dc_info("systheme: rofi theme written to %s", dir);
}

/* --- wofi ------------------------------------------------------------------ */

#define DC_SYSTHEME_WOFI_MARKER "/* Added by DankC Settings > Theme & Colors */"
#define DC_SYSTHEME_WOFI_IMPORT_LINE "@import 'dank-colors.css';"

static char *build_wofi_colors_css(size_t *out_len)
{
    char panel[8], accent[8], on_accent[8], fg[8];

    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, on_accent);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("/*\n"
          " * Dank Colors\n"
          " * Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          " */\n\n",
            f);
    fputs("window {\n", f);
    fprintf(f, "    background-color: %s;\n", panel);
    fprintf(f, "    color: %s;\n", fg);
    fputs("}\n\n", f);

    fputs("#input {\n", f);
    fprintf(f, "    background-color: %s;\n", panel);
    fprintf(f, "    color: %s;\n", fg);
    fprintf(f, "    border: 1px solid %s;\n", accent);
    fputs("}\n\n", f);

    fputs("#entry {\n", f);
    fprintf(f, "    color: %s;\n", fg);
    fputs("}\n\n", f);

    fputs("#text {\n", f);
    fprintf(f, "    color: %s;\n", fg);
    fputs("}\n\n", f);

    fputs("#entry:selected {\n", f);
    fprintf(f, "    background-color: %s;\n", accent);
    fprintf(f, "    color: %s;\n", on_accent);
    fputs("}\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

/* Minimal-but-usable style.css for a machine that has wofi installed but has
 * never had one (no user rules to preserve/back up). Deliberately small --
 * just enough structure that the @import's #entry:selected/#input/window
 * rules above actually have something to attach to -- rather than trying to
 * replicate wofi's entire stock stylesheet. */
static char *build_wofi_default_style_css(size_t *out_len)
{
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fprintf(f, "%s\n%s\n\n", DC_SYSTHEME_WOFI_MARKER, DC_SYSTHEME_WOFI_IMPORT_LINE);
    fputs("#window {\n"
          "    margin: 0px;\n"
          "    border: 1px solid;\n"
          "}\n\n"
          "#outer-box {\n"
          "    margin: 4px;\n"
          "    padding: 4px;\n"
          "}\n\n"
          "#input {\n"
          "    margin: 4px;\n"
          "    padding: 4px;\n"
          "}\n\n"
          "#inner-box {\n"
          "    margin: 4px;\n"
          "}\n\n"
          "#entry {\n"
          "    padding: 4px;\n"
          "}\n",
            f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_wofi(bool light)
{
    (void)light;
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("wofi", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping wofi");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping wofi", dir);
        return;
    }

    size_t len = 0;
    char *css = build_wofi_colors_css(&len);
    if (!css) {
        dc_warn("systheme: failed to build wofi dank-colors.css");
        return;
    }

    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    char style_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.css", dir);
    snprintf(style_path, sizeof(style_path), "%s/style.css", dir);

    dc_systheme_write_owned(colors_path, css, len);
    free(css);

    if (file_exists(style_path)) {
        /* User already has a style.css: only ever prepend the @import,
         * never touch the rest (marker+backup, same idempotency contract as
         * every other ensure_line* caller). */
        dc_systheme_ensure_line_top(style_path, DC_SYSTHEME_WOFI_IMPORT_LINE,
                DC_SYSTHEME_WOFI_MARKER);
    } else {
        /* Nothing to preserve: write a fresh minimal style.css (import +
         * a handful of selectors) directly, same "creation path" as
         * systheme.c's apply_gtk_variant() creating gtk.css from scratch. */
        size_t style_len = 0;
        char *style = build_wofi_default_style_css(&style_len);
        if (!style) {
            dc_warn("systheme: failed to build default wofi style.css");
            return;
        }
        dc_systheme_write_owned(style_path, style, style_len);
        free(style);
    }

    /* wofi re-reads its config on every invocation -- no reload spawn. */
    dc_info("systheme: wofi theme written to %s", dir);
}

/* --- fuzzel ----------------------------------------------------------------- */

#define DC_SYSTHEME_FUZZEL_MARKER "# Added by DankC Settings > Theme & Colors"

/* "rrggbbaa" (8 hex digits, no leading '#', alpha LAST) -- fuzzel's required
 * [colors] format. Distinct from dc_systheme_hex_argb()'s "#aarrggbb"
 * (alpha first, Qt/kitty convention) -- see this file's header. */
static void hex_rgba_no_hash(dc_color c, char out[9])
{
    snprintf(out, 9, "%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
}

static char *build_fuzzel_ini(size_t *out_len)
{
    char background[9], text[9], prompt[9], input[9], match[9], selection[9],
            selection_text[9], selection_match[9], border[9];

    hex_rgba_no_hash(dc_theme_current->surface, background);
    hex_rgba_no_hash(dc_theme_current->surface_text, text);
    hex_rgba_no_hash(dc_theme_current->primary, prompt);
    hex_rgba_no_hash(dc_theme_current->surface_text, input);
    hex_rgba_no_hash(dc_theme_current->primary, match);
    hex_rgba_no_hash(dc_theme_current->primary_container, selection);
    hex_rgba_no_hash(dc_theme_current->surface_text, selection_text);
    hex_rgba_no_hash(dc_theme_current->primary, selection_match);
    hex_rgba_no_hash(dc_theme_current->outline, border);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fputs("[colors]\n", f);
    fprintf(f, "background=%s\n", background);
    fprintf(f, "text=%s\n", text);
    fprintf(f, "prompt=%s\n", prompt);
    fprintf(f, "input=%s\n", input);
    fprintf(f, "match=%s\n", match);
    fprintf(f, "selection=%s\n", selection);
    fprintf(f, "selection-text=%s\n", selection_text);
    fprintf(f, "selection-match=%s\n", selection_match);
    fprintf(f, "border=%s\n", border);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_fuzzel(bool light)
{
    (void)light;
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("fuzzel", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping fuzzel");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping fuzzel", dir);
        return;
    }

    size_t len = 0;
    char *ini = build_fuzzel_ini(&len);
    if (!ini) {
        dc_warn("systheme: failed to build fuzzel dank-colors.ini");
        return;
    }

    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.ini", dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/fuzzel.ini", dir);

    dc_systheme_write_owned(colors_path, ini, len);
    free(ini);

    char include_line[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(include_line, sizeof(include_line), "include=%s", colors_path);
    dc_systheme_ensure_line(cfg_path, "dank-colors.ini", DC_SYSTHEME_FUZZEL_MARKER, include_line);

    /* fuzzel re-reads its config on every invocation -- no reload spawn. */
    dc_info("systheme: fuzzel theme written to %s", dir);
}

/* --- tofi ------------------------------------------------------------------- */

#define DC_SYSTHEME_TOFI_MARKER "# Added by DankC Settings > Theme & Colors"

static char *build_tofi_colors(size_t *out_len)
{
    char background[8], text[8], selection[8], border[8], outline_hex[8];

    dc_systheme_hex_rgb(dc_theme_current->surface, background);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, text);
    dc_systheme_hex_rgb(dc_theme_current->primary_container, selection);
    dc_systheme_hex_rgb(dc_theme_current->outline, border);
    dc_systheme_hex_rgb(dc_theme_current->primary, outline_hex);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fprintf(f, "background-color = %s\n", background);
    fprintf(f, "text-color = %s\n", text);
    fprintf(f, "selection-color = %s\n", selection);
    fprintf(f, "border-color = %s\n", border);
    fprintf(f, "outline-color = %s\n", outline_hex);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_tofi(bool light)
{
    (void)light;
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!app_dir("tofi", dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping tofi");
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping tofi", dir);
        return;
    }

    size_t len = 0;
    char *colors = build_tofi_colors(&len);
    if (!colors) {
        dc_warn("systheme: failed to build tofi dank-colors");
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
    dc_systheme_ensure_line(cfg_path, "dank-colors", DC_SYSTHEME_TOFI_MARKER, include_line);

    /* tofi re-reads its config on every invocation -- no reload spawn. */
    dc_info("systheme: tofi theme written to %s", dir);
}
