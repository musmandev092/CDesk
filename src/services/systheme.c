/* systheme.c — system-wide theming engine core + the GTK3/GTK4 emitter
 * (Task 1 of the system-theming feature; see systheme.h/systheme_internal.h
 * for the split between the public API and the shared plumbing later
 * per-app emitters reuse).
 *
 * Approach: dankc writes each app's native theme file directly from its own
 * live palette (dc_theme_current, src/theme/theme.h) and dc_config_light_mode().
 * There is no external `matugen` binary invocation anywhere in this path.
 *
 * GTK role mapping: reverse-engineered from DankMaterialShell's own matugen
 * template (quickshell/matugen/templates/gtk-colors.css upstream, credited
 * there to "thairanaru on GitHub"), confirmed role-for-role against a real
 * dank-colors.css this palette produced:
 *   accent_*            <- primary / primary_text
 *   window/view/headerbar/dialog/secondary_sidebar *_bg <- surface
 *   sidebar/card/overview/popover *_bg                   <- surface_container
 *   every *_fg above (other than accent/destructive/error) <- surface_text
 *   destructive_* / error_*                              <- error (+ synthesized
 *                                                            contrasting text)
 * One known, deliberate gap: DMS's template sources destructive_fg_color/
 * error_fg_color from matugen's `on_error` role, a proper M3 tone computed
 * alongside `error`. dc_theme (theme.h) only has a single flat `error`
 * swatch (no on_error/error_container/on_error_container), so this emitter
 * synthesizes error_fg via a black/white contrast pick
 * (dc_systheme_prefers_black_text()) rather than matching DMS's tuned
 * dark-red-on-pale-pink value byte-for-byte. Every other role above is a
 * direct, exact field read and matches DMS's stock-theme output exactly.
 *
 * Safety (non-negotiable):
 *   - Opt-in only: a file is touched iff cfg->systheme_enabled AND that
 *     app's own toggle AND dc_systheme_app_detected() are all true.
 *   - Disabling a toggle never deletes a file dankc previously wrote.
 *   - dank-colors.css (fully dankc-owned) is written atomically.
 *   - gtk.css (user-owned, we only ever add one @import line) is backed up
 *     once before the first touch and the @import is never duplicated.
 *   - $DANKC_THEME_DRYRUN=1 must be exported for ALL testing: every write
 *     and every reload-nudge spawn logs instead of touching disk/exec'ing.
 *     A one-time non-dryrun verification run should target a scratch
 *     $XDG_CONFIG_HOME, never the real ~/.config.
 */
#include "services/systheme.h"

#include "services/systheme_apps.h"
#include "services/systheme_editors.h"
#include "services/systheme_internal.h"
#include "services/systheme_launchers.h"
#include "services/systheme_notify.h"
#include "services/systheme_qtkde.h"
#include "services/systheme_term.h"
#include "services/systheme_term2.h"

#include "core/config.h"
#include "core/log.h"
#include "theme/theme.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* --- dryrun gate ------------------------------------------------------------ */

bool dc_systheme_dryrun(void)
{
    const char *v = getenv("DANKC_THEME_DRYRUN");
    return v && v[0] == '1';
}

/* --- hex formatting ---------------------------------------------------------- */

void dc_systheme_hex_rgb(dc_color c, char out[8])
{
    snprintf(out, 8, "#%02x%02x%02x", c.r, c.g, c.b);
}

void dc_systheme_hex_argb(dc_color c, char out[10])
{
    snprintf(out, 10, "#%02x%02x%02x%02x", c.a, c.r, c.g, c.b);
}

/* Standard perceptual-luminance threshold (Rec. 601-ish weights), same
 * formula commonly used for "pick readable text color for this swatch". */
bool dc_systheme_prefers_black_text(dc_color c)
{
    double luma = 0.299 * (double)c.r + 0.587 * (double)c.g + 0.114 * (double)c.b;
    return luma > 140.0;
}

/* --- filesystem helpers ------------------------------------------------------ */

bool dc_systheme_dir_exists(const char *dir)
{
    struct stat st;
    return dir && dir[0] && stat(dir, &st) == 0;
}

bool dc_systheme_on_path(const char *binary)
{
    if (!binary || !binary[0])
        return false;
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin:/usr/local/bin";
    char buf[DC_SYSTHEME_PATH_MAX];
    const char *p = path;
    while (*p) {
        const char *sep = strchr(p, ':');
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        if (len > 0 && len < sizeof(buf) - 8) {
            snprintf(buf, sizeof(buf), "%.*s/%s", (int)len, p, binary);
            if (access(buf, X_OK) == 0)
                return true;
        }
        if (!sep)
            break;
        p = sep + 1;
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

bool dc_systheme_write_owned(const char *path, const char *data, size_t len)
{
    if (dc_systheme_dryrun()) {
        dc_info("[DRYRUN] systheme: would write %zu bytes to %s", len, path);
        return true;
    }

    char tmp[DC_SYSTHEME_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        dc_warn("systheme: could not open %s for writing", tmp);
        return false;
    }
    size_t written = fwrite(data, 1, len, f);
    bool close_ok = fclose(f) == 0;
    if (written != len || !close_ok) {
        dc_warn("systheme: short/failed write to %s", tmp);
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        dc_warn("systheme: rename(%s -> %s) failed", tmp, path);
        unlink(tmp);
        return false;
    }
    return true;
}

bool dc_systheme_ensure_line(const char *path, const char *needle, const char *marker_comment,
                             const char *line)
{
    char *text = read_whole_file(path);
    if (text && strstr(text, needle)) {
        free(text);
        return true; /* already present: idempotent no-op */
    }

    if (dc_systheme_dryrun()) {
        if (text)
            dc_info("[DRYRUN] systheme: would back up %s to %s.bak-<epoch>", path, path);
        dc_info("[DRYRUN] systheme: would write %zu bytes to %s",
                (text ? strlen(text) : 0) + strlen(marker_comment) + strlen(line) + 2, path);
        free(text);
        return true;
    }

    if (text) {
        char backup_path[DC_SYSTHEME_PATH_MAX + 32];
        snprintf(backup_path, sizeof(backup_path), "%s.bak-%ld", path, (long)time(NULL));
        FILE *bf = fopen(backup_path, "w");
        if (!bf) {
            dc_warn("systheme: could not create backup %s; leaving %s untouched", backup_path,
                    path);
            free(text);
            return false;
        }
        fputs(text, bf);
        fclose(bf);
        dc_info("systheme: backed up %s to %s before first edit", path, backup_path);
    }

    size_t text_len = text ? strlen(text) : 0;
    size_t new_len = text_len + strlen(marker_comment) + strlen(line) + 2;
    char *new_content = malloc(new_len + 1);
    if (!new_content) {
        free(text);
        return false;
    }
    if (text_len)
        memcpy(new_content, text, text_len);
    snprintf(new_content + text_len, new_len + 1 - text_len, "%s\n%s\n", marker_comment, line);
    free(text);

    bool ok = dc_systheme_write_owned(path, new_content, strlen(new_content));
    free(new_content);
    if (ok)
        dc_info("systheme: added include to %s", path);
    return ok;
}

bool dc_systheme_ensure_line_top(const char *user_file, const char *line, const char *marker)
{
    char *text = read_whole_file(user_file);
    if (text && strstr(text, line)) {
        free(text);
        return true; /* already present: idempotent no-op */
    }

    if (dc_systheme_dryrun()) {
        if (text)
            dc_info("[DRYRUN] systheme: would back up %s to %s.bak-<epoch>", user_file, user_file);
        dc_info("[DRYRUN] systheme: would prepend %zu bytes to %s",
                strlen(marker) + strlen(line) + 2, user_file);
        free(text);
        return true;
    }

    if (text) {
        char backup_path[DC_SYSTHEME_PATH_MAX + 32];
        snprintf(backup_path, sizeof(backup_path), "%s.bak-%ld", user_file, (long)time(NULL));
        FILE *bf = fopen(backup_path, "w");
        if (!bf) {
            dc_warn("systheme: could not create backup %s; leaving %s untouched", backup_path,
                    user_file);
            free(text);
            return false;
        }
        fputs(text, bf);
        fclose(bf);
        dc_info("systheme: backed up %s to %s before first edit", user_file, backup_path);
    }

    size_t text_len = text ? strlen(text) : 0;
    size_t head_len = strlen(marker) + strlen(line) + 2;
    size_t new_len = head_len + text_len;
    char *new_content = malloc(new_len + 1);
    if (!new_content) {
        free(text);
        return false;
    }
    snprintf(new_content, head_len + 1, "%s\n%s\n", marker, line);
    if (text_len)
        memcpy(new_content + head_len, text, text_len);
    new_content[new_len] = '\0';
    free(text);

    bool ok = dc_systheme_write_owned(user_file, new_content, strlen(new_content));
    free(new_content);
    if (ok)
        dc_info("systheme: prepended include to %s", user_file);
    return ok;
}

/* --- reload-nudge spawn (fire-and-forget, SIGCHLD-safe) ---------------------- */

void dc_systheme_spawn(const char *tag, const char *const argv[], int argc)
{
    if (dc_systheme_dryrun()) {
        char line[1024];
        int off = snprintf(line, sizeof(line), "would spawn:");
        for (int i = 0; i < argc && argv[i]; i++) {
            off += snprintf(line + off, off < (int)sizeof(line) ? sizeof(line) - (size_t)off : 0,
                    " %s", argv[i]);
        }
        dc_info("[DRYRUN] systheme: %s: %s", tag, line);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        dc_warn("systheme: %s: fork() failed", tag);
        return;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    /* Parent: fire-and-forget, reaped by main's SIGCHLD = SIG_IGN. */
}

/* --- app detection ------------------------------------------------------------ */

/* Resolve $XDG_CONFIG_HOME, falling back to "$HOME/.config"; false if
 * neither is set. Shared by detection and the GTK emitter below so both
 * agree on where "the config dir" lives (in particular, honoring
 * $XDG_CONFIG_HOME lets the scratch-dir verification run required before any
 * real (non-DANKC_THEME_DRYRUN) test point HOME-based detection and the
 * actual writes at the same sandboxed tree). */
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

/* Resolve $XDG_DATA_HOME, falling back to "$HOME/.local/share"; false if
 * neither is set. Some apps (KDE color-schemes, Konsole profiles) live under
 * the XDG *data* dir rather than the config dir config_home() resolves. */
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

/* Shared "does this app look installed" rule for the common case (docs'
 * Task 0 spec, detection rule D): a config dir under config_home() named
 * `app_dir_name`, OR `binary` on $PATH. */
static bool generic_detect(bool have_base, const char *base, const char *app_dir_name,
                           const char *binary)
{
    if (have_base) {
        char dir[DC_SYSTHEME_PATH_MAX + 32];
        snprintf(dir, sizeof(dir), "%s/%s", base, app_dir_name);
        if (dc_systheme_dir_exists(dir))
            return true;
    }
    return dc_systheme_on_path(binary);
}

bool dc_systheme_app_detected(const char *app)
{
    if (!app)
        return false;

    char base[DC_SYSTHEME_PATH_MAX];
    bool have_base = config_home(base, sizeof(base));
    char dir[DC_SYSTHEME_PATH_MAX + 32];

    if (strcmp(app, "gtk") == 0) {
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/gtk-3.0", base);
            if (dc_systheme_dir_exists(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/gtk-4.0", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("gtk-launch") || dc_systheme_on_path("gtk4-launch");
    }
    if (strcmp(app, "qt") == 0) {
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/qt5ct", base);
            if (dc_systheme_dir_exists(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/qt6ct", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("qt5ct") || dc_systheme_on_path("qt6ct");
    }
    if (strcmp(app, "alacritty") == 0) {
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/alacritty", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("alacritty");
    }
    if (strcmp(app, "vscode") == 0) {
        if (have_base) {
            /* "Code - OSS" (note the space) is the upstream-built-from-source
             * package name several distros use; "Code" is the official
             * Microsoft build; "VSCodium" is the de-Microsoft-branded
             * community build. systheme_apps.c's find_vscode_settings()
             * checks the same three dirs, same order, for the actual
             * settings.json write target. */
            snprintf(dir, sizeof(dir), "%s/Code - OSS/User", base);
            if (dc_systheme_dir_exists(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/Code/User", base);
            if (dc_systheme_dir_exists(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/VSCodium/User", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("code");
    }
    if (strcmp(app, "kitty") == 0) {
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/kitty", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("kitty");
    }
    if (strcmp(app, "foot") == 0) {
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/foot", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("foot");
    }

    /* --- wave 2 (Task 0 detection spec) --- */
    if (strcmp(app, "kvantum") == 0)
        return generic_detect(have_base, base, "Kvantum", "kvantummanager");
    if (strcmp(app, "kde") == 0) {
        char data[DC_SYSTHEME_PATH_MAX];
        if (data_home(data, sizeof(data))) {
            snprintf(dir, sizeof(dir), "%s/color-schemes", data);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/kdeglobals", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("kreadconfig6");
    }
    if (strcmp(app, "ghostty") == 0)
        return generic_detect(have_base, base, "ghostty", "ghostty");
    if (strcmp(app, "wezterm") == 0)
        return generic_detect(have_base, base, "wezterm", "wezterm");
    if (strcmp(app, "konsole") == 0) {
        char data[DC_SYSTHEME_PATH_MAX];
        if (data_home(data, sizeof(data))) {
            snprintf(dir, sizeof(dir), "%s/konsole", data);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("konsole");
    }
    if (strcmp(app, "xresources") == 0)
        return (dc_systheme_on_path("urxvt") || dc_systheme_on_path("xterm")) &&
                dc_systheme_on_path("xrdb");
    if (strcmp(app, "zed") == 0)
        return generic_detect(have_base, base, "zed", "zed");
    if (strcmp(app, "helix") == 0)
        return generic_detect(have_base, base, "helix", "hx");
    if (strcmp(app, "neovim") == 0)
        return generic_detect(have_base, base, "nvim", "nvim");
    if (strcmp(app, "vim") == 0) {
        /* Not generic_detect(): Vim's traditional config dir is ~/.vim,
         * directly under $HOME -- NOT $XDG_CONFIG_HOME/vim (unlike every
         * other app in this list, Vim doesn't follow the XDG base dir spec
         * by default). Matches systheme_editors.c's dc_systheme_apply_vim(),
         * which writes into that same ~/.vim/colors/. */
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(dir, sizeof(dir), "%s/.vim", home);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("vim");
    }
    if (strcmp(app, "sublime") == 0)
        return generic_detect(have_base, base, "sublime-text", "subl");
    if (strcmp(app, "emacs") == 0) {
        /* Not generic_detect(): Emacs' traditional config dir is
         * ~/.emacs.d, directly under $HOME, with $XDG_CONFIG_HOME/emacs as
         * a newer opt-in alternative -- check both, matching
         * systheme_editors.c's dc_systheme_apply_emacs(), which writes into
         * whichever of the two already exists. */
        const char *home = getenv("HOME");
        if (home && home[0]) {
            snprintf(dir, sizeof(dir), "%s/.emacs.d", home);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/emacs", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return dc_systheme_on_path("emacs");
    }
    if (strcmp(app, "rofi") == 0)
        return generic_detect(have_base, base, "rofi", "rofi");
    if (strcmp(app, "wofi") == 0)
        return generic_detect(have_base, base, "wofi", "wofi");
    if (strcmp(app, "fuzzel") == 0)
        return generic_detect(have_base, base, "fuzzel", "fuzzel");
    if (strcmp(app, "tofi") == 0)
        return generic_detect(have_base, base, "tofi", "tofi");
    if (strcmp(app, "mako") == 0)
        return generic_detect(have_base, base, "mako", "mako");
    if (strcmp(app, "dunst") == 0)
        return generic_detect(have_base, base, "dunst", "dunst");
    if (strcmp(app, "swaync") == 0)
        return generic_detect(have_base, base, "swaync", "swaync");
    if (strcmp(app, "btop") == 0)
        return generic_detect(have_base, base, "btop", "btop");
    if (strcmp(app, "cava") == 0)
        return generic_detect(have_base, base, "cava", "cava");
    if (strcmp(app, "zathura") == 0)
        return generic_detect(have_base, base, "zathura", "zathura");
    if (strcmp(app, "qutebrowser") == 0)
        return generic_detect(have_base, base, "qutebrowser", "qutebrowser");
    if (strcmp(app, "spicetify") == 0)
        return generic_detect(have_base, base, "spicetify", "spicetify");
    if (strcmp(app, "gtk2") == 0) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char f[DC_SYSTHEME_PATH_MAX];
            snprintf(f, sizeof(f), "%s/.gtkrc-2.0", home);
            if (dc_systheme_dir_exists(f))
                return true;
        }
        return dc_systheme_on_path("gimp");
    }
    if (strcmp(app, "firefox") == 0) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            char f[DC_SYSTHEME_PATH_MAX];
            snprintf(f, sizeof(f), "%s/.mozilla/firefox/profiles.ini", home);
            if (dc_systheme_dir_exists(f))
                return true;
            snprintf(f, sizeof(f), "%s/.zen/profiles.ini", home);
            if (dc_systheme_dir_exists(f))
                return true;
            snprintf(f, sizeof(f), "%s/.librewolf/profiles.ini", home);
            if (dc_systheme_dir_exists(f))
                return true;
        }
        return false;
    }
    if (strcmp(app, "discord") == 0) {
        if (have_base) {
            snprintf(dir, sizeof(dir), "%s/vesktop/themes", base);
            if (dc_systheme_dir_exists(dir))
                return true;
            snprintf(dir, sizeof(dir), "%s/Vencord/themes", base);
            if (dc_systheme_dir_exists(dir))
                return true;
        }
        return false;
    }
    return false;
}

/* --- GTK3/GTK4 emitter -------------------------------------------------------- */

/* dank-colors.css: fully dankc-owned, @define-color palette. Same content
 * for gtk-3.0 and gtk-4.0 (matches DMS's own template, which is identical
 * between its dark/light variants module the actual hex values). Built via
 * open_memstream() so the whole file is one buffer handed to
 * dc_systheme_write_owned() -- never a partially-written file on disk. */
static char *build_gtk_colors_css(size_t *out_len)
{
    char primary[8], primary_text[8], surface[8], surface_text[8], surface_container[8];
    char error_hex[8], error_fg[8];

    dc_systheme_hex_rgb(dc_theme_current->primary, primary);
    dc_systheme_hex_rgb(dc_theme_current->primary_text, primary_text);
    dc_systheme_hex_rgb(dc_theme_current->surface, surface);
    dc_systheme_hex_rgb(dc_theme_current->surface_text, surface_text);
    dc_systheme_hex_rgb(dc_theme_current->surface_container, surface_container);
    dc_systheme_hex_rgb(dc_theme_current->error, error_hex);
    snprintf(error_fg, sizeof(error_fg), "%s",
            dc_systheme_prefers_black_text(dc_theme_current->error) ? "#000000" : "#ffffff");

    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    if (!f)
        return NULL;

    fputs("/*\n"
          "* GTK Colors\n"
          "* Generated by DankC's system theming engine (Settings > Theme & Colors)\n"
          "*/\n\n"
          "/* Role mapping adapted from DankMaterialShell's matugen gtk-colors\n"
          " * template (via thairanaru on GitHub), sourced natively from dankc's\n"
          " * own live palette -- no matugen binary is invoked. */\n\n",
            f);

    fputs("/* Destructive colors are basically error colors */\n", f);
    fprintf(f, "@define-color destructive_bg_color %s;\n", error_hex);
    fprintf(f, "@define-color destructive_fg_color %s;\n", error_fg);
    fputs("/* Follow material spec */\n", f);
    fprintf(f, "@define-color error_bg_color %s;\n", error_hex);
    fprintf(f, "@define-color error_fg_color %s;\n", error_fg);
    fputs("/* Follow material spec (also I think looks nicer) */\n", f);
    fprintf(f, "@define-color accent_fg_color %s;\n", primary_text);
    fprintf(f, "@define-color accent_bg_color %s;\n", primary);
    fputs("/* Use surface instead of background */\n", f);
    fprintf(f, "@define-color window_bg_color %s;\n", surface);
    fprintf(f, "@define-color window_fg_color %s;\n", surface_text);
    fputs("/* Make it more similar to Adwaita */\n", f);
    fprintf(f, "@define-color view_bg_color %s;\n", surface);
    fprintf(f, "@define-color view_fg_color %s;\n", surface_text);
    fprintf(f, "@define-color headerbar_bg_color %s;\n", surface);
    fprintf(f, "@define-color headerbar_fg_color %s;\n", surface_text);
    fprintf(f, "@define-color sidebar_bg_color %s;\n", surface_container);
    fprintf(f, "@define-color sidebar_fg_color %s;\n", surface_text);
    fputs("/* There is only like one application I know that uses this, and there isn't a good way to get\n"
          "a color between surface and surface container so I think just giving this surface is a good enough fallback*/\n",
            f);
    fprintf(f, "@define-color secondary_sidebar_bg_color %s;\n", surface);
    fprintf(f, "@define-color secondary_sidebar_fg_color %s;\n", surface_text);
    fprintf(f, "@define-color card_bg_color %s;\n", surface_container);
    fprintf(f, "@define-color card_fg_color %s;\n", surface_text);
    fprintf(f, "@define-color overview_bg_color %s;\n", surface_container);
    fprintf(f, "@define-color overview_fg_color %s;\n", surface_text);
    fprintf(f, "@define-color popover_bg_color %s;\n", surface_container);
    fprintf(f, "@define-color popover_fg_color %s;\n", surface_text);
    fprintf(f, "@define-color dialog_bg_color %s;\n", surface);
    fprintf(f, "@define-color dialog_fg_color %s;\n", surface_text);

    fputs("\n/* Backdrop/unfocused states - prevents white flash on window unfocus */\n"
          "@define-color headerbar_backdrop_color @window_bg_color;\n"
          "@define-color sidebar_backdrop_color @sidebar_bg_color;\n"
          "@define-color theme_unfocused_fg_color @window_fg_color;\n"
          "@define-color theme_unfocused_text_color @view_fg_color;\n"
          "@define-color theme_unfocused_bg_color @window_bg_color;\n"
          "@define-color theme_unfocused_base_color @window_bg_color;\n"
          "@define-color theme_unfocused_selected_bg_color @accent_bg_color;\n"
          "@define-color theme_unfocused_selected_fg_color @accent_fg_color;\n",
            f);

    fclose(f);
    *out_len = len;
    return buf;
}

#define DC_SYSTHEME_GTK_IMPORT_LINE "@import 'dank-colors.css';"
#define DC_SYSTHEME_GTK_MARKER "/* Added by DankC Settings > Theme & Colors */"

/* Resolve config_home() + "/gtk-<ver>.0"; false if config_home() itself is
 * unresolvable (no $XDG_CONFIG_HOME/$HOME). Shares config_home() with
 * dc_systheme_app_detected() (see its comment) so detection and the actual
 * write target always agree. */
static bool gtk_dir(const char *ver, char *out, size_t cap)
{
    char base[DC_SYSTHEME_PATH_MAX];
    if (!config_home(base, sizeof(base)))
        return false;
    snprintf(out, cap, "%s/gtk-%s.0", base, ver);
    return true;
}

static void apply_gtk_variant(const char *ver)
{
    char dir[DC_SYSTHEME_PATH_MAX];
    if (!gtk_dir(ver, dir, sizeof(dir))) {
        dc_warn("systheme: no $XDG_CONFIG_HOME/$HOME, skipping gtk-%s.0", ver);
        return;
    }
    if (!dc_systheme_dir_exists(dir)) {
        /* Never create an app's config dir (safety contract, see this
         * file's header): if gtk-N.0 doesn't exist yet, that GTK version
         * just isn't in use on this machine -- skip it silently rather
         * than manufacturing a directory no app asked for. */
        dc_info("systheme: %s does not exist, skipping gtk-%s.0", dir, ver);
        return;
    }

    size_t len = 0;
    char *css = build_gtk_colors_css(&len);
    if (!css) {
        dc_warn("systheme: failed to build gtk-%s.0 dank-colors.css", ver);
        return;
    }

    char colors_path[DC_SYSTHEME_PATH_MAX + 32];
    char gtk_css_path[DC_SYSTHEME_PATH_MAX + 32];
    snprintf(colors_path, sizeof(colors_path), "%s/dank-colors.css", dir);
    snprintf(gtk_css_path, sizeof(gtk_css_path), "%s/gtk.css", dir);

    dc_systheme_write_owned(colors_path, css, len);
    free(css);

    /* gtk.css doesn't exist on a fresh install -- ensure_line() creates it
     * (with the marker) rather than silently no-op'ing, which would leave
     * the whole GTK tier inert. If it does exist (hand-authored), it's
     * backed up once and the import appended, never duplicated. */
    dc_systheme_ensure_line(gtk_css_path, DC_SYSTHEME_GTK_IMPORT_LINE, DC_SYSTHEME_GTK_MARKER,
                             DC_SYSTHEME_GTK_IMPORT_LINE);

    dc_info("systheme: gtk-%s.0 theme written to %s", ver, dir);
}

static void apply_gtk(bool light)
{
    apply_gtk_variant("3");
    apply_gtk_variant("4");

    /* Best-effort reload nudge: already-running GTK3 apps read gtk.css only
     * at startup (no live file-watch), so this doesn't repaint them -- but
     * newly-launched apps and GTK4 apps (which do watch) pick up the change
     * immediately, and this at least flips the portal's light/dark hint for
     * apps that follow it. Restarting an already-open GTK3 app is the only
     * way to force it to re-read gtk.css. */
    const char *argv[] = {"gsettings", "set", "org.gnome.desktop.interface", "color-scheme",
            light ? "prefer-light" : "prefer-dark", NULL};
    dc_systheme_spawn("gtk-reload", argv, 5);
}

/* --- palette-hash debounce + top-level entry point ---------------------------- */

static uint64_t fnv1a(const void *data, size_t len, uint64_t hash)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Every systheme_* per-app toggle, folded one byte at a time into the FNV
 * hash below. Widened from the original uint8_t bitmask (which only had
 * room for ~6 apps) now that the toggle count has grown past 30 -- folding
 * each bool as its own hashed byte scales to any number of toggles without
 * another bitmask-width bump later. */
static uint64_t hash_toggles(const struct dc_config *cfg, uint64_t h)
{
    const bool toggles[] = {
        cfg->systheme_gtk,
        cfg->systheme_qt,
        cfg->systheme_alacritty,
        cfg->systheme_vscode,
        cfg->systheme_kitty,
        cfg->systheme_foot,
        cfg->systheme_kvantum,
        cfg->systheme_kde,
        cfg->systheme_ghostty,
        cfg->systheme_wezterm,
        cfg->systheme_konsole,
        cfg->systheme_xresources,
        cfg->systheme_zed,
        cfg->systheme_helix,
        cfg->systheme_neovim,
        cfg->systheme_vim,
        cfg->systheme_sublime,
        cfg->systheme_emacs,
        cfg->systheme_rofi,
        cfg->systheme_wofi,
        cfg->systheme_fuzzel,
        cfg->systheme_tofi,
        cfg->systheme_mako,
        cfg->systheme_dunst,
        cfg->systheme_swaync,
        cfg->systheme_btop,
        cfg->systheme_cava,
        cfg->systheme_zathura,
        cfg->systheme_qutebrowser,
        cfg->systheme_firefox,
        cfg->systheme_discord,
        cfg->systheme_spicetify,
        cfg->systheme_gtk2,
    };
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++) {
        uint8_t b = toggles[i] ? 1 : 0;
        h = fnv1a(&b, 1, h);
    }
    return h;
}

static uint64_t compute_palette_hash(const struct dc_config *cfg, bool light)
{
    uint64_t h = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
    h = fnv1a(dc_theme_current, sizeof(*dc_theme_current), h);
    uint8_t lightb = light ? 1 : 0;
    h = fnv1a(&lightb, 1, h);
    h = hash_toggles(cfg, h);
    return h;
}

static uint64_t g_last_hash;
static bool g_have_last_hash;

void dc_systheme_apply(const struct dc_config *cfg)
{
    if (!cfg->systheme_enabled) {
        /* Force a real re-apply the next time the master switch flips back
         * on, even if the palette hasn't changed meanwhile. */
        g_have_last_hash = false;
        return;
    }

    bool light = dc_config_light_mode();
    uint64_t h = compute_palette_hash(cfg, light);
    if (g_have_last_hash && h == g_last_hash)
        return; /* palette + toggles unchanged since last call: no disk churn */
    g_last_hash = h;
    g_have_last_hash = true;

    if (cfg->systheme_gtk && dc_systheme_app_detected("gtk"))
        apply_gtk(light);

    if (cfg->systheme_alacritty && dc_systheme_app_detected("alacritty"))
        dc_systheme_apply_alacritty(light);
    if (cfg->systheme_kitty && dc_systheme_app_detected("kitty"))
        dc_systheme_apply_kitty(light);
    if (cfg->systheme_foot && dc_systheme_app_detected("foot"))
        dc_systheme_apply_foot(light);

    if (cfg->systheme_qt && dc_systheme_app_detected("qt"))
        dc_systheme_apply_qt(light);
    if (cfg->systheme_vscode && dc_systheme_app_detected("vscode"))
        dc_systheme_apply_vscode(light);

    if (cfg->systheme_rofi && dc_systheme_app_detected("rofi"))
        dc_systheme_apply_rofi(light);
    if (cfg->systheme_wofi && dc_systheme_app_detected("wofi"))
        dc_systheme_apply_wofi(light);
    if (cfg->systheme_fuzzel && dc_systheme_app_detected("fuzzel"))
        dc_systheme_apply_fuzzel(light);
    if (cfg->systheme_tofi && dc_systheme_app_detected("tofi"))
        dc_systheme_apply_tofi(light);
    if (cfg->systheme_ghostty && dc_systheme_app_detected("ghostty"))
        dc_systheme_apply_ghostty(light);
    if (cfg->systheme_wezterm && dc_systheme_app_detected("wezterm"))
        dc_systheme_apply_wezterm(light);
    if (cfg->systheme_konsole && dc_systheme_app_detected("konsole"))
        dc_systheme_apply_konsole(light);
    if (cfg->systheme_xresources && dc_systheme_app_detected("xresources"))
        dc_systheme_apply_xresources(light);
    if (cfg->systheme_mako && dc_systheme_app_detected("mako"))
        dc_systheme_apply_mako(light);
    if (cfg->systheme_dunst && dc_systheme_app_detected("dunst"))
        dc_systheme_apply_dunst(light);
    if (cfg->systheme_swaync && dc_systheme_app_detected("swaync"))
        dc_systheme_apply_swaync(light);
    if (cfg->systheme_kvantum && dc_systheme_app_detected("kvantum"))
        dc_systheme_apply_kvantum(light);
    if (cfg->systheme_kde && dc_systheme_app_detected("kde"))
        dc_systheme_apply_kde(light);
    if (cfg->systheme_gtk2 && dc_systheme_app_detected("gtk2"))
        dc_systheme_apply_gtk2(light);
    if (cfg->systheme_zed && dc_systheme_app_detected("zed"))
        dc_systheme_apply_zed(light);
    if (cfg->systheme_helix && dc_systheme_app_detected("helix"))
        dc_systheme_apply_helix(light);
    if (cfg->systheme_neovim && dc_systheme_app_detected("neovim"))
        dc_systheme_apply_neovim(light);
    if (cfg->systheme_vim && dc_systheme_app_detected("vim"))
        dc_systheme_apply_vim(light);
    if (cfg->systheme_sublime && dc_systheme_app_detected("sublime"))
        dc_systheme_apply_sublime(light);
    if (cfg->systheme_emacs && dc_systheme_app_detected("emacs"))
        dc_systheme_apply_emacs(light);
}
