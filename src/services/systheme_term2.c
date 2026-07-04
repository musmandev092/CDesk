/* systheme_term2.c — system-wide theming Task T1 (Tier-2 coverage pass, see
 * docs/21-THEMING-COVERAGE-PLAN.md): ghostty, wezterm, konsole and
 * urxvt/xterm (via Xresources). See systheme_term2.h for the per-function
 * contract summary; reuses every atomic-write / marker+backup / dryrun /
 * detection / spawn primitive from systheme_internal.h exactly like Task 2's
 * systheme_term.c and Task 3's systheme_apps.c -- see systheme.c's file
 * header for the full safety contract (opt-in only, dankc-owned files
 * written atomically, user-owned files only ever nudged with a one-time
 * backup, DANKC_THEME_DRYRUN gates every write/spawn, an app's own config
 * dir is never created by dankc).
 *
 * --- ghostty -------------------------------------------------------------
 *
 * ~/.config/ghostty/themes/dank-theme is a dankc-owned "key = value" file
 * (ghostty's native theme format): `palette = N=#hex` for the 16 ANSI slots,
 * plus background/foreground/cursor-color/cursor-text/selection-background/
 * selection-foreground. Role mapping matches systheme_term.c's alacritty/
 * kitty/foot emitters exactly:
 *   background            <- surface
 *   foreground             <- surface_text
 *   cursor-color           <- primary   (the cursor block itself)
 *   cursor-text            <- surface   (cursor's own text colour)
 *   selection-background   <- primary_container
 *   selection-foreground   <- surface_text
 * `theme = dank-theme` is then ensured in ~/.config/ghostty/config
 * (user-owned, marker+backup, same duplicate-block caveat as
 * systheme_term.c's alacritty.toml/foot.ini patching if the user already has
 * some other `theme = ...` line -- ghostty's own config parser takes the
 * last occurrence of a repeated key, so this is harmless in practice).
 * `pkill -USR2 ghostty` nudges already-running instances (ghostty's
 * documented config-reload signal).
 *
 * --- wezterm ---------------------------------------------------------------
 *
 * ~/.config/wezterm/colors/DankC.toml is a dankc-owned TOML fragment
 * ([colors] ansi=[8 hex]/brights=[8 hex]/background/foreground/cursor_bg/
 * cursor_fg/selection_bg/selection_fg, [metadata] name). Same role mapping as
 * ghostty above (cursor_bg<-primary, cursor_fg<-surface, selection_bg<-
 * primary_container, selection_fg<-surface_text). wezterm.lua is a Lua
 * program, not data dankc can safely round-trip -- this emitter never
 * touches it; the one-time `color_scheme = "DankC"` hint for the user to add
 * themselves belongs to the settings-UI task, not this file.
 *
 * --- konsole -----------------------------------------------------------
 *
 * ~/.local/share/konsole/DankC.colorscheme is a dankc-owned KConfig INI
 * (konsole's native colour-scheme format): [Background]/[Foreground]
 * Color=r,g,b (decimal triplets), plus [Color0]..[Color7] from
 * dc_dynamic_ansi16()'s normal slots and [Color0Intense]..[Color7Intense]
 * from its bright slots. Background/Foreground reuse the same surface/
 * surface_text mapping as every other terminal emitter here.
 *
 * Konsole has no single global "active scheme" setting -- each *profile*
 * carries its own ColorScheme= key, and which profile is "the" one a fresh
 * `konsole` window opens with is named by `DefaultProfile=` in
 * ~/.config/konsolerc. This emitter parses that value (a plain textual
 * search for a `DefaultProfile=` line -- read-only, no KConfig section
 * awareness needed just to pull one value) and, if the referenced
 * `~/.local/share/konsole/<name>.profile` file exists, patches its
 * [Appearance] ColorScheme=DankC key the same marker+backup way
 * systheme_apps.c's qt5ct/qt6ct emitter patches [Appearance]
 * custom_palette=/color_scheme_path=. If konsolerc has no DefaultProfile= key,
 * or the named profile file doesn't exist, the .colorscheme file is still
 * written (so "DankC" already shows up in Konsole's own colour-scheme
 * picker) and this just logs that the profile-side patch was skipped rather
 * than guessing at a profile to create. restart-only: konsole reads its
 * profile at window-open time, no reload spawn.
 *
 * --- Xresources (urxvt/xterm) ---------------------------------------------
 *
 * ~/.config/dank/xresources is a dankc-owned file in dankc's own namespace
 * directory (NOT nested inside urxvt's or xterm's own config -- neither app
 * has one; X resources are process-wide server state, not a per-app file).
 * Because there's no existing app directory to defer to here, this is the
 * one emitter in the whole systheme_* family that creates its own directory
 * outright (one level under $XDG_CONFIG_HOME/$HOME/.config, same
 * mkdir-if-missing shape as core/config.c's own ensure_parent_dir() for
 * dankc's own config file). Contents: `*background`/`*foreground`/
 * `*cursorColor`/`*color0`..`*color15` (the loosely-bound `*` resource class
 * so it applies to any client that reads X resources, not just urxvt/xterm
 * specifically), same surface/surface_text/primary role mapping as the hex
 * terminal emitters above. `#include "<abs path>"` is then ensured in
 * ~/.Xresources (user-owned, marker+backup, HOME-rooted rather than
 * XDG_CONFIG_HOME-rooted -- that's simply where X clients look). If `xrdb`
 * is on $PATH, `xrdb -merge ~/.Xresources` nudges the running X server's
 * resource database; already-open urxvt/xterm windows still won't repaint
 * (X resources are read once at client startup) -- only new instances pick
 * up the change, hence the default-OFF toggle documented in
 * systheme_term2.h. XWayland-only: there's no X resource database to merge
 * into under a pure Wayland session, but `xrdb` simply won't be on $PATH
 * there either, so this degrades to a no-op spawn skip rather than an error.
 */
#include "services/systheme_term2.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- shared path helpers (each systheme_*.c keeps its own tiny copy, same
 * precedent as systheme_term.c's config_home()/app_dir() and
 * systheme_apps.c's config_home()) ----------------------------------------- */

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

static char *read_whole_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* mkdir() a single directory (no-op if it already exists); DRYRUN-gated.
 * Same shape as systheme_apps.c's ensure_dir() -- a real failure just
 * surfaces as a write failure from dc_systheme_write_owned() right after,
 * which already logs. */
static void ensure_dir(const char *dir)
{
    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would mkdir -p %s", dir);
        return;
    }
    mkdir(dir, 0755);
}

/* --- ANSI-16 hex/decimal helpers ------------------------------------------ */

static void hex_ansi16(bool light, char out[16][8])
{
    dc_color ansi[16];
    dc_dynamic_ansi16(dc_theme_current, light, ansi);
    for (int i = 0; i < 16; i++)
        dc_systheme_hex_rgb(ansi[i], out[i]);
}

/* --- ghostty --------------------------------------------------------------- */

#define DC_SYSTHEME_GHOSTTY_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_SYSTHEME_GHOSTTY_THEME_LINE "theme = dank-theme"

static char *build_ghostty_theme(bool light, size_t *out_len)
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

    for (int i = 0; i < 16; i++)
        fprintf(f, "palette = %d=%s\n", i, ansi[i]);

    fprintf(f, "\nbackground = %s\n", surface);
    fprintf(f, "foreground = %s\n", surface_text);
    fprintf(f, "cursor-color = %s\n", primary);
    fprintf(f, "cursor-text = %s\n", surface);
    fprintf(f, "selection-background = %s\n", primary_container);
    fprintf(f, "selection-foreground = %s\n", surface_text);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_ghostty(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping ghostty");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 32];
    app_dir(base, "ghostty", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping ghostty", dir);
        return;
    }

    char themes_dir[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(themes_dir, sizeof(themes_dir), "%s/themes", dir);
    /* themes/ is a conventional subdirectory of ghostty's own config dir
     * (which just got confirmed to exist above) -- not "creating ghostty's
     * config dir", so safe/necessary to create if this is dankc's first
     * theme drop for it. */
    ensure_dir(themes_dir);

    size_t len = 0;
    char *theme = build_ghostty_theme(light, &len);
    if (!theme) {
        dc_warn("systheme: failed to build ghostty dank-theme");
        return;
    }

    char theme_path[DC_SYSTHEME_PATH_MAX + 64];
    char cfg_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(theme_path, sizeof(theme_path), "%s/dank-theme", themes_dir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/config", dir);

    dc_systheme_write_owned(theme_path, theme, len);
    free(theme);

    dc_systheme_ensure_line(cfg_path, DC_SYSTHEME_GHOSTTY_THEME_LINE, DC_SYSTHEME_GHOSTTY_MARKER,
            DC_SYSTHEME_GHOSTTY_THEME_LINE);

    /* Nudge already-running ghostty instances (ghostty's documented
     * config-reload signal); best-effort, matches kitty's pkill -USR1. */
    const char *argv[] = {"pkill", "-USR2", "ghostty", NULL};
    dc_systheme_spawn("ghostty-reload", argv, 3);

    dc_info("systheme: ghostty theme written to %s", themes_dir);
}

/* --- wezterm --------------------------------------------------------------- */

static char *build_wezterm_toml(bool light, size_t *out_len)
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
    fputs("[colors]\n", f);

    fputs("ansi = [", f);
    for (int i = 0; i < 8; i++)
        fprintf(f, "\"%s\"%s", ansi[i], i < 7 ? ", " : "");
    fputs("]\n", f);

    fputs("brights = [", f);
    for (int i = 0; i < 8; i++)
        fprintf(f, "\"%s\"%s", ansi[8 + i], i < 7 ? ", " : "");
    fputs("]\n", f);

    fprintf(f, "background = \"%s\"\n", surface);
    fprintf(f, "foreground = \"%s\"\n", surface_text);
    fprintf(f, "cursor_bg = \"%s\"\n", primary);
    fprintf(f, "cursor_fg = \"%s\"\n", surface);
    fprintf(f, "selection_bg = \"%s\"\n", primary_container);
    fprintf(f, "selection_fg = \"%s\"\n", surface_text);

    fputs("\n[metadata]\n", f);
    fputs("name = \"DankC\"\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_wezterm(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping wezterm");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 32];
    app_dir(base, "wezterm", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping wezterm", dir);
        return;
    }

    char colors_dir[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(colors_dir, sizeof(colors_dir), "%s/colors", dir);
    /* colors/ is a conventional subdirectory of wezterm's own config dir
     * (just confirmed to exist above), same precedent as ghostty's themes/
     * and qt5ct/qt6ct's colors/ (systheme_apps.c). */
    ensure_dir(colors_dir);

    size_t len = 0;
    char *toml = build_wezterm_toml(light, &len);
    if (!toml) {
        dc_warn("systheme: failed to build wezterm DankC.toml");
        return;
    }

    char theme_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(theme_path, sizeof(theme_path), "%s/DankC.toml", colors_dir);
    dc_systheme_write_owned(theme_path, toml, len);
    free(toml);

    /* wezterm.lua is Lua, not data -- never touched here. The user adds
     * `color_scheme = "DankC"` once themselves (settings-UI hint is a later
     * task's job, not this emitter's). No reload spawn: wezterm's
     * scheme-file hot-reload behaviour is unconfirmed (see this file's
     * header); restart is the documented fallback. */
    dc_info("systheme: wezterm theme written to %s", colors_dir);
}

/* --- konsole ----------------------------------------------------------- */

#define DC_KONSOLE_MARKER "; Added by DankC Settings > Theme & Colors"

/* "r,g,b" decimal triplet, konsole's native .colorscheme format -- unlike
 * every other emitter in this file (hex), so this has its own tiny
 * formatter rather than reusing dc_systheme_hex_rgb(). */
static void decimal_rgb(dc_color c, char out[16])
{
    snprintf(out, 16, "%d,%d,%d", c.r, c.g, c.b);
}

static char *build_konsole_colorscheme(bool light, size_t *out_len)
{
    dc_color ansi[16];
    dc_dynamic_ansi16(dc_theme_current, light, ansi);

    char bg[16], fg[16];
    decimal_rgb(dc_theme_current->surface, bg);
    decimal_rgb(dc_theme_current->surface_text, fg);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("; Generated by DankC's system theming engine (Settings > Theme & Colors)\n", f);
    fputs("[General]\n", f);
    fputs("Description=DankC\n", f);
    fputs("Opacity=1\n\n", f);

    fprintf(f, "[Background]\nColor=%s\n\n", bg);
    fprintf(f, "[Foreground]\nColor=%s\n\n", fg);

    for (int i = 0; i < 8; i++) {
        char rgb[16];
        decimal_rgb(ansi[i], rgb);
        fprintf(f, "[Color%d]\nColor=%s\n\n", i, rgb);
    }
    for (int i = 0; i < 8; i++) {
        char rgb[16];
        decimal_rgb(ansi[8 + i], rgb);
        fprintf(f, "[Color%dIntense]\nColor=%s\n\n", i, rgb);
    }

    fclose(f);
    *out_len = len;
    return buf;
}

/* Reads (read-only) `konsolerc_path` looking for a `DefaultProfile=` line
 * and returns its value (the raw filename, e.g. "Profile1.profile") in
 * `out`. No KConfig section-awareness needed for a single scalar value --
 * this is deliberately a plain textual scan, matching this emitter's
 * "read-only parse" contract (systheme_term2.h). Returns false if the file
 * doesn't exist or has no such key. */
static bool find_default_profile(const char *konsolerc_path, char *out, size_t cap)
{
    char *text = read_whole_file(konsolerc_path);
    if (!text)
        return false;

    bool found = false;
    const char *key = "DefaultProfile=";
    size_t keylen = strlen(key);
    const char *p = text;
    while (*p) {
        /* Only match at the start of a line (a mid-line substring like
         * "FooDefaultProfile=" would be a different, unrelated key). */
        if ((p == text || p[-1] == '\n') && strncmp(p, key, keylen) == 0) {
            const char *val = p + keylen;
            const char *end = strchr(val, '\n');
            size_t vlen = end ? (size_t)(end - val) : strlen(val);
            /* Trim a trailing '\r' (CRLF-saved konsolerc). */
            if (vlen > 0 && val[vlen - 1] == '\r')
                vlen--;
            if (vlen > 0 && vlen < cap) {
                memcpy(out, val, vlen);
                out[vlen] = '\0';
                found = true;
            }
            break;
        }
        const char *nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }

    free(text);
    return found;
}

void dc_systheme_apply_konsole(bool light)
{
    char data[DC_SYSTHEME_PATH_MAX];
    if (!data_home(data, sizeof(data))) {
        dc_warn("systheme: no $XDG_DATA_HOME/$HOME, skipping konsole");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 32];
    app_dir(data, "konsole", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping konsole", dir);
        return;
    }

    size_t len = 0;
    char *scheme = build_konsole_colorscheme(light, &len);
    if (!scheme) {
        dc_warn("systheme: failed to build konsole DankC.colorscheme");
        return;
    }

    char scheme_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(scheme_path, sizeof(scheme_path), "%s/DankC.colorscheme", dir);
    dc_systheme_write_owned(scheme_path, scheme, len);
    free(scheme);
    dc_info("systheme: konsole colorscheme written to %s", scheme_path);

    /* Best-effort: patch the default profile's ColorScheme= key too, so a
     * freshly-opened konsole window actually picks up "DankC" without the
     * user hunting for it in Settings > Edit Current Profile. Read-only
     * parse of konsolerc (never rewritten) per this emitter's contract. */
    char base[DC_SYSTHEME_PATH_MAX];
    char konsolerc[DC_SYSTHEME_PATH_MAX + 32];
    char profile_name[256];
    if (!config_home(base, sizeof(base))) {
        dc_info("systheme: no $XDG_CONFIG_HOME/$HOME, wrote colorscheme only -- select "
                "\"DankC\" manually in Konsole's profile settings");
        return;
    }
    snprintf(konsolerc, sizeof(konsolerc), "%s/konsolerc", base);
    if (!find_default_profile(konsolerc, profile_name, sizeof(profile_name))) {
        dc_info("systheme: no DefaultProfile= in ~/.config/konsolerc, wrote colorscheme only "
                "-- select \"DankC\" manually in Konsole's profile settings");
        return;
    }

    char profile_path[DC_SYSTHEME_PATH_MAX + 320];
    snprintf(profile_path, sizeof(profile_path), "%s/%s", dir, profile_name);
    if (!dc_systheme_dir_exists(profile_path)) {
        dc_info("systheme: default konsole profile %s not found, wrote colorscheme only -- "
                "select \"DankC\" manually in Konsole's profile settings",
                profile_path);
        return;
    }

    dc_systheme_ensure_line(profile_path, "ColorScheme=DankC", DC_KONSOLE_MARKER,
            "[Appearance]\nColorScheme=DankC");

    /* konsole only reads a profile's ColorScheme at window-open time --
     * restart-only, no reload spawn. */
    dc_info("systheme: konsole default profile %s patched to use DankC", profile_path);
}

/* --- Xresources (urxvt/xterm) ---------------------------------------------- */

#define DC_XRESOURCES_MARKER "! Added by DankC Settings > Theme & Colors"

static char *build_xresources(bool light, size_t *out_len)
{
    char surface[8], surface_text[8], primary[8];
    char ansi[16][8];

    dc_systheme_hex_rgb(dc_theme_current->surface, surface);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, surface_text);
    dc_systheme_hex_rgb(dc_theme_current->primary, primary);
    hex_ansi16(light, ansi);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("! Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fprintf(f, "*background: %s\n", surface);
    fprintf(f, "*foreground: %s\n", surface_text);
    fprintf(f, "*cursorColor: %s\n", primary);
    for (int i = 0; i < 16; i++)
        fprintf(f, "*color%d: %s\n", i, ansi[i]);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_xresources(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping xresources");
        return;
    }

    /* Unlike every other emitter in this file, ~/.config/dank isn't any
     * app's own config dir -- it's dankc's own namespace, with nothing else
     * to defer to. Safe/necessary to create outright (one mkdir, same shape
     * as core/config.c's own ensure_parent_dir() for dankc's own config
     * file), see this file's header. */
    char dank_dir[DC_SYSTHEME_PATH_MAX + 16];
    snprintf(dank_dir, sizeof(dank_dir), "%s/dank", base);
    ensure_dir(dank_dir);

    size_t len = 0;
    char *xres = build_xresources(light, &len);
    if (!xres) {
        dc_warn("systheme: failed to build ~/.config/dank/xresources");
        return;
    }

    char theme_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(theme_path, sizeof(theme_path), "%s/xresources", dank_dir);
    dc_systheme_write_owned(theme_path, xres, len);
    free(xres);

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("systheme: no $HOME, wrote %s but could not patch ~/.Xresources", theme_path);
        return;
    }
    char xresources_file[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(xresources_file, sizeof(xresources_file), "%s/.Xresources", home);

    char include_line[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(include_line, sizeof(include_line), "#include \"%s\"", theme_path);
    dc_systheme_ensure_line(xresources_file, theme_path, DC_XRESOURCES_MARKER, include_line);

    /* Only freshly-started urxvt/xterm instances (and anything else that
     * reads X resources at connection time) pick this up; already-open
     * windows won't repaint. Merge into the running X server's resource
     * database if xrdb is available -- skip entirely (not just dryrun-log)
     * if it isn't, since there'd be nothing for xrdb to do under a pure
     * Wayland session with no X resource database at all. */
    if (dc_systheme_on_path("xrdb")) {
        const char *argv[] = {"xrdb", "-merge", xresources_file, NULL};
        dc_systheme_spawn("xrdb-merge", argv, 3);
    }

    dc_info("systheme: xresources theme written to %s", theme_path);
}
