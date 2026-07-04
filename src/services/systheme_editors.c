/* systheme_editors.c — system-wide theming Task T4 (Tier-2 coverage pass,
 * see docs/21-THEMING-COVERAGE-PLAN.md): Zed, Helix, Neovim, Vim, Sublime
 * Text, Emacs. See systheme_editors.h for the per-function contract
 * summary; reuses every atomic-write / marker+backup / ensure_line_top /
 * dryrun / detection / spawn primitive from systheme_internal.h exactly
 * like Task T1's systheme_term2.c and Task 3's systheme_apps.c -- see
 * systheme.c's file header for the full safety contract (opt-in only,
 * dankc-owned files written atomically, user-owned files only ever nudged
 * with a one-time backup, DANKC_THEME_DRYRUN gates every write/spawn, an
 * app's own config dir is never created by dankc).
 *
 * --- shared syntax-highlighting mapping ------------------------------------
 *
 * Every editor below (except the two that are chrome-only, i.e. none of
 * them -- all six do at least some syntax coloring) derives its
 * comment/string/keyword/function/type/constant/tag scopes from the SAME
 * eight-slot subset of dc_dynamic_ansi16()'s normal ANSI-8 range (see
 * build_syntax() below), so DankC's editor syntax highlighting agrees with
 * itself across apps and with the terminal emitters' ANSI palette:
 *   comment  <- ANSI 8  (bright black)     keyword   <- ANSI 5 (magenta)
 *   string   <- ANSI 2  (green)            function  <- ANSI 4 (blue)
 *   type     <- ANSI 3  (yellow)           constant  <- ANSI 6 (cyan)
 *   tag      <- ANSI 1  (red)              variable/operator <- surface_text
 * This is a deliberate, documented convention choice (not derived from any
 * upstream spec) so a user who changes dankc's active theme sees consistent
 * "keywords are magenta, strings are green" behaviour everywhere at once.
 *
 * --- Zed --------------------------------------------------------------
 *
 * ~/.config/zed/themes/DankC.json is a dankc-owned Zed "theme family": one
 * JSON document with a top-level "themes" array holding two members,
 * "DankC Dark" (appearance "dark") and "DankC Light" (appearance "light").
 * Zed's family-JSON schema requires a family to actually have both
 * appearances present to be valid, but dankc only ever holds ONE mode's
 * resolved palette at a time (dc_theme_current mirrors whichever of
 * dark/light dc_config_light_mode() currently selects) -- there is no
 * second, simultaneously-available palette to source the "other" member
 * from without re-invoking the theme engine in the opposite mode, which
 * would mutate global state the rest of the app depends on. This emitter's
 * documented compromise: BOTH family members are (re)built from whatever
 * dc_theme_current holds right now on every apply, tagged with their
 * appearance name but not necessarily matching contrast for that
 * appearance. Because ~/.config/zed/settings.json's "theme" key is set
 * explicitly to the *name* of the live member ("DankC Dark" or
 * "DankC Light", matching the current mode) rather than "system", Zed
 * always renders the correct, currently-accurate colors; only the *other*,
 * currently-unselected family member would show stale/mismatched-contrast
 * colors if a user manually switched Zed's theme to it out-of-band. Restart
 * may be needed for an already-open Zed window to pick up a changed theme
 * *file*; settings.json's "theme" key itself is applied live.
 *
 * settings.json is user-owned; only the top-level "theme" string key is
 * dankc's. Same cJSON parse/patch/reserialize round-trip as
 * systheme_apps.c's VS Code emitter (see its file header for the full
 * rationale) -- treated here by json_set_string_key() below, generalized
 * to any single string-valued settings key with an "already ours" prefix
 * test (since a bare string, unlike VS Code's colorCustomizations *object*,
 * has nowhere to embed a "//"-style ownership marker key): any current
 * value starting with "DankC " is treated as dankc's own prior write (no
 * re-backup needed, matches either family member across a light/dark
 * flip); anything else (the user's real prior theme, or no key at all) is
 * backed up once before the first overwrite. cJSON_Parse failure (Zed
 * tolerates trailing commas some releases don't -- cJSON does not) aborts
 * the whole settings.json write, logged, touching nothing.
 *
 * --- Helix -----------------------------------------------------------
 *
 * ~/.config/helix/themes/dank.toml is a dankc-owned Helix theme (TOML):
 * top-level scope keys (attribute/type/constant/string/comment/variable/
 * keyword/operator/function/tag/namespace/special/diagnostic.*) plus a
 * "ui.*" section for chrome (ui.background/ui.text/ui.cursor.primary/
 * ui.selection/ui.statusline/ui.linenr/ui.popup/ui.menu.*). `theme = "dank"`
 * is then ensured at the very TOP of ~/.config/helix/config.toml (user-
 * owned) via dc_systheme_ensure_line_top() -- a bare top-level TOML key
 * MUST precede any `[section]` table header in the same document to parse
 * at all, so appending it at the end (like every other ensure_line() call
 * in this codebase) would silently corrupt any config.toml that already
 * has a `[keys.normal]`-style section. `pkill -USR1 hx` nudges already-
 * running Helix instances (its documented config-reload signal).
 *
 * --- Neovim ------------------------------------------------------------
 *
 * ~/.config/nvim/colors/dank.lua is a dankc-owned Lua colorscheme script:
 * clears any existing highlighting, sets vim.g.colors_name, applies core
 * highlight groups via vim.api.nvim_set_hl(), and sets
 * vim.g.terminal_color_0.._15 for :terminal's own ANSI palette. Write-only:
 * Neovim never auto-loads a colorscheme dankc drops into colors/ -- the
 * user runs `:colorscheme dank` (or wires it into their own init.lua)
 * themselves; Task 8's settings UI is expected to surface that as a hint,
 * not this emitter's job (docs' Task 8 note). No reload spawn, no
 * init.lua edit.
 *
 * --- Vim -------------------------------------------------------------
 *
 * ~/.vim/colors/dank.vim is a dankc-owned classic Vimscript colorscheme:
 * `hi` highlight-group commands (guifg/guibg only -- no cterm approximation
 * attempted, matching this task's "write-only, simple" scope) plus
 * `g:terminal_ansi_colors` for Vim 8+'s :terminal. Same write-only,
 * `:colorscheme dank`-activated story as Neovim above.
 *
 * --- Sublime Text --------------------------------------------------------
 *
 * ~/.config/sublime-text/Packages/User/DankC.sublime-color-scheme is a
 * dankc-owned JSON color scheme (Sublime's modern .sublime-color-scheme
 * format: "globals" for chrome + "rules" for scope-based syntax coloring).
 * Preferences.sublime-settings is user-owned; only the top-level
 * "color_scheme" string key is dankc's, patched via the same
 * json_set_string_key() round-trip as Zed above. Unlike Zed there is only
 * ONE possible dankc-owned value here (the fixed resource path to
 * DankC.sublime-color-scheme -- the scheme *file itself* is what changes
 * with light/dark mode, not this reference to it), so the "already ours"
 * prefix test is just that exact path. Sublime applies both files live, no
 * reload spawn needed.
 *
 * --- Emacs -------------------------------------------------------------
 *
 * ~/.emacs.d/dank-theme.el is a dankc-owned Emacs Lisp theme: `deftheme` +
 * `custom-theme-set-faces` covering the core faces (default/cursor/region/
 * mode-line/font-lock-*). Write-only: activating a custom theme file
 * requires the user to add its directory to `custom-theme-load-path` and
 * call `(load-theme 'dank t)` themselves (a one-time hint, Task 8's job,
 * exactly like the docs plan's Neovim/Vim/wezterm precedent) -- dankc never
 * edits ~/.emacs or ~/.emacs.d/init.el. No reload spawn.
 */
#include "services/systheme_editors.h"

#include "services/systheme_internal.h"

#include "cJSON.h"
#include "core/log.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* --- shared path/file helpers (each systheme_*.c keeps its own tiny copy,
 * same precedent as systheme_term2.c's config_home()/data_home()/app_dir()
 * and systheme_apps.c's config_home()/read_whole_file()/
 * backup_timestamped()) ------------------------------------------------- */

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

/* Timestamped backup, identical naming/behaviour to
 * dc_systheme_ensure_line()'s own backup step and systheme_apps.c's
 * standalone copy of the same helper. DRYRUN-gated; returns false (and
 * logs) only on a real I/O failure. */
static bool backup_timestamped(const char *path, const char *original_text)
{
    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would back up %s to %s.bak-<epoch>", path, path);
        return true;
    }
    char backup_path[DC_SYSTHEME_PATH_MAX + 96];
    snprintf(backup_path, sizeof(backup_path), "%s.bak-%ld", path, (long)time(NULL));
    FILE *bf = fopen(backup_path, "w");
    if (!bf) {
        dc_warn("systheme: could not create backup %s; leaving %s untouched", backup_path, path);
        return false;
    }
    fputs(original_text, bf);
    fclose(bf);
    dc_info("systheme: backed up %s to %s before first edit", path, backup_path);
    return true;
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

/* --- shared hex + syntax-mapping helpers -------------------------------- */

/* "#rrggbbff" (9 chars + NUL); `out` must be >= 10 bytes. Zed's native
 * RGBA-with-alpha-last hex convention (distinct from
 * dc_systheme_hex_argb()'s alpha-FIRST Qt/kitty-style ARGB, which doesn't
 * fit Zed's schema). Always fully opaque -- dc_theme has no per-role alpha
 * channel worth threading through here. */
static void hex_rgba(dc_color c, char out[10])
{
    char rgb[8];
    dc_systheme_hex_rgb(c, rgb);
    snprintf(out, 10, "%sff", rgb);
}

typedef struct {
    dc_color comment;
    dc_color string_;
    dc_color keyword;
    dc_color function_;
    dc_color type_;
    dc_color constant_;
    dc_color tag;
    dc_color variable;
    dc_color operator_;
} dank_syntax;

/* The shared comment/string/keyword/function/type/constant/tag -> ANSI-16
 * mapping documented in this file's header, built once per apply and
 * reused by every emitter below that does syntax coloring. */
static void build_syntax(bool light, dank_syntax *out)
{
    dc_color ansi[16];
    dc_dynamic_ansi16(dc_theme_current, light, ansi);
    out->comment = ansi[8];
    out->string_ = ansi[2];
    out->keyword = ansi[5];
    out->function_ = ansi[4];
    out->type_ = ansi[3];
    out->constant_ = ansi[6];
    out->tag = ansi[1];
    out->variable = dc_theme_current->surface_text;
    out->operator_ = dc_theme_current->surface_text;
}

/* --- shared settings-key JSON round-trip (Zed "theme" / Sublime
 * "color_scheme") ---------------------------------------------------------
 *
 * Generalizes systheme_apps.c's VS Code colorCustomizations round-trip to a
 * single *string*-valued top-level settings key: parse the whole document
 * strictly, replace/insert just `key`, reserialize, write atomically. Same
 * "cJSON_Parse failure means abort entirely, never a partial rewrite"
 * contract -- see this file's header. Never creates `path` if it doesn't
 * already exist (settings.json/Preferences.sublime-settings are the user's
 * files; dankc only ever patches an existing one, exactly like
 * systheme_apps.c's find_vscode_settings() never synthesizing one).
 *
 * `own_prefix`, if non-NULL, is compared against the *start* of the
 * existing value to decide "is this already dankc's own prior write" (skip
 * the backup) vs. "this is the user's real setting, or a different one of
 * dankc's own values" (back up once before overwriting) -- see this file's
 * Zed/Sublime header sections for why a prefix test (rather than exact
 * equality with the new `value`) is needed for Zed's two possible values.
 * If `own_prefix` is NULL, exact equality with `value` is used instead
 * (Sublime's case: there is only ever one dankc-owned value). */
static bool json_set_string_key(const char *path, const char *key, const char *value,
        const char *own_prefix, const char *tag)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        dc_info("systheme: %s not found, skipping %s settings key", path, tag);
        return false;
    }

    char *text = read_whole_file(path);
    if (!text) {
        dc_warn("systheme: could not read %s, skipping %s settings key", path, tag);
        return false;
    }

    cJSON *root = cJSON_Parse(text);
    if (!root) {
        /* Some of these formats legally allow JSONC-style comments/trailing
         * commas that cJSON (strict JSON only) rejects -- never rewrite a
         * file we couldn't fully parse. */
        dc_warn("systheme: %s did not parse as strict JSON -- skipping %s settings key "
                "rather than risk corrupting it",
                path, tag);
        free(text);
        return false;
    }
    if (!cJSON_IsObject(root)) {
        dc_warn("systheme: %s top level is not a JSON object, skipping %s settings key", path,
                tag);
        cJSON_Delete(root);
        free(text);
        return false;
    }

    cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, key);
    bool already_dankc = false;
    if (cJSON_IsString(existing) && existing->valuestring) {
        if (own_prefix)
            already_dankc = strncmp(existing->valuestring, own_prefix, strlen(own_prefix)) == 0;
        else
            already_dankc = strcmp(existing->valuestring, value) == 0;
    }

    if (!already_dankc && !backup_timestamped(path, text)) {
        /* Backup failed for real (not dryrun): abort rather than risk the
         * user's original file, matching dc_systheme_ensure_line()'s own
         * contract. */
        cJSON_Delete(root);
        free(text);
        return false;
    }
    free(text);

    cJSON_DeleteItemFromObjectCaseSensitive(root, key);
    cJSON_AddStringToObject(root, key, value);

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) {
        dc_warn("systheme: failed to serialize %s, skipping %s settings key", path, tag);
        return false;
    }

    dc_systheme_write_owned(path, out, strlen(out));
    free(out);
    dc_info("systheme: %s \"%s\" set to \"%s\" in %s", tag, key, value, path);
    return true;
}

/* --- Zed ----------------------------------------------------------------- */

#define DC_ZED_DARK_NAME "DankC Dark"
#define DC_ZED_LIGHT_NAME "DankC Light"
#define DC_ZED_OWN_PREFIX "DankC "

static void jf_color(FILE *f, const char *indent, const char *key, dc_color c, bool comma)
{
    char rgba[10];
    hex_rgba(c, rgba);
    fprintf(f, "%s\"%s\": \"%s\"%s\n", indent, key, rgba, comma ? "," : "");
}

static void jf_syntax(FILE *f, const char *scope, dc_color c, bool comma)
{
    char rgba[10];
    hex_rgba(c, rgba);
    fprintf(f, "          \"%s\": { \"color\": \"%s\" }%s\n", scope, rgba, comma ? "," : "");
}

static void write_zed_style(FILE *f, bool light, const dank_syntax *syn)
{
    const dc_theme *t = dc_theme_current;
    fputs("      \"style\": {\n", f);
    jf_color(f, "        ", "background", t->surface, true);
    jf_color(f, "        ", "border", t->outline, true);
    jf_color(f, "        ", "border.variant", t->surface_container, true);
    jf_color(f, "        ", "text", t->surface_text, true);
    jf_color(f, "        ", "text.muted", t->surface_container_high, true);
    jf_color(f, "        ", "text.placeholder", t->surface_container_high, true);
    jf_color(f, "        ", "icon", t->surface_text, true);
    jf_color(f, "        ", "icon.muted", t->surface_container_high, true);
    jf_color(f, "        ", "element.background", t->surface_container, true);
    jf_color(f, "        ", "element.hover", t->surface_container_high, true);
    jf_color(f, "        ", "element.selected", t->primary_container, true);
    jf_color(f, "        ", "ghost_element.hover", t->surface_container_high, true);
    jf_color(f, "        ", "ghost_element.selected", t->primary_container, true);
    jf_color(f, "        ", "tab.active_background", t->surface, true);
    jf_color(f, "        ", "tab.inactive_background", t->surface_container, true);
    jf_color(f, "        ", "toolbar.background", t->surface, true);
    jf_color(f, "        ", "title_bar.background", t->surface_container, true);
    jf_color(f, "        ", "status_bar.background", t->surface_container, true);
    jf_color(f, "        ", "panel.background", t->surface_container, true);
    jf_color(f, "        ", "scrollbar.thumb.background", t->surface_container_high, true);
    jf_color(f, "        ", "editor.background", t->surface, true);
    jf_color(f, "        ", "editor.foreground", t->surface_text, true);
    jf_color(f, "        ", "editor.gutter.background", t->surface, true);
    jf_color(f, "        ", "editor.line_number", t->surface_container_high, true);
    jf_color(f, "        ", "editor.active_line_number", t->surface_text, true);
    jf_color(f, "        ", "editor.active_line.background", t->surface_container, true);
    jf_color(f, "        ", "editor.highlighted_line.background", t->primary_container, true);
    jf_color(f, "        ", "editor.wrap_guide", t->surface_container, true);
    jf_color(f, "        ", "terminal.background", t->surface, true);
    jf_color(f, "        ", "terminal.foreground", t->surface_text, true);
    jf_color(f, "        ", "conflict", t->error, true);
    jf_color(f, "        ", "created", syn->string_, true);
    jf_color(f, "        ", "deleted", t->error, true);
    jf_color(f, "        ", "modified", syn->type_, true);

    dc_color ansi[16];
    dc_dynamic_ansi16(t, light, ansi);
    static const char *const kAnsiNames[16] = {
        "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
        "bright_black", "bright_red", "bright_green", "bright_yellow",
        "bright_blue", "bright_magenta", "bright_cyan", "bright_white",
    };
    for (int i = 0; i < 16; i++) {
        char key[32];
        snprintf(key, sizeof(key), "terminal.ansi.%s", kAnsiNames[i]);
        jf_color(f, "        ", key, ansi[i], true);
    }

    fputs("        \"syntax\": {\n", f);
    jf_syntax(f, "comment", syn->comment, true);
    jf_syntax(f, "comment.doc", syn->comment, true);
    jf_syntax(f, "string", syn->string_, true);
    jf_syntax(f, "string.special", syn->string_, true);
    jf_syntax(f, "keyword", syn->keyword, true);
    jf_syntax(f, "function", syn->function_, true);
    jf_syntax(f, "function.method", syn->function_, true);
    jf_syntax(f, "type", syn->type_, true);
    jf_syntax(f, "type.builtin", syn->type_, true);
    jf_syntax(f, "constant", syn->constant_, true);
    jf_syntax(f, "number", syn->constant_, true);
    jf_syntax(f, "boolean", syn->constant_, true);
    jf_syntax(f, "variable", syn->variable, true);
    jf_syntax(f, "property", syn->type_, true);
    jf_syntax(f, "tag", syn->tag, true);
    jf_syntax(f, "attribute", syn->tag, true);
    jf_syntax(f, "operator", syn->operator_, false);
    fputs("        }\n", f);
    fputs("      }\n", f);
}

static void write_zed_member(FILE *f, const char *name, const char *appearance, bool light,
        const dank_syntax *syn, bool comma)
{
    fputs("    {\n", f);
    fprintf(f, "      \"name\": \"%s\",\n", name);
    fprintf(f, "      \"appearance\": \"%s\",\n", appearance);
    write_zed_style(f, light, syn);
    fprintf(f, "    }%s\n", comma ? "," : "");
}

static char *build_zed_theme_family(bool light, size_t *out_len)
{
    dank_syntax syn;
    build_syntax(light, &syn);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("{\n", f);
    fputs("  \"$schema\": \"https://zed.dev/schema/themes/v0.2.0.json\",\n", f);
    fputs("  \"name\": \"DankC\",\n", f);
    fputs("  \"author\": \"DankC\",\n", f);
    fputs("  \"themes\": [\n", f);
    write_zed_member(f, DC_ZED_DARK_NAME, "dark", light, &syn, true);
    write_zed_member(f, DC_ZED_LIGHT_NAME, "light", light, &syn, false);
    fputs("  ]\n", f);
    fputs("}\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_zed(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping zed");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "zed", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping zed", dir);
        return;
    }

    char themes_dir[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(themes_dir, sizeof(themes_dir), "%s/themes", dir);
    /* themes/ is a conventional subdirectory of zed's own config dir (just
     * confirmed to exist above) -- not "creating zed's config dir", same
     * precedent as ghostty's themes/ / wezterm's colors/. */
    ensure_dir(themes_dir);

    size_t len = 0;
    char *json = build_zed_theme_family(light, &len);
    if (!json) {
        dc_warn("systheme: failed to build zed DankC.json");
        return;
    }
    char theme_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(theme_path, sizeof(theme_path), "%s/DankC.json", themes_dir);
    dc_systheme_write_owned(theme_path, json, len);
    free(json);

    char settings_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(settings_path, sizeof(settings_path), "%s/settings.json", dir);
    json_set_string_key(settings_path, "theme", light ? DC_ZED_LIGHT_NAME : DC_ZED_DARK_NAME,
            DC_ZED_OWN_PREFIX, "zed");

    dc_info("systheme: zed theme written to %s", themes_dir);
}

/* --- Helix --------------------------------------------------------------- */

#define DC_HELIX_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_HELIX_THEME_LINE "theme = \"dank\""

static char *build_helix_theme(bool light, size_t *out_len)
{
    const dc_theme *t = dc_theme_current;
    dank_syntax syn;
    build_syntax(light, &syn);

    char surface[8], surface_text[8], surface_container[8], surface_container_high[8];
    char primary[8], primary_text[8], primary_container[8], outline[8], error_hex[8];
    char comment[8], string_[8], keyword[8], function_[8], type_[8], constant_[8], tag[8];

    dc_systheme_hex_rgb(t->surface, surface);
    dc_systheme_hex_rgb(t->surface_text, surface_text);
    dc_systheme_hex_rgb(t->surface_container, surface_container);
    dc_systheme_hex_rgb(t->surface_container_high, surface_container_high);
    dc_systheme_hex_rgb(t->primary, primary);
    dc_systheme_hex_rgb(t->primary_text, primary_text);
    dc_systheme_hex_rgb(t->primary_container, primary_container);
    dc_systheme_hex_rgb(t->outline, outline);
    dc_systheme_hex_rgb(t->error, error_hex);
    dc_systheme_hex_rgb(syn.comment, comment);
    dc_systheme_hex_rgb(syn.string_, string_);
    dc_systheme_hex_rgb(syn.keyword, keyword);
    dc_systheme_hex_rgb(syn.function_, function_);
    dc_systheme_hex_rgb(syn.type_, type_);
    dc_systheme_hex_rgb(syn.constant_, constant_);
    dc_systheme_hex_rgb(syn.tag, tag);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);

    fprintf(f, "\"attribute\" = \"%s\"\n", tag);
    fprintf(f, "\"type\" = \"%s\"\n", type_);
    fprintf(f, "\"constructor\" = \"%s\"\n", type_);
    fprintf(f, "\"constant\" = \"%s\"\n", constant_);
    fprintf(f, "\"constant.numeric\" = \"%s\"\n", constant_);
    fprintf(f, "\"constant.character.escape\" = \"%s\"\n", keyword);
    fprintf(f, "\"string\" = \"%s\"\n", string_);
    fprintf(f, "\"comment\" = { fg = \"%s\", modifiers = [\"italic\"] }\n", comment);
    fprintf(f, "\"variable\" = \"%s\"\n", surface_text);
    fprintf(f, "\"variable.builtin\" = \"%s\"\n", tag);
    fprintf(f, "\"variable.parameter\" = \"%s\"\n", surface_text);
    fprintf(f, "\"variable.other.member\" = \"%s\"\n", surface_text);
    fprintf(f, "\"label\" = \"%s\"\n", tag);
    fprintf(f, "\"punctuation\" = \"%s\"\n", surface_text);
    fprintf(f, "\"punctuation.delimiter\" = \"%s\"\n", surface_text);
    fprintf(f, "\"keyword\" = \"%s\"\n", keyword);
    fprintf(f, "\"keyword.control\" = \"%s\"\n", keyword);
    fprintf(f, "\"operator\" = \"%s\"\n", surface_text);
    fprintf(f, "\"function\" = \"%s\"\n", function_);
    fprintf(f, "\"function.builtin\" = \"%s\"\n", function_);
    fprintf(f, "\"tag\" = \"%s\"\n", tag);
    fprintf(f, "\"namespace\" = \"%s\"\n", type_);
    fprintf(f, "\"special\" = \"%s\"\n", tag);
    fprintf(f, "\"markup.heading\" = { fg = \"%s\", modifiers = [\"bold\"] }\n", primary);
    fputs("\"markup.bold\" = { modifiers = [\"bold\"] }\n", f);
    fputs("\"markup.italic\" = { modifiers = [\"italic\"] }\n", f);
    fprintf(f, "\"diff.plus\" = \"%s\"\n", string_);
    fprintf(f, "\"diff.minus\" = \"%s\"\n", error_hex);
    fprintf(f, "\"diff.delta\" = \"%s\"\n", type_);
    fprintf(f, "\"error\" = \"%s\"\n", error_hex);
    fprintf(f, "\"warning\" = \"%s\"\n", type_);
    fprintf(f, "\"info\" = \"%s\"\n", function_);
    fprintf(f, "\"hint\" = \"%s\"\n", constant_);

    fputs("\n", f);
    fprintf(f, "\"ui.background\" = { bg = \"%s\" }\n", surface);
    fprintf(f, "\"ui.background.separator\" = { fg = \"%s\" }\n", outline);
    fprintf(f, "\"ui.text\" = \"%s\"\n", surface_text);
    fprintf(f, "\"ui.text.focus\" = \"%s\"\n", surface_text);
    fprintf(f, "\"ui.text.inactive\" = \"%s\"\n", surface_container_high);
    fprintf(f, "\"ui.cursor.primary\" = { fg = \"%s\", bg = \"%s\" }\n", surface, primary);
    fprintf(f, "\"ui.cursor.match\" = { fg = \"%s\", bg = \"%s\" }\n", surface_text,
            primary_container);
    fprintf(f, "\"ui.selection\" = { bg = \"%s\" }\n", primary_container);
    fprintf(f, "\"ui.selection.primary\" = { bg = \"%s\" }\n", primary_container);
    fprintf(f, "\"ui.linenr\" = { fg = \"%s\" }\n", surface_container_high);
    fprintf(f, "\"ui.linenr.selected\" = { fg = \"%s\" }\n", surface_text);
    fprintf(f, "\"ui.statusline\" = { fg = \"%s\", bg = \"%s\" }\n", surface_text,
            surface_container);
    fprintf(f, "\"ui.statusline.inactive\" = { fg = \"%s\", bg = \"%s\" }\n",
            surface_container_high, surface_container);
    fprintf(f, "\"ui.popup\" = { fg = \"%s\", bg = \"%s\" }\n", surface_text, surface_container);
    fprintf(f, "\"ui.window\" = { fg = \"%s\" }\n", outline);
    fprintf(f, "\"ui.help\" = { fg = \"%s\", bg = \"%s\" }\n", surface_text, surface_container);
    fprintf(f, "\"ui.menu\" = { fg = \"%s\", bg = \"%s\" }\n", surface_text, surface_container);
    fprintf(f, "\"ui.menu.selected\" = { fg = \"%s\", bg = \"%s\" }\n", primary_text, primary);
    fprintf(f, "\"ui.virtual.whitespace\" = { fg = \"%s\" }\n", surface_container_high);
    fprintf(f, "\"ui.virtual.ruler\" = { bg = \"%s\" }\n", surface_container);
    fprintf(f, "\"ui.virtual.jump-label\" = { fg = \"%s\", modifiers = [\"bold\"] }\n", primary);
    fprintf(f, "\"diagnostic.error\" = { underline = { color = \"%s\", style = \"curl\" } }\n",
            error_hex);
    fprintf(f, "\"diagnostic.warning\" = { underline = { color = \"%s\", style = \"curl\" } }\n",
            type_);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_helix(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping helix");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "helix", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping helix", dir);
        return;
    }

    char themes_dir[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(themes_dir, sizeof(themes_dir), "%s/themes", dir);
    ensure_dir(themes_dir);

    size_t len = 0;
    char *toml = build_helix_theme(light, &len);
    if (!toml) {
        dc_warn("systheme: failed to build helix dank.toml");
        return;
    }
    char theme_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(theme_path, sizeof(theme_path), "%s/dank.toml", themes_dir);
    dc_systheme_write_owned(theme_path, toml, len);
    free(toml);

    char config_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(config_path, sizeof(config_path), "%s/config.toml", dir);
    /* Bare top-level key MUST precede any [section] table -- ensure_line()
     * (append-at-end) would corrupt a config.toml that already has one;
     * see this file's header. */
    dc_systheme_ensure_line_top(config_path, DC_HELIX_THEME_LINE, DC_HELIX_MARKER);

    /* Best-effort nudge for already-running `hx` instances (helix's
     * documented config-reload signal), same unconditional-spawn precedent
     * as systheme_term2.c's ghostty pkill -USR2. */
    const char *argv[] = {"pkill", "-USR1", "hx", NULL};
    dc_systheme_spawn("helix-reload", argv, 3);

    dc_info("systheme: helix theme written to %s", themes_dir);
}

/* --- Neovim ---------------------------------------------------------------- */

static char *build_neovim_colors(bool light, size_t *out_len)
{
    const dc_theme *t = dc_theme_current;
    dank_syntax syn;
    build_syntax(light, &syn);

    char bg[8], fg[8], panel[8], panel_hi[8], border[8], accent[8], accent_fg[8], sel_bg[8];
    char error_fg[8], comment[8], string_[8], keyword[8], function_[8], type_[8], constant_[8],
            tag[8];
    dc_color ansi[16];
    dc_dynamic_ansi16(t, light, ansi);

    dc_systheme_hex_rgb(t->surface, bg);
    dc_systheme_hex_rgb(t->surface_text, fg);
    dc_systheme_hex_rgb(t->surface_container, panel);
    dc_systheme_hex_rgb(t->surface_container_high, panel_hi);
    dc_systheme_hex_rgb(t->outline, border);
    dc_systheme_hex_rgb(t->primary, accent);
    dc_systheme_hex_rgb(t->primary_text, accent_fg);
    dc_systheme_hex_rgb(t->primary_container, sel_bg);
    dc_systheme_hex_rgb(t->error, error_fg);
    dc_systheme_hex_rgb(syn.comment, comment);
    dc_systheme_hex_rgb(syn.string_, string_);
    dc_systheme_hex_rgb(syn.keyword, keyword);
    dc_systheme_hex_rgb(syn.function_, function_);
    dc_systheme_hex_rgb(syn.type_, type_);
    dc_systheme_hex_rgb(syn.constant_, constant_);
    dc_systheme_hex_rgb(syn.tag, tag);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("-- Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          "-- Activate with :colorscheme dank\n\n",
            f);
    fputs("vim.cmd(\"hi clear\")\n", f);
    fputs("if vim.fn.exists(\"syntax_on\") == 1 then\n  vim.cmd(\"syntax reset\")\nend\n\n", f);
    fprintf(f, "vim.o.background = \"%s\"\n", light ? "light" : "dark");
    fputs("vim.g.colors_name = \"dank\"\n\n", f);

    fputs("local hl = vim.api.nvim_set_hl\n\n", f);

    fprintf(f, "hl(0, \"Normal\", { fg = \"%s\", bg = \"%s\" })\n", fg, bg);
    fprintf(f, "hl(0, \"NormalFloat\", { fg = \"%s\", bg = \"%s\" })\n", fg, panel);
    fprintf(f, "hl(0, \"FloatBorder\", { fg = \"%s\", bg = \"%s\" })\n", border, panel);
    fprintf(f, "hl(0, \"Cursor\", { fg = \"%s\", bg = \"%s\" })\n", bg, accent);
    fprintf(f, "hl(0, \"CursorLine\", { bg = \"%s\" })\n", panel);
    fprintf(f, "hl(0, \"CursorLineNr\", { fg = \"%s\", bold = true })\n", fg);
    fprintf(f, "hl(0, \"LineNr\", { fg = \"%s\" })\n", panel_hi);
    fprintf(f, "hl(0, \"Visual\", { bg = \"%s\" })\n", sel_bg);
    fprintf(f, "hl(0, \"Search\", { fg = \"%s\", bg = \"%s\" })\n", bg, accent);
    fprintf(f, "hl(0, \"IncSearch\", { fg = \"%s\", bg = \"%s\" })\n", bg, accent);
    fprintf(f, "hl(0, \"Pmenu\", { fg = \"%s\", bg = \"%s\" })\n", fg, panel);
    fprintf(f, "hl(0, \"PmenuSel\", { fg = \"%s\", bg = \"%s\" })\n", accent_fg, accent);
    fprintf(f, "hl(0, \"StatusLine\", { fg = \"%s\", bg = \"%s\" })\n", fg, panel);
    fprintf(f, "hl(0, \"StatusLineNC\", { fg = \"%s\", bg = \"%s\" })\n", panel_hi, bg);
    fprintf(f, "hl(0, \"VertSplit\", { fg = \"%s\" })\n", border);
    fprintf(f, "hl(0, \"WinSeparator\", { fg = \"%s\" })\n", border);
    fprintf(f, "hl(0, \"SignColumn\", { bg = \"%s\" })\n", bg);
    fputs("\n", f);
    fprintf(f, "hl(0, \"Comment\", { fg = \"%s\", italic = true })\n", comment);
    fprintf(f, "hl(0, \"Constant\", { fg = \"%s\" })\n", constant_);
    fprintf(f, "hl(0, \"String\", { fg = \"%s\" })\n", string_);
    fprintf(f, "hl(0, \"Identifier\", { fg = \"%s\" })\n", fg);
    fprintf(f, "hl(0, \"Function\", { fg = \"%s\" })\n", function_);
    fprintf(f, "hl(0, \"Statement\", { fg = \"%s\" })\n", keyword);
    fprintf(f, "hl(0, \"Keyword\", { fg = \"%s\" })\n", keyword);
    fprintf(f, "hl(0, \"Type\", { fg = \"%s\" })\n", type_);
    fprintf(f, "hl(0, \"Special\", { fg = \"%s\" })\n", tag);
    fprintf(f, "hl(0, \"Error\", { fg = \"%s\", bg = \"%s\" })\n", error_fg, bg);
    fprintf(f, "hl(0, \"Todo\", { fg = \"%s\", bg = \"%s\", bold = true })\n", bg, type_);
    fputs("\n", f);
    fprintf(f, "hl(0, \"DiagnosticError\", { fg = \"%s\" })\n", error_fg);
    fprintf(f, "hl(0, \"DiagnosticWarn\", { fg = \"%s\" })\n", type_);
    fprintf(f, "hl(0, \"DiagnosticInfo\", { fg = \"%s\" })\n", function_);
    fprintf(f, "hl(0, \"DiagnosticHint\", { fg = \"%s\" })\n", constant_);
    fputs("\n", f);

    for (int i = 0; i < 16; i++) {
        char rgb[8];
        dc_systheme_hex_rgb(ansi[i], rgb);
        fprintf(f, "vim.g.terminal_color_%d = \"%s\"\n", i, rgb);
    }

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_neovim(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping neovim");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "nvim", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping neovim", dir);
        return;
    }

    char colors_dir[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_dir, sizeof(colors_dir), "%s/colors", dir);
    ensure_dir(colors_dir);

    size_t len = 0;
    char *lua = build_neovim_colors(light, &len);
    if (!lua) {
        dc_warn("systheme: failed to build nvim colors/dank.lua");
        return;
    }
    char path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(path, sizeof(path), "%s/dank.lua", colors_dir);
    dc_systheme_write_owned(path, lua, len);
    free(lua);

    /* Write-only: Neovim only loads a colorscheme on `:colorscheme dank` --
     * no init.lua edit, no reload spawn, see this file's header. */
    dc_info("systheme: neovim colorscheme written to %s (activate with :colorscheme dank)", path);
}

/* --- Vim ------------------------------------------------------------------- */

static char *build_vim_colors(bool light, size_t *out_len)
{
    const dc_theme *t = dc_theme_current;
    dank_syntax syn;
    build_syntax(light, &syn);

    char bg[8], fg[8], panel[8], panel_hi[8], border[8], accent[8], accent_fg[8], sel_bg[8];
    char error_fg[8], comment[8], string_[8], keyword[8], function_[8], type_[8], constant_[8],
            tag[8];
    dc_color ansi[16];
    dc_dynamic_ansi16(t, light, ansi);

    dc_systheme_hex_rgb(t->surface, bg);
    dc_systheme_hex_rgb(t->surface_text, fg);
    dc_systheme_hex_rgb(t->surface_container, panel);
    dc_systheme_hex_rgb(t->surface_container_high, panel_hi);
    dc_systheme_hex_rgb(t->outline, border);
    dc_systheme_hex_rgb(t->primary, accent);
    dc_systheme_hex_rgb(t->primary_text, accent_fg);
    dc_systheme_hex_rgb(t->primary_container, sel_bg);
    dc_systheme_hex_rgb(t->error, error_fg);
    dc_systheme_hex_rgb(syn.comment, comment);
    dc_systheme_hex_rgb(syn.string_, string_);
    dc_systheme_hex_rgb(syn.keyword, keyword);
    dc_systheme_hex_rgb(syn.function_, function_);
    dc_systheme_hex_rgb(syn.type_, type_);
    dc_systheme_hex_rgb(syn.constant_, constant_);
    dc_systheme_hex_rgb(syn.tag, tag);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("\" Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          "\" Activate with :colorscheme dank\n\n",
            f);
    fputs("hi clear\n", f);
    fputs("if exists(\"syntax_on\")\n  syntax reset\nendif\n\n", f);
    fprintf(f, "set background=%s\n", light ? "light" : "dark");
    fputs("let g:colors_name = \"dank\"\n\n", f);

    fprintf(f, "hi Normal guifg=%s guibg=%s\n", fg, bg);
    fprintf(f, "hi Cursor guifg=%s guibg=%s\n", bg, accent);
    fprintf(f, "hi CursorLine guibg=%s\n", panel);
    fprintf(f, "hi CursorLineNr guifg=%s gui=bold\n", fg);
    fprintf(f, "hi LineNr guifg=%s\n", panel_hi);
    fprintf(f, "hi Visual guibg=%s\n", sel_bg);
    fprintf(f, "hi Search guifg=%s guibg=%s\n", bg, accent);
    fprintf(f, "hi IncSearch guifg=%s guibg=%s\n", bg, accent);
    fprintf(f, "hi Pmenu guifg=%s guibg=%s\n", fg, panel);
    fprintf(f, "hi PmenuSel guifg=%s guibg=%s\n", accent_fg, accent);
    fprintf(f, "hi StatusLine guifg=%s guibg=%s\n", fg, panel);
    fprintf(f, "hi StatusLineNC guifg=%s guibg=%s\n", panel_hi, bg);
    fprintf(f, "hi VertSplit guifg=%s\n", border);
    fputs("\n", f);
    fprintf(f, "hi Comment guifg=%s gui=italic\n", comment);
    fprintf(f, "hi Constant guifg=%s\n", constant_);
    fprintf(f, "hi String guifg=%s\n", string_);
    fprintf(f, "hi Identifier guifg=%s\n", fg);
    fprintf(f, "hi Function guifg=%s\n", function_);
    fprintf(f, "hi Statement guifg=%s\n", keyword);
    fprintf(f, "hi Keyword guifg=%s\n", keyword);
    fprintf(f, "hi Type guifg=%s\n", type_);
    fprintf(f, "hi Special guifg=%s\n", tag);
    fprintf(f, "hi Error guifg=%s guibg=%s\n", error_fg, bg);
    fprintf(f, "hi Todo guifg=%s guibg=%s gui=bold\n", bg, type_);
    fputs("\n", f);
    fprintf(f, "hi DiffAdd guibg=%s\n", sel_bg);
    fprintf(f, "hi DiffDelete guibg=%s\n", error_fg);
    fputs("\n", f);

    fputs("let g:terminal_ansi_colors = [", f);
    for (int i = 0; i < 16; i++) {
        char rgb[8];
        dc_systheme_hex_rgb(ansi[i], rgb);
        fprintf(f, "'%s'%s", rgb, i < 15 ? ", " : "");
    }
    fputs("]\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_vim(bool light)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("systheme: no $HOME, skipping vim");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.vim", home);
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping vim", dir);
        return;
    }

    char colors_dir[DC_SYSTHEME_PATH_MAX + 16];
    snprintf(colors_dir, sizeof(colors_dir), "%s/colors", dir);
    ensure_dir(colors_dir);

    size_t len = 0;
    char *vimscript = build_vim_colors(light, &len);
    if (!vimscript) {
        dc_warn("systheme: failed to build vim colors/dank.vim");
        return;
    }
    char path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/dank.vim", colors_dir);
    dc_systheme_write_owned(path, vimscript, len);
    free(vimscript);

    dc_info("systheme: vim colorscheme written to %s (activate with :colorscheme dank)", path);
}

/* --- Sublime Text -------------------------------------------------------- */

#define DC_SUBLIME_SCHEME_RESOURCE_PATH "Packages/User/DankC.sublime-color-scheme"

static char *build_sublime_scheme(bool light, size_t *out_len)
{
    const dc_theme *t = dc_theme_current;
    dank_syntax syn;
    build_syntax(light, &syn);

    char bg[8], fg[8], panel[8], border[8], accent[8], accent_fg[8], sel_bg[8], error_hex[8];
    char comment[8], string_[8], keyword[8], function_[8], type_[8], constant_[8], tag[8];

    dc_systheme_hex_rgb(t->surface, bg);
    dc_systheme_hex_rgb(t->surface_text, fg);
    dc_systheme_hex_rgb(t->surface_container, panel);
    dc_systheme_hex_rgb(t->outline, border);
    dc_systheme_hex_rgb(t->primary, accent);
    dc_systheme_hex_rgb(t->primary_text, accent_fg);
    dc_systheme_hex_rgb(t->primary_container, sel_bg);
    dc_systheme_hex_rgb(t->error, error_hex);
    dc_systheme_hex_rgb(syn.comment, comment);
    dc_systheme_hex_rgb(syn.string_, string_);
    dc_systheme_hex_rgb(syn.keyword, keyword);
    dc_systheme_hex_rgb(syn.function_, function_);
    dc_systheme_hex_rgb(syn.type_, type_);
    dc_systheme_hex_rgb(syn.constant_, constant_);
    dc_systheme_hex_rgb(syn.tag, tag);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("{\n", f);
    fputs("    \"name\": \"DankC\",\n", f);
    fputs("    \"author\": \"DankC\",\n", f);
    fputs("    \"globals\": {\n", f);
    fprintf(f, "        \"background\": \"%s\",\n", bg);
    fprintf(f, "        \"foreground\": \"%s\",\n", fg);
    fprintf(f, "        \"caret\": \"%s\",\n", accent);
    fprintf(f, "        \"block_caret\": \"%s\",\n", accent);
    fprintf(f, "        \"invisibles\": \"%s\",\n", border);
    fprintf(f, "        \"guide\": \"%s\",\n", panel);
    fprintf(f, "        \"active_guide\": \"%s\",\n", border);
    fprintf(f, "        \"selection\": \"%s\",\n", sel_bg);
    fprintf(f, "        \"selection_foreground\": \"%s\",\n", fg);
    fprintf(f, "        \"selection_border\": \"%s\",\n", accent);
    fprintf(f, "        \"inactive_selection\": \"%s\",\n", panel);
    fprintf(f, "        \"line_highlight\": \"%s\",\n", panel);
    fprintf(f, "        \"gutter\": \"%s\",\n", bg);
    fprintf(f, "        \"gutter_foreground\": \"%s\",\n", panel);
    fprintf(f, "        \"find_highlight\": \"%s\",\n", accent);
    fprintf(f, "        \"find_highlight_foreground\": \"%s\",\n", accent_fg);
    fputs("        \"brackets_options\": \"underline\",\n", f);
    fprintf(f, "        \"brackets_foreground\": \"%s\",\n", tag);
    fputs("        \"tags_options\": \"stippled_underline\"\n", f);
    fputs("    },\n", f);
    fputs("    \"rules\": [\n", f);
    fprintf(f, "        { \"scope\": \"comment\", \"foreground\": \"%s\", \"font_style\": "
               "\"italic\" },\n",
            comment);
    fprintf(f, "        { \"scope\": \"string\", \"foreground\": \"%s\" },\n", string_);
    fprintf(f, "        { \"scope\": \"constant.numeric, constant.language, "
               "constant.character\", \"foreground\": \"%s\" },\n",
            constant_);
    fprintf(f, "        { \"scope\": \"keyword, keyword.control\", \"foreground\": \"%s\" },\n",
            keyword);
    fprintf(f, "        { \"scope\": \"storage, storage.type, storage.modifier\", "
               "\"foreground\": \"%s\" },\n",
            keyword);
    fprintf(f, "        { \"scope\": \"entity.name.function, support.function\", "
               "\"foreground\": \"%s\" },\n",
            function_);
    fprintf(f, "        { \"scope\": \"entity.name.class, entity.name.type, support.class, "
               "support.type\", \"foreground\": \"%s\" },\n",
            type_);
    fprintf(f, "        { \"scope\": \"entity.name.tag\", \"foreground\": \"%s\" },\n", tag);
    fprintf(f, "        { \"scope\": \"entity.other.attribute-name\", \"foreground\": \"%s\" "
               "},\n",
            type_);
    fprintf(f, "        { \"scope\": \"variable\", \"foreground\": \"%s\" },\n", fg);
    fprintf(f, "        { \"scope\": \"invalid, invalid.illegal\", \"foreground\": \"%s\", "
               "\"background\": \"%s\" }\n",
            bg, error_hex);
    fputs("    ]\n", f);
    fputs("}\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_sublime(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping sublime");
        return;
    }
    char user_dir[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(user_dir, sizeof(user_dir), "%s/sublime-text/Packages/User", base);
    if (!dc_systheme_dir_exists(user_dir)) {
        dc_info("systheme: %s does not exist, skipping sublime", user_dir);
        return;
    }

    size_t len = 0;
    char *scheme = build_sublime_scheme(light, &len);
    if (!scheme) {
        dc_warn("systheme: failed to build sublime DankC.sublime-color-scheme");
        return;
    }
    char scheme_path[DC_SYSTHEME_PATH_MAX + 96];
    snprintf(scheme_path, sizeof(scheme_path), "%s/DankC.sublime-color-scheme", user_dir);
    dc_systheme_write_owned(scheme_path, scheme, len);
    free(scheme);

    char settings_path[DC_SYSTHEME_PATH_MAX + 96];
    snprintf(settings_path, sizeof(settings_path), "%s/Preferences.sublime-settings", user_dir);
    /* Only one possible dankc-owned value here (the fixed resource path) --
     * see this file's header -- so the "already ours" test is exact
     * equality with that same value. */
    json_set_string_key(settings_path, "color_scheme", DC_SUBLIME_SCHEME_RESOURCE_PATH,
            DC_SUBLIME_SCHEME_RESOURCE_PATH, "sublime");

    dc_info("systheme: sublime color scheme written to %s", scheme_path);
}

/* --- Emacs ------------------------------------------------------------- */

static char *build_emacs_theme(bool light, size_t *out_len)
{
    const dc_theme *t = dc_theme_current;
    dank_syntax syn;
    build_syntax(light, &syn);

    char bg[8], fg[8], panel[8], panel_fg[8], accent[8], sel_bg[8], border[8], error_hex[8];
    char warning_hex[8], success_hex[8];
    char comment[8], string_[8], keyword[8], function_[8], type_[8], constant_[8];

    dc_systheme_hex_rgb(t->surface, bg);
    dc_systheme_hex_rgb(t->surface_text, fg);
    dc_systheme_hex_rgb(t->surface_container, panel);
    dc_systheme_hex_rgb(t->surface_container_high, panel_fg);
    dc_systheme_hex_rgb(t->primary, accent);
    dc_systheme_hex_rgb(t->primary_container, sel_bg);
    dc_systheme_hex_rgb(t->outline, border);
    dc_systheme_hex_rgb(t->error, error_hex);
    dc_systheme_hex_rgb(t->warning, warning_hex);
    dc_systheme_hex_rgb(t->success, success_hex);
    dc_systheme_hex_rgb(syn.comment, comment);
    dc_systheme_hex_rgb(syn.string_, string_);
    dc_systheme_hex_rgb(syn.keyword, keyword);
    dc_systheme_hex_rgb(syn.function_, function_);
    dc_systheme_hex_rgb(syn.type_, type_);
    dc_systheme_hex_rgb(syn.constant_, constant_);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs(";; Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          ";; Add (add-to-list 'custom-theme-load-path \"~/.emacs.d/\") and\n"
          ";; (load-theme 'dank t) to your init file to activate.\n\n",
            f);
    fprintf(f, "(deftheme dank \"DankC generated theme (%s)\")\n\n", light ? "light" : "dark");
    fputs("(custom-theme-set-faces\n", f);
    fputs(" 'dank\n", f);
    fprintf(f, " '(default ((t (:background \"%s\" :foreground \"%s\"))))\n", bg, fg);
    fprintf(f, " '(cursor ((t (:background \"%s\"))))\n", accent);
    fprintf(f, " '(region ((t (:background \"%s\"))))\n", sel_bg);
    fprintf(f, " '(fringe ((t (:background \"%s\"))))\n", bg);
    fprintf(f, " '(vertical-border ((t (:foreground \"%s\"))))\n", border);
    fprintf(f, " '(mode-line ((t (:background \"%s\" :foreground \"%s\"))))\n", panel, fg);
    fprintf(f, " '(mode-line-inactive ((t (:background \"%s\" :foreground \"%s\"))))\n", bg,
            panel_fg);
    fprintf(f, " '(fixed-pitch ((t (:foreground \"%s\"))))\n", fg);
    fprintf(f, " '(line-number ((t (:foreground \"%s\"))))\n", panel_fg);
    fprintf(f, " '(line-number-current-line ((t (:foreground \"%s\"))))\n", fg);
    fprintf(f, " '(font-lock-comment-face ((t (:foreground \"%s\" :slant italic))))\n", comment);
    fprintf(f, " '(font-lock-string-face ((t (:foreground \"%s\"))))\n", string_);
    fprintf(f, " '(font-lock-keyword-face ((t (:foreground \"%s\"))))\n", keyword);
    fprintf(f, " '(font-lock-function-name-face ((t (:foreground \"%s\"))))\n", function_);
    fprintf(f, " '(font-lock-variable-name-face ((t (:foreground \"%s\"))))\n", fg);
    fprintf(f, " '(font-lock-type-face ((t (:foreground \"%s\"))))\n", type_);
    fprintf(f, " '(font-lock-constant-face ((t (:foreground \"%s\"))))\n", constant_);
    fprintf(f, " '(minibuffer-prompt ((t (:foreground \"%s\"))))\n", accent);
    fprintf(f, " '(highlight ((t (:background \"%s\"))))\n", panel);
    fprintf(f, " '(isearch ((t (:background \"%s\" :foreground \"%s\"))))\n", accent, bg);
    fprintf(f, " '(lazy-highlight ((t (:background \"%s\"))))\n", sel_bg);
    fprintf(f, " '(link ((t (:foreground \"%s\" :underline t))))\n", accent);
    fprintf(f, " '(error ((t (:foreground \"%s\"))))\n", error_hex);
    fprintf(f, " '(warning ((t (:foreground \"%s\"))))\n", warning_hex);
    fprintf(f, " '(success ((t (:foreground \"%s\"))))\n", success_hex);
    fputs(")\n\n", f);
    fputs("(provide-theme 'dank)\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_emacs(bool light)
{
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("systheme: no $HOME, skipping emacs");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.emacs.d", home);
    if (!dc_systheme_dir_exists(dir)) {
        /* Also check the XDG-style ~/.config/emacs layout newer Emacs
         * versions support before giving up -- but only ever write into
         * whichever one already exists, never create either. */
        char xdg_base[DC_SYSTHEME_PATH_MAX];
        if (config_home(xdg_base, sizeof(xdg_base))) {
            char xdg_dir[DC_SYSTHEME_PATH_MAX + 16];
            app_dir(xdg_base, "emacs", xdg_dir, sizeof(xdg_dir));
            if (dc_systheme_dir_exists(xdg_dir)) {
                snprintf(dir, sizeof(dir), "%s", xdg_dir);
            } else {
                dc_info("systheme: neither ~/.emacs.d nor $XDG_CONFIG_HOME/emacs exists, "
                        "skipping emacs");
                return;
            }
        } else {
            dc_info("systheme: %s does not exist, skipping emacs", dir);
            return;
        }
    }

    size_t len = 0;
    char *theme = build_emacs_theme(light, &len);
    if (!theme) {
        dc_warn("systheme: failed to build emacs dank-theme.el");
        return;
    }
    char path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/dank-theme.el", dir);
    dc_systheme_write_owned(path, theme, len);
    free(theme);

    dc_info("systheme: emacs theme written to %s (add its directory to "
            "custom-theme-load-path and (load-theme 'dank t) to activate)",
            path);
}
