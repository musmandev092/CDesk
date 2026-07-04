/* systheme_misc.c — system-wide theming Task T7 (see
 * docs/21-THEMING-COVERAGE-PLAN.md): btop, cava, zathura, spicetify (Spotify).
 * See systheme_misc.h for the per-function contract summary; reuses every
 * atomic-write / marker+backup / dryrun / detection / spawn primitive from
 * systheme_internal.h exactly like Task T4's systheme_editors.c -- see
 * systheme.c's file header for the full safety contract (opt-in only,
 * dankc-owned files written atomically, user-owned files only ever nudged
 * with a one-time backup, DANKC_THEME_DRYRUN gates every write/spawn, an
 * app's own config dir is never created by dankc).
 *
 * --- btop ------------------------------------------------------------------
 *
 * ~/.config/btop/themes/dank.theme is a dankc-owned btop theme file: bash-
 * style `theme[key]="#rrggbb"` assignments (btop's own native format, see
 * upstream's `themes/` directory for the reference schema). Chrome keys
 * (main_bg/main_fg/title/hi_fg/selected_bg/selected_fg/inactive_fg/
 * graph_text/meter_bg/div_line/proc_misc/cpu_box/mem_box/net_box/proc_box)
 * come straight from dc_theme_current's surface/primary roles; the three
 * graph widgets (cpu, mem, net) each get a 3-stop start/mid/end gradient
 * built from primary/success/warning/error (any key btop doesn't find in a
 * theme file falls back to its own hardcoded default, so a deliberately
 * partial theme -- e.g. no per-core "temp" gradient here -- is a safe,
 * supported subset, not a bug). ~/.config/btop/btop.conf (user-owned) is
 * append-only: `color_theme = "dank"` is added via dc_systheme_ensure_line()
 * (idempotent on the literal line -- if a *different* color_theme value is
 * already set, this appends a second `color_theme` key rather than editing
 * the first, trusting btop's config parser to take the last occurrence of a
 * duplicate key, same "last-wins append" approach this task's plan
 * document calls for -- verified empirically in this task's scratch-XDG
 * test, see the commit message). `pkill -USR2 btop` is btop's documented
 * config-reload signal for already-running instances.
 *
 * --- cava --------------------------------------------------------------
 *
 * cava has no `@include`/`source`-style directive at all, so its gradient
 * has to be embedded directly inside the user's own
 * ~/.config/cava/config -- there is no separate dankc-owned file the way
 * every other "append a directive" emitter in this codebase gets to write
 * one. That also means dc_systheme_ensure_line()'s "idempotent iff `line`
 * (the exact text about to be appended) is already present verbatim"
 * contract is the wrong tool here: the block's *content* (hex colors)
 * changes every time the active palette changes, so a plain
 * dc_systheme_ensure_line() call would treat every single palette flip as
 * "not present yet" and keep appending a fresh `[color]` block forever
 * (ever-growing file, ever-growing backup chain) instead of updating the one
 * dankc already added. cava_ensure_color_block() below is a small
 * cava-specific variant of the same idempotent-marker/backup contract:
 * it finds a *previous* dankc block by its fixed marker comment (not by the
 * colors themselves) and replaces just that span, so the file only ever
 * carries ONE dankc-owned `[color]` block no matter how many times the
 * palette changes; the very first time dankc touches an existing config, the
 * original is backed up once, matching every other user-owned-file emitter's
 * contract. gradient_color_1..8 (cava's supported maximum) are an 8-stop
 * linear interpolation from primary (accent) to secondary, so the spectrum
 * meter reads as a coherent brand-colored ramp rather than 8 unrelated
 * swatches. `pkill -USR2 cava` is cava's documented reload signal.
 *
 * --- zathura -----------------------------------------------------------
 *
 * ~/.config/zathura/dank-colors is a dankc-owned zathurarc-syntax file
 * (`set <key> <value>`, exactly like any other zathurarc snippet): default-
 * bg/fg, statusbar-fg/bg, inputbar-fg/bg, notification-{,error-,warning-}
 * fg/bg, completion-{,group-,highlight-}fg/bg, highlight-color (the search-
 * match highlight, from primary) + highlight-active-color (the current
 * match, from primary_text-on-primary contrast so it visually pops against
 * the plain highlight), and the recolor-lightcolor/recolor-darkcolor pair
 * (zathura's "invert PDF colors" feature -- set unconditionally so it
 * matches the active palette *if* the user has recolor mode on; dankc never
 * flips that mode itself). ~/.config/zathura/zathurarc (user-owned) gets a
 * plain, never-changing `include dank-colors` directive via
 * dc_systheme_ensure_line() -- correct here (unlike cava) because the
 * directive line itself is static; only the file it points at changes.
 * Zathura has no documented live-reload signal, so this is restart-only: no
 * spawn.
 *
 * --- spicetify -----------------------------------------------------------
 *
 * ~/.config/spicetify/Themes/DankC/color.ini is a dankc-owned spicetify
 * color scheme: a single `[Base]` INI section, values as bare "rrggbb" hex
 * (NO leading '#' -- spicetify's own convention, unlike every other emitter
 * in this codebase). Covers text/subtext/main/sidebar/player/card/button/
 * button-active/notification (this task's exact spec) plus two directly
 * analogous extras sourced from the same palette (notification-error,
 * misc) -- spicetify tolerates any subset of `[Base]` keys, falling back to
 * its bundled default for whatever's missing, so this deliberately-partial
 * set is safe. `spicetify config current_theme DankC` + `spicetify apply`
 * are then spawned -- but `apply` restarts the user's actual Spotify client,
 * so this is the one emitter in the whole systheme_* family that runs a
 * second, disruptive command beyond writing its own file. Three separate
 * layers keep that from happening more than strictly necessary: (1)
 * cfg->systheme_spicetify defaults to false (Task 0's config plumbing) --
 * spicetify is opt-in even among opt-in apps; (2) systheme.c's
 * dc_systheme_apply() only calls this function at all when the whole-config
 * FNV palette-hash has actually changed since the last call; (3) this
 * function ALSO keeps its own static per-process hash of the exact color.ini
 * bytes it last spawned `spicetify apply` for, and skips the spawn (while
 * still atomically refreshing color.ini itself) if the content is
 * byte-identical to last time -- belt-and-suspenders so this function's own
 * documented contract ("never spawn spicetify apply twice for the same
 * palette") holds even if some future caller ever invoked it directly,
 * bypassing systheme.c's outer debounce. Needs the spicetify CLI on $PATH
 * and the user's own one-time `spicetify backup apply`; dankc never runs
 * that setup step.
 */
#include "services/systheme_misc.h"

#include "services/systheme_internal.h"

#include "core/log.h"
#include "theme/theme.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* --- shared path/file helpers (each systheme_*.c keeps its own tiny copy,
 * same precedent as systheme_editors.c's/systheme_term2.c's/
 * systheme_apps.c's identical copies) -------------------------------------- */

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
 * dc_systheme_ensure_line()'s own backup step. DRYRUN-gated; returns false
 * (and logs) only on a real I/O failure. */
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
 * Same shape as systheme_editors.c's ensure_dir() -- a real failure just
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

/* "rrggbb" (6 chars + NUL, no leading '#') -- spicetify's own hex
 * convention, distinct from every other emitter's "#rrggbb". */
static void hex_bare(dc_color c, char out[7])
{
    snprintf(out, 7, "%02x%02x%02x", c.r, c.g, c.b);
}

static dc_color lerp_color(dc_color a, dc_color b, double t)
{
    dc_color out;
    out.r = (uint8_t)((double)a.r + ((double)b.r - (double)a.r) * t);
    out.g = (uint8_t)((double)a.g + ((double)b.g - (double)a.g) * t);
    out.b = (uint8_t)((double)a.b + ((double)b.b - (double)a.b) * t);
    out.a = 255;
    return out;
}

/* --- btop ------------------------------------------------------------------ */

#define DC_BTOP_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_BTOP_THEME_LINE "color_theme = \"dank\""

static char *build_btop_theme(bool light, size_t *out_len)
{
    (void)light;
    const dc_theme *t = dc_theme_current;

    char main_bg[8], main_fg[8], title[8], hi_fg[8], sel_bg[8], sel_fg[8], inactive_fg[8];
    char graph_text[8], meter_bg[8], div_line[8], box_border[8];
    char accent[8], success_hex[8], warning_hex[8], error_hex[8];

    dc_systheme_hex_rgb(t->surface, main_bg);
    dc_systheme_hex_rgb(t->surface_text, main_fg);
    dc_systheme_hex_rgb(t->primary, title);
    dc_systheme_hex_rgb(t->primary, hi_fg);
    dc_systheme_hex_rgb(t->primary_container, sel_bg);
    dc_systheme_hex_rgb(t->primary_text, sel_fg);
    dc_systheme_hex_rgb(t->surface_container_high, inactive_fg);
    dc_systheme_hex_rgb(t->surface_text, graph_text);
    dc_systheme_hex_rgb(t->surface_container, meter_bg);
    dc_systheme_hex_rgb(t->outline, div_line);
    dc_systheme_hex_rgb(t->outline, box_border);
    dc_systheme_hex_rgb(t->primary, accent);
    dc_systheme_hex_rgb(t->success, success_hex);
    dc_systheme_hex_rgb(t->warning, warning_hex);
    dc_systheme_hex_rgb(t->error, error_hex);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          "# btop native theme format -- see upstream btop's themes/ for the reference schema\n\n",
            f);

    fprintf(f, "theme[main_bg]=\"%s\"\n", main_bg);
    fprintf(f, "theme[main_fg]=\"%s\"\n", main_fg);
    fprintf(f, "theme[title]=\"%s\"\n", title);
    fprintf(f, "theme[hi_fg]=\"%s\"\n", hi_fg);
    fprintf(f, "theme[selected_bg]=\"%s\"\n", sel_bg);
    fprintf(f, "theme[selected_fg]=\"%s\"\n", sel_fg);
    fprintf(f, "theme[inactive_fg]=\"%s\"\n", inactive_fg);
    fprintf(f, "theme[graph_text]=\"%s\"\n", graph_text);
    fprintf(f, "theme[meter_bg]=\"%s\"\n", meter_bg);
    fprintf(f, "theme[proc_misc]=\"%s\"\n", accent);
    fprintf(f, "theme[cpu_box]=\"%s\"\n", box_border);
    fprintf(f, "theme[mem_box]=\"%s\"\n", box_border);
    fprintf(f, "theme[net_box]=\"%s\"\n", box_border);
    fprintf(f, "theme[proc_box]=\"%s\"\n", box_border);
    fprintf(f, "theme[div_line]=\"%s\"\n", div_line);
    fputs("\n", f);

    /* cpu graph: usage rising from the brand accent color through warning
     * to error, same "load gets more alarming" reading as the mem/net
     * gradients below. */
    fprintf(f, "theme[cpu_start]=\"%s\"\n", accent);
    fprintf(f, "theme[cpu_mid]=\"%s\"\n", warning_hex);
    fprintf(f, "theme[cpu_end]=\"%s\"\n", error_hex);
    fputs("\n", f);

    /* mem: "used" reads the same alarming direction as cpu; "free"/
     * "available"/"cached" read the opposite direction (plenty free =
     * success, running low = warning) since they're the inverse metric. */
    fprintf(f, "theme[used_start]=\"%s\"\n", accent);
    fprintf(f, "theme[used_mid]=\"%s\"\n", warning_hex);
    fprintf(f, "theme[used_end]=\"%s\"\n", error_hex);
    fprintf(f, "theme[free_start]=\"%s\"\n", success_hex);
    fprintf(f, "theme[free_mid]=\"%s\"\n", accent);
    fprintf(f, "theme[free_end]=\"%s\"\n", warning_hex);
    fprintf(f, "theme[available_start]=\"%s\"\n", success_hex);
    fprintf(f, "theme[available_mid]=\"%s\"\n", accent);
    fprintf(f, "theme[available_end]=\"%s\"\n", warning_hex);
    fprintf(f, "theme[cached_start]=\"%s\"\n", success_hex);
    fprintf(f, "theme[cached_mid]=\"%s\"\n", accent);
    fprintf(f, "theme[cached_end]=\"%s\"\n", warning_hex);
    fputs("\n", f);

    /* net: no crit-failure state to reserve error for, so download/upload
     * both stay within accent -> success -> warning. */
    fprintf(f, "theme[download_start]=\"%s\"\n", accent);
    fprintf(f, "theme[download_mid]=\"%s\"\n", success_hex);
    fprintf(f, "theme[download_end]=\"%s\"\n", warning_hex);
    fprintf(f, "theme[upload_start]=\"%s\"\n", accent);
    fprintf(f, "theme[upload_mid]=\"%s\"\n", success_hex);
    fprintf(f, "theme[upload_end]=\"%s\"\n", warning_hex);
    fputs("\n", f);

    fprintf(f, "theme[process_start]=\"%s\"\n", accent);
    fprintf(f, "theme[process_mid]=\"%s\"\n", warning_hex);
    fprintf(f, "theme[process_end]=\"%s\"\n", error_hex);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_btop(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping btop");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "btop", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping btop", dir);
        return;
    }

    char themes_dir[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(themes_dir, sizeof(themes_dir), "%s/themes", dir);
    /* themes/ is a conventional subdirectory of btop's own config dir (just
     * confirmed to exist above) -- not "creating btop's config dir", same
     * precedent as zed's/helix's themes/ handling in systheme_editors.c. */
    ensure_dir(themes_dir);

    size_t len = 0;
    char *theme = build_btop_theme(light, &len);
    if (!theme) {
        dc_warn("systheme: failed to build btop dank.theme");
        return;
    }
    char theme_path[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(theme_path, sizeof(theme_path), "%s/dank.theme", themes_dir);
    dc_systheme_write_owned(theme_path, theme, len);
    free(theme);

    char conf_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(conf_path, sizeof(conf_path), "%s/btop.conf", dir);
    /* btop.conf is last-key-wins, so a plain idempotent append is enough --
     * see this file's header. */
    dc_systheme_ensure_line(conf_path, DC_BTOP_THEME_LINE, DC_BTOP_MARKER, DC_BTOP_THEME_LINE);

    /* Best-effort nudge for already-running btop instances (its documented
     * config-reload signal), same unconditional-spawn precedent as
     * systheme_editors.c's helix pkill -USR1. */
    const char *argv[] = {"pkill", "-USR2", "btop", NULL};
    dc_systheme_spawn("btop-reload", argv, 3);

    dc_info("systheme: btop theme written to %s", themes_dir);
}

/* --- cava ------------------------------------------------------------------ */

#define DC_CAVA_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_CAVA_GRADIENT_STOPS 8

static char *build_cava_block(bool light, size_t *out_len)
{
    (void)light;
    const dc_theme *t = dc_theme_current;

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fprintf(f, "%s\n", DC_CAVA_MARKER);
    fputs("[color]\n", f);
    fputs("gradient = 1\n", f);
    for (int i = 0; i < DC_CAVA_GRADIENT_STOPS; i++) {
        double frac = (double)i / (double)(DC_CAVA_GRADIENT_STOPS - 1);
        dc_color c = lerp_color(t->primary, t->secondary, frac);
        char hex[8];
        dc_systheme_hex_rgb(c, hex);
        fprintf(f, "gradient_color_%d = '%s'\n", i + 1, hex);
    }
    /* Trailing blank line so cava_ensure_color_block() below can always
     * find exactly where dankc's own block ends via a "\n\n" search, even
     * if the user has content of their own after it -- see that function's
     * comment. */
    fputs("\n", f);

    fclose(f);
    *out_len = len;
    return buf;
}

/* cava-specific analogue of dc_systheme_ensure_line() for a block whose
 * *contents* change every palette update (see this file's header for why
 * the generic helper is the wrong fit here): finds a previously-inserted
 * dankc block by its marker comment and replaces exactly that span,
 * otherwise appends the marker+block for the first time (backing up the
 * pre-existing file exactly once, only on that first touch). DRYRUN-gated
 * like every other write helper in this codebase. */
static bool cava_ensure_color_block(const char *path, const char *block)
{
    char *text = read_whole_file(path);

    const char *marker_at = text ? strstr(text, DC_CAVA_MARKER) : NULL;
    size_t prefix_len = marker_at ? (size_t)(marker_at - text) : (text ? strlen(text) : 0);

    /* If a prior dankc block exists, find where it ends: the next blank
     * line after the marker, or end of file if there isn't one. That drops
     * the whole old marker+`[color]`+gradient_color_* span so the fresh one
     * below is the only copy left in the file. */
    const char *suffix = "";
    if (marker_at) {
        const char *blank = strstr(marker_at, "\n\n");
        suffix = blank ? blank + 2 : marker_at + strlen(marker_at);
    }

    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would %s dankc [color] block in %s",
                marker_at ? "replace" : "append", path);
        free(text);
        return true;
    }

    /* Only back up on the very first touch (no prior marker found yet) of
     * an existing file -- once dankc owns a block in this file, every later
     * update is just replacing dankc's own prior block, not risking any of
     * the user's original content again. */
    if (text && !marker_at && !backup_timestamped(path, text)) {
        free(text);
        return false;
    }

    size_t prefix_extra = (prefix_len > 0 && text[prefix_len - 1] != '\n') ? 1 : 0;
    size_t new_len = prefix_len + prefix_extra + strlen(block) + strlen(suffix);
    char *new_content = malloc(new_len + 1);
    if (!new_content) {
        free(text);
        return false;
    }
    size_t off = 0;
    if (prefix_len) {
        memcpy(new_content, text, prefix_len);
        off += prefix_len;
    }
    if (prefix_extra)
        new_content[off++] = '\n';
    size_t block_len = strlen(block);
    memcpy(new_content + off, block, block_len);
    off += block_len;
    size_t suffix_len = strlen(suffix);
    if (suffix_len)
        memcpy(new_content + off, suffix, suffix_len);
    off += suffix_len;
    new_content[off] = '\0';
    free(text);

    bool ok = dc_systheme_write_owned(path, new_content, strlen(new_content));
    free(new_content);
    if (ok)
        dc_info("systheme: %s dankc [color] block in %s", marker_at ? "replaced" : "added", path);
    return ok;
}

void dc_systheme_apply_cava(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping cava");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "cava", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping cava", dir);
        return;
    }

    size_t len = 0;
    char *block = build_cava_block(light, &len);
    if (!block) {
        dc_warn("systheme: failed to build cava [color] block");
        return;
    }

    char config_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(config_path, sizeof(config_path), "%s/config", dir);
    cava_ensure_color_block(config_path, block);
    free(block);

    /* Best-effort nudge for already-running cava instances (its documented
     * reload signal). */
    const char *argv[] = {"pkill", "-USR2", "cava", NULL};
    dc_systheme_spawn("cava-reload", argv, 3);

    dc_info("systheme: cava gradient written to %s", config_path);
}

/* --- zathura --------------------------------------------------------------- */

#define DC_ZATHURA_MARKER "# Added by DankC Settings > Theme & Colors"
#define DC_ZATHURA_INCLUDE_LINE "include dank-colors"

static char *build_zathura_colors(bool light, size_t *out_len)
{
    (void)light;
    const dc_theme *t = dc_theme_current;

    char bg[8], fg[8], statusbar_bg[8], statusbar_fg[8], inputbar_bg[8], inputbar_fg[8];
    char notif_bg[8], notif_fg[8], notif_err_bg[8], notif_err_fg[8], notif_warn_bg[8],
            notif_warn_fg[8];
    char completion_bg[8], completion_fg[8], completion_group_bg[8], completion_group_fg[8];
    char completion_hi_bg[8], completion_hi_fg[8];
    char highlight[8], highlight_active[8], recolor_light[8], recolor_dark[8];

    dc_systheme_hex_rgb(t->surface, bg);
    dc_systheme_hex_rgb(t->surface_text, fg);
    dc_systheme_hex_rgb(t->surface_container, statusbar_bg);
    dc_systheme_hex_rgb(t->surface_text, statusbar_fg);
    dc_systheme_hex_rgb(t->surface_container, inputbar_bg);
    dc_systheme_hex_rgb(t->surface_text, inputbar_fg);
    dc_systheme_hex_rgb(t->surface_container, notif_bg);
    dc_systheme_hex_rgb(t->surface_text, notif_fg);
    dc_systheme_hex_rgb(t->error, notif_err_bg);
    snprintf(notif_err_fg, sizeof(notif_err_fg), "%s",
            dc_systheme_prefers_black_text(t->error) ? "#000000" : "#ffffff");
    dc_systheme_hex_rgb(t->warning, notif_warn_bg);
    snprintf(notif_warn_fg, sizeof(notif_warn_fg), "%s",
            dc_systheme_prefers_black_text(t->warning) ? "#000000" : "#ffffff");
    dc_systheme_hex_rgb(t->surface_container, completion_bg);
    dc_systheme_hex_rgb(t->surface_text, completion_fg);
    dc_systheme_hex_rgb(t->surface_container_high, completion_group_bg);
    dc_systheme_hex_rgb(t->surface_text, completion_group_fg);
    dc_systheme_hex_rgb(t->primary_container, completion_hi_bg);
    dc_systheme_hex_rgb(t->surface_text, completion_hi_fg);
    dc_systheme_hex_rgb(t->primary, highlight);
    dc_systheme_hex_rgb(t->primary_text, highlight_active);
    /* recolor mode repaints the page bg/fg pair: "light" is the page
     * background substitute, "dark" is the text substitute -- same
     * surface/surface_text roles as everywhere else in this file. */
    dc_systheme_hex_rgb(t->surface, recolor_light);
    dc_systheme_hex_rgb(t->surface_text, recolor_dark);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("# Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);

    fprintf(f, "set default-bg \"%s\"\n", bg);
    fprintf(f, "set default-fg \"%s\"\n", fg);
    fputs("\n", f);
    fprintf(f, "set statusbar-bg \"%s\"\n", statusbar_bg);
    fprintf(f, "set statusbar-fg \"%s\"\n", statusbar_fg);
    fputs("\n", f);
    fprintf(f, "set inputbar-bg \"%s\"\n", inputbar_bg);
    fprintf(f, "set inputbar-fg \"%s\"\n", inputbar_fg);
    fputs("\n", f);
    fprintf(f, "set notification-bg \"%s\"\n", notif_bg);
    fprintf(f, "set notification-fg \"%s\"\n", notif_fg);
    fprintf(f, "set notification-error-bg \"%s\"\n", notif_err_bg);
    fprintf(f, "set notification-error-fg \"%s\"\n", notif_err_fg);
    fprintf(f, "set notification-warning-bg \"%s\"\n", notif_warn_bg);
    fprintf(f, "set notification-warning-fg \"%s\"\n", notif_warn_fg);
    fputs("\n", f);
    fprintf(f, "set completion-bg \"%s\"\n", completion_bg);
    fprintf(f, "set completion-fg \"%s\"\n", completion_fg);
    fprintf(f, "set completion-group-bg \"%s\"\n", completion_group_bg);
    fprintf(f, "set completion-group-fg \"%s\"\n", completion_group_fg);
    fprintf(f, "set completion-highlight-bg \"%s\"\n", completion_hi_bg);
    fprintf(f, "set completion-highlight-fg \"%s\"\n", completion_hi_fg);
    fputs("\n", f);
    fprintf(f, "set highlight-color \"%s\"\n", highlight);
    fprintf(f, "set highlight-active-color \"%s\"\n", highlight_active);
    fputs("\n", f);
    fputs("# Only takes effect if the user has \"recolor\" enabled\n", f);
    fprintf(f, "set recolor-lightcolor \"%s\"\n", recolor_light);
    fprintf(f, "set recolor-darkcolor \"%s\"\n", recolor_dark);

    fclose(f);
    *out_len = len;
    return buf;
}

void dc_systheme_apply_zathura(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping zathura");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "zathura", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping zathura", dir);
        return;
    }

    size_t len = 0;
    char *colors = build_zathura_colors(light, &len);
    if (!colors) {
        dc_warn("systheme: failed to build zathura dank-colors");
        return;
    }
    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors", dir);
    dc_systheme_write_owned(colors_path, colors, len);
    free(colors);

    char zathurarc_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(zathurarc_path, sizeof(zathurarc_path), "%s/zathurarc", dir);
    /* The include directive itself never changes across palette updates
     * (only dank-colors' contents do), so the generic idempotent-append
     * helper is the right fit here, unlike cava's embedded block above. */
    dc_systheme_ensure_line(zathurarc_path, DC_ZATHURA_INCLUDE_LINE, DC_ZATHURA_MARKER,
            DC_ZATHURA_INCLUDE_LINE);

    /* No documented live-reload signal -- restart-only, see this file's
     * header. */
    dc_info("systheme: zathura colors written to %s", colors_path);
}

/* --- spicetify --------------------------------------------------------------- */

static char *build_spicetify_ini(bool light, size_t *out_len)
{
    (void)light;
    const dc_theme *t = dc_theme_current;

    char text_hex[7], subtext_hex[7], main_hex[7], sidebar_hex[7], player_hex[7], card_hex[7];
    char button_hex[7], button_active_hex[7], notification_hex[7], notification_error_hex[7];
    char misc_hex[7];

    hex_bare(t->surface_text, text_hex);
    hex_bare(t->surface_container_high, subtext_hex);
    hex_bare(t->surface, main_hex);
    hex_bare(t->surface_container, sidebar_hex);
    hex_bare(t->surface_container, player_hex);
    hex_bare(t->surface_container_high, card_hex);
    hex_bare(t->primary_container, button_hex);
    hex_bare(t->primary, button_active_hex);
    hex_bare(t->primary, notification_hex);
    hex_bare(t->error, notification_error_hex);
    hex_bare(t->outline, misc_hex);

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("; Generated by DankC's system theming engine (Settings > Theme & Colors)\n\n", f);
    fputs("[Base]\n", f);
    fprintf(f, "text                  = %s\n", text_hex);
    fprintf(f, "subtext               = %s\n", subtext_hex);
    fprintf(f, "main                  = %s\n", main_hex);
    fprintf(f, "sidebar               = %s\n", sidebar_hex);
    fprintf(f, "player                = %s\n", player_hex);
    fprintf(f, "card                  = %s\n", card_hex);
    fprintf(f, "button                = %s\n", button_hex);
    fprintf(f, "button-active         = %s\n", button_active_hex);
    fprintf(f, "notification          = %s\n", notification_hex);
    fprintf(f, "notification-error    = %s\n", notification_error_hex);
    fprintf(f, "misc                  = %s\n", misc_hex);

    fclose(f);
    *out_len = len;
    return buf;
}

static uint64_t fnv1a_bytes(const void *data, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Per-process guard: even though systheme.c's dc_systheme_apply() already
 * only calls this function when its own whole-config palette hash changes,
 * this keeps its own independent hash of the exact color.ini bytes it last
 * spawned `spicetify apply` for, so the "never run the disruptive apply
 * command twice for the same palette" contract holds on its own merits --
 * see this file's header. */
static uint64_t g_spicetify_last_hash;
static bool g_spicetify_have_last_hash;

void dc_systheme_apply_spicetify(bool light)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping spicetify");
        return;
    }
    char dir[DC_SYSTHEME_PATH_MAX + 16];
    app_dir(base, "spicetify", dir, sizeof(dir));
    if (!dc_systheme_dir_exists(dir)) {
        dc_info("systheme: %s does not exist, skipping spicetify", dir);
        return;
    }

    char themes_dir[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(themes_dir, sizeof(themes_dir), "%s/Themes", dir);
    /* Themes/ and Themes/DankC/ are conventional subdirectories of
     * spicetify's own config dir (just confirmed to exist above) -- not
     * "creating spicetify's config dir", same precedent as zed's/btop's
     * themes/ subdirectory handling. */
    ensure_dir(themes_dir);
    char theme_dir[DC_SYSTHEME_PATH_MAX + 48];
    snprintf(theme_dir, sizeof(theme_dir), "%s/DankC", themes_dir);
    ensure_dir(theme_dir);

    size_t len = 0;
    char *ini = build_spicetify_ini(light, &len);
    if (!ini) {
        dc_warn("systheme: failed to build spicetify color.ini");
        return;
    }

    char ini_path[DC_SYSTHEME_PATH_MAX + 64];
    snprintf(ini_path, sizeof(ini_path), "%s/color.ini", theme_dir);
    dc_systheme_write_owned(ini_path, ini, len);

    uint64_t h = fnv1a_bytes(ini, len);
    free(ini);

    if (g_spicetify_have_last_hash && h == g_spicetify_last_hash) {
        dc_info("systheme: spicetify color.ini unchanged since last apply, skipping "
                "`spicetify apply` (never re-running the disruptive Spotify restart for "
                "the same palette)");
        return;
    }
    g_spicetify_last_hash = h;
    g_spicetify_have_last_hash = true;

    const char *config_argv[] = {"spicetify", "config", "current_theme", "DankC", NULL};
    dc_systheme_spawn("spicetify-config", config_argv, 4);
    const char *apply_argv[] = {"spicetify", "apply", NULL};
    dc_systheme_spawn("spicetify-apply", apply_argv, 2);

    dc_info("systheme: spicetify theme written to %s, spicetify apply spawned", ini_path);
}
