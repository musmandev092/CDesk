/* systheme_apps.c — system-wide theming Task 3: the VS Code + Qt (qt5ct/
 * qt6ct) emitters (see systheme_apps.h). Reuses systheme_internal.h's
 * atomic-write / marker+backup / dryrun / dir-exists / spawn primitives
 * exactly like Task 2's systheme_term.c; see systheme.c's file header for
 * the full safety contract (opt-in only, dankc-owned files written
 * atomically, user-owned files backed up before their first real edit,
 * DANKC_THEME_DRYRUN gates every write/spawn, an app's config dir is never
 * created by dankc).
 *
 * --- VS Code -----------------------------------------------------------
 *
 * settings.json is a *user-owned* file where dankc only ever owns one key,
 * "workbench.colorCustomizations" -- every other key (font settings,
 * extension config, etc.) must survive byte-for-byte. That rules out
 * systheme_internal.h's dc_systheme_write_owned() (for files dankc fully
 * owns) and dc_systheme_ensure_line() (a plain-text line/block appender,
 * not JSON-aware) -- this file round-trips the whole document through
 * cJSON instead: parse, replace exactly the one top-level key, re-serialize,
 * write atomically.
 *
 * VS Code's settings.json is technically JSONC (`//` comments, trailing
 * commas are tolerated by VS Code's own editor/parser) but cJSON is a
 * strict-JSON parser and will reject any file that actually uses those
 * extensions. Re-serializing a cJSON tree also can't preserve comments even
 * for a file that DOES happen to be strict JSON (cJSON has no concept of
 * them). Given that, this emitter treats "cJSON_Parse fails" as "back off
 * entirely" -- logging a warning and touching nothing -- rather than ever
 * attempting a partial/lossy rewrite of a file it can't fully parse.
 *
 * Idempotency + "backup before first write" both hinge on a single
 * marker: dankc's own colorCustomizations block always carries a "//" key
 * (the common poor-man's-JSON-comment trick -- VS Code's schema for this
 * object ignores unrecognized keys) naming dankc as the owner. On each
 * apply: if the settings.json we just parsed already has that marker in its
 * *current* colorCustomizations object, this is a re-apply of a file dankc
 * already owns end to end -- no backup (the pristine pre-dankc copy was
 * already captured the first time). If the marker is absent -- either a
 * fresh file with no colorCustomizations at all, or the user's own
 * hand-authored block we're about to replace -- back it up
 * (timestamped, matching dc_systheme_ensure_line()'s own backup naming)
 * before writing.
 *
 * --- Qt (qt5ct/qt6ct) ----------------------------------------------------
 *
 * qt5ct/qt6ct read their palette from a fully dankc-owned INI
 * (colors/dankc.conf, a 21-role QPalette::ColorRole dump -- see
 * build_qt_colorscheme()'s comment for the exact role mapping), so that file
 * is written like dank-colors.css: unconditionally, atomically, no backup
 * needed. What IS user-owned is qt5ct.conf/qt6ct.conf's [Appearance]
 * section (custom_palette=/color_scheme_path=), which this patches via
 * dc_systheme_ensure_line() -- same "append a fresh duplicate [Appearance]
 * block if our path isn't already the configured one" precedent (and the
 * same documented caveat about hand-rolled-stricter INI parsers) as
 * systheme_term.c's alacritty.toml/foot.ini patching.
 */
#include "services/systheme_apps.h"

#include "services/systheme_internal.h"

#include "cJSON.h"
#include "core/log.h"
#include "theme/dynamic.h"
#include "theme/theme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* --- shared path helpers (each systheme_*.c keeps its own tiny copy, same
 * precedent as systheme_term.c's config_home()/app_dir()) ----------------- */

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

/* Timestamped backup, same naming/behaviour as dc_systheme_ensure_line()'s
 * own backup step (systheme.c) -- duplicated here because that one is
 * folded into the line-injector and this file needs it standalone around a
 * full-document JSON rewrite. DRYRUN-gated; returns false (and logs) only on
 * a real I/O failure, in which case the caller must abort the whole write
 * rather than risk losing the user's original file. */
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

/* --- VS Code -------------------------------------------------------------- */

#define DC_VSCODE_BLOCK_KEY "workbench.colorCustomizations"
#define DC_VSCODE_MARKER_KEY "//"
#define DC_VSCODE_MARKER_VAL \
    "Managed by DankC (Settings > Theme & Colors) -- edits here are overwritten on theme change"

/* Tries, in order, the three known VS Code / VSCodium user-data layouts.
 * "Code - OSS" (note the space) is the upstream-built-from-source package
 * name used by several distros; "Code" is the official Microsoft build;
 * "VSCodium" is the de-Microsoft-branded community build. Detection IS the
 * settings.json existing -- never fall back to creating one. */
static bool find_vscode_settings(char *out_path, size_t cap)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base)))
        return false;

    static const char *const kAppDirs[] = {"Code - OSS", "Code", "VSCodium"};
    for (size_t i = 0; i < sizeof(kAppDirs) / sizeof(kAppDirs[0]); i++) {
        char path[DC_SYSTHEME_PATH_MAX + 64];
        snprintf(path, sizeof(path), "%s/%s/User/settings.json", base, kAppDirs[i]);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out_path, cap, "%s", path);
            return true;
        }
    }
    return false;
}

/* Builds the dankc-owned "workbench.colorCustomizations" object (including
 * its own "//" ownership marker -- see this file's header). Role mapping
 * mirrors systheme.c's GTK emitter and systheme_term.c's terminal emitters
 * (same palette, same intent: editor/sidebar/status chrome from
 * surface/surface_container tones, accent chrome from primary, ANSI-16 from
 * dc_dynamic_ansi16() so VS Code's integrated terminal matches the
 * standalone terminal emitters exactly):
 *   editor.background/foreground        <- surface / surface_text
 *   sideBar.background/foreground       <- surface_container / surface_text
 *   activityBar.background/foreground   <- surface_container_high / surface_text
 *   statusBar.background/foreground     <- primary / primary_text
 *   titleBar.active*                    <- surface_container / surface_text
 *   terminal.ansi<Name>/ansiBright<Name> <- dc_dynamic_ansi16() out[0..15]
 */
static cJSON *build_vscode_block(bool light)
{
    char surface[8], surface_text[8], surface_container[8], surface_container_high[8];
    char primary[8], primary_text[8];
    char ansi[16][8];

    dc_systheme_hex_rgb(dc_theme_current->surface, surface);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, surface_text);
    dc_systheme_hex_rgb(dc_theme_current->surface_container, surface_container);
    dc_systheme_hex_rgb(dc_theme_current->surface_container_high, surface_container_high);
    dc_systheme_hex_rgb(dc_theme_current->primary, primary);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, primary_text);

    dc_color ansi_colors[16];
    dc_dynamic_ansi16(dc_theme_current, light, ansi_colors);
    for (int i = 0; i < 16; i++)
        dc_systheme_hex_rgb(ansi_colors[i], ansi[i]);

    static const char *const kAnsiNames[8] = {"Black", "Red", "Green", "Yellow",
            "Blue", "Magenta", "Cyan", "White"};

    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;

    cJSON_AddStringToObject(o, DC_VSCODE_MARKER_KEY, DC_VSCODE_MARKER_VAL);

    cJSON_AddStringToObject(o, "editor.background", surface);
    cJSON_AddStringToObject(o, "editor.foreground", surface_text);
    cJSON_AddStringToObject(o, "sideBar.background", surface_container);
    cJSON_AddStringToObject(o, "sideBar.foreground", surface_text);
    cJSON_AddStringToObject(o, "activityBar.background", surface_container_high);
    cJSON_AddStringToObject(o, "activityBar.foreground", surface_text);
    cJSON_AddStringToObject(o, "statusBar.background", primary);
    cJSON_AddStringToObject(o, "statusBar.foreground", primary_text);
    cJSON_AddStringToObject(o, "titleBar.activeBackground", surface_container);
    cJSON_AddStringToObject(o, "titleBar.activeForeground", surface_text);

    for (int i = 0; i < 8; i++) {
        char key[32];
        snprintf(key, sizeof(key), "terminal.ansi%s", kAnsiNames[i]);
        cJSON_AddStringToObject(o, key, ansi[i]);
    }
    for (int i = 0; i < 8; i++) {
        char key[32];
        snprintf(key, sizeof(key), "terminal.ansiBright%s", kAnsiNames[i]);
        cJSON_AddStringToObject(o, key, ansi[8 + i]);
    }

    return o;
}

void dc_systheme_apply_vscode(bool light)
{
    char path[DC_SYSTHEME_PATH_MAX + 64];
    if (!find_vscode_settings(path, sizeof(path))) {
        dc_info("systheme: no Code - OSS/Code/VSCodium settings.json found, skipping vscode");
        return;
    }

    char *text = read_whole_file(path);
    if (!text) {
        dc_warn("systheme: could not read %s, skipping vscode", path);
        return;
    }

    cJSON *root = cJSON_Parse(text);
    if (!root) {
        /* VS Code's settings.json legally allows JSONC comments/trailing
         * commas; cJSON is strict-JSON only. Never rewrite a file we
         * couldn't fully parse -- see this file's header. */
        dc_warn("systheme: %s did not parse as strict JSON (VS Code allows JSONC "
                "comments/trailing commas) -- skipping vscode theming rather than risk "
                "corrupting it",
                path);
        free(text);
        return;
    }
    if (!cJSON_IsObject(root)) {
        dc_warn("systheme: %s top level is not a JSON object, skipping vscode", path);
        cJSON_Delete(root);
        free(text);
        return;
    }

    bool already_dankc = false;
    cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, DC_VSCODE_BLOCK_KEY);
    if (cJSON_IsObject(existing)) {
        cJSON *marker = cJSON_GetObjectItemCaseSensitive(existing, DC_VSCODE_MARKER_KEY);
        already_dankc = cJSON_IsString(marker) && marker->valuestring &&
                strstr(marker->valuestring, "DankC") != NULL;
    }

    if (!already_dankc && !backup_timestamped(path, text)) {
        /* Backup failed for real (not dryrun): abort rather than risk the
         * user's original file, matching dc_systheme_ensure_line()'s own
         * contract. */
        cJSON_Delete(root);
        free(text);
        return;
    }
    free(text);

    cJSON *block = build_vscode_block(light);
    if (!block) {
        dc_warn("systheme: failed to build vscode colorCustomizations block");
        cJSON_Delete(root);
        return;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(root, DC_VSCODE_BLOCK_KEY);
    cJSON_AddItemToObject(root, DC_VSCODE_BLOCK_KEY, block);

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) {
        dc_warn("systheme: failed to serialize %s, skipping vscode", path);
        return;
    }

    dc_systheme_write_owned(path, out, strlen(out));
    free(out);

    /* VS Code watches and applies settings.json live -- no reload spawn. */
    dc_info("systheme: vscode theme written to %s", path);
}

/* --- Qt (qt5ct/qt6ct) ------------------------------------------------------ */

/* QPalette::ColorRole order, exactly as qt5ct/qt6ct serialize their
 * colors/dankc.conf [ColorScheme] active_colors/inactive_colors/
 * disabled_colors lists (21 entries per list, including the
 * unused-but-present NoRole slot). */
enum {
    QT_WINDOW_TEXT = 0,
    QT_BUTTON,
    QT_LIGHT,
    QT_MIDLIGHT,
    QT_DARK,
    QT_MID,
    QT_TEXT,
    QT_BRIGHT_TEXT,
    QT_BUTTON_TEXT,
    QT_BASE,
    QT_WINDOW,
    QT_SHADOW,
    QT_HIGHLIGHT,
    QT_HIGHLIGHTED_TEXT,
    QT_LINK,
    QT_LINK_VISITED,
    QT_ALTERNATE_BASE,
    QT_NO_ROLE,
    QT_TOOLTIP_BASE,
    QT_TOOLTIP_TEXT,
    QT_PLACEHOLDER_TEXT,
    QT_ROLE_COUNT,
};

static dc_color blend(dc_color a, dc_color b, float t)
{
    dc_color out;
    out.r = (uint8_t)(a.r + ((float)b.r - (float)a.r) * t);
    out.g = (uint8_t)(a.g + ((float)b.g - (float)a.g) * t);
    out.b = (uint8_t)(a.b + ((float)b.b - (float)a.b) * t);
    out.a = 255;
    return out;
}

/* Maps dc_theme_current's roles onto the 21 QPalette::ColorRole slots.
 * Everything reuses an existing dc_theme field (no invented tones):
 *   WindowText/Text/ButtonText/BrightText <- surface_text
 *   Button/Midlight/AlternateBase/ToolTipBase <- surface_container(_high)
 *   Light/Dark/Shadow                     <- surface_container_highest/lowest
 *   Mid/NoRole/Window/Base                <- surface / surface_container_low
 *   Highlight/Link                        <- primary
 *   HighlightedText                       <- primary_text
 *   LinkVisited                           <- secondary
 *   PlaceholderText                       <- surface_variant_text
 */
static void build_active_roles(dc_color out[QT_ROLE_COUNT])
{
    const dc_theme *t = dc_theme_current;
    out[QT_WINDOW_TEXT] = t->surface_text;
    out[QT_BUTTON] = t->surface_container;
    out[QT_LIGHT] = t->surface_container_highest;
    out[QT_MIDLIGHT] = t->surface_container_high;
    out[QT_DARK] = t->surface_container_lowest;
    out[QT_MID] = t->surface_container_low;
    out[QT_TEXT] = t->surface_text;
    out[QT_BRIGHT_TEXT] = t->surface_text;
    out[QT_BUTTON_TEXT] = t->surface_text;
    out[QT_BASE] = t->surface;
    out[QT_WINDOW] = t->surface;
    out[QT_SHADOW] = t->surface_container_lowest;
    out[QT_HIGHLIGHT] = t->primary;
    out[QT_HIGHLIGHTED_TEXT] = t->primary_text;
    out[QT_LINK] = t->primary;
    out[QT_LINK_VISITED] = t->secondary;
    out[QT_ALTERNATE_BASE] = t->surface_container_low;
    out[QT_NO_ROLE] = t->surface;
    out[QT_TOOLTIP_BASE] = t->surface_container_high;
    out[QT_TOOLTIP_TEXT] = t->surface_text;
    out[QT_PLACEHOLDER_TEXT] = t->surface_variant_text;
}

/* Appends "<#AARRGGBB>[, ...]" for all 21 roles, blending each active colour
 * `dim` of the way toward the Window tone (roles[QT_WINDOW]) -- qt5ct/qt6ct
 * have no separate "derive disabled/inactive automatically" mode, so this
 * approximates Qt's usual washed-out disabled/inactive look. */
static void append_role_list(FILE *f, const char *key, const dc_color roles[QT_ROLE_COUNT],
        float dim)
{
    fprintf(f, "%s=", key);
    dc_color window = roles[QT_WINDOW];
    for (int i = 0; i < QT_ROLE_COUNT; i++) {
        dc_color c = dim > 0.0f ? blend(roles[i], window, dim) : roles[i];
        char hex[10];
        dc_systheme_hex_argb(c, hex);
        fprintf(f, "%s%s", hex, i + 1 < QT_ROLE_COUNT ? ", " : "\n");
    }
}

static char *build_qt_colorscheme(size_t *out_len)
{
    dc_color roles[QT_ROLE_COUNT];
    build_active_roles(roles);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("; Generated by DankC's system theming engine (Settings > Theme & Colors)\n",
            f);
    fputs("[ColorScheme]\n", f);
    append_role_list(f, "active_colors", roles, 0.0f);
    append_role_list(f, "inactive_colors", roles, 0.15f);
    append_role_list(f, "disabled_colors", roles, 0.45f);

    fclose(f);
    *out_len = len;
    return buf;
}

#define DC_QT_MARKER "; Added by DankC Settings > Theme & Colors"

static void ensure_dir(const char *dir)
{
    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would mkdir -p %s", dir);
        return;
    }
    mkdir(dir, 0755); /* ignore EEXIST/errors: a real failure just surfaces as
                        * a write failure from dc_systheme_write_owned() right
                        * after, which already logs. */
}

static void apply_qt_variant(const char *appdir_name, const char *conf_name)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping %s", appdir_name);
        return;
    }

    char appdir[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(appdir, sizeof(appdir), "%s/%s", base, appdir_name);
    if (!dc_systheme_dir_exists(appdir)) {
        /* Never create an app's config dir -- if qt5ct/qt6ct itself isn't
         * in use on this machine, skip it silently (same contract as
         * systheme.c's apply_gtk_variant()). */
        dc_info("systheme: %s does not exist, skipping %s", appdir, appdir_name);
        return;
    }

    char colors_dir[DC_SYSTHEME_PATH_MAX + 48];
    char theme_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(colors_dir, sizeof(colors_dir), "%s/colors", appdir);
    snprintf(theme_path, sizeof(theme_path), "%s/dankc.conf", colors_dir);

    /* colors/ is a conventional subdirectory of an app dir that DOES already
     * exist (just checked above) -- not "creating the app's config dir",
     * so it's safe/necessary to create if this is qt5ct/qt6ct's first-ever
     * custom colour scheme. */
    ensure_dir(colors_dir);

    size_t len = 0;
    char *ini = build_qt_colorscheme(&len);
    if (!ini) {
        dc_warn("systheme: failed to build %s dankc.conf", appdir_name);
        return;
    }
    dc_systheme_write_owned(theme_path, ini, len);
    free(ini);

    char conf_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(conf_path, sizeof(conf_path), "%s/%s", appdir, conf_name);

    char patch[DC_SYSTHEME_PATH_MAX + 128];
    snprintf(patch, sizeof(patch), "[Appearance]\ncustom_palette=true\ncolor_scheme_path=%s",
            theme_path);
    /* Needle = the absolute dankc.conf path itself: idempotent once that
     * exact path is set as color_scheme_path anywhere in the file. If a
     * different color_scheme_path was previously set, this appends a fresh
     * [Appearance] block after backing up (same duplicate-section caveat as
     * systheme_term.c's alacritty.toml/foot.ini patching -- QSettings' INI
     * reader tolerates a repeated section header, later keys win). */
    dc_systheme_ensure_line(conf_path, theme_path, DC_QT_MARKER, patch);

    /* qt5ct/qt6ct only read their palette + config at startup -- restart
     * required, no reload spawn (same as foot). */
    dc_info("systheme: %s theme written to %s", appdir_name, colors_dir);
}

void dc_systheme_apply_qt(bool light)
{
    (void)light; /* Qt palette has no distinct light/dark tuning beyond
                   * what's already baked into dc_theme_current -- matches
                   * the GTK emitter, which also derives its whole palette
                   * from dc_theme_current without a separate `light` path
                   * (only the gsettings reload-nudge cares about `light`
                   * there). */
    apply_qt_variant("qt5ct", "qt5ct.conf");
    apply_qt_variant("qt6ct", "qt6ct.conf");
}
