#include "services/keybinds.h"

#include "core/log.h"
#include "dc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#define DC_KEYBINDS_PATH_MAX 512
#define DC_KEYBINDS_MANAGED_FILENAME "dankc-binds.kdl"
#define DC_KEYBINDS_INCLUDE_LINE "include \"" DC_KEYBINDS_MANAGED_FILENAME "\""

#define DC_KB_MAX_INCLUDE_FILES 32
#define DC_KB_MAX_DEPTH 6
#define DC_KB_CHORD_TOKENS_MAX 8

static bool dryrun_enabled(void)
{
    const char *v = getenv("DANKC_BINDS_DRYRUN");
    return v && v[0] == '1';
}

static dc_keybinds_validate_result g_last_validate = DC_KEYBINDS_VALIDATE_UNKNOWN;

dc_keybinds_validate_result dc_keybinds_last_validate(void)
{
    return g_last_validate;
}

/* ---------------------------------------------------------------------- *
 * Action-preset tables.
 * ---------------------------------------------------------------------- */

/* Moved verbatim (verb/label/cat shape unchanged) from ui/keybinds_modal.c's
 * KB_NIRI_ACTIONS -- see that file's comment for provenance (a hand-picked
 * subset of DankMaterialShell's Common/KeybindActions.js NIRI_ACTIONS). */
static const dc_keybind_action_preset KB_NIRI_ACTIONS[] = {
    {"close-window", "Close Window", "Window"},
    {"fullscreen-window", "Fullscreen", "Window"},
    {"maximize-column", "Maximize Column", "Window"},
    {"center-column", "Center Column", "Window"},
    {"center-visible-columns", "Center Visible Columns", "Window"},
    {"toggle-window-floating", "Toggle Floating", "Window"},
    {"switch-focus-between-floating-and-tiling", "Switch Floating/Tiling Focus", "Window"},
    {"switch-preset-column-width", "Cycle Column Width", "Window"},
    {"switch-preset-window-height", "Cycle Window Height", "Window"},
    {"set-column-width", "Set Column Width", "Window"},
    {"set-window-height", "Set Window Height", "Window"},
    {"reset-window-height", "Reset Window Height", "Window"},
    {"expand-column-to-available-width", "Expand to Available Width", "Window"},
    {"consume-or-expel-window-left", "Consume/Expel Left", "Window"},
    {"consume-or-expel-window-right", "Consume/Expel Right", "Window"},
    {"consume-window-into-column", "Consume Into Column", "Window"},
    {"expel-window-from-column", "Expel From Column", "Window"},
    {"toggle-column-tabbed-display", "Toggle Tabbed", "Window"},
    {"toggle-window-rule-opacity", "Toggle Window Opacity", "Window"},
    {"toggle-window-urgent", "Toggle Urgent", "Window"},

    {"focus-column-left", "Focus Left", "Focus"},
    {"focus-column-right", "Focus Right", "Focus"},
    {"focus-window-down", "Focus Down", "Focus"},
    {"focus-window-up", "Focus Up", "Focus"},
    {"focus-column-first", "Focus First Column", "Focus"},
    {"focus-column-last", "Focus Last Column", "Focus"},
    {"focus-window-down-or-column-left", "Focus Down or Left", "Focus"},
    {"focus-window-up-or-column-left", "Focus Up or Left", "Focus"},

    {"move-column-left", "Move Left", "Move"},
    {"move-column-right", "Move Right", "Move"},
    {"move-window-down", "Move Down", "Move"},
    {"move-window-up", "Move Up", "Move"},
    {"move-column-to-first", "Move to First", "Move"},
    {"move-column-to-last", "Move to Last", "Move"},

    {"focus-workspace-down", "Focus Workspace Down", "Workspace"},
    {"focus-workspace-up", "Focus Workspace Up", "Workspace"},
    {"focus-workspace-previous", "Focus Previous Workspace", "Workspace"},
    {"focus-workspace", "Focus Workspace", "Workspace"},
    {"move-column-to-workspace-down", "Move to Workspace Down", "Workspace"},
    {"move-column-to-workspace-up", "Move to Workspace Up", "Workspace"},
    {"move-column-to-workspace", "Move to Workspace", "Workspace"},
    {"move-workspace-down", "Move Workspace Down", "Workspace"},
    {"move-workspace-up", "Move Workspace Up", "Workspace"},

    {"focus-monitor-left", "Focus Monitor Left", "Monitor"},
    {"focus-monitor-right", "Focus Monitor Right", "Monitor"},
    {"focus-monitor-down", "Focus Monitor Down", "Monitor"},
    {"focus-monitor-up", "Focus Monitor Up", "Monitor"},
    {"move-column-to-monitor-left", "Move Column to Monitor Left", "Monitor"},
    {"move-column-to-monitor-right", "Move Column to Monitor Right", "Monitor"},
    {"move-column-to-monitor-down", "Move Column to Monitor Down", "Monitor"},
    {"move-column-to-monitor-up", "Move Column to Monitor Up", "Monitor"},
    {"move-workspace-to-monitor-left", "Move Workspace to Monitor Left", "Monitor"},
    {"move-workspace-to-monitor-right", "Move Workspace to Monitor Right", "Monitor"},
    {"move-workspace-to-monitor-down", "Move Workspace to Monitor Down", "Monitor"},
    {"move-workspace-to-monitor-up", "Move Workspace to Monitor Up", "Monitor"},

    {"screenshot", "Screenshot (Interactive)", "Screenshot"},
    {"screenshot-screen", "Screenshot Screen", "Screenshot"},
    {"screenshot-window", "Screenshot Window", "Screenshot"},

    {"toggle-overview", "Toggle Overview", "System"},
    {"show-hotkey-overlay", "Show Hotkey Overlay", "System"},
    {"do-screen-transition", "Screen Transition", "System"},
    {"power-off-monitors", "Power Off Monitors", "System"},
    {"power-on-monitors", "Power On Monitors", "System"},
    {"toggle-keyboard-shortcuts-inhibit", "Toggle Shortcuts Inhibit", "System"},
    {"quit", "Quit Niri", "System"},
    {"suspend", "Suspend", "System"},

    {"next-window", "Next Window", "Alt-Tab"},
    {"previous-window", "Previous Window", "Alt-Tab"},
};

const dc_keybind_action_preset *dc_keybinds_niri_actions(int *count)
{
    if (count)
        *count = (int)DC_ARRAY_LEN(KB_NIRI_ACTIONS);
    return KB_NIRI_ACTIONS;
}

/* Seeded from ipc/control.c's control_dispatch() vocabulary (the commands
 * `dankc ctl <words...>` understands) plus main.c's print_keybinds() list --
 * these become `spawn "dankc" "ctl" "<verb>";` actions when a Settings-tab
 * user picks one. `verb` here is passed as-is after "ctl" (may itself
 * contain a space, e.g. "processes memory" -> `ctl processes memory`,
 * matching control_dispatch()'s own multi-word command matching). */
static const dc_keybind_action_preset KB_DANKC_ACTIONS[] = {
    {"launcher", "App Launcher", "DankC"},
    {"control-center", "Control Center", "DankC"},
    {"notifications", "Notifications", "DankC"},
    {"battery", "Battery Popout", "DankC"},
    {"clipboard", "Clipboard History", "DankC"},
    {"processes", "Task Manager (CPU sort)", "DankC"},
    {"processes memory", "Task Manager (Memory sort)", "DankC"},
    {"settings", "Settings", "DankC"},
    {"dashboard", "Dashboard: Overview", "DankC"},
    {"dashboard media", "Dashboard: Media", "DankC"},
    {"dashboard weather", "Dashboard: Weather", "DankC"},
    {"dashboard wallpapers", "Dashboard: Wallpapers", "DankC"},
    {"dock", "Toggle Dock", "DankC"},
    {"keybinds-overlay", "Keybinds Cheat Sheet", "DankC"},
    {"power-menu", "Power Menu", "DankC"},
    {"lock", "Lock Screen", "DankC"},
    {"screenshot", "Screenshot (Full Screen)", "DankC"},
    {"screenshot-region", "Screenshot (Region)", "DankC"},
    {"color-picker", "Color Picker", "DankC"},
    {"night", "Night Light Toggle", "DankC"},
};

const dc_keybind_action_preset *dc_keybinds_dankc_actions(int *count)
{
    if (count)
        *count = (int)DC_ARRAY_LEN(KB_DANKC_ACTIONS);
    return KB_DANKC_ACTIONS;
}

/* ---------------------------------------------------------------------- *
 * Parsing (generalized from ui/keybinds_modal.c's kb_parse_config chain).
 * ---------------------------------------------------------------------- */

typedef struct {
    dc_keybind *out;
    int max;
    int count;
    char source_base[DC_KEYBIND_SOURCE_MAX]; /* basename of the file currently
                                               * being scanned */
} kb_load_ctx;

static const char *kb_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Reads a whole file into a NUL-terminated malloc'd buffer, capped at 4MiB
 * (sanity bound, same as keybinds_modal.c's kb_read_file). Returns NULL if
 * the file doesn't exist, can't be read, or exceeds the cap. */
static char *kb_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Blanks out `// ...` line comments in place (tolerant parser, doesn't
 * special-case `//` inside a quoted string -- see keybinds_modal.c). */
static void kb_strip_comments(char *text)
{
    for (char *p = text; *p; p++) {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                *p++ = ' ';
            if (!*p)
                break;
        }
    }
}

/* Pulls `hotkey-overlay-title="..."` out of a bind header's post-chord
 * remainder into `title_out`, and copies whatever's left (with the matched
 * span removed and internal whitespace runs collapsed to single spaces,
 * trimmed at both ends) into `props_out` -- e.g. "repeat=false" or
 * "hotkey-overlay-title=\"Foo\" allow-when-locked=true" -> title_out="Foo",
 * props_out="allow-when-locked=true". */
static void kb_extract_title_and_props(const char *header_rest, size_t hlen, char *title_out,
                                       size_t title_sz, char *props_out, size_t props_sz)
{
    title_out[0] = '\0';
    const char *needle = "hotkey-overlay-title=\"";
    size_t nlen = strlen(needle);
    size_t match_start = hlen, match_end = hlen; /* empty span: nothing cut */

    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(header_rest + i, needle, nlen) != 0)
            continue;
        size_t vstart = i + nlen;
        size_t vend = vstart;
        while (vend < hlen && header_rest[vend] != '"')
            vend++;
        size_t vn = vend - vstart;
        if (vn >= title_sz)
            vn = title_sz - 1;
        memcpy(title_out, header_rest + vstart, vn);
        title_out[vn] = '\0';
        match_start = i;
        match_end = (vend < hlen) ? vend + 1 : vend;
        break;
    }

    size_t oi = 0;
    bool pending_space = false;
    bool any = false;
    for (size_t i = 0; i < hlen; i++) {
        if (i >= match_start && i < match_end)
            continue;
        unsigned char c = (unsigned char)header_rest[i];
        if (isspace(c)) {
            if (any)
                pending_space = true;
            continue;
        }
        if (pending_space && oi + 1 < props_sz)
            props_out[oi++] = ' ';
        pending_space = false;
        if (oi + 1 < props_sz)
            props_out[oi++] = (char)c;
        any = true;
    }
    props_out[oi] = '\0';
}

static void kb_add_bind(kb_load_ctx *ctx, const char *key_raw, size_t key_len,
                        const char *header_rest, size_t header_rest_len, const char *action_raw,
                        size_t action_len)
{
    if (ctx->count >= ctx->max || key_len == 0)
        return;

    /* Trim the action body and drop a single trailing ';'. */
    size_t as = 0, ae = action_len;
    while (as < ae && isspace((unsigned char)action_raw[as]))
        as++;
    while (ae > as && isspace((unsigned char)action_raw[ae - 1]))
        ae--;
    if (ae > as && action_raw[ae - 1] == ';') {
        ae--;
        while (ae > as && isspace((unsigned char)action_raw[ae - 1]))
            ae--;
    }
    if (ae <= as)
        return; /* empty action body: not a real bind */

    dc_keybind *b = &ctx->out[ctx->count];
    memset(b, 0, sizeof(*b));

    size_t kl = key_len < sizeof(b->chord) - 1 ? key_len : sizeof(b->chord) - 1;
    memcpy(b->chord, key_raw, kl);
    b->chord[kl] = '\0';

    kb_extract_title_and_props(header_rest, header_rest_len, b->title, sizeof(b->title), b->props,
                               sizeof(b->props));

    size_t al = ae - as;
    if (al >= sizeof(b->action))
        al = sizeof(b->action) - 1;
    memcpy(b->action, action_raw + as, al);
    b->action[al] = '\0';

    snprintf(b->source, sizeof(b->source), "%s", ctx->source_base);
    b->managed = strcmp(ctx->source_base, DC_KEYBINDS_MANAGED_FILENAME) == 0;

    ctx->count++;
}

/* One `<header> { <action> }` unit inside a binds{} block -- the key chord
 * is the header's first whitespace-delimited token, the rest may contain
 * `hotkey-overlay-title="..."` plus other props. */
static void kb_parse_one_bind_span(kb_load_ctx *ctx, const char *header, size_t hlen,
                                   const char *action, size_t alen)
{
    size_t hi = 0;
    while (hi < hlen && isspace((unsigned char)header[hi]))
        hi++;
    size_t key_start = hi;
    while (hi < hlen && !isspace((unsigned char)header[hi]))
        hi++;
    size_t key_len = hi - key_start;
    if (key_len == 0)
        return;
    kb_add_bind(ctx, header + key_start, key_len, header + hi, hlen - hi, action, alen);
}

/* Splits the inner content of one `binds { ... }` node into
 * `<header>{<action>}` units, brace- and quote-aware so nested `{}`/`"`
 * inside an action body doesn't confuse the split (see keybinds_modal.c's
 * kb_parse_binds_inner for the same logic). */
static void kb_parse_binds_inner_span(kb_load_ctx *ctx, const char *inner, size_t inner_len)
{
    size_t pos = 0;
    while (pos < inner_len) {
        size_t hstart = pos;
        while (pos < inner_len && inner[pos] != '{') {
            if (inner[pos] == '"') {
                pos++;
                while (pos < inner_len && inner[pos] != '"') {
                    if (inner[pos] == '\\' && pos + 1 < inner_len)
                        pos++;
                    pos++;
                }
                if (pos < inner_len)
                    pos++;
                continue;
            }
            pos++;
        }
        if (pos >= inner_len)
            break;
        size_t hlen = pos - hstart;

        size_t astart = pos + 1;
        int depth = 1;
        size_t apos = astart;
        while (apos < inner_len && depth > 0) {
            char c = inner[apos];
            if (c == '"') {
                apos++;
                while (apos < inner_len && inner[apos] != '"') {
                    if (inner[apos] == '\\' && apos + 1 < inner_len)
                        apos++;
                    apos++;
                }
                apos++;
                continue;
            }
            if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0)
                    break;
            }
            apos++;
        }
        if (depth != 0)
            break; /* malformed -- bail out of this block */
        size_t alen = apos - astart;

        kb_parse_one_bind_span(ctx, inner + hstart, hlen, inner + astart, alen);
        pos = apos + 1;
    }
}

/* Finds every `binds { ... }` node anywhere in `text` (word-boundary
 * checked) and hands its inner content to kb_parse_binds_inner_span(). */
static void kb_scan_binds_blocks_span(kb_load_ctx *ctx, const char *text)
{
    const char *p = text;
    while ((p = strstr(p, "binds")) != NULL) {
        bool left_ok = (p == text) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '-');
        const char *after = p + 5;
        bool right_ok = !(isalnum((unsigned char)after[0]) || after[0] == '_' || after[0] == '-');
        if (left_ok && right_ok) {
            const char *q = after;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q == '{') {
                const char *inner_start = q + 1;
                int depth = 1;
                const char *r = inner_start;
                while (*r && depth > 0) {
                    if (*r == '"') {
                        r++;
                        while (*r && *r != '"') {
                            if (*r == '\\' && r[1])
                                r++;
                            r++;
                        }
                        if (*r)
                            r++;
                        continue;
                    }
                    if (*r == '{')
                        depth++;
                    else if (*r == '}') {
                        depth--;
                        if (depth == 0)
                            break;
                    }
                    r++;
                }
                if (depth == 0) {
                    kb_parse_binds_inner_span(ctx, inner_start, (size_t)(r - inner_start));
                    p = r + 1;
                    continue;
                }
            }
        }
        p = after;
    }
}

/* Follows `include "path";` recursively (depth- and visited-set-bounded,
 * tolerates cycles/typos) from `path`, scanning every `binds { }` node it
 * finds along the way. `ctx->source_base` is set to each file's basename
 * before it's scanned, so every bind ends up tagged with the file it
 * actually came from. */
static void kb_parse_file_recursive(kb_load_ctx *ctx, const char *path, int depth,
                                    char seen[][DC_KEYBINDS_PATH_MAX], int *seen_count)
{
    if (depth > DC_KB_MAX_DEPTH)
        return;
    for (int i = 0; i < *seen_count; i++) {
        if (strcmp(seen[i], path) == 0)
            return; /* already parsed (dedupe + cycle guard) */
    }
    if (*seen_count < DC_KB_MAX_INCLUDE_FILES)
        snprintf(seen[(*seen_count)++], DC_KEYBINDS_PATH_MAX, "%s", path);

    char *text = kb_read_file(path);
    if (!text)
        return;
    kb_strip_comments(text);

    snprintf(ctx->source_base, sizeof(ctx->source_base), "%s", kb_basename(path));
    kb_scan_binds_blocks_span(ctx, text);

    char dir[DC_KEYBINDS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof(dir), ".");

    const char *p = text;
    while ((p = strstr(p, "include")) != NULL) {
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == '"') {
            q++;
            const char *end = strchr(q, '"');
            if (end && end > q) {
                char rel[300];
                size_t rl = (size_t)(end - q);
                if (rl >= sizeof(rel))
                    rl = sizeof(rel) - 1;
                memcpy(rel, q, rl);
                rel[rl] = '\0';
                char childpath[DC_KEYBINDS_PATH_MAX + sizeof(rel) + 2];
                snprintf(childpath, sizeof(childpath), "%s/%s", dir, rel);
                kb_parse_file_recursive(ctx, childpath, depth + 1, seen, seen_count);
                /* recursion clobbers ctx->source_base -- restore it in case
                 * more scanning of *this* file happens later. */
                snprintf(ctx->source_base, sizeof(ctx->source_base), "%s", kb_basename(path));
            }
            p = end ? end + 1 : q;
        } else {
            p = q;
        }
    }
    free(text);
}

/* ---------------------------------------------------------------------- *
 * dc_keybinds_load()
 * ---------------------------------------------------------------------- */

static bool resolve_config_dir(char *dir, size_t cap, const char *override_dir)
{
    if (override_dir && override_dir[0]) {
        snprintf(dir, cap, "%s", override_dir);
        return true;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("keybinds: $HOME unset, cannot locate niri config dir");
        return false;
    }
    snprintf(dir, cap, "%s/.config/niri", home);
    return true;
}

int dc_keybinds_load(dc_keybind *out, int max, const char *config_dir_override)
{
    if (!out || max <= 0)
        return 0;
    char dir[DC_KEYBINDS_PATH_MAX];
    if (!resolve_config_dir(dir, sizeof(dir), config_dir_override))
        return 0;
    char path[DC_KEYBINDS_PATH_MAX];
    snprintf(path, sizeof(path), "%s/config.kdl", dir);

    kb_load_ctx ctx = {.out = out, .max = max, .count = 0};
    char seen[DC_KB_MAX_INCLUDE_FILES][DC_KEYBINDS_PATH_MAX];
    int seen_count = 0;
    kb_parse_file_recursive(&ctx, path, 0, seen, &seen_count);

    dc_info("keybinds: parsed %d bind(s) from %d file(s)", ctx.count, seen_count);
    return ctx.count;
}

/* ---------------------------------------------------------------------- *
 * dc_keybinds_persist() -- fragment serialization + include + validate +
 * rollback.
 * ---------------------------------------------------------------------- */

/* Escapes '"' and '\\' for embedding `in` inside a KDL double-quoted
 * string (used for hotkey-overlay-title, which comes from free-form user
 * input via the capture UI -- chord/action/props are trusted verbatim,
 * either round-tripped byte-stable from a load() or built by this service
 * itself). */
static void kb_kdl_escape(const char *in, char *out, size_t outsz)
{
    size_t oi = 0;
    for (const char *p = in; *p && oi + 2 < outsz; p++) {
        if (*p == '"' || *p == '\\')
            out[oi++] = '\\';
        if (oi + 1 < outsz)
            out[oi++] = *p;
    }
    out[oi] = '\0';
}

static void serialize_fragment(FILE *f, const dc_keybind *managed, int n)
{
    fputs("// Managed by DankC's Settings > Keybinds tab.\n"
          "// Hand edits are fine, but saving a change through the UI rewrites this whole\n"
          "// file from what dankc currently understands -- anything else here will be\n"
          "// lost on the next save.\n\n",
          f);

    fputs("binds {\n", f);
    for (int i = 0; i < n; i++) {
        const dc_keybind *b = &managed[i];
        if (!b->chord[0] || !b->action[0])
            continue;
        fprintf(f, "    %s", b->chord);
        if (b->title[0]) {
            char esc[DC_KEYBIND_TITLE_MAX * 2];
            kb_kdl_escape(b->title, esc, sizeof(esc));
            fprintf(f, " hotkey-overlay-title=\"%s\"", esc);
        }
        if (b->props[0])
            fprintf(f, " %s", b->props);
        fprintf(f, " { %s; }\n", b->action);
    }
    fputs("}\n", f);
}

static bool write_raw_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    if (content && content[0])
        fputs(content, f);
    fclose(f);
    return true;
}

/* Independent copy of niri_input.c's ensure_include() (same file-ownership
 * boundary rationale documented in niri_input.h/keybinds.h: not a shared
 * helper, each managed-fragment owner keeps its own copy of this
 * write+backup logic). */
static bool ensure_include(const char *config_path, const char *managed_filename)
{
    char *text = kb_read_file(config_path);
    if (!text) {
        FILE *f = fopen(config_path, "w");
        if (!f) {
            dc_warn("keybinds: could not create %s", config_path);
            return false;
        }
        fprintf(f, "%s\n", DC_KEYBINDS_INCLUDE_LINE);
        fclose(f);
        return true;
    }
    if (strstr(text, managed_filename)) {
        free(text);
        return true;
    }

    char backup_path[DC_KEYBINDS_PATH_MAX + 32];
    snprintf(backup_path, sizeof(backup_path), "%s.bak-%ld", config_path, (long)time(NULL));
    FILE *bf = fopen(backup_path, "w");
    if (!bf) {
        dc_warn("keybinds: could not create backup %s; aborting include", backup_path);
        free(text);
        return false;
    }
    fputs(text, bf);
    fclose(bf);
    free(text);

    FILE *f = fopen(config_path, "a");
    if (!f) {
        dc_warn("keybinds: could not append to %s (backup at %s is safe to restore)", config_path,
                backup_path);
        return false;
    }
    fprintf(f, "\n// Added by DankC Settings > Keybinds (backup: %s):\n", backup_path);
    fprintf(f, "%s\n", DC_KEYBINDS_INCLUDE_LINE);
    fclose(f);
    dc_info("keybinds: added include to %s (backup %s)", config_path, backup_path);
    return true;
}

/* Independent copy of niri_input.c's run_niri_validate() SIGCHLD dance --
 * see that file's comment for why the process-wide `signal(SIGCHLD,
 * SIG_IGN)` (main.c) needs to be temporarily undone around this one
 * synchronous fork+wait. */
static dc_keybinds_validate_result run_niri_validate(const char *config_path)
{
    if (dryrun_enabled()) {
        dc_info("keybinds: [dryrun] niri validate -c %s", config_path);
        return DC_KEYBINDS_VALIDATE_UNKNOWN;
    }

    struct sigaction old_sa;
    struct sigaction dfl_sa = {0};
    dfl_sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &dfl_sa, &old_sa);

    pid_t pid = fork();
    if (pid < 0) {
        sigaction(SIGCHLD, &old_sa, NULL);
        dc_warn("keybinds: fork() failed, skipping validate");
        return DC_KEYBINDS_VALIDATE_UNKNOWN;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        execlp("niri", "niri", "validate", "-c", config_path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    sigaction(SIGCHLD, &old_sa, NULL);

    if (r != pid) {
        dc_warn("keybinds: waitpid() for `niri validate` failed");
        return DC_KEYBINDS_VALIDATE_UNKNOWN;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        dc_info("keybinds: `niri validate` OK");
        return DC_KEYBINDS_VALIDATE_OK;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        dc_info("keybinds: `niri` binary not found, skipping validate");
        return DC_KEYBINDS_VALIDATE_UNKNOWN;
    }
    dc_warn("keybinds: `niri validate` reported problems (see niri's own log/stderr)");
    return DC_KEYBINDS_VALIDATE_FAILED;
}

bool dc_keybinds_persist(const dc_keybind *managed, int n, const char *config_dir_override)
{
    char dir[DC_KEYBINDS_PATH_MAX];
    if (!resolve_config_dir(dir, sizeof(dir), config_dir_override))
        return false;

    char managed_path[DC_KEYBINDS_PATH_MAX];
    char config_path[DC_KEYBINDS_PATH_MAX];
    snprintf(managed_path, sizeof(managed_path), "%s/" DC_KEYBINDS_MANAGED_FILENAME, dir);
    snprintf(config_path, sizeof(config_path), "%s/config.kdl", dir);

    if (dryrun_enabled()) {
        dc_info("keybinds: [dryrun] would write %s:", managed_path);
        FILE *f = tmpfile();
        if (f) {
            serialize_fragment(f, managed, n);
            rewind(f);
            char line[256];
            while (fgets(line, sizeof(line), f))
                dc_info("keybinds: [dryrun]   %.*s", (int)strcspn(line, "\n"), line);
            fclose(f);
        }
        dc_info("keybinds: [dryrun] would ensure include %s in %s", DC_KEYBINDS_INCLUDE_LINE,
                config_path);
        g_last_validate = run_niri_validate(config_path);
        return true;
    }

    /* Snapshot the fragment's prior bytes (empty string if it doesn't exist
     * yet) so a validate failure below can be rolled back byte-for-byte. */
    char *snapshot = kb_read_file(managed_path);
    bool snapshot_existed = snapshot != NULL;
    if (!snapshot)
        snapshot = strdup("");
    if (!snapshot) {
        dc_warn("keybinds: out of memory snapshotting %s", managed_path);
        return false;
    }

    FILE *f = fopen(managed_path, "w");
    if (!f) {
        dc_warn("keybinds: could not write %s", managed_path);
        free(snapshot);
        return false;
    }
    serialize_fragment(f, managed, n);
    fclose(f);

    if (!ensure_include(config_path, DC_KEYBINDS_MANAGED_FILENAME)) {
        free(snapshot);
        return false;
    }

    dc_info("keybinds: persisted %d managed bind(s) to %s", n, managed_path);
    dc_keybinds_validate_result result = run_niri_validate(config_path);

    if (result == DC_KEYBINDS_VALIDATE_FAILED) {
        dc_warn("keybinds: `niri validate` failed after persist, rolling back %s", managed_path);
        bool restored;
        if (snapshot_existed)
            restored = write_raw_file(managed_path, snapshot);
        else
            restored = (unlink(managed_path) == 0 || errno == ENOENT);
        if (!restored) {
            dc_warn("keybinds: rollback of %s FAILED -- config may be left invalid", managed_path);
            free(snapshot);
            g_last_validate = DC_KEYBINDS_VALIDATE_FAILED;
            return false;
        }
        (void)run_niri_validate(config_path); /* best-effort re-check, not surfaced separately */
        g_last_validate = DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK;
        free(snapshot);
        return true;
    }

    g_last_validate = result;
    free(snapshot);
    return true;
}

/* ---------------------------------------------------------------------- *
 * Chord normalization / conflict detection / capture.
 * ---------------------------------------------------------------------- */

static void kb_append(char *out, size_t n, size_t *oi, const char *s)
{
    if (*oi >= n)
        return;
    int w = snprintf(out + *oi, n - *oi, "%s", s);
    if (w > 0) {
        size_t add = (size_t)w;
        if (*oi + add > n) /* snprintf's return is the would-be length, which
                             * can exceed the buffer on truncation -- clamp
                             * so *oi never overshoots n */
            add = n - *oi;
        *oi += add;
    }
}

typedef struct {
    const char *alias; /* matched case-insensitively */
    const char *canon;
    int rank; /* lower sorts first */
} kb_mod_alias;

static const kb_mod_alias KB_MOD_ALIASES[] = {
    {"mod", "Mod", 0},
    {"super", "Super", 1},
    {"logo", "Super", 1},
    {"win", "Super", 1},
    {"ctrl", "Ctrl", 2},
    {"control", "Ctrl", 2},
    {"alt", "Alt", 3},
    {"shift", "Shift", 4},
    {"iso_level3shift", "ISO_Level3Shift", 5},
    {"iso_level3", "ISO_Level3Shift", 5},
    {"iso_level5shift", "ISO_Level5Shift", 6},
    {"iso_level5", "ISO_Level5Shift", 6},
};

/* Returns the canonical spelling for a modifier token (case-insensitive
 * match) and writes its sort rank to `*rank_out`; returns NULL (leaving
 * `*rank_out` untouched) for anything not in the table. */
static const char *kb_mod_canon(const char *tok, int *rank_out)
{
    for (size_t i = 0; i < DC_ARRAY_LEN(KB_MOD_ALIASES); i++) {
        if (strcasecmp(tok, KB_MOD_ALIASES[i].alias) == 0) {
            *rank_out = KB_MOD_ALIASES[i].rank;
            return KB_MOD_ALIASES[i].canon;
        }
    }
    return NULL;
}

void dc_keybinds_normalize_chord(const char *in, char *out, size_t n)
{
    if (!out || n == 0)
        return;
    out[0] = '\0';
    if (!in || !in[0])
        return;

    char buf[DC_KEYBIND_CHORD_MAX];
    snprintf(buf, sizeof(buf), "%s", in);

    char *tokens[DC_KB_CHORD_TOKENS_MAX];
    int tok_n = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, "+", &save); t && tok_n < DC_KB_CHORD_TOKENS_MAX;
         t = strtok_r(NULL, "+", &save)) {
        while (*t == ' ' || *t == '\t')
            t++;
        size_t tl = strlen(t);
        while (tl > 0 && (t[tl - 1] == ' ' || t[tl - 1] == '\t'))
            t[--tl] = '\0';
        if (t[0]) /* drop empty tokens from e.g. a stray "++" */
            tokens[tok_n++] = t;
    }
    if (tok_n == 0)
        return;

    const char *base = tokens[tok_n - 1];
    int mod_count = tok_n - 1;

    const char *mod_canon[DC_KB_CHORD_TOKENS_MAX];
    int mod_rank[DC_KB_CHORD_TOKENS_MAX];
    for (int i = 0; i < mod_count; i++) {
        int rank = 100 + i; /* unrecognized: sort after all known mods, stable */
        const char *canon = kb_mod_canon(tokens[i], &rank);
        mod_canon[i] = canon ? canon : tokens[i];
        mod_rank[i] = rank;
    }
    /* Stable insertion sort by rank -- mod_count is at most 7, plenty small. */
    for (int i = 1; i < mod_count; i++) {
        const char *c = mod_canon[i];
        int r = mod_rank[i];
        int j = i - 1;
        while (j >= 0 && mod_rank[j] > r) {
            mod_canon[j + 1] = mod_canon[j];
            mod_rank[j + 1] = mod_rank[j];
            j--;
        }
        mod_canon[j + 1] = c;
        mod_rank[j + 1] = r;
    }

    char base_canon[64];
    xkb_keysym_t sym = xkb_keysym_from_name(base, XKB_KEYSYM_CASE_INSENSITIVE);
    if (sym != XKB_KEY_NoSymbol && xkb_keysym_get_name(sym, base_canon, sizeof(base_canon)) > 0) {
        /* resolved */
    } else {
        snprintf(base_canon, sizeof(base_canon), "%s", base);
    }

    size_t oi = 0;
    for (int i = 0; i < mod_count; i++) {
        kb_append(out, n, &oi, mod_canon[i]);
        kb_append(out, n, &oi, "+");
    }
    kb_append(out, n, &oi, base_canon);
}

int dc_keybinds_find_conflict(const dc_keybind *all, int n, const char *chord, int ignore_idx)
{
    if (!all || !chord || !chord[0])
        return -1;
    char norm_a[DC_KEYBIND_CHORD_MAX];
    dc_keybinds_normalize_chord(chord, norm_a, sizeof(norm_a));
    if (!norm_a[0])
        return -1;
    for (int i = 0; i < n; i++) {
        if (i == ignore_idx)
            continue;
        char norm_b[DC_KEYBIND_CHORD_MAX];
        dc_keybinds_normalize_chord(all[i].chord, norm_b, sizeof(norm_b));
        if (strcmp(norm_a, norm_b) == 0)
            return i;
    }
    return -1;
}

static bool kb_is_modifier_keysym(uint32_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_Scroll_Lock:
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_ISO_Level5_Shift:
        return true;
    default:
        return false;
    }
}

bool dc_keybinds_chord_from_capture(uint32_t base_keysym, bool super, bool ctrl, bool alt,
                                    bool shift, char *out, size_t n)
{
    if (!out || n == 0)
        return false;
    if (base_keysym == XKB_KEY_NoSymbol || kb_is_modifier_keysym(base_keysym))
        return false;

    char keyname[64];
    if (xkb_keysym_get_name((xkb_keysym_t)base_keysym, keyname, sizeof(keyname)) <= 0)
        return false;

    size_t oi = 0;
    if (super) {
        kb_append(out, n, &oi, "Mod");
        kb_append(out, n, &oi, "+");
    }
    if (ctrl) {
        kb_append(out, n, &oi, "Ctrl");
        kb_append(out, n, &oi, "+");
    }
    if (alt) {
        kb_append(out, n, &oi, "Alt");
        kb_append(out, n, &oi, "+");
    }
    if (shift) {
        kb_append(out, n, &oi, "Shift");
        kb_append(out, n, &oi, "+");
    }
    kb_append(out, n, &oi, keyname);
    return true;
}
