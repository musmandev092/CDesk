/* keybinds_modal.c — the keybind cheat-sheet overlay.
 *
 * Reference: DankMaterialShell's Modals/KeybindsModal.qml +
 * Modals/KeybindsContent.qml (DankModal-based centered overlay; masonry
 * columns of categorized key-chord chips + descriptions, `Theme.spacingL`
 * inset, per-category header + divider, scrollable if it overflows). This is
 * the read-only cheat-sheet, not DMS's editable keybind-remapping settings UI
 * (Widgets/KeybindItem.qml) -- there is no click-to-edit here (edits happen in
 * dankc's own Settings > Keybinds tab, see below).
 *
 * Surface/animation/scrim plumbing is copy-pattern from powermenu.c (centered
 * full-output overlay with a dim scrim, fade+scale entrance/exit, Esc /
 * click-outside dismiss); the scrollable-list plumbing (nvgScissor content
 * region + a thumb on the right edge) is copy-pattern from processes.c.
 *
 * DATA SOURCE (KB-T4, docs/23-KEYBIND-EDITING-PLAN.md): this file used to
 * parse ~/.config/niri/config.kdl itself with a private tolerant KDL parser
 * (kb_parse_config -> kb_parse_file -> kb_scan_binds_blocks ->
 * kb_parse_one_bind). That parser has been generalized into a shared backend,
 * services/keybinds.c/.h, so both this overlay and the Settings > Keybinds
 * CRUD tab (ui/settings.c) read the same binds the same way. This file now
 * only calls dc_keybinds_load() (kb_load_config() below) to get every bind
 * (each tagged with its `source` file and a `managed` bool -- true when the
 * bind lives in dankc's own dankc-binds.kdl fragment) and keeps the
 * presentation logic: action -> category/label prettification, per-category
 * masonry layout, and the "dankc"-managed row badge. Deliberately distinct
 * from `dankc keybinds` (main.c's print_keybinds()), which only prints a
 * snippet of dankc's OWN control-command binds to paste into the user's
 * config; this overlay reads the user's real, live binds back out (including
 * anything reached via the user's own `include "...";` chain, e.g. a DMS
 * install's dms/binds.kdl, or dankc's managed dankc-binds.kdl) and displays
 * them, re-loading fresh from the service every time the overlay is shown so
 * edits made in Settings > Keybinds appear immediately. A footer hint points
 * users at Settings > Keybinds to actually edit dankc-managed binds -- this
 * overlay itself remains strictly read-only.
 */
#include "ui/keybinds_modal.h"

#include "core/anim.h"
#include "core/log.h"
#include "dc.h"
#include "services/keybinds.h"
#include "theme/theme.h"
#include "render/nvg.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_SCALE_BASE 120

/* Card geometry (logical px): DMS's KeybindsModalOverlay caps modalWidth/
 * modalHeight at 92% of the screen, up to 1200x900 -- matched exactly. */
#define DC_KB_MAX_W 1200.0f
#define DC_KB_MAX_H 900.0f
#define DC_KB_SCREEN_FRAC 0.92f
#define DC_KB_PAD 6.0f    /* outer gutter for the drop shadow */
#define DC_KB_RADIUS 20.0f
#define DC_KB_INSET 20.0f /* Theme.spacingL: card edge -> content column */
#define DC_KB_HEADER_H 40.0f
#define DC_KB_BOTTOM_PAD 14.0f

#define DC_KB_COL_TARGET_W 350.0f /* DMS: Math.floor(width / 350), capped 1..3 */
#define DC_KB_COL_MAX 3
#define DC_KB_COL_GAP 20.0f

#define DC_KB_CAT_HEADER_H 24.0f
#define DC_KB_CAT_GAP 22.0f
#define DC_KB_ROW_H 22.0f
#define DC_KB_ROW_GAP 5.0f
#define DC_KB_KEY_COL_W 178.0f /* fixed offset to desc text, DMS: leftMargin 170 */
#define DC_KB_CHIP_H 20.0f
#define DC_KB_CHIP_MAX_W 164.0f

#define DC_KB_SCROLL_STEP 48.0f

#define DC_KB_MAX_BINDS 512
#define DC_KB_MAX_CATS 16
#define DC_KB_KEY_MAX 64
#define DC_KB_DESC_MAX 160
#define DC_KB_CAT_MAX 24

#define DC_KB_BADGE_W 42.0f  /* "dankc"-managed row marker pill */
#define DC_KB_BADGE_H 15.0f
#define DC_KB_FOOTER_H 22.0f /* reserved strip for the Settings hint line */

typedef struct {
    char cat[DC_KB_CAT_MAX];
    char key[DC_KB_KEY_MAX];
    char desc[DC_KB_DESC_MAX];
    bool managed; /* true when this bind's source is dankc-binds.kdl */
} dc_kb_entry;

struct dc_keybinds_modal {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height, scale120, phys_width, phys_height;

    /* Parsed keybinds, grouped by category (kb_group_by_category() partitions
     * `entries` in place and records each category's contiguous [start,
     * start+count) range -- see that function for why). */
    dc_kb_entry entries[DC_KB_MAX_BINDS];
    int entry_count;
    const char *cat_name[DC_KB_MAX_CATS];
    int cat_start[DC_KB_MAX_CATS];
    int cat_count[DC_KB_MAX_CATS];
    int cat_n;

    float scroll, scroll_max;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    bool visible, configured, egl_ready;
};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}
static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static void kb_render(dc_keybinds_modal *kb);
static void kb_teardown(dc_keybinds_modal *kb);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_keybinds_modal *kb = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    kb->frame_cb = NULL;
    if (!kb->visible)
        return;
    if (dc_anim_active(&kb->anim))
        kb_render(kb);
    else if (kb->closing)
        kb_teardown(kb);
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

/* ---------------------------------------------------------------------- *
 * Action -> category/label prettification (a hand-picked subset of DMS's
 * Common/KeybindActions.js NIRI_ACTIONS/DMS_ACTIONS tables -- not a byte-for-
 * byte port; unrecognized verbs fall back to a generic kebab/camelCase ->
 * Title Case prettifier so any user's config still reads reasonably). The
 * niri-verb table itself (verb/label/cat) lives in services/keybinds.c now --
 * dc_keybinds_niri_actions() below -- shared with the Settings action picker;
 * this file only prettifies with it.
 * ---------------------------------------------------------------------- */

/* Subsystem-name prettification for `spawn dms ipc call <subsystem> ...`. */
static const char *const KB_DMS_SUBSYS[][2] = {
    {"spotlight", "Launcher"},
    {"spotlight-bar", "Launcher Bar"},
    {"clipboard", "Clipboard"},
    {"notifications", "Notifications"},
    {"processlist", "Task Manager"},
    {"settings", "Settings"},
    {"powermenu", "Power Menu"},
    {"control-center", "Control Center"},
    {"notepad", "Notepad"},
    {"dash", "Dashboard"},
    {"dankdash", "Wallpaper Browser"},
    {"file", "File"},
    {"color-picker", "Color Picker"},
    {"lock", "Lock Screen"},
    {"inhibit", "Idle Inhibit"},
    {"theme", "Theme"},
    {"night", "Night Mode"},
    {"bar", "Bar"},
    {"dock", "Dock"},
    {"wallpaper", "Wallpaper"},
    {"window-rules", "Window Rules"},
    {"keybinds", "Keybinds Cheatsheet"},
    {"defaultApp", "Default App"},
    {"niri", "Niri"},
};

/* kebab-case or camelCase `s` -> "Title Case Words" in `out`. */
static void kb_title_words(const char *s, char *out, size_t outsz)
{
    size_t oi = 0;
    bool start_word = true;
    bool prev_lower = false;
    for (const char *p = s; *p && oi + 1 < outsz; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '-' || c == '_') {
            start_word = true;
            prev_lower = false;
            continue;
        }
        bool boundary = prev_lower && isupper(c);
        if ((start_word || boundary) && oi > 0 && out[oi - 1] != ' ') {
            if (oi + 1 < outsz)
                out[oi++] = ' ';
        }
        if (start_word || boundary) {
            if (oi + 1 < outsz)
                out[oi++] = (char)toupper(c);
        } else {
            if (oi + 1 < outsz)
                out[oi++] = (char)tolower(c);
        }
        start_word = false;
        prev_lower = islower(c) || isdigit(c);
    }
    out[oi] = '\0';
    if (oi == 0)
        snprintf(out, outsz, "%s", s);
}

static const char *kb_subsys_label(const char *subsys, char *buf, size_t bufsz)
{
    for (size_t i = 0; i < DC_ARRAY_LEN(KB_DMS_SUBSYS); i++) {
        if (strcmp(KB_DMS_SUBSYS[i][0], subsys) == 0)
            return KB_DMS_SUBSYS[i][1];
    }
    kb_title_words(subsys, buf, bufsz);
    return buf;
}

/* `spawn "dms" "ipc" "call" <subsys> <action> [extra...]` -> a human label.
 * `args`/`argc` start right after "call". */
static void kb_dms_label(char **args, int argc, char *out, size_t outsz)
{
    if (argc < 1) {
        snprintf(out, outsz, "DMS Action");
        return;
    }
    const char *subsys = args[0];
    const char *action = argc >= 2 ? args[1] : "";
    const char *extra = argc >= 3 ? args[2] : NULL;

    if (strcmp(subsys, "audio") == 0) {
        if (strcmp(action, "micmute") == 0) {
            snprintf(out, outsz, "Microphone Mute Toggle");
            return;
        }
        if (strcmp(action, "mute") == 0) {
            snprintf(out, outsz, "Volume Mute Toggle");
            return;
        }
        if (strcmp(action, "cycleoutput") == 0) {
            snprintf(out, outsz, "Audio Output: Cycle");
            return;
        }
        if (strcmp(action, "increment") == 0 || strcmp(action, "decrement") == 0) {
            const char *dir = strcmp(action, "increment") == 0 ? "Up" : "Down";
            if (extra && extra[0])
                snprintf(out, outsz, "Volume %s (%s%%)", dir, extra);
            else
                snprintf(out, outsz, "Volume %s", dir);
            return;
        }
    }
    if (strcmp(subsys, "mic") == 0 && strcmp(action, "mute") == 0) {
        snprintf(out, outsz, "Microphone Mute Toggle");
        return;
    }
    if (strcmp(subsys, "brightness") == 0 &&
        (strcmp(action, "increment") == 0 || strcmp(action, "decrement") == 0)) {
        const char *dir = strcmp(action, "increment") == 0 ? "Up" : "Down";
        if (extra && extra[0])
            snprintf(out, outsz, "Brightness %s (%s%%)", dir, extra);
        else
            snprintf(out, outsz, "Brightness %s", dir);
        return;
    }
    if (strcmp(subsys, "mpris") == 0) {
        if (strcmp(action, "playPause") == 0) {
            snprintf(out, outsz, "Media: Play/Pause");
            return;
        }
        if (strcmp(action, "next") == 0) {
            snprintf(out, outsz, "Media: Next Track");
            return;
        }
        if (strcmp(action, "previous") == 0) {
            snprintf(out, outsz, "Media: Previous Track");
            return;
        }
        if (strcmp(action, "stop") == 0) {
            snprintf(out, outsz, "Media: Stop");
            return;
        }
        if (strcmp(action, "increment") == 0 || strcmp(action, "decrement") == 0) {
            const char *dir = strcmp(action, "increment") == 0 ? "Up" : "Down";
            if (extra && extra[0])
                snprintf(out, outsz, "Player Volume %s (%s%%)", dir, extra);
            else
                snprintf(out, outsz, "Player Volume %s", dir);
            return;
        }
    }
    if (strcmp(subsys, "processlist") == 0 && strcmp(action, "focusOrToggle") == 0) {
        snprintf(out, outsz, "Task Manager: Focus or Toggle");
        return;
    }
    if (strcmp(subsys, "settings") == 0 && strcmp(action, "focusOrToggle") == 0) {
        snprintf(out, outsz, "Settings: Focus or Toggle");
        return;
    }
    if (strcmp(subsys, "dankdash") == 0) {
        snprintf(out, outsz, "Wallpaper Browser");
        return;
    }
    if (strcmp(subsys, "workspace-rename") == 0) {
        snprintf(out, outsz, "Workspace: Rename");
        return;
    }

    char subsys_buf[48];
    const char *subsys_label = kb_subsys_label(subsys, subsys_buf, sizeof(subsys_buf));
    char action_words[64] = {0};
    if (action[0])
        kb_title_words(action, action_words, sizeof(action_words));
    if (action_words[0])
        snprintf(out, outsz, "%s: %s", subsys_label, action_words);
    else
        snprintf(out, outsz, "%s", subsys_label);
}

static void kb_join_args(char **args, int argc, char *out, size_t outsz)
{
    size_t oi = 0;
    out[0] = '\0';
    for (int i = 0; i < argc && oi + 1 < outsz; i++) {
        int w = snprintf(out + oi, outsz - oi, "%s%s", i ? " " : "", args[i]);
        if (w > 0)
            oi += (size_t)w;
    }
}

/* Category + human label for one bind's action (`argv[0]` is the verb,
 * `argv[1..]` its arguments). Used as the description only when the bind has
 * no explicit `hotkey-overlay-title="..."`. */
static const char *kb_category_and_label(char **argv, int argc, char *label_out, size_t label_sz)
{
    if (argc < 1) {
        snprintf(label_out, label_sz, "(no action)");
        return "Other";
    }
    const char *verb = argv[0];

    if (strcmp(verb, "spawn") == 0) {
        if (argc >= 4 && strcmp(argv[1], "dms") == 0 && strcmp(argv[2], "ipc") == 0 &&
            strcmp(argv[3], "call") == 0) {
            kb_dms_label(argv + 4, argc - 4, label_out, label_sz);
            return "DMS";
        }
        char joined[128];
        kb_join_args(argv + 1, argc - 1, joined, sizeof(joined));
        if (joined[0])
            snprintf(label_out, label_sz, "Launch %s", joined);
        else
            snprintf(label_out, label_sz, "Run Command");
        return "Applications";
    }

    int niri_count = 0;
    const dc_keybind_action_preset *niri_actions = dc_keybinds_niri_actions(&niri_count);
    for (int i = 0; i < niri_count; i++) {
        if (strcmp(niri_actions[i].verb, verb) != 0)
            continue;
        char extra[80] = {0};
        if (argc > 1) {
            char joined[64];
            kb_join_args(argv + 1, argc - 1, joined, sizeof(joined));
            snprintf(extra, sizeof(extra), " %s", joined);
        }
        snprintf(label_out, label_sz, "%s%s", niri_actions[i].label, extra);
        return niri_actions[i].cat;
    }

    /* Unknown verb: generic kebab-case -> Title Case fallback so any action
     * this table doesn't know about still reads reasonably. */
    char words[64];
    kb_title_words(verb, words, sizeof(words));
    if (argc > 1) {
        char joined[64];
        kb_join_args(argv + 1, argc - 1, joined, sizeof(joined));
        snprintf(label_out, label_sz, "%s %s", words, joined);
    } else {
        snprintf(label_out, label_sz, "%s", words);
    }
    return "Other";
}

/* "Mod+Shift+D" -> "Mod + Shift + D" (DMS's KeybindsContent.qml: `key.replace(/\+/g, " + ")`). */
static void kb_format_key(const char *raw, char *out, size_t outsz)
{
    size_t oi = 0;
    for (const char *p = raw; *p && oi + 3 < outsz; p++) {
        if (*p == '+') {
            out[oi++] = ' ';
            out[oi++] = '+';
            out[oi++] = ' ';
        } else {
            out[oi++] = *p;
        }
    }
    out[oi] = '\0';
}

/* ---------------------------------------------------------------------- *
 * Sourcing binds from the shared keybind service (services/keybinds.c/.h).
 * ---------------------------------------------------------------------- */

/* Tokenize a (already trimmed, `;`-stripped) action body respecting quoted
 * strings, e.g. `spawn "dms" "ipc" "call" "audio" "mute"` -> ["spawn","dms",
 * "ipc","call","audio","mute"]. Returns the token count (capped at max). */
static int kb_tokenize(const char *text, size_t len, char argv[][48], int max)
{
    int argc = 0;
    size_t i = 0;
    while (i < len && argc < max) {
        while (i < len && isspace((unsigned char)text[i]))
            i++;
        if (i >= len)
            break;
        char *dst = argv[argc];
        size_t di = 0;
        if (text[i] == '"') {
            i++;
            while (i < len && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < len)
                    i++;
                if (di + 1 < 48)
                    dst[di++] = text[i];
                i++;
            }
            if (i < len)
                i++; /* closing quote */
        } else {
            while (i < len && !isspace((unsigned char)text[i])) {
                if (di + 1 < 48)
                    dst[di++] = text[i];
                i++;
            }
        }
        dst[di] = '\0';
        argc++;
    }
    return argc;
}

/* Turns one service-provided dc_keybind into a display dc_kb_entry: formats
 * the chord for display, tokenizes the (already-trimmed, verbatim) action
 * body for kb_category_and_label()'s prettifier, and carries the `managed`
 * flag through so the render path can draw its badge. */
static void kb_add_entry_from_bind(dc_keybinds_modal *kb, const dc_keybind *bind)
{
    if (kb->entry_count >= DC_KB_MAX_BINDS || bind->chord[0] == '\0')
        return;
    dc_kb_entry *e = &kb->entries[kb->entry_count];

    kb_format_key(bind->chord, e->key, sizeof(e->key));

    char argv[8][48];
    int argc = kb_tokenize(bind->action, strlen(bind->action), argv, 8);
    char *argv_ptrs[8];
    for (int i = 0; i < argc; i++)
        argv_ptrs[i] = argv[i];

    char label[DC_KB_DESC_MAX];
    const char *cat = kb_category_and_label(argv_ptrs, argc, label, sizeof(label));
    snprintf(e->cat, sizeof(e->cat), "%s", cat);
    if (bind->title[0])
        snprintf(e->desc, sizeof(e->desc), "%s", bind->title);
    else
        snprintf(e->desc, sizeof(e->desc), "%s", label);
    e->managed = bind->managed;

    kb->entry_count++;
}

/* Stable-partition `kb->entries` by category, in KB_ORDER's preference order,
 * recording each present category's contiguous [start, start+count) range --
 * this way the masonry layout only needs per-category counts, not to re-scan
 * `entries` by string comparison on every render. */
static void kb_group_by_category(dc_keybinds_modal *kb)
{
    static const char *const KB_ORDER[] = {"DMS",     "Applications", "Workspace", "Window",
                                           "Focus",   "Move",         "Monitor",   "Screenshot",
                                           "System",  "Alt-Tab",      "Other"};
    kb->cat_n = 0;
    if (kb->entry_count == 0)
        return;

    dc_kb_entry *tmp = malloc(sizeof(dc_kb_entry) * (size_t)kb->entry_count);
    bool *taken = calloc((size_t)kb->entry_count, sizeof(bool));
    if (!tmp || !taken) {
        free(tmp);
        free(taken);
        return;
    }

    int w = 0;
    for (size_t ci = 0; ci < DC_ARRAY_LEN(KB_ORDER); ci++) {
        int start = w;
        for (int i = 0; i < kb->entry_count; i++) {
            if (taken[i])
                continue;
            if (strcmp(kb->entries[i].cat, KB_ORDER[ci]) != 0)
                continue;
            tmp[w++] = kb->entries[i];
            taken[i] = true;
        }
        int count = w - start;
        if (count > 0 && kb->cat_n < DC_KB_MAX_CATS) {
            kb->cat_name[kb->cat_n] = KB_ORDER[ci];
            kb->cat_start[kb->cat_n] = start;
            kb->cat_count[kb->cat_n] = count;
            kb->cat_n++;
        }
    }
    /* Any entry with a category outside KB_ORDER (shouldn't happen --
     * kb_category_and_label() only ever returns names from that list) is
     * still preserved rather than silently dropped. */
    for (int i = 0; i < kb->entry_count; i++) {
        if (!taken[i])
            tmp[w++] = kb->entries[i];
    }

    memcpy(kb->entries, tmp, sizeof(dc_kb_entry) * (size_t)kb->entry_count);
    free(tmp);
    free(taken);
}

/* Loads every bind via dc_keybinds_load() (real ~/.config/niri, following
 * `include "...";` recursively -- see services/keybinds.h) and rebuilds
 * `kb->entries`/category grouping from scratch. Called on every kb_show(), so
 * edits made in Settings > Keybinds are picked up the next time the overlay
 * opens. */
static void kb_load_config(dc_keybinds_modal *kb)
{
    kb->entry_count = 0;
    kb->cat_n = 0;

    dc_keybind *binds = malloc(sizeof(dc_keybind) * DC_KB_MAX_BINDS);
    if (!binds) {
        dc_warn("keybinds-overlay: out of memory loading binds");
        return;
    }
    int n = dc_keybinds_load(binds, DC_KB_MAX_BINDS, NULL);
    for (int i = 0; i < n; i++)
        kb_add_entry_from_bind(kb, &binds[i]);
    free(binds);

    kb_group_by_category(kb);
    dc_info("keybinds-overlay: loaded %d bind(s) in %d categor%s via shared keybind service",
            kb->entry_count, kb->cat_n, kb->cat_n == 1 ? "y" : "ies");
}

/* ---------------------------------------------------------------------- *
 * Masonry column layout.
 * ---------------------------------------------------------------------- */

typedef struct {
    int cat_idx;
    float height;
} kb_ch;

static int kb_cmp_height_desc(const void *a, const void *b)
{
    const kb_ch *x = a, *y = b;
    if (x->height > y->height)
        return -1;
    if (x->height < y->height)
        return 1;
    return 0;
}

static float kb_cat_height(int count)
{
    return DC_KB_CAT_HEADER_H + (float)count * (DC_KB_ROW_H + DC_KB_ROW_GAP) - DC_KB_ROW_GAP;
}

/* Greedy shortest-column-first assignment, matching KeybindsContent.qml's
 * distributeCategories(): sort categories by estimated height descending,
 * drop each into whichever column is currently shortest. */
static void kb_distribute_columns(dc_keybinds_modal *kb, int cols, int col_cats[DC_KB_COL_MAX][DC_KB_MAX_CATS],
                                  int col_cat_count[DC_KB_COL_MAX], float col_height[DC_KB_COL_MAX])
{
    kb_ch arr[DC_KB_MAX_CATS];
    for (int i = 0; i < kb->cat_n; i++) {
        arr[i].cat_idx = i;
        arr[i].height = kb_cat_height(kb->cat_count[i]);
    }
    qsort(arr, (size_t)kb->cat_n, sizeof(arr[0]), kb_cmp_height_desc);

    for (int c = 0; c < cols; c++) {
        col_cat_count[c] = 0;
        col_height[c] = 0.0f;
    }
    for (int i = 0; i < kb->cat_n; i++) {
        int min_c = 0;
        for (int c = 1; c < cols; c++) {
            if (col_height[c] < col_height[min_c])
                min_c = c;
        }
        col_cats[min_c][col_cat_count[min_c]++] = arr[i].cat_idx;
        col_height[min_c] += arr[i].height + DC_KB_CAT_GAP;
    }
}

/* ---------------------------------------------------------------------- *
 * Rendering.
 * ---------------------------------------------------------------------- */

typedef struct {
    float card_x, card_y, card_w, card_h;
    float content_x, content_y, content_w, content_h;
    float footer_y; /* baseline for the "Edit ... in Settings" hint */
} kb_layout;

static kb_layout kb_get_layout(float screen_w, float screen_h)
{
    kb_layout lay;
    lay.card_w = screen_w * DC_KB_SCREEN_FRAC;
    if (lay.card_w > DC_KB_MAX_W)
        lay.card_w = DC_KB_MAX_W;
    lay.card_h = screen_h * DC_KB_SCREEN_FRAC;
    if (lay.card_h > DC_KB_MAX_H)
        lay.card_h = DC_KB_MAX_H;
    lay.card_x = (screen_w - lay.card_w) / 2.0f;
    lay.card_y = (screen_h - lay.card_h) / 2.0f;
    lay.content_x = lay.card_x + DC_KB_PAD + DC_KB_INSET;
    lay.content_y = lay.card_y + DC_KB_PAD + DC_KB_HEADER_H;
    lay.content_w = lay.card_w - 2.0f * DC_KB_PAD - 2.0f * DC_KB_INSET;
    lay.content_h =
        lay.card_y + lay.card_h - DC_KB_PAD - DC_KB_BOTTOM_PAD - DC_KB_FOOTER_H - lay.content_y;
    lay.footer_y = lay.content_y + lay.content_h + DC_KB_FOOTER_H / 2.0f;
    return lay;
}

/* Truncate on a UTF-8 boundary + ellipsis (same pattern as processes.c's
 * ps_ellipsize()/controlcenter.c's cc_ellipsize(), duplicated locally). */
static void kb_ellipsize(NVGcontext *vg, char *buf, size_t bufsize, float max_w)
{
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, buf, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_w)
        return;
    size_t len = strlen(buf);
    if (bufsize < 4)
        return;
    char tmp[192];
    size_t cap = bufsize > sizeof(tmp) ? sizeof(tmp) : bufsize;
    if (len > cap - 4)
        len = cap - 4;
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--;
        snprintf(tmp, cap, "%.*s\xe2\x80\xa6", (int)len, buf);
        nvgTextBounds(vg, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_w || len == 0) {
            memcpy(buf, tmp, cap);
            return;
        }
    }
}

static void kb_draw_chip(dc_render *r, float x, float y, const char *key)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 11.0f);

    /* Long chords (e.g. "Mod + Ctrl + WheelScrollDown") would otherwise get
     * hard-clipped by the scissor below with no indication text is missing
     * -- ellipsize instead, same as the description column. */
    char key_buf[DC_KB_KEY_MAX];
    snprintf(key_buf, sizeof(key_buf), "%s", key);
    kb_ellipsize(vg, key_buf, sizeof(key_buf), DC_KB_CHIP_MAX_W - 14.0f);

    float b[4];
    nvgTextBounds(vg, 0, 0, key_buf, NULL, b);
    float w = (b[2] - b[0]) + 14.0f;
    if (w > DC_KB_CHIP_MAX_W)
        w = DC_KB_CHIP_MAX_W;
    if (w < 24.0f)
        w = 24.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, DC_KB_CHIP_H, DC_KB_CHIP_H / 4.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->secondary));
    nvgText(vg, x + w / 2.0f, y + DC_KB_CHIP_H / 2.0f + 0.5f, key_buf, NULL);
}

/* Subtle pill tag on rows whose bind is managed by dankc's Settings > Keybinds
 * tab (dc_keybind.managed / dc_kb_entry.managed), so a glance at the cheat
 * sheet distinguishes dankc-managed binds from user-defined/unmanaged ones. */
static void kb_draw_managed_badge(dc_render *r, float x, float y)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, DC_KB_BADGE_W, DC_KB_BADGE_H, DC_KB_BADGE_H / 2.0f);
    nvgFillColor(vg, tc_alpha(t->primary, 36));
    nvgFill(vg);

    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 9.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, x + DC_KB_BADGE_W / 2.0f, y + DC_KB_BADGE_H / 2.0f + 0.5f, "dankc", NULL);
}

static void kb_render(dc_keybinds_modal *kb)
{
    if (!kb->configured || kb->phys_width <= 0)
        return;
    if (!kb->egl_ready) {
        if (!dc_egl_window_init(&kb->egl_window, kb->egl, kb->surface, kb->phys_width, kb->phys_height))
            return;
        kb->egl_ready = true;
    } else {
        dc_egl_window_resize(&kb->egl_window, kb->phys_width, kb->phys_height);
    }
    if (!dc_egl_make_current(kb->egl, &kb->egl_window))
        return;
    if (!dc_render_ensure(kb->render))
        return;
    if (kb->viewport)
        wp_viewport_set_destination(kb->viewport, kb->logical_width, kb->logical_height);

    NVGcontext *vg = kb->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = (float)kb->logical_width, h = (float)kb->logical_height;

    glViewport(0, 0, kb->phys_width, kb->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)kb->scale120 / DC_SCALE_BASE);

    float pr = dc_anim_progress(&kb->anim);
    if (kb->closing)
        pr = 1.0f - (pr > 1.0f ? 1.0f : pr);
    float alpha = pr > 1.0f ? 1.0f : (pr < 0.0f ? 0.0f : pr);
    float scale = 0.94f + 0.06f * (pr > 1.0f ? 1.0f : pr);

    kb_layout lay = kb_get_layout(w, h);
    float pivot_x = lay.card_x + lay.card_w / 2.0f;
    float pivot_y = lay.card_y + lay.card_h / 2.0f;

    /* Scrim (DMS: black @ ~0.5, same as powermenu.c). */
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, (unsigned char)(alpha * 128.0f)));
    nvgFill(vg);

    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, pivot_x, pivot_y);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -pivot_x, -pivot_y);

    NVGpaint shadow = nvgBoxGradient(vg, lay.card_x, lay.card_y + 2.0f, lay.card_w, lay.card_h,
                                     DC_KB_RADIUS, 24.0f, nvgRGBA(0, 0, 0, 130), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, lay.card_x - 40.0f, lay.card_y - 40.0f, lay.card_w + 80.0f, lay.card_h + 80.0f);
    nvgRoundedRect(vg, lay.card_x, lay.card_y, lay.card_w, lay.card_h, DC_KB_RADIUS);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, lay.card_x, lay.card_y, lay.card_w, lay.card_h, DC_KB_RADIUS);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* --- Header: title + bind count, Esc hint on the right --- */
    const float header_cy = lay.card_y + DC_KB_PAD + DC_KB_HEADER_H / 2.0f;
    nvgFontFaceId(vg, kb->render->font_ui);
    nvgFontSize(vg, 18.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, lay.content_x, header_cy, "Keybinds", NULL);

    char count_buf[32];
    snprintf(count_buf, sizeof(count_buf), "%d shortcut%s", kb->entry_count,
             kb->entry_count == 1 ? "" : "s");
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, tc_alpha(t->surface_text, 130));
    nvgText(vg, lay.content_x + 96.0f, header_cy, count_buf, NULL);

    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 130));
    nvgText(vg, lay.content_x + lay.content_w, header_cy, "Esc or click outside to close", NULL);

    nvgBeginPath(vg);
    nvgMoveTo(vg, lay.content_x, lay.card_y + DC_KB_PAD + DC_KB_HEADER_H);
    nvgLineTo(vg, lay.content_x + lay.content_w, lay.card_y + DC_KB_PAD + DC_KB_HEADER_H);
    nvgStrokeColor(vg, tc_alpha(t->outline, 60));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* --- Content: masonry columns of categorized binds --- */
    if (kb->entry_count == 0) {
        nvgFontFaceId(vg, kb->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 150));
        nvgText(vg, lay.card_x + lay.card_w / 2.0f, lay.content_y + lay.content_h / 2.0f,
               "No keybinds found in ~/.config/niri/config.kdl", NULL);
    } else {
        int cols = (int)(lay.content_w / DC_KB_COL_TARGET_W);
        if (cols < 1)
            cols = 1;
        if (cols > DC_KB_COL_MAX)
            cols = DC_KB_COL_MAX;
        float col_w = (lay.content_w - (float)(cols - 1) * DC_KB_COL_GAP) / (float)cols;

        int col_cats[DC_KB_COL_MAX][DC_KB_MAX_CATS];
        int col_cat_count[DC_KB_COL_MAX];
        float col_height[DC_KB_COL_MAX];
        kb_distribute_columns(kb, cols, col_cats, col_cat_count, col_height);

        float content_total_h = 0.0f;
        for (int c = 0; c < cols; c++) {
            float ch = col_height[c] > 0.0f ? col_height[c] - DC_KB_CAT_GAP : 0.0f;
            if (ch > content_total_h)
                content_total_h = ch;
        }
        kb->scroll_max = content_total_h > lay.content_h ? content_total_h - lay.content_h : 0.0f;
        if (kb->scroll < 0.0f)
            kb->scroll = 0.0f;
        if (kb->scroll > kb->scroll_max)
            kb->scroll = kb->scroll_max;

        nvgSave(vg);
        nvgScissor(vg, lay.content_x, lay.content_y, lay.content_w, lay.content_h);
        for (int c = 0; c < cols; c++) {
            float x = lay.content_x + (float)c * (col_w + DC_KB_COL_GAP);
            float y = lay.content_y - kb->scroll;
            for (int k = 0; k < col_cat_count[c]; k++) {
                int ci = col_cats[c][k];
                float cat_h = kb_cat_height(kb->cat_count[ci]);
                if (y + cat_h >= lay.content_y && y <= lay.content_y + lay.content_h) {
                    nvgFontFaceId(vg, kb->render->font_ui);
                    nvgFontSize(vg, 13.0f);
                    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
                    nvgFillColor(vg, tc(t->primary));
                    nvgText(vg, x, y, kb->cat_name[ci], NULL);

                    nvgBeginPath(vg);
                    nvgMoveTo(vg, x, y + 18.0f);
                    nvgLineTo(vg, x + col_w, y + 18.0f);
                    nvgStrokeColor(vg, tc_alpha(t->primary, 80));
                    nvgStrokeWidth(vg, 1.0f);
                    nvgStroke(vg);

                    float row_y = y + DC_KB_CAT_HEADER_H;
                    for (int bi = 0; bi < kb->cat_count[ci]; bi++) {
                        const dc_kb_entry *e = &kb->entries[kb->cat_start[ci] + bi];
                        if (row_y + DC_KB_ROW_H >= lay.content_y && row_y <= lay.content_y + lay.content_h) {
                            kb_draw_chip(kb->render, x, row_y + (DC_KB_ROW_H - DC_KB_CHIP_H) / 2.0f, e->key);

                            float badge_reserve = e->managed ? DC_KB_BADGE_W + 6.0f : 0.0f;
                            char desc_buf[DC_KB_DESC_MAX];
                            snprintf(desc_buf, sizeof(desc_buf), "%s", e->desc);
                            nvgFontFaceId(vg, kb->render->font_ui);
                            nvgFontSize(vg, 12.0f);
                            float desc_w = col_w - DC_KB_KEY_COL_W - badge_reserve;
                            if (desc_w > 8.0f)
                                kb_ellipsize(vg, desc_buf, sizeof(desc_buf), desc_w);
                            nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                            nvgFillColor(vg, tc_alpha(t->surface_text, 230));
                            nvgText(vg, x + DC_KB_KEY_COL_W, row_y + DC_KB_ROW_H / 2.0f, desc_buf, NULL);

                            if (e->managed)
                                kb_draw_managed_badge(kb->render, x + col_w - DC_KB_BADGE_W,
                                                      row_y + (DC_KB_ROW_H - DC_KB_BADGE_H) / 2.0f);
                        }
                        row_y += DC_KB_ROW_H + DC_KB_ROW_GAP;
                    }
                }
                y += cat_h + DC_KB_CAT_GAP;
            }
        }
        nvgRestore(vg);

        if (kb->scroll_max > 0.0f) {
            float track_x = lay.content_x + lay.content_w - 3.0f;
            float thumb_h = lay.content_h * (lay.content_h / content_total_h);
            if (thumb_h < 24.0f)
                thumb_h = 24.0f;
            float thumb_y =
                lay.content_y + (lay.content_h - thumb_h) * (kb->scroll / kb->scroll_max);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
            nvgFillColor(vg, tc_alpha(t->outline, 140));
            nvgFill(vg);
        }
    }

    /* --- Footer: points users at Settings > Keybinds for editing dankc-
     * managed binds (this overlay itself stays strictly read-only). --- */
    nvgFontFaceId(vg, kb->render->font_ui);
    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc_alpha(t->surface_text, 130));
    nvgText(vg, lay.card_x + lay.card_w / 2.0f, lay.footer_y,
            "Edit dankc-managed binds in Settings \xe2\x86\x92 Keybinds", NULL);

    nvgEndFrame(vg);
    if ((dc_anim_active(&kb->anim) || kb->closing) && !kb->frame_cb) {
        kb->frame_cb = wl_surface_frame(kb->surface);
        wl_callback_add_listener(kb->frame_cb, &frame_listener, kb);
    }
    dc_egl_swap(kb->egl, &kb->egl_window);
}

/* ---------------------------------------------------------------------- *
 * Surface lifecycle (copy-pattern from powermenu.c).
 * ---------------------------------------------------------------------- */

static void recompute_physical(dc_keybinds_modal *kb)
{
    kb->phys_width = (kb->logical_width * kb->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    kb->phys_height = (kb->logical_height * kb->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_keybinds_modal *kb = data;
    DC_UNUSED(fs);
    kb->scale120 = (int)scale;
    recompute_physical(kb);
    kb_render(kb);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_keybinds_modal *kb = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    kb->logical_width = width > 0 ? (int)width : 800;
    kb->logical_height = height > 0 ? (int)height : 600;
    kb->configured = true;
    recompute_physical(kb);
    kb_render(kb);
}
static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_keybinds_modal *kb = data;
    DC_UNUSED(surface);
    kb->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_keybinds_modal *dc_keybinds_modal_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_keybinds_modal *kb = calloc(1, sizeof(*kb));
    kb->wl = wl;
    kb->egl = egl;
    kb->render = render;
    kb->scale120 = DC_SCALE_BASE;
    return kb;
}

static void kb_show(dc_keybinds_modal *kb, dc_output *output)
{
    kb->output = output;
    kb->configured = false;
    kb->egl_ready = false;
    kb->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    kb->scroll = 0.0f;

    kb_load_config(kb);

    dc_anim_start(&kb->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    kb->surface = wl_compositor_create_surface(kb->wl->compositor);
    if (kb->wl->fractional_scale_mgr) {
        kb->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            kb->wl->fractional_scale_mgr, kb->surface);
        wp_fractional_scale_v1_add_listener(kb->fractional_scale, &fractional_scale_listener, kb);
    }
    if (kb->wl->viewporter)
        kb->viewport = wp_viewporter_get_viewport(kb->wl->viewporter, kb->surface);

    kb->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        kb->wl->layer_shell, kb->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:keybinds-overlay");

    /* Fill the whole output, same rationale as powermenu.c: a single surface
     * carries both the full-screen dismiss-scrim and the centered card. */
    zwlr_layer_surface_v1_set_anchor(kb->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(kb->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        kb->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(kb->layer_surface, &layer_surface_listener, kb);

    wl_surface_commit(kb->surface);
    kb->visible = true;
    kb->closing = false;
    dc_debug("keybinds-overlay shown");
}

static void kb_teardown(dc_keybinds_modal *kb)
{
    if (kb->frame_cb) {
        wl_callback_destroy(kb->frame_cb);
        kb->frame_cb = NULL;
    }
    if (kb->egl_ready)
        dc_egl_window_finish(&kb->egl_window, kb->egl);
    if (kb->viewport)
        wp_viewport_destroy(kb->viewport);
    if (kb->fractional_scale)
        wp_fractional_scale_v1_destroy(kb->fractional_scale);
    if (kb->layer_surface)
        zwlr_layer_surface_v1_destroy(kb->layer_surface);
    if (kb->surface)
        wl_surface_destroy(kb->surface);
    kb->egl_ready = false;
    kb->configured = false;
    kb->viewport = NULL;
    kb->fractional_scale = NULL;
    kb->layer_surface = NULL;
    kb->surface = NULL;
    kb->visible = false;
    kb->closing = false;
    dc_debug("keybinds-overlay hidden");
}

static void kb_begin_close(dc_keybinds_modal *kb)
{
    if (!kb->visible || kb->closing)
        return;
    dc_anim_start(&kb->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    kb->closing = true;
    if (!dc_anim_active(&kb->anim)) {
        kb_teardown(kb);
        return;
    }
    kb_render(kb);
}

void dc_keybinds_modal_toggle(dc_keybinds_modal *kb, dc_output *output)
{
    if (kb->visible)
        kb_begin_close(kb);
    else
        kb_show(kb, output);
}

void dc_keybinds_modal_hide(dc_keybinds_modal *kb)
{
    kb_begin_close(kb);
}

bool dc_keybinds_modal_visible(dc_keybinds_modal *kb)
{
    return kb->visible;
}

struct wl_surface *dc_keybinds_modal_surface(dc_keybinds_modal *kb)
{
    return kb->surface;
}

void dc_keybinds_modal_handle_key(dc_keybinds_modal *kb, uint32_t keysym, const char *utf8)
{
    DC_UNUSED(utf8);
    if (!kb->visible || kb->closing)
        return;
    if (keysym == XKB_KEY_Escape)
        kb_begin_close(kb);
}

void dc_keybinds_modal_handle_click(dc_keybinds_modal *kb, double x, double y)
{
    if (!kb->visible || kb->closing)
        return;
    kb_layout lay = kb_get_layout((float)kb->logical_width, (float)kb->logical_height);
    bool in_card = in_rect(x, y, lay.card_x, lay.card_y, lay.card_x + lay.card_w,
                           lay.card_y + lay.card_h);
    if (!in_card)
        kb_begin_close(kb);
    /* Clicks inside the card are a no-op -- nothing here is interactive. */
}

void dc_keybinds_modal_handle_scroll(dc_keybinds_modal *kb, int steps_v)
{
    if (!kb->visible || kb->closing || steps_v == 0)
        return;
    float s = kb->scroll + (float)steps_v * DC_KB_SCROLL_STEP;
    if (s < 0.0f)
        s = 0.0f;
    if (s > kb->scroll_max)
        s = kb->scroll_max;
    if (s == kb->scroll)
        return;
    kb->scroll = s;
    kb_render(kb);
}

void dc_keybinds_modal_destroy(dc_keybinds_modal *kb)
{
    if (!kb)
        return;
    if (kb->visible)
        kb_teardown(kb);
    free(kb);
}
