/* systheme_browser.c — system-wide theming Task T6 (Tier-3 coverage pass,
 * see docs/21-THEMING-COVERAGE-PLAN.md): Firefox/Zen/LibreWolf, qutebrowser,
 * Discord (Vesktop/Vencord). See systheme_browser.h for the per-function
 * contract summary; reuses every atomic-write / marker+backup / dryrun /
 * dir-exists / spawn primitive from systheme_internal.h exactly like every
 * earlier Task's systheme_*.c -- see systheme.c's file header for the full
 * safety contract (opt-in only, dankc-owned files written atomically,
 * user-owned files only ever nudged with a one-time backup,
 * DANKC_THEME_DRYRUN gates every write/spawn, an app's own config dir is
 * never created by dankc).
 *
 * Palette mapping (shorthand per docs/21-THEMING-COVERAGE-PLAN.md):
 *   bg <- surface   panel <- surface_container   fg <- surface_text
 *   accent <- primary   on-accent <- primary_text   border <- outline
 *
 * --- Firefox / Zen / LibreWolf --------------------------------------------
 *
 * All three are Firefox forks that keep the exact same profile-manager INI
 * format (profiles.ini) and per-profile chrome/ layout, just under a
 * different top-level dotdir (~/.mozilla/firefox, ~/.zen, ~/.librewolf) --
 * one dankc toggle (cfg->systheme_firefox) and one code path cover all
 * three. profiles.ini is parsed READ-ONLY (never written): each
 * `[ProfileN]`/`[Default]`-prefixed... actually just `[Profile*]` section's
 * `Path=`/`IsRelative=` pair resolves to an absolute profile directory
 * (relative to the root dotdir when IsRelative=1, the default; an absolute
 * path when IsRelative=0), and EVERY profile found gets themed, not just
 * whichever section carries `Default=1` -- a user who runs multiple
 * profiles day-to-day would otherwise only get half of them themed for no
 * discoverable reason.
 *
 * For each profile directory profiles.ini pointed at (re-verified to
 * actually exist on disk before anything is touched -- a stale profiles.ini
 * entry for a deleted profile is silently skipped): chrome/dank-colors.css
 * is dankc-owned and atomic (a :root block of Firefox's `--lwt-*`/
 * `--toolbar-*`/`--tab-*` custom properties, which browser.css's Photon/
 * Proton chrome already consumes -- no new selectors invented).
 * chrome/userChrome.css (user-owned) gets a `@import "dank-colors.css";`
 * ensured via dc_systheme_ensure_line() -- created from scratch if this is
 * the profile's first-ever userChrome.css, since without that import the
 * CSS file above is otherwise inert. <profile>/user.js (also user-owned)
 * gets `user_pref("toolkit.legacyUserProfileCustomizations.stylesheets",
 * true);` ensured the same way -- Firefox silently ignores chrome/ entirely
 * unless that pref is set. The chrome/ subdirectory itself is the one
 * narrow exception to "never create a directory the app didn't already
 * make" in this file (exactly analogous to systheme_apps.c's qt5ct/qt6ct
 * colors/ subdir and systheme_notify.c's dunst dunstrc.d/ subdir
 * precedents): it is only ever created *inside* a profile directory
 * profiles.ini itself already pointed dankc at, never at the top level.
 *
 * Chrome (browser UI) only -- there is no supported way for dankc to theme
 * page CONTENT short of shipping a userContent.css + userstyles-style
 * extension, which is out of scope here (a Task 8 settings-UI hint, per the
 * plan doc). Restart-only: Firefox reads chrome/ and user.js only at
 * startup, there is no live-reload signal to nudge.
 *
 * --- qutebrowser -----------------------------------------------------------
 *
 * ~/.config/qutebrowser/dank-colors.py is a dankc-owned Python config
 * fragment (qutebrowser's config.py *is* Python, executed by the browser
 * itself -- there is no separate data format to generate) setting
 * `c.colors.*` for the completion menu, statusbar, tab bar, link hints, and
 * message popups. config.py itself is user-owned and, per qutebrowser's own
 * docs, creating one from nothing switches qutebrowser from its default
 * autoconfig.yml-based settings (managed entirely through `:set`/the GUI)
 * to config.py-based settings -- a much bigger behavioral change than a
 * theming engine should make unasked. So `config.source('dank-colors.py')`
 * is only ever ensured (marker+backup, dc_systheme_ensure_line()) into an
 * ALREADY-EXISTING config.py; if none exists yet, dank-colors.py is still
 * written (inert but harmless on disk) and the injection step is silently
 * skipped, with Task 8's settings UI expected to hint the one-line manual
 * fix (`config.source('dank-colors.py')` at the end of a config.py the user
 * creates themselves). qutebrowser re-reads its whole config directory on
 * every launch -- no reload spawn, and deliberately no runtime
 * `:config-source` IPC either (per-invocation freshness is enough; there's
 * no cheap way to reach a specific running instance here).
 *
 * --- Discord (Vesktop/Vencord) ---------------------------------------------
 *
 * Stock Discord has no theming hook at all -- this emitter only ever writes
 * into ~/.config/vesktop/themes or ~/.config/Vencord/themes (vesktop checked
 * first, matching dc_systheme_app_detected("discord")'s own order in
 * systheme.c), and only if one of those directories already exists (neither
 * is ever created -- if a user has neither client mod installed/run yet,
 * this is a silent no-op). <themes>/DankC.theme.css is a dankc-owned,
 * atomically-written BetterDiscord-style theme: an "at-name ... at-version"
 * doc-comment header (the convention both Vencord's and Vesktop's theme
 * loaders scan for) followed by a :root block of --background-*,
 * --text-*, --interactive-*, --brand-experiment, and --status-danger
 * custom properties, all read from dc_theme_current -- no new roles
 * invented. Needs the client mod (Vencord or standalone Vesktop) AND the
 * user enabling "DankC" once in its theme list; dankc has no way to nudge a
 * running Discord/Vesktop process, so there is no reload spawn here.
 */
#include "services/systheme_browser.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* --- shared path helpers (each systheme_*.c keeps its own tiny copy, same
 * precedent as every earlier Task file's config_home()/read_whole_file())
 * ------------------------------------------------------------------------ */

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

/* mkdir a single, already-justified subdirectory (see this file's header for
 * the narrow "inside an already-existing parent" exception each of the two
 * call sites below documents). DRYRUN-gated; ignores EEXIST/other errors --
 * a real failure just surfaces as a write failure from
 * dc_systheme_write_owned() right after, which already logs. Same shape as
 * systheme_apps.c's ensure_dir(). */
static void ensure_dir(const char *dir)
{
    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would mkdir -p %s", dir);
        return;
    }
    mkdir(dir, 0755);
}

/* --- Firefox / Zen / LibreWolf --------------------------------------------- */

#define DC_FIREFOX_MARKER "/* Added by DankC Settings > Theme & Colors */"
#define DC_FIREFOX_IMPORT_LINE "@import \"dank-colors.css\";"
#define DC_FIREFOX_USERJS_MARKER "// Added by DankC Settings > Theme & Colors"
#define DC_FIREFOX_USERJS_PREF \
    "user_pref(\"toolkit.legacyUserProfileCustomizations.stylesheets\", true);"
#define DC_FIREFOX_MAX_PROFILES 32

static char *build_firefox_css(size_t *out_len)
{
    char bg[8], panel[8], fg[8], accent[8], border[8];
    dc_systheme_hex_rgb(dc_theme_current->surface, bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->outline, border);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("/*\n"
          " * dank-colors.css -- Generated by DankC's system theming engine\n"
          " * (Settings > Theme & Colors). Chrome (browser UI) only; imported by\n"
          " * userChrome.css. Page content is not themed by this file.\n"
          " */\n\n",
            f);
    fputs(":root {\n", f);
    fprintf(f, "  --lwt-accent-color: %s;\n", accent);
    fprintf(f, "  --lwt-accent-color-inactive: %s;\n", accent);
    fprintf(f, "  --lwt-text-color: %s;\n", fg);
    fprintf(f, "  --lwt-selected-tab-background-color: %s;\n", accent);
    fprintf(f, "  --toolbar-bgcolor: %s;\n", panel);
    fprintf(f, "  --toolbar-color: %s;\n", fg);
    fprintf(f, "  --tab-selected-bgcolor: %s;\n", bg);
    fprintf(f, "  --tab-selected-textcolor: %s;\n", fg);
    fprintf(f, "  --lwt-toolbar-field-background-color: %s;\n", panel);
    fprintf(f, "  --lwt-toolbar-field-color: %s;\n", fg);
    fprintf(f, "  --lwt-toolbar-field-border-color: %s;\n", border);
    fputs("}\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

/* Records profile `dirs[*count]` (root/Path if IsRelative, else Path as-is)
 * iff the just-closed section was a `[Profile*]` one that carried a Path=
 * key -- called both at every `[section]` boundary and once more at EOF, so
 * a profiles.ini with no trailing blank line still gets its last section
 * counted. */
static void finalize_profile_section(bool in_profile_section, bool have_path,
        const char *root_dir, const char *path_value, bool is_relative,
        char dirs[][DC_SYSTHEME_PATH_MAX], int *count, int max)
{
    if (!in_profile_section || !have_path || *count >= max)
        return;
    if (is_relative)
        snprintf(dirs[*count], DC_SYSTHEME_PATH_MAX, "%s/%s", root_dir, path_value);
    else
        snprintf(dirs[*count], DC_SYSTHEME_PATH_MAX, "%s", path_value);
    (*count)++;
}

/* Parses `root_dir`/profiles.ini (read-only, never written) and appends the
 * absolute directory of every `[Profile*]` section found to `dirs`/`count`
 * (capped at `max`) -- ALL profiles, not just the one marked Default=1 (see
 * this file's header for why). Silently does nothing if profiles.ini
 * doesn't exist or fails to parse into any profile section (a directory
 * that just isn't a real Firefox-family root yet). */
static void collect_profile_dirs(const char *root_dir, char dirs[][DC_SYSTHEME_PATH_MAX],
        int *count, int max)
{
    char ini_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(ini_path, sizeof(ini_path), "%s/profiles.ini", root_dir);
    char *text = read_whole_file(ini_path);
    if (!text)
        return;

    bool in_profile_section = false;
    bool have_path = false;
    bool is_relative = true;
    char path_value[DC_SYSTHEME_PATH_MAX] = {0};

    char *saveptr = NULL;
    char *line = strtok_r(text, "\n", &saveptr);
    while (line) {
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r')
            line[--l] = '\0';
        while (*line == ' ' || *line == '\t')
            line++;

        if (line[0] == '[') {
            finalize_profile_section(in_profile_section, have_path, root_dir, path_value,
                    is_relative, dirs, count, max);
            in_profile_section = strncmp(line, "[Profile", 8) == 0;
            path_value[0] = '\0';
            is_relative = true;
            have_path = false;
        } else if (in_profile_section) {
            if (strncmp(line, "Path=", 5) == 0) {
                snprintf(path_value, sizeof(path_value), "%s", line + 5);
                have_path = true;
            } else if (strncmp(line, "IsRelative=", 11) == 0) {
                is_relative = line[11] != '0';
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    finalize_profile_section(in_profile_section, have_path, root_dir, path_value, is_relative,
            dirs, count, max);

    free(text);
}

static void apply_one_firefox_profile(const char *profile_dir)
{
    if (!dc_systheme_dir_exists(profile_dir)) {
        /* profiles.ini pointed at a profile that's since been deleted --
         * skip silently rather than manufacturing it back. */
        dc_info("systheme: firefox profile %s no longer exists, skipping", profile_dir);
        return;
    }

    char chrome_dir[DC_SYSTHEME_PATH_MAX + 16];
    snprintf(chrome_dir, sizeof(chrome_dir), "%s/chrome", profile_dir);
    if (!dc_systheme_dir_exists(chrome_dir))
        ensure_dir(chrome_dir);

    size_t len = 0;
    char *css = build_firefox_css(&len);
    if (!css) {
        dc_warn("systheme: failed to build firefox dank-colors.css for %s", profile_dir);
        return;
    }
    char colors_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.css", chrome_dir);
    dc_systheme_write_owned(colors_path, css, len);
    free(css);

    char userchrome_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(userchrome_path, sizeof(userchrome_path), "%s/userChrome.css", chrome_dir);
    dc_systheme_ensure_line(userchrome_path, DC_FIREFOX_IMPORT_LINE, DC_FIREFOX_MARKER,
            DC_FIREFOX_IMPORT_LINE);

    char userjs_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(userjs_path, sizeof(userjs_path), "%s/user.js", profile_dir);
    dc_systheme_ensure_line(userjs_path, DC_FIREFOX_USERJS_PREF, DC_FIREFOX_USERJS_MARKER,
            DC_FIREFOX_USERJS_PREF);

    dc_info("systheme: firefox-family theme written to %s", chrome_dir);
}

void dc_systheme_apply_firefox(bool light)
{
    (void)light; /* chrome CSS has no separate light/dark tuning beyond what's
                  * already baked into dc_theme_current, same convention as
                  * systheme_apps.c's Qt emitter. */
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("systheme: no $HOME, skipping firefox/zen/librewolf");
        return;
    }

    static const char *const kRoots[] = {".mozilla/firefox", ".zen", ".librewolf"};
    int total = 0;
    for (size_t i = 0; i < sizeof(kRoots) / sizeof(kRoots[0]); i++) {
        char root[DC_SYSTHEME_PATH_MAX];
        snprintf(root, sizeof(root), "%s/%s", home, kRoots[i]);
        if (!dc_systheme_dir_exists(root))
            continue;

        char dirs[DC_FIREFOX_MAX_PROFILES][DC_SYSTHEME_PATH_MAX];
        int count = 0;
        collect_profile_dirs(root, dirs, &count, DC_FIREFOX_MAX_PROFILES);
        for (int j = 0; j < count; j++) {
            apply_one_firefox_profile(dirs[j]);
            total++;
        }
    }

    if (total == 0)
        dc_info("systheme: no firefox/zen/librewolf profiles found, skipping");
}

/* --- qutebrowser ------------------------------------------------------------ */

#define DC_QUTE_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_QUTE_SOURCE_LINE "config.source('dank-colors.py')"

static char *build_qutebrowser_colors_py(size_t *out_len)
{
    char panel[8], panel_high[8], fg[8], accent[8], accent_text[8];
    char warn_hex[8], error_hex[8], info_hex[8];
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_container_high, panel_high);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, accent_text);
    dc_systheme_hex_rgb(dc_theme_current->warning, warn_hex);
    dc_systheme_hex_rgb(dc_theme_current->error, error_hex);
    dc_systheme_hex_rgb(dc_theme_current->info, info_hex);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# dank-colors.py -- Generated by DankC's system theming engine\n"
          "# (Settings > Theme & Colors). Sourced from config.py via\n"
          "# config.source('dank-colors.py').\n\n",
            f);

    fputs("# Completion menu\n", f);
    fprintf(f, "c.colors.completion.fg = '%s'\n", fg);
    fprintf(f, "c.colors.completion.odd.bg = '%s'\n", panel);
    fprintf(f, "c.colors.completion.even.bg = '%s'\n", panel_high);
    fprintf(f, "c.colors.completion.category.fg = '%s'\n", fg);
    fprintf(f, "c.colors.completion.category.bg = '%s'\n", panel);
    fprintf(f, "c.colors.completion.item.selected.fg = '%s'\n", accent_text);
    fprintf(f, "c.colors.completion.item.selected.bg = '%s'\n", accent);
    fprintf(f, "c.colors.completion.item.selected.match.fg = '%s'\n", accent_text);
    fprintf(f, "c.colors.completion.match.fg = '%s'\n", accent);
    fprintf(f, "c.colors.completion.scrollbar.fg = '%s'\n", fg);
    fprintf(f, "c.colors.completion.scrollbar.bg = '%s'\n", panel);

    fputs("\n# Statusbar\n", f);
    fprintf(f, "c.colors.statusbar.normal.fg = '%s'\n", fg);
    fprintf(f, "c.colors.statusbar.normal.bg = '%s'\n", panel);
    fprintf(f, "c.colors.statusbar.insert.fg = '%s'\n", accent_text);
    fprintf(f, "c.colors.statusbar.insert.bg = '%s'\n", accent);
    fprintf(f, "c.colors.statusbar.command.fg = '%s'\n", fg);
    fprintf(f, "c.colors.statusbar.command.bg = '%s'\n", panel);
    fprintf(f, "c.colors.statusbar.url.fg = '%s'\n", fg);
    fprintf(f, "c.colors.statusbar.url.warn.fg = '%s'\n", warn_hex);
    fprintf(f, "c.colors.statusbar.url.error.fg = '%s'\n", error_hex);

    fputs("\n# Tabs\n", f);
    fprintf(f, "c.colors.tabs.bar.bg = '%s'\n", panel);
    fprintf(f, "c.colors.tabs.odd.fg = '%s'\n", fg);
    fprintf(f, "c.colors.tabs.odd.bg = '%s'\n", panel);
    fprintf(f, "c.colors.tabs.even.fg = '%s'\n", fg);
    fprintf(f, "c.colors.tabs.even.bg = '%s'\n", panel_high);
    fprintf(f, "c.colors.tabs.selected.odd.fg = '%s'\n", accent_text);
    fprintf(f, "c.colors.tabs.selected.odd.bg = '%s'\n", accent);
    fprintf(f, "c.colors.tabs.selected.even.fg = '%s'\n", accent_text);
    fprintf(f, "c.colors.tabs.selected.even.bg = '%s'\n", accent);

    fputs("\n# Link hints\n", f);
    fprintf(f, "c.colors.hints.fg = '%s'\n", accent_text);
    fprintf(f, "c.colors.hints.bg = '%s'\n", accent);
    fprintf(f, "c.colors.hints.match.fg = '%s'\n", fg);

    fputs("\n# Messages\n", f);
    fprintf(f, "c.colors.messages.info.fg = '%s'\n", fg);
    fprintf(f, "c.colors.messages.info.bg = '%s'\n", info_hex);
    fprintf(f, "c.colors.messages.warning.fg = '%s'\n", fg);
    fprintf(f, "c.colors.messages.warning.bg = '%s'\n", warn_hex);
    fprintf(f, "c.colors.messages.error.fg = '%s'\n", fg);
    fprintf(f, "c.colors.messages.error.bg = '%s'\n", error_hex);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_qutebrowser(bool light)
{
    (void)light;
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping qutebrowser");
        return;
    }

    char appdir[DC_SYSTHEME_PATH_MAX + 16];
    snprintf(appdir, sizeof(appdir), "%s/qutebrowser", base);
    if (!dc_systheme_dir_exists(appdir)) {
        dc_info("systheme: %s does not exist, skipping qutebrowser", appdir);
        return;
    }

    size_t len = 0;
    char *py = build_qutebrowser_colors_py(&len);
    if (!py) {
        dc_warn("systheme: failed to build qutebrowser dank-colors.py");
        return;
    }
    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.py", appdir);
    dc_systheme_write_owned(colors_path, py, len);
    free(py);

    char config_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(config_path, sizeof(config_path), "%s/config.py", appdir);
    struct stat st;
    if (stat(config_path, &st) == 0 && S_ISREG(st.st_mode)) {
        dc_systheme_ensure_line(config_path, DC_QUTE_SOURCE_LINE, DC_QUTE_MARKER,
                DC_QUTE_SOURCE_LINE);
        dc_info("systheme: qutebrowser theme written to %s (sourced from config.py)", appdir);
    } else {
        /* Writing a config.py from scratch would flip qutebrowser from
         * autoconfig.yml-based settings to config.py-based settings -- a
         * behavior change well beyond "apply a theme". Leave it to the user
         * (Task 8 settings-UI hint), same precedent as wezterm/neovim/vim/
         * emacs' manual one-line activation. */
        dc_info("systheme: qutebrowser config.py not found, skipping config.source() "
                "injection (creating one would disable autoconfig.yml) -- dank-colors.py "
                "still written for manual config.source('dank-colors.py')");
    }
}

/* --- Discord (Vesktop/Vencord) ---------------------------------------------- */

#define DC_DISCORD_THEME_FILE "DankC.theme.css"

static char *build_discord_theme_css(size_t *out_len)
{
    char bg[8], panel[8], panel_high[8], fg[8], muted[8];
    char accent[8], accent_text[8], error_hex[8];
    dc_systheme_hex_rgb(dc_theme_current->surface, bg);
    dc_systheme_hex_rgb(dc_theme_current->surface_container, panel);
    dc_systheme_hex_rgb(dc_theme_current->surface_container_high, panel_high);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, fg);
    dc_systheme_hex_rgb(dc_theme_current->surface_variant_text, muted);
    dc_systheme_hex_rgb(dc_theme_current->primary, accent);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, accent_text);
    dc_systheme_hex_rgb(dc_theme_current->error, error_hex);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("/**\n"
          " * @name DankC\n"
          " * @description Live palette from DankC's system theming engine "
          "(Settings > Theme & Colors)\n"
          " * @author DankC\n"
          " * @version 1.0.0\n"
          " */\n\n",
            f);

    fputs(":root {\n", f);
    fprintf(f, "  --background-primary: %s;\n", bg);
    fprintf(f, "  --background-secondary: %s;\n", panel);
    fprintf(f, "  --background-secondary-alt: %s;\n", panel_high);
    fprintf(f, "  --background-tertiary: %s;\n", bg);
    fprintf(f, "  --background-floating: %s;\n", panel);
    fprintf(f, "  --background-mobile-primary: %s;\n", bg);
    fprintf(f, "  --background-mobile-secondary: %s;\n", panel);
    fprintf(f, "  --text-normal: %s;\n", fg);
    fprintf(f, "  --text-muted: %s;\n", muted);
    fprintf(f, "  --header-primary: %s;\n", fg);
    fprintf(f, "  --header-secondary: %s;\n", muted);
    fprintf(f, "  --interactive-normal: %s;\n", muted);
    fprintf(f, "  --interactive-hover: %s;\n", fg);
    fprintf(f, "  --interactive-active: %s;\n", accent_text);
    fprintf(f, "  --text-link: %s;\n", accent);
    fprintf(f, "  --brand-experiment: %s;\n", accent);
    fprintf(f, "  --status-danger: %s;\n", error_hex);
    fputs("}\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_discord(bool light)
{
    (void)light;
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping discord");
        return;
    }

    char themes_dir[DC_SYSTHEME_PATH_MAX + 24];
    snprintf(themes_dir, sizeof(themes_dir), "%s/vesktop/themes", base);
    if (!dc_systheme_dir_exists(themes_dir)) {
        snprintf(themes_dir, sizeof(themes_dir), "%s/Vencord/themes", base);
        if (!dc_systheme_dir_exists(themes_dir)) {
            dc_info("systheme: no vesktop/Vencord themes dir, skipping discord");
            return;
        }
    }

    size_t len = 0;
    char *css = build_discord_theme_css(&len);
    if (!css) {
        dc_warn("systheme: failed to build discord theme css");
        return;
    }
    char theme_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(theme_path, sizeof(theme_path), "%s/%s", themes_dir, DC_DISCORD_THEME_FILE);
    dc_systheme_write_owned(theme_path, css, len);
    free(css);

    dc_info("systheme: discord theme written to %s (enable \"DankC\" once in "
            "Vencord/Vesktop's theme settings)",
            theme_path);
}
