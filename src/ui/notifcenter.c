#include "ui/notifcenter.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "services/notifications.h"
#include "theme/theme.h"
#include "ui/hover.h"
#include "ui/material_bg.h"
#include "ui/notif_image.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Sized to match the user's live DMS reference screenshot (docs/13-POPOUTS-
 * SPEC.md sec.3, ~420x560 logical) rather than the previous 400x480 flat
 * list. */
#define DC_NC_WIDTH 420
#define DC_NC_HEIGHT 560
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/3: opens near the bell, effectively the bar's right cluster). */
#define DC_NC_SIDE_MARGIN 12

#define DC_NC_PAD 6.0f    /* outer gutter for the drop shadow */
#define DC_NC_RADIUS 14.0f
#define DC_NC_INSET 16.0f /* left/right content inset inside the card */

#define DC_NC_HEADER_TOP 14.0f
#define DC_NC_HEADER_H 28.0f
#define DC_NC_TABS_GAP 12.0f
#define DC_NC_TABS_H 30.0f
#define DC_NC_LIST_GAP 12.0f
#define DC_NC_BOTTOM_PAD 14.0f

/* DND chip row + History time-filter chip row (docs/26-DND-SCHEDULING-
 * PLAN.md UI/T3): same pill shape as draw_tab()'s tabs, just sized to each
 * chip's own label instead of a fixed slot. DC_NC_LIST_GAP is reused as the
 * vertical gap above/below each chip row so spacing stays visually
 * consistent with the existing tabs->list gap. */
#define DC_NC_CHIP_ROW_H 24.0f
#define DC_NC_CHIP_GAP 6.0f

#define DC_NC_CARD_H 120.0f
#define DC_NC_CARD_GAP 10.0f
#define DC_NC_MAX_CARDS DC_NOTIF_MAX

/* Grouping (docs/14-COMPLETION-PLAN.md W1.4): notifications sharing the same
 * app_name (case-insensitive) collapse under one header row with a count
 * badge; a lone notification (no siblings) still renders as a plain card,
 * no header. Worst case every notification is its own group (no header
 * rows) or every notification shares one group (one header + N member
 * cards when expanded) -- either way the number of *groups* is bounded by
 * the notification count, and the number of *rows* (headers + member
 * cards) by roughly 2x that. */
#define DC_NC_GROUP_HEADER_H 52.0f
#define DC_NC_MAX_GROUPS DC_NOTIF_MAX
#define DC_NC_MAX_ROWS (DC_NC_MAX_CARDS * 2)

#define DC_NC_SCROLL_STEP 48.0f

typedef enum {
    DC_NC_TAB_CURRENT = 0,
    DC_NC_TAB_HISTORY = 1,
} dc_nc_tab;

/* History time-filter chip selection (docs/26-DND-SCHEDULING-PLAN.md T3) --
 * values double as nc->history_filter and as indices into hist_chip_*[]. */
typedef enum {
    NC_HIST_ALL = 0,
    NC_HIST_TODAY = 1,
    NC_HIST_YESTERDAY = 2,
    NC_HIST_WEEK = 3,
} nc_hist_filter;

/* Hit-test rects for one visible card, captured during nc_render() and
 * consumed by dc_notif_center_handle_click() -- same "record while drawing"
 * convention controlcenter.c uses for its own buttons. */
typedef struct {
    uint32_t id;
    dc_notif_status status;
    float card_x0, card_y0, card_x1, card_y1;
    float close_x0, close_y0, close_x1, close_y1;
    float dismiss_x0, dismiss_y0, dismiss_x1, dismiss_y1;
    bool has_dismiss;
    int action_count;
    float action_x0[DC_NOTIF_ACTION_MAX], action_x1[DC_NOTIF_ACTION_MAX];
    float action_y0, action_y1; /* shared row -- same for every action button */
} nc_card_hit;

/* One collapsed/expanded app-group header row (grouping, docs/14-COMPLETION-
 * PLAN.md W1.4): notifications sharing the same app_name (case-insensitive)
 * collapse under this header with a count badge; the whole row is one click
 * target that toggles expansion (dc_notif_center's group_state[] persists
 * which keys are expanded across renders/tab switches). Only emitted for
 * groups with more than one member -- a lone notification renders as a
 * normal draw_card() with no header at all. */
typedef struct {
    char key[DC_NOTIF_APP]; /* lowercased app_name -- matches group_state[].key */
    float x0, y0, x1, y1;   /* whole header rect -- click/hover toggles expand */
} nc_group_hit;

/* Hover ids (docs/13-POPOUTS-SPEC.md sec.3: hover bg on cards, X, Dismiss/
 * action buttons, tabs, Clear). The fixed header elements get small named
 * ids; each card's sub-regions are packed as
 * NC_HOVER_CARD_BASE + hit-index*NC_HOVER_CARD_STRIDE + kind, kind in
 * {0:body,1:close,2:dismiss,3+j:action j} since the card list is dynamic
 * (count/order changes every render). */
#define NC_HOVER_NONE 0
#define NC_HOVER_TAB0 1
#define NC_HOVER_TAB1 2
#define NC_HOVER_CLEAR 3
#define NC_HOVER_SETTINGS 4
/* DND chip row (docs/26-DND-SCHEDULING-PLAN.md T3): Off|15m|1h|8AM|inf, in
 * that left-to-right draw order -- contiguous ids so hover/hittest/click can
 * range-check them as one block (nc->dnd_chip_*[id - NC_HOVER_DND_OFF]). */
#define NC_HOVER_DND_OFF 5
#define NC_HOVER_DND_15M 6
#define NC_HOVER_DND_1H 7
#define NC_HOVER_DND_8AM 8
#define NC_HOVER_DND_INF 9
/* History time-filter chips (History tab only): All|Today|Yesterday|This
 * week, same contiguous-block convention as the DND chips above
 * (nc->hist_chip_*[id - NC_HOVER_HIST_ALL]). */
#define NC_HOVER_HIST_ALL 10
#define NC_HOVER_HIST_TODAY 11
#define NC_HOVER_HIST_YESTERDAY 12
#define NC_HOVER_HIST_WEEK 13
/* Card ids start at 20 (was 10) to leave room 5-19 for the two chip-id
 * blocks above without colliding with the packed per-card range below. */
#define NC_HOVER_CARD_BASE 20
#define NC_HOVER_CARD_STRIDE (3 + DC_NOTIF_ACTION_MAX)
/* One hover id per app-group header row (grouping); placed past the entire
 * card-hit id space so the two ranges never collide. */
#define NC_HOVER_GROUP_BASE (NC_HOVER_CARD_BASE + DC_NC_MAX_CARDS * NC_HOVER_CARD_STRIDE)

struct dc_notif_center {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_notifications *notifications;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width;
    int logical_height;
    int scale120;
    int phys_width;
    int phys_height;

    dc_nc_tab tab;
    float scroll[2];     /* per-tab list scroll offset, logical px */
    float scroll_max[2]; /* recomputed every render; clamps wheel input */

    /* Header/tab hit-test rects, recomputed every render. */
    float clear_x0, clear_y0, clear_x1, clear_y1;
    float settings_x0, settings_y0, settings_x1, settings_y1;
    float tab_x0[2], tab_y0[2], tab_x1[2], tab_y1[2];

    /* DND chip row (docs/26-DND-SCHEDULING-PLAN.md T3): Off|15m|1h|8AM|inf,
     * hit rects recomputed every render like the tabs above -- always drawn
     * regardless of tab. */
    float dnd_chip_x0[5], dnd_chip_y0[5], dnd_chip_x1[5], dnd_chip_y1[5];

    /* History time-filter chips (History tab only): All|Today|Yesterday/
     * This week. Zeroed out (x1<=x0) on Current-tab renders so hittest/click
     * never match a stale rect left over from the last History render --
     * same convention nc_render() already uses for clear_x0/x1 above. */
    float hist_chip_x0[4], hist_chip_y0[4], hist_chip_x1[4], hist_chip_y1[4];

    /* Selected History time filter (docs/26-DND-SCHEDULING-PLAN.md T3);
     * intentionally NOT persisted to config -- resets to "All" (0) every
     * process start, same lifetime as scroll[]/hover_id below. */
    int history_filter;

    nc_card_hit hits[DC_NC_MAX_CARDS];
    int hit_count;

    nc_group_hit group_hits[DC_NC_MAX_GROUPS];
    int group_hit_count;

    /* Persisted expand/collapse state per app-group key, across renders (and
     * across tab switches -- DMS keeps one global expandedGroups map too).
     * Absence of a key means collapsed (the default); entries are only
     * appended on first toggle, same lazy-init idea as bar.c's per-widget
     * hover maps. Bounded to DC_NC_MAX_GROUPS, which can never be exceeded
     * since there can be at most one group per live notification. */
    struct {
        char key[DC_NOTIF_APP];
        bool expanded;
    } group_state[DC_NC_MAX_GROUPS];
    int group_state_count;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    /* Entrance/exit scale-and-fade pivot, bar-position-aware — see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;

    /* Hover tracking (docs/13-POPOUTS-SPEC.md sec.3), same guard pattern as
     * bar.c's dc_bar_pointer_motion() / controlcenter.c's hover_id: only
     * re-render when the hovered id actually changes. */
    int hover_id;

    bool visible;
    bool configured;
    bool egl_ready;
};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void nc_render(dc_notif_center *nc);
static void nc_teardown(dc_notif_center *nc);

static void nc_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener nc_frame_listener = {.done = nc_frame_done};

static void nc_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_notif_center *nc = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    nc->frame_cb = NULL;
    if (!nc->visible)
        return;
    if (dc_anim_active(&nc->anim))
        nc_render(nc);
    else if (nc->closing)
        nc_teardown(nc);
}

static void recompute_physical(dc_notif_center *nc)
{
    nc->phys_width = (nc->logical_width * nc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    nc->phys_height = (nc->logical_height * nc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Truncate `buf` in place to fit `max_w`, appending an ellipsis -- delegates
 * to render/shape.c's dc_shape_ellipsize() (BiDi+HarfBuzz-aware measurement
 * and truncation for Arabic/Urdu/Hebrew, identical plain-nanovg behavior for
 * everything else) the same way bar.c's bar_ellipsize() does. Kept as a
 * thin per-file wrapper (like controlcenter.c's cc_ellipsize()) rather than
 * factored into a shared helper -- no shared string-util module yet. */
static void nc_ellipsize(dc_render *render, char *buf, size_t bufsize, float max_w)
{
    char tmp[256];
    size_t cap = bufsize > sizeof(tmp) ? sizeof(tmp) : bufsize;
    dc_shape_ellipsize(render, buf, max_w, tmp, cap);
    memcpy(buf, tmp, cap);
}

/* Group key for grouping (docs/14-COMPLETION-PLAN.md W1.4): lowercased
 * app_name, so senders that vary the case ("Firefox" vs "firefox") still
 * collapse together. A notification with no app_name at all gets a unique
 * "#<id>" key so it never groups with anything (matches the "no app name ->
 * no group chrome" expectation instead of lumping every anonymous sender
 * into one bucket). */
static void nc_group_key(const dc_notification *n, char *out, size_t outsz)
{
    if (n->app_name[0]) {
        size_t j = 0;
        for (; n->app_name[j] && j + 1 < outsz; j++)
            out[j] = (char)tolower((unsigned char)n->app_name[j]);
        out[j] = '\0';
    } else {
        snprintf(out, outsz, "#%u", n->id);
    }
}

static bool nc_group_is_expanded(dc_notif_center *nc, const char *key)
{
    for (int i = 0; i < nc->group_state_count; i++)
        if (strcmp(nc->group_state[i].key, key) == 0)
            return nc->group_state[i].expanded;
    return false;
}

static void nc_group_toggle_expanded(dc_notif_center *nc, const char *key)
{
    /* Copy to a stack-local buffer first: callers pass `key` as a pointer
     * into another array inside this same `nc` (e.g. nc->group_hits[g].key),
     * so once this call inlines, GCC's -Wrestrict can't rule out `key`
     * aliasing the group_state[] slot this function is about to write below
     * (both are just offsets from the same `nc` base pointer to it). Going
     * through a local that's provably disjoint from `nc` resolves the false
     * positive without weakening the actual bounds-safety. */
    char key_copy[DC_NOTIF_APP];
    snprintf(key_copy, sizeof(key_copy), "%s", key);

    for (int i = 0; i < nc->group_state_count; i++) {
        if (strcmp(nc->group_state[i].key, key_copy) == 0) {
            nc->group_state[i].expanded = !nc->group_state[i].expanded;
            return;
        }
    }
    if (nc->group_state_count < DC_NC_MAX_GROUPS) {
        snprintf(nc->group_state[nc->group_state_count].key,
                sizeof(nc->group_state[nc->group_state_count].key), "%s", key_copy);
        nc->group_state[nc->group_state_count].expanded = true;
        nc->group_state_count++;
    }
}

/* "app-name • time" label per docs/13-POPOUTS-SPEC.md sec.3: same calendar
 * day as now -> "HH:MM" (or "h:MM AP"); otherwise "M/D/YY, HH:MM", matching
 * the reference screenshot's "7/1/26, 21:27". created_wall_ms is a
 * CLOCK_REALTIME stamp (see notifications.h) so it's meaningful as a date. */
static void nc_format_time(int64_t wall_ms, char *out, size_t outsz)
{
    time_t t = (time_t)(wall_ms / 1000);
    time_t now = time(NULL);
    struct tm tm_item, tm_now;
    localtime_r(&t, &tm_item);
    localtime_r(&now, &tm_now);

    bool clock_24h = !dc_config_current || dc_config_current->clock_24h;
    const char *fmt = clock_24h ? "%H:%M" : "%I:%M %p";

    if (tm_item.tm_year == tm_now.tm_year && tm_item.tm_yday == tm_now.tm_yday) {
        strftime(out, outsz, fmt, &tm_item);
        return;
    }

    /* "%-m/%-d/%y, ..." (GNU strftime extension for no leading zeros) to
     * match the reference screenshot's "7/1/26, 21:27" exactly. strftime
     * never overflows `out` (unlike a hand-built %d/%d snprintf, which is
     * what used to trip -Wformat-truncation here since the compiler can't
     * bound struct tm's int fields). */
    char date_fmt[32];
    snprintf(date_fmt, sizeof(date_fmt), "%%-m/%%-d/%%y, %s", fmt);
    strftime(out, outsz, date_fmt, &tm_item);
}

/* One notification card (docs/13-POPOUTS-SPEC.md sec.3): rounded card, app
 * avatar, "app • time" / title / 2-line body, an X (top-right) and Dismiss
 * (bottom-right) that both resolve the card one step (see
 * dc_notifications_dismiss()), plus an optional first-action button next to
 * Dismiss. Appends the drawn hit-test rects to nc->hits[]. */
static void draw_card(dc_notif_center *nc, const dc_notification *n, float x, float y, float w)
{
    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;

    if (nc->hit_count >= DC_NC_MAX_CARDS)
        return;
    nc_card_hit *hit = &nc->hits[nc->hit_count++];
    memset(hit, 0, sizeof(*hit));
    hit->id = n->id;
    hit->status = n->status;
    hit->card_x0 = x;
    hit->card_y0 = y;
    hit->card_x1 = x + w;
    hit->card_y1 = y + DC_NC_CARD_H;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, DC_NC_CARD_H, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    /* Avatar: the notification's image (image-data hint / image-path /
     * app_icon file) cover-fit into the circle, else an initial-letter
     * circle -- centered on the header+title+body text block, leaving the
     * bottom button row clear (matching the reference). See notif_image.h;
     * the cache is shared with toasts.c so this doesn't re-decode/upload a
     * texture that's already on-screen in the toast stack. */
    const float av_r = 18.0f;
    const float av_cx = x + 16.0f + av_r;
    const float av_cy = y + 14.0f + 34.0f;
    int img_w = 0, img_h = 0;
    int img = dc_notif_image_get(nc->render, n, &img_w, &img_h);
    nvgBeginPath(vg);
    nvgCircle(vg, av_cx, av_cy, av_r);
    if (img > 0 && img_w > 0 && img_h > 0) {
        float scale = fmaxf((av_r * 2.0f) / (float)img_w, (av_r * 2.0f) / (float)img_h);
        float iw = (float)img_w * scale, ih = (float)img_h * scale;
        NVGpaint pat =
            nvgImagePattern(vg, av_cx - iw / 2.0f, av_cy - ih / 2.0f, iw, ih, 0.0f, img, 1.0f);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
    } else {
        nvgFillColor(vg, tc_alpha(t->primary, n->urgency == DC_URGENCY_CRITICAL ? 255 : 150));
        nvgFill(vg);
        char initial[2] = {n->app_name[0] ? (char)toupper((unsigned char)n->app_name[0]) : '?', 0};
        nvgFontFaceId(vg, nc->render->font_ui);
        nvgFontSize(vg, 16.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_container));
        nvgText(vg, av_cx, av_cy + 1.0f, initial, NULL);
    }

    /* X (close/history) button, top-right. */
    const float close_r = 12.0f;
    const float close_cx = x + w - 14.0f - close_r;
    const float close_cy = y + 14.0f + close_r;
    dc_render_icon(nc->render, DC_ICON_CLOSE, close_cx, close_cy, 15.0f, t->surface_variant_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    hit->close_x0 = close_cx - close_r - 4.0f;
    hit->close_y0 = close_cy - close_r - 4.0f;
    hit->close_x1 = close_cx + close_r + 4.0f;
    hit->close_y1 = close_cy + close_r + 4.0f;

    const float tx = av_cx + av_r + 12.0f;
    const float tw = (x + w - 14.0f) - (close_r * 2.0f + 8.0f) - tx;

    /* Sized generously above DC_NOTIF_APP + the separator + the time string's
     * worst case so snprintf can never be flagged as possibly truncating
     * (the actual on-screen text is clipped to `tw` by nc_ellipsize() below
     * regardless). */
    char header_buf[DC_NOTIF_APP + 48];
    char time_buf[40];
    nc_format_time(n->created_wall_ms, time_buf, sizeof(time_buf));
    if (n->app_name[0])
        snprintf(header_buf, sizeof(header_buf), "%s  \xe2\x80\xa2  %s", n->app_name, time_buf);
    else
        snprintf(header_buf, sizeof(header_buf), "%s", time_buf);
    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 11.0f);
    nc_ellipsize(nc->render, header_buf, sizeof(header_buf), tw);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc_alpha(t->surface_text, 150));
    dc_shape_draw_text(nc->render, tx, y + 14.0f, header_buf, NULL);

    char title_buf[DC_NOTIF_SUMMARY];
    snprintf(title_buf, sizeof(title_buf), "%s", n->summary);
    nvgFontSize(vg, 14.0f);
    nc_ellipsize(nc->render, title_buf, sizeof(title_buf), tw);
    nvgFillColor(vg, tc(t->surface_text));
    dc_shape_draw_text(nc->render, tx, y + 32.0f, title_buf, NULL);

    if (n->body[0]) {
        nvgSave(vg);
        nvgScissor(vg, tx, y + 52.0f, tw, 30.0f);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc_alpha(t->surface_text, 160));
        nvgTextLineHeight(vg, 1.15f);
        /* This 30px scissor only budgets 2 rows -- but dc_shape_draw_textbox()
         * word-wraps by width alone and will happily lay out a 3rd (or more)
         * row, whose clipped remnant peeks out just above the button row
         * directly below (reproduced with a real 3-line Slack message: a
         * sliver of wrapped text bled in above "Dismiss/Reply/Mark as
         * read"). Clamp to 2 rows and ellipsize the 2nd instead of letting a
         * 3rd row exist at all. */
        NVGtextRow rows[3];
        int nrows = nvgTextBreakLines(vg, n->body, NULL, tw, rows, 3);
        if (nrows <= 2) {
            dc_shape_draw_textbox(nc->render, tx, y + 53.0f, tw, n->body, NULL);
        } else {
            dc_shape_draw_text(nc->render, tx, y + 53.0f, rows[0].start, rows[0].end);
            char rest_buf[DC_NOTIF_BODY], line2_buf[DC_NOTIF_BODY];
            snprintf(rest_buf, sizeof(rest_buf), "%s", rows[1].start);
            dc_shape_ellipsize(nc->render, rest_buf, tw, line2_buf, sizeof(line2_buf));
            dc_shape_draw_text(nc->render, tx, y + 53.0f + 12.0f * 1.15f, line2_buf, NULL);
        }
        nvgRestore(vg);
    }

    /* Bottom-right button row: [actions...] Dismiss, right-aligned. Actions
     * are drawn right-to-left (closest to Dismiss first) so array index
     * order still reads left-to-right, matching the sender's actions[]
     * order. Real actions are laid out *before* Dismiss (not after): Dismiss
     * only duplicates the X close button already in the card's top-right, so
     * if the row is too narrow to fit every real action button plus Dismiss
     * (seen with a real 2-action NetworkManager Applet notification whose
     * "Reconnect" + "Don't show this message again" + "Dismiss" don't all
     * fit at DC_NC_WIDTH), Dismiss is the one that silently gives way --
     * losing it costs nothing, losing an actual action button means the user
     * can no longer invoke it at all. DMS additionally hides Dismiss outright
     * once actionCount>=3 (NotificationCard.qml clearButton.visible:
     * actionCount<3) regardless of space; mirrored here too. */
    const float row_y1 = y + DC_NC_CARD_H - 12.0f;
    const float row_h = 22.0f;
    const float row_y0 = row_y1 - row_h;
    const float row_min_x = tx; /* never crowd past the text column's left edge */
    float cursor_x1 = x + w - 14.0f;

    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    float b[4];
    hit->action_count = n->action_count;
    hit->action_y0 = row_y0;
    hit->action_y1 = row_y1;
    for (int i = n->action_count - 1; i >= 0; i--) {
        const char *label = n->actions[i].label[0] ? n->actions[i].label : "Open";
        nvgTextBounds(vg, 0, 0, label, NULL, b);
        float aw = b[2] - b[0] + 18.0f;
        if (aw < 48.0f)
            aw = 48.0f;
        float a_x0 = cursor_x1 - aw;
        if (a_x0 < row_min_x || cursor_x1 <= row_min_x) {
            hit->action_x0[i] = hit->action_x1[i] = 0.0f; /* no room -- not drawn, not clickable */
            continue;
        }
        nvgFillColor(vg, tc(t->primary));
        nvgText(vg, (a_x0 + cursor_x1) / 2.0f, (row_y0 + row_y1) / 2.0f, label, NULL);
        hit->action_x0[i] = a_x0;
        hit->action_x1[i] = cursor_x1;
        cursor_x1 = a_x0 - 8.0f;
    }

    hit->has_dismiss = false;
    if (n->action_count < 3) {
        nvgTextBounds(vg, 0, 0, "Dismiss", NULL, b);
        float dismiss_w = b[2] - b[0] + 18.0f;
        float dismiss_x0 = cursor_x1 - dismiss_w;
        if (dismiss_x0 >= row_min_x && cursor_x1 > row_min_x) {
            nvgFillColor(vg, tc_alpha(t->surface_text, 190));
            nvgText(vg, (dismiss_x0 + cursor_x1) / 2.0f, (row_y0 + row_y1) / 2.0f, "Dismiss", NULL);
            hit->has_dismiss = true;
            hit->dismiss_x0 = dismiss_x0;
            hit->dismiss_y0 = row_y0;
            hit->dismiss_x1 = cursor_x1;
            hit->dismiss_y1 = row_y1;
        }
    }
}

/* One app-group header row (grouping, docs/14-COMPLETION-PLAN.md W1.4):
 * avatar (newest member's image/initial) + count badge, app name + "N
 * notifications" subtitle, and an expand/collapse chevron. The whole row is
 * one click target (see dc_notif_center_handle_click()) -- clicking
 * anywhere toggles `expanded`. Appends the drawn hit rect to
 * nc->group_hits[]. Only ever called for groups with more than one member;
 * a lone notification draws as a plain draw_card() instead. */
static void draw_group_header(dc_notif_center *nc, const char *key, const char *app_name,
                              const dc_notification *latest, int count, bool expanded, float x,
                              float y, float w)
{
    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;

    if (nc->group_hit_count >= DC_NC_MAX_GROUPS)
        return;
    nc_group_hit *hit = &nc->group_hits[nc->group_hit_count++];
    snprintf(hit->key, sizeof(hit->key), "%s", key);
    hit->x0 = x;
    hit->y0 = y;
    hit->x1 = x + w;
    hit->y1 = y + DC_NC_GROUP_HEADER_H;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, DC_NC_GROUP_HEADER_H, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    const float av_r = 16.0f;
    const float av_cx = x + 16.0f + av_r;
    const float av_cy = y + DC_NC_GROUP_HEADER_H / 2.0f;
    int img_w = 0, img_h = 0;
    int img = dc_notif_image_get(nc->render, latest, &img_w, &img_h);
    nvgBeginPath(vg);
    nvgCircle(vg, av_cx, av_cy, av_r);
    if (img > 0 && img_w > 0 && img_h > 0) {
        float scale = fmaxf((av_r * 2.0f) / (float)img_w, (av_r * 2.0f) / (float)img_h);
        float iw = (float)img_w * scale, ih = (float)img_h * scale;
        NVGpaint pat =
            nvgImagePattern(vg, av_cx - iw / 2.0f, av_cy - ih / 2.0f, iw, ih, 0.0f, img, 1.0f);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
    } else {
        nvgFillColor(vg, tc_alpha(t->primary, 150));
        nvgFill(vg);
        char initial[2] = {app_name[0] ? (char)toupper((unsigned char)app_name[0]) : '?', 0};
        nvgFontFaceId(vg, nc->render->font_ui);
        nvgFontSize(vg, 14.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_container));
        nvgText(vg, av_cx, av_cy + 1.0f, initial, NULL);
    }

    /* Count badge, top-right of the avatar -- DMS's NotificationCard.qml
     * puts the same badge on the collapsed card's avatar. */
    const float badge_r = 9.0f;
    const float badge_cx = av_cx + av_r * 0.75f;
    const float badge_cy = av_cy - av_r * 0.75f;
    nvgBeginPath(vg);
    nvgCircle(vg, badge_cx, badge_cy, badge_r);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
    char count_buf[16]; /* sized generously above "99" -- the actual on-screen
                          * value is capped by the ternary below, but the
                          * compiler's -Wformat-truncation can't see that from
                          * a plain %d on an int, so the buffer itself must be
                          * wide enough for any int's worst case. */
    snprintf(count_buf, sizeof(count_buf), "%d", count > 99 ? 99 : count);
    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 10.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->primary_text));
    nvgText(vg, badge_cx, badge_cy + 0.5f, count_buf, NULL);

    /* Expand/collapse chevron, right edge. */
    const float chev_r = 12.0f;
    const float chev_cx = x + w - 14.0f - chev_r;
    const float chev_cy = av_cy;
    dc_render_icon(nc->render, expanded ? DC_ICON_EXPAND_LESS : DC_ICON_EXPAND_MORE, chev_cx,
                  chev_cy, 18.0f, t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* App name + "N notifications" subtitle, between avatar and chevron. */
    const float tx = av_cx + av_r + 12.0f;
    const float tw = (chev_cx - chev_r - 8.0f) - tx;

    char name_buf[DC_NOTIF_APP];
    snprintf(name_buf, sizeof(name_buf), "%s", app_name[0] ? app_name : "Unknown");
    nvgFontFaceId(vg, nc->render->font_ui); /* the chevron icon draw above left the icon face active */
    nvgFontSize(vg, 14.0f);
    nc_ellipsize(nc->render, name_buf, sizeof(name_buf), tw);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc(t->surface_text));
    dc_shape_draw_text(nc->render, tx, y + 10.0f, name_buf, NULL);

    char sub_buf[32];
    snprintf(sub_buf, sizeof(sub_buf), "%d notifications", count);
    nvgFontSize(vg, 11.0f);
    nvgFillColor(vg, tc_alpha(t->surface_text, 150));
    dc_shape_draw_text(nc->render, tx, y + 28.0f, sub_buf, NULL);
}

/* Draw a "Current (N)"/"History (M)" pill tab and record its hit rect.
 * Active = solid primary pill with dark (primary_text) text; inactive = dim
 * surface_container_high pill (docs/13-POPOUTS-SPEC.md sec.3). */
static void draw_tab(dc_notif_center *nc, int index, float x, float y, float h,
                     const char *label, bool active, float *out_x1)
{
    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;

    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 13.0f);
    float b[4];
    nvgTextBounds(vg, 0, 0, label, NULL, b);
    float w = (b[2] - b[0]) + 28.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h / 2.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, active ? tc(t->primary_text) : tc_alpha(t->surface_text, 170));
    nvgText(vg, x + w / 2.0f, y + h / 2.0f, label, NULL);

    nc->tab_x0[index] = x;
    nc->tab_y0[index] = y;
    nc->tab_x1[index] = x + w;
    nc->tab_y1[index] = y + h;
    *out_x1 = x + w;
}

/* Draw one pill-style chip (DND presets, History time-filter) at `x` and
 * report its hit rect via the out params -- same active/inactive styling as
 * draw_tab() just sized to its own label rather than a fixed slot index, and
 * returning the next x cursor (x + w, no gap) so callers can chain calls
 * left-to-right adding their own DC_NC_CHIP_GAP between. */
static float draw_nc_chip(dc_notif_center *nc, float x, float y, float h, const char *label,
                          bool active, float *out_x0, float *out_y0, float *out_x1, float *out_y1)
{
    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;

    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 11.5f);
    float b[4];
    nvgTextBounds(vg, 0, 0, label, NULL, b);
    float w = (b[2] - b[0]) + 20.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, h / 2.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, active ? tc(t->primary_text) : tc_alpha(t->surface_text, 170));
    nvgText(vg, x + w / 2.0f, y + h / 2.0f, label, NULL);

    *out_x0 = x;
    *out_y0 = y;
    *out_x1 = x + w;
    *out_y1 = y + h;
    return x + w;
}

/* "8AM" chip label (docs/26-DND-SCHEDULING-PLAN.md UI): dnd_until_hour
 * formatted per clock_24h, e.g. hour=8 -> "08:00" (24h) or "8 AM" (12h). */
static void nc_format_hour_label(int hour, bool clock_24h, char *out, size_t outsz)
{
    if (hour < 0 || hour > 23)
        hour = 8;
    if (clock_24h) {
        snprintf(out, outsz, "%02d:00", hour);
    } else {
        int h12 = hour % 12;
        if (h12 == 0)
            h12 = 12;
        snprintf(out, outsz, "%d %s", h12, hour < 12 ? "AM" : "PM");
    }
}

/* Hover bg (docs/13-POPOUTS-SPEC.md sec.3; formula from bar.c's
 * draw_hover_overlay(), shared via hover.h): painted last, on top of
 * whatever's already drawn at that hit rect. Stadium shape (radius = half
 * the rect's own height) for the pill-like tab/Clear/settings/dismiss/action
 * hits, a circle for the small square close-button hit, the card's own 12px
 * corner radius for its body. */
static void draw_nc_hover(dc_notif_center *nc)
{
    if (nc->hover_id == NC_HOVER_NONE)
        return;

    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, radius = 6.0f;

    if (nc->hover_id == NC_HOVER_TAB0) {
        x0 = nc->tab_x0[0];
        y0 = nc->tab_y0[0];
        x1 = nc->tab_x1[0];
        y1 = nc->tab_y1[0];
        radius = (y1 - y0) / 2.0f;
    } else if (nc->hover_id == NC_HOVER_TAB1) {
        x0 = nc->tab_x0[1];
        y0 = nc->tab_y0[1];
        x1 = nc->tab_x1[1];
        y1 = nc->tab_y1[1];
        radius = (y1 - y0) / 2.0f;
    } else if (nc->hover_id == NC_HOVER_CLEAR) {
        x0 = nc->clear_x0;
        y0 = nc->clear_y0;
        x1 = nc->clear_x1;
        y1 = nc->clear_y1;
        radius = (y1 - y0) / 2.0f;
    } else if (nc->hover_id == NC_HOVER_SETTINGS) {
        x0 = nc->settings_x0;
        y0 = nc->settings_y0;
        x1 = nc->settings_x1;
        y1 = nc->settings_y1;
        radius = (y1 - y0) / 2.0f;
    } else if (nc->hover_id >= NC_HOVER_DND_OFF && nc->hover_id <= NC_HOVER_DND_INF) {
        int i = nc->hover_id - NC_HOVER_DND_OFF;
        x0 = nc->dnd_chip_x0[i];
        y0 = nc->dnd_chip_y0[i];
        x1 = nc->dnd_chip_x1[i];
        y1 = nc->dnd_chip_y1[i];
        radius = (y1 - y0) / 2.0f;
    } else if (nc->hover_id >= NC_HOVER_HIST_ALL && nc->hover_id <= NC_HOVER_HIST_WEEK) {
        int i = nc->hover_id - NC_HOVER_HIST_ALL;
        x0 = nc->hist_chip_x0[i];
        y0 = nc->hist_chip_y0[i];
        x1 = nc->hist_chip_x1[i];
        y1 = nc->hist_chip_y1[i];
        radius = (y1 - y0) / 2.0f;
    } else if (nc->hover_id >= NC_HOVER_GROUP_BASE) {
        int gi = nc->hover_id - NC_HOVER_GROUP_BASE;
        if (gi < 0 || gi >= nc->group_hit_count)
            return;
        const nc_group_hit *gh = &nc->group_hits[gi];
        x0 = gh->x0;
        y0 = gh->y0;
        x1 = gh->x1;
        y1 = gh->y1;
        radius = 12.0f;
    } else if (nc->hover_id >= NC_HOVER_CARD_BASE) {
        int rel = nc->hover_id - NC_HOVER_CARD_BASE;
        int i = rel / NC_HOVER_CARD_STRIDE, kind = rel % NC_HOVER_CARD_STRIDE;
        if (i < 0 || i >= nc->hit_count)
            return;
        const nc_card_hit *hit = &nc->hits[i];
        if (kind == 0) {
            x0 = hit->card_x0;
            y0 = hit->card_y0;
            x1 = hit->card_x1;
            y1 = hit->card_y1;
            radius = 12.0f;
        } else if (kind == 1) {
            x0 = hit->close_x0;
            y0 = hit->close_y0;
            x1 = hit->close_x1;
            y1 = hit->close_y1;
            radius = (x1 - x0) / 2.0f;
        } else if (kind == 2) {
            if (!hit->has_dismiss)
                return;
            x0 = hit->dismiss_x0;
            y0 = hit->dismiss_y0;
            x1 = hit->dismiss_x1;
            y1 = hit->dismiss_y1;
            radius = (y1 - y0) / 2.0f;
        } else if (kind - 3 < hit->action_count) {
            int j = kind - 3;
            x0 = hit->action_x0[j];
            y0 = hit->action_y0;
            x1 = hit->action_x1[j];
            y1 = hit->action_y1;
            radius = (y1 - y0) / 2.0f;
        } else {
            return;
        }
    } else {
        return;
    }
    if (x1 <= x0 || y1 <= y0)
        return;

    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    dc_color hc =
        dc_hover_bg_color(t->surface_container_high, t->primary, cfg->bar_widget_transparency);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x0, y0, x1 - x0, y1 - y0, radius);
    nvgFillColor(vg, nvgRGBA(hc.r, hc.g, hc.b, hc.a));
    nvgFill(vg);
}

static void nc_render(dc_notif_center *nc)
{
    if (!nc->configured || nc->phys_width <= 0)
        return;
    if (!nc->egl_ready) {
        if (!dc_egl_window_init(&nc->egl_window, nc->egl, nc->surface, nc->phys_width,
                                nc->phys_height))
            return;
        nc->egl_ready = true;
    } else {
        dc_egl_window_resize(&nc->egl_window, nc->phys_width, nc->phys_height);
    }
    if (!dc_egl_make_current(nc->egl, &nc->egl_window))
        return;
    if (!dc_render_ensure(nc->render))
        return;
    if (nc->viewport)
        wp_viewport_set_destination(nc->viewport, nc->logical_width, nc->logical_height);

    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = nc->logical_width;
    const float h = nc->logical_height;
    const float pad = DC_NC_PAD;
    const float ix = pad + DC_NC_INSET;
    const float iw = w - 2.0f * ix;

    glViewport(0, 0, nc->phys_width, nc->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)nc->scale120 / DC_SCALE_BASE);

    float p = dc_anim_progress(&nc->anim);
    if (nc->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad + (w - 2.0f * pad) * nc->anim_ox;
    float oy = pad + (h - 2.0f * pad) * nc->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Shadow + card. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, DC_NC_RADIUS,
                                     18.0f, nvgRGBA(0, 0, 0, 100), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, DC_NC_RADIUS);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card: blurred+dimmed wallpaper ("material" bg) when enabled, else the
     * flat surfaceContainer fill (docs/POLISH.md P2, ui/material_bg.c). */
    dc_material_bg_fill_card(vg, nc->render, pad, pad, w - 2 * pad, h - 2 * pad, DC_NC_RADIUS);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* --- Header: "Notifications" + bell (left); gear + Clear (right) --- */
    const float header_y = pad + DC_NC_HEADER_TOP;
    const float header_cy = header_y + DC_NC_HEADER_H / 2.0f;

    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 17.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, ix, header_cy, "Notifications", NULL);
    float title_b[4];
    nvgTextBounds(vg, ix, header_cy, "Notifications", NULL, title_b);
    dc_render_icon(nc->render, DC_ICON_NOTIFICATIONS, title_b[2] + 16.0f, header_cy, 17.0f,
                  t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* Clear (rightmost) — clears whichever tab is active; only shown when
     * that tab actually has entries (mirrors NotificationHeader.qml). */
    int current_n = dc_notifications_current_count(nc->notifications);
    int history_n = dc_notifications_history_count(nc->notifications);
    bool active_tab_has_items = (nc->tab == DC_NC_TAB_CURRENT) ? current_n > 0 : history_n > 0;

    if (active_tab_has_items) {
        const char *label = "Clear";
        nvgFontFaceId(vg, nc->render->font_ui); /* the bell icon above left the icon face active */
        nvgFontSize(vg, 13.0f);
        float b[4];
        nvgTextBounds(vg, 0, 0, label, NULL, b);
        float label_w = b[2] - b[0];
        float bw = label_w + 20.0f + 22.0f; /* icon + gap + label + padding */
        float bx1 = ix + iw;
        float bx0 = bx1 - bw;
        float by0 = header_y, by1 = header_y + DC_NC_HEADER_H;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx0, by0, bx1 - bx0, by1 - by0, (by1 - by0) / 2.0f);
        nvgFillColor(vg, tc(t->surface_container_high));
        nvgFill(vg);
        dc_render_icon(nc->render, DC_ICON_DELETE_SWEEP, bx0 + 18.0f, (by0 + by1) / 2.0f, 15.0f,
                      t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        /* dc_render_icon() switches the active font face to the icon font
         * (render/nvg.c) and doesn't restore it -- every text draw after an
         * icon draw must re-select font_ui, or nvgText() silently falls back
         * through fontstash to whatever face happens to have those glyphs. */
        nvgFontFaceId(vg, nc->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgFillColor(vg, tc(t->surface_text));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, bx0 + 32.0f, (by0 + by1) / 2.0f, label, NULL);
        nc->clear_x0 = bx0;
        nc->clear_x1 = bx1;
        nc->clear_y0 = by0;
        nc->clear_y1 = by1;
    } else {
        nc->clear_x0 = nc->clear_x1 = 0.0f;
    }

    /* Settings gear — left of Clear. No dankc settings hook wired from this
     * popout yet (out of this task's touch-scope, main.c changes are limited
     * to axis routing), so it's a visible no-op for now. */
    {
        float gx1 = (nc->clear_x1 > nc->clear_x0) ? nc->clear_x0 - 10.0f : ix + iw;
        float gx0 = gx1 - DC_NC_HEADER_H;
        dc_render_icon(nc->render, DC_ICON_SETTINGS, (gx0 + gx1) / 2.0f, header_cy, 17.0f,
                      t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nc->settings_x0 = gx0;
        nc->settings_x1 = gx1;
        nc->settings_y0 = header_y;
        nc->settings_y1 = header_y + DC_NC_HEADER_H;
    }

    /* --- Tabs: "Current (N)" / "History (M)" -------------------------- */
    const float tabs_y = header_y + DC_NC_HEADER_H + DC_NC_TABS_GAP;
    char current_label[32], history_label[32];
    snprintf(current_label, sizeof(current_label), "Current (%d)", current_n);
    snprintf(history_label, sizeof(history_label), "History (%d)", history_n);
    float next_x = ix;
    float tab_x1;
    draw_tab(nc, 0, next_x, tabs_y, DC_NC_TABS_H, current_label, nc->tab == DC_NC_TAB_CURRENT,
            &tab_x1);
    next_x = tab_x1 + 8.0f;
    draw_tab(nc, 1, next_x, tabs_y, DC_NC_TABS_H, history_label, nc->tab == DC_NC_TAB_HISTORY,
            &tab_x1);

    /* --- DND chip row: Off|15m|1h|8AM|inf (docs/26-DND-SCHEDULING-PLAN.md
     * UI/T3) -- always shown, between the tabs row and the card list. When a
     * timed session is running, a right-aligned "resumes HH:MM (Nm)" label
     * shares the row. "Off"/"inf" double as toggle state (highlighted when
     * DND is off / on-indefinitely); 15m/1h/8AM are one-shot presets, never
     * highlighted themselves. */
    const float dnd_row_y = tabs_y + DC_NC_TABS_H + DC_NC_LIST_GAP;
    {
        int64_t remaining = dc_notif_dnd_remaining_sec();
        bool clock24 = !dc_config_current || dc_config_current->clock_24h;
        int until_hour = dc_config_current ? dc_config_current->dnd_until_hour : 8;
        char hour_label[16];
        nc_format_hour_label(until_hour, clock24, hour_label, sizeof(hour_label));

        float cx = ix;
        cx = draw_nc_chip(nc, cx, dnd_row_y, DC_NC_CHIP_ROW_H, "Off", remaining == -1,
                          &nc->dnd_chip_x0[0], &nc->dnd_chip_y0[0], &nc->dnd_chip_x1[0],
                          &nc->dnd_chip_y1[0]) +
             DC_NC_CHIP_GAP;
        cx = draw_nc_chip(nc, cx, dnd_row_y, DC_NC_CHIP_ROW_H, "15m", false, &nc->dnd_chip_x0[1],
                          &nc->dnd_chip_y0[1], &nc->dnd_chip_x1[1], &nc->dnd_chip_y1[1]) +
             DC_NC_CHIP_GAP;
        cx = draw_nc_chip(nc, cx, dnd_row_y, DC_NC_CHIP_ROW_H, "1h", false, &nc->dnd_chip_x0[2],
                          &nc->dnd_chip_y0[2], &nc->dnd_chip_x1[2], &nc->dnd_chip_y1[2]) +
             DC_NC_CHIP_GAP;
        cx = draw_nc_chip(nc, cx, dnd_row_y, DC_NC_CHIP_ROW_H, hour_label, false,
                          &nc->dnd_chip_x0[3], &nc->dnd_chip_y0[3], &nc->dnd_chip_x1[3],
                          &nc->dnd_chip_y1[3]) +
             DC_NC_CHIP_GAP;
        /* "Indef" rather than the "inf" (infinity) symbol from the plan text
         * -- verified live (docs/26-DND-SCHEDULING-PLAN.md T3 risk-review):
         * plain nvgText() (same path draw_tab()'s labels already use, no
         * HarfBuzz fallback chain) renders U+221E as a tofu box in the
         * bundled InterVariable face, so an ASCII label is used instead. */
        draw_nc_chip(nc, cx, dnd_row_y, DC_NC_CHIP_ROW_H, "Indef", remaining == 0,
                    &nc->dnd_chip_x0[4], &nc->dnd_chip_y0[4], &nc->dnd_chip_x1[4],
                    &nc->dnd_chip_y1[4]);

        if (remaining > 0) {
            time_t resume_t = time(NULL) + (time_t)remaining;
            struct tm tm_r;
            localtime_r(&resume_t, &tm_r);
            char time_buf[16];
            strftime(time_buf, sizeof(time_buf), clock24 ? "%H:%M" : "%I:%M %p", &tm_r);
            int mins = (int)((remaining + 30) / 60);
            char countdown_buf[48];
            snprintf(countdown_buf, sizeof(countdown_buf), "resumes %s (%dm)", time_buf, mins);

            nvgFontFaceId(vg, nc->render->font_ui);
            nvgFontSize(vg, 11.0f);
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, tc_alpha(t->surface_text, 160));
            nvgText(vg, ix + iw, dnd_row_y + DC_NC_CHIP_ROW_H / 2.0f, countdown_buf, NULL);
        }
    }
    const float after_dnd_y = dnd_row_y + DC_NC_CHIP_ROW_H;

    /* --- History time-filter chip row: All|Today|Yesterday|This week
     * (docs/26-DND-SCHEDULING-PLAN.md UI/T3) -- History tab only, a second
     * chip row below the DND row. Only shifts list_y0 further down while
     * that tab is active; zeroed out otherwise so a stale rect from the last
     * History render can never hit-test true on the Current tab. */
    float list_y0;
    if (nc->tab == DC_NC_TAB_HISTORY) {
        const float hist_row_y = after_dnd_y + DC_NC_LIST_GAP;
        static const char *const hist_labels[4] = {"All", "Today", "Yesterday", "This week"};
        float hx = ix;
        for (int i = 0; i < 4; i++) {
            hx = draw_nc_chip(nc, hx, hist_row_y, DC_NC_CHIP_ROW_H, hist_labels[i],
                              nc->history_filter == i, &nc->hist_chip_x0[i], &nc->hist_chip_y0[i],
                              &nc->hist_chip_x1[i], &nc->hist_chip_y1[i]) +
                 DC_NC_CHIP_GAP;
        }
        list_y0 = hist_row_y + DC_NC_CHIP_ROW_H + DC_NC_LIST_GAP;
    } else {
        for (int i = 0; i < 4; i++)
            nc->hist_chip_x0[i] = nc->hist_chip_x1[i] = 0.0f;
        list_y0 = after_dnd_y + DC_NC_LIST_GAP;
    }

    /* --- Card list, scrollable, grouped by app ---------------------------
     * Grouping (docs/14-COMPLETION-PLAN.md W1.4): entries sharing the same
     * app_name (case-insensitive) collapse into one header row with a count
     * badge; a lone notification (no siblings) still renders as a plain
     * card, no header. Groups are built in first-seen order over `entries`
     * (already newest-first), so a group's position tracks its most recent
     * member with no separate sort needed. */
    const float list_y1 = h - pad - DC_NC_BOTTOM_PAD;
    const float list_h = list_y1 - list_y0;

    const dc_notification *entries[DC_NC_MAX_CARDS];
    int count = (nc->tab == DC_NC_TAB_CURRENT)
                   ? dc_notifications_current(nc->notifications, entries, DC_NC_MAX_CARDS)
                   : dc_notifications_history(nc->notifications, entries, DC_NC_MAX_CARDS);

    /* History time-filter (docs/26-DND-SCHEDULING-PLAN.md UI/T3): compare
     * each entry's created_wall_ms against local-midnight boundaries computed
     * once here per render via localtime_r/mktime (not per-entry) -- cheap
     * enough at 1 render/interaction and avoids DST edge cases from a flat
     * +/-N*86400s offset in seconds. */
    if (nc->tab == DC_NC_TAB_HISTORY && nc->history_filter != NC_HIST_ALL) {
        time_t now_t = time(NULL);
        struct tm tm_mid;
        localtime_r(&now_t, &tm_mid);
        tm_mid.tm_hour = 0;
        tm_mid.tm_min = 0;
        tm_mid.tm_sec = 0;
        time_t today0 = mktime(&tm_mid);
        struct tm tm_y = tm_mid;
        tm_y.tm_mday -= 1;
        time_t yest0 = mktime(&tm_y);
        struct tm tm_w = tm_mid;
        tm_w.tm_mday -= 6;
        time_t week0 = mktime(&tm_w);

        int64_t today0_ms = (int64_t)today0 * 1000;
        int64_t yest0_ms = (int64_t)yest0 * 1000;
        int64_t week0_ms = (int64_t)week0 * 1000;

        int kept = 0;
        for (int i = 0; i < count; i++) {
            int64_t wm = entries[i]->created_wall_ms;
            bool keep;
            switch (nc->history_filter) {
            case NC_HIST_TODAY:
                keep = wm >= today0_ms;
                break;
            case NC_HIST_YESTERDAY:
                keep = wm >= yest0_ms && wm < today0_ms;
                break;
            case NC_HIST_WEEK:
                keep = wm >= week0_ms;
                break;
            default:
                keep = true;
                break;
            }
            if (keep)
                entries[kept++] = entries[i];
        }
        count = kept;
    }

    typedef struct {
        char key[DC_NOTIF_APP];
        const dc_notification *members[DC_NC_MAX_CARDS];
        int member_count;
    } nc_group;
    nc_group groups[DC_NC_MAX_GROUPS];
    int group_count = 0;
    for (int i = 0; i < count; i++) {
        char key[DC_NOTIF_APP];
        nc_group_key(entries[i], key, sizeof(key));
        int found = -1;
        for (int g = 0; g < group_count; g++) {
            if (strcmp(groups[g].key, key) == 0) {
                found = g;
                break;
            }
        }
        if (found < 0 && group_count < DC_NC_MAX_GROUPS) {
            found = group_count++;
            snprintf(groups[found].key, sizeof(groups[found].key), "%s", key);
            groups[found].member_count = 0;
        }
        if (found >= 0 && groups[found].member_count < DC_NC_MAX_CARDS)
            groups[found].members[groups[found].member_count++] = entries[i];
    }

    /* One row per header (group with >1 member) or standalone card (group
     * with exactly 1 member); expanded groups also get one row per member. */
    typedef struct {
        int group_index;
        int member_index; /* -1 for a header row */
        float height;
    } nc_row;
    nc_row rows[DC_NC_MAX_ROWS];
    int row_count = 0;
    for (int gi = 0; gi < group_count && row_count < DC_NC_MAX_ROWS; gi++) {
        if (groups[gi].member_count > 1) {
            rows[row_count].group_index = gi;
            rows[row_count].member_index = -1;
            rows[row_count].height = DC_NC_GROUP_HEADER_H;
            row_count++;
            if (nc_group_is_expanded(nc, groups[gi].key)) {
                for (int m = 0; m < groups[gi].member_count && row_count < DC_NC_MAX_ROWS; m++) {
                    rows[row_count].group_index = gi;
                    rows[row_count].member_index = m;
                    rows[row_count].height = DC_NC_CARD_H;
                    row_count++;
                }
            }
        } else if (groups[gi].member_count == 1) {
            rows[row_count].group_index = gi;
            rows[row_count].member_index = 0;
            rows[row_count].height = DC_NC_CARD_H;
            row_count++;
        }
    }

    float content_h = 0.0f;
    for (int r = 0; r < row_count; r++)
        content_h += rows[r].height + (r > 0 ? DC_NC_CARD_GAP : 0.0f);
    float scroll_max = content_h > list_h ? content_h - list_h : 0.0f;
    if (nc->scroll[nc->tab] < 0.0f)
        nc->scroll[nc->tab] = 0.0f;
    if (nc->scroll[nc->tab] > scroll_max)
        nc->scroll[nc->tab] = scroll_max;
    nc->scroll_max[nc->tab] = scroll_max;
    float scroll = nc->scroll[nc->tab];

    nc->hit_count = 0;
    nc->group_hit_count = 0;

    if (count == 0) {
        dc_color dim = t->surface_text;
        dim.a = 90;
        dc_render_icon(nc->render, DC_ICON_NOTIFICATIONS, w / 2.0f, list_y0 + list_h / 2.0f - 16.0f,
                      36.0f, dim, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, nc->render->font_ui); /* icon draw above left the icon face active */
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, list_y0 + list_h / 2.0f + 14.0f,
               nc->tab == DC_NC_TAB_CURRENT ? "No notifications" : "No history", NULL);
    } else {
        nvgSave(vg);
        nvgScissor(vg, ix, list_y0, iw, list_h);
        float running_y = list_y0 - scroll;
        for (int r = 0; r < row_count; r++) {
            if (r > 0)
                running_y += DC_NC_CARD_GAP;
            float row_y = running_y;
            float row_h = rows[r].height;
            running_y += row_h;
            if (row_y + row_h < list_y0 || row_y > list_y1)
                continue; /* fully outside the viewport -- skip drawing + hit-test */
            const nc_group *grp = &groups[rows[r].group_index];
            if (rows[r].member_index < 0)
                draw_group_header(nc, grp->key, grp->members[0]->app_name, grp->members[0],
                                  grp->member_count, nc_group_is_expanded(nc, grp->key), ix, row_y,
                                  iw);
            else
                draw_card(nc, grp->members[rows[r].member_index], ix, row_y, iw);
        }
        nvgRestore(vg);

        /* Minimal scroll indicator so an overflowing History tab doesn't
         * look like it just silently clips (docs/13-POPOUTS-SPEC.md sec.3:
         * "history can exceed panel height"). */
        if (scroll_max > 0.0f) {
            float track_x = ix + iw - 3.0f;
            float thumb_h = list_h * (list_h / content_h);
            if (thumb_h < 24.0f)
                thumb_h = 24.0f;
            float thumb_y = list_y0 + (list_h - thumb_h) * (scroll / scroll_max);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
            nvgFillColor(vg, tc_alpha(t->outline, 140));
            nvgFill(vg);
        }
    }

    draw_nc_hover(nc);

    nvgEndFrame(vg);

    /* GL context is still current here -- drop cached textures for any
     * notification that's no longer Current/History (dismissed/cleared)
     * before it's possible to forget and leak the texture. */
    dc_notif_image_gc(nc->render, nc->notifications);

    if ((dc_anim_active(&nc->anim) || nc->closing) && !nc->frame_cb) {
        nc->frame_cb = wl_surface_frame(nc->surface);
        wl_callback_add_listener(nc->frame_cb, &nc_frame_listener, nc);
    }
    dc_egl_swap(nc->egl, &nc->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_notif_center *nc = data;
    DC_UNUSED(fs);
    nc->scale120 = (int)scale;
    recompute_physical(nc);
    nc_render(nc);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_notif_center *nc = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    nc->logical_width = width > 0 ? (int)width : DC_NC_WIDTH;
    nc->logical_height = height > 0 ? (int)height : DC_NC_HEIGHT;
    nc->configured = true;
    recompute_physical(nc);
    nc_render(nc);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_notif_center *nc = data;
    DC_UNUSED(surface);
    nc->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_notif_center *dc_notif_center_create(dc_wayland *wl, dc_egl *egl, dc_render *render,
                                        dc_notifications *notifications)
{
    dc_notif_center *nc = calloc(1, sizeof(*nc));
    nc->wl = wl;
    nc->egl = egl;
    nc->render = render;
    nc->notifications = notifications;
    nc->logical_width = DC_NC_WIDTH;
    nc->logical_height = DC_NC_HEIGHT;
    nc->scale120 = DC_SCALE_BASE;
    nc->tab = DC_NC_TAB_CURRENT;
    return nc;
}

static void nc_show(dc_notif_center *nc, dc_output *output)
{
    nc->output = output;
    nc->configured = false;
    nc->egl_ready = false;
    nc->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&nc->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    nc->surface = wl_compositor_create_surface(nc->wl->compositor);
    if (nc->wl->fractional_scale_mgr) {
        nc->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            nc->wl->fractional_scale_mgr, nc->surface);
        wp_fractional_scale_v1_add_listener(nc->fractional_scale, &fractional_scale_listener, nc);
    }
    if (nc->wl->viewporter)
        nc->viewport = wp_viewporter_get_viewport(nc->wl->viewporter, nc->surface);

    nc->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        nc->wl->layer_shell, nc->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:notif-center");

    /* Bar-adjacent, right-aligned (docs/13-POPOUTS-SPEC.md sec.0/3). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_NC_SIDE_MARGIN);
    nc->anim_ox = pa.origin_x;
    nc->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(nc->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(nc->layer_surface, DC_NC_WIDTH, DC_NC_HEIGHT);
    zwlr_layer_surface_v1_set_margin(nc->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_add_listener(nc->layer_surface, &layer_surface_listener, nc);

    wl_surface_commit(nc->surface);
    nc->visible = true;
    nc->closing = false;
    dc_debug("notification center shown");
}

static void nc_teardown(dc_notif_center *nc)
{
    if (nc->frame_cb) {
        wl_callback_destroy(nc->frame_cb);
        nc->frame_cb = NULL;
    }
    if (nc->egl_ready)
        dc_egl_window_finish(&nc->egl_window, nc->egl);
    if (nc->viewport)
        wp_viewport_destroy(nc->viewport);
    if (nc->fractional_scale)
        wp_fractional_scale_v1_destroy(nc->fractional_scale);
    if (nc->layer_surface)
        zwlr_layer_surface_v1_destroy(nc->layer_surface);
    if (nc->surface)
        wl_surface_destroy(nc->surface);
    nc->egl_ready = false;
    nc->configured = false;
    nc->viewport = NULL;
    nc->fractional_scale = NULL;
    nc->layer_surface = NULL;
    nc->surface = NULL;
    nc->visible = false;
    nc->closing = false;
    nc->hover_id = NC_HOVER_NONE;
    dc_debug("notification center hidden");
}

static void nc_begin_close(dc_notif_center *nc)
{
    if (!nc->visible || nc->closing)
        return;
    dc_anim_start(&nc->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    nc->closing = true;
    if (!dc_anim_active(&nc->anim)) {
        nc_teardown(nc);
        return;
    }
    nc_render(nc);
}

void dc_notif_center_toggle(dc_notif_center *nc, dc_output *output)
{
    if (nc->visible)
        nc_begin_close(nc);
    else
        nc_show(nc, output);
}

void dc_notif_center_hide(dc_notif_center *nc)
{
    nc_begin_close(nc);
}

bool dc_notif_center_visible(dc_notif_center *nc)
{
    return nc->visible;
}

struct wl_surface *dc_notif_center_surface(dc_notif_center *nc)
{
    return nc->surface;
}

void dc_notif_center_refresh(dc_notif_center *nc)
{
    if (nc && nc->visible)
        nc_render(nc);
}

static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* Which interactive element (if any) sits under (x, y) -- shares the exact
 * hit boundaries dc_notif_center_handle_click() dispatches against, so
 * hover and click can never disagree (same discipline as controlcenter.c's
 * cc_hittest()). Priority mirrors the click handler: header buttons/tabs,
 * then each card's close/dismiss/action before its own body. */
static int nc_hittest(dc_notif_center *nc, double x, double y)
{
    if (in_rect(x, y, nc->tab_x0[0], nc->tab_y0[0], nc->tab_x1[0], nc->tab_y1[0]))
        return NC_HOVER_TAB0;
    if (in_rect(x, y, nc->tab_x0[1], nc->tab_y0[1], nc->tab_x1[1], nc->tab_y1[1]))
        return NC_HOVER_TAB1;
    if (in_rect(x, y, nc->clear_x0, nc->clear_y0, nc->clear_x1, nc->clear_y1))
        return NC_HOVER_CLEAR;
    if (in_rect(x, y, nc->settings_x0, nc->settings_y0, nc->settings_x1, nc->settings_y1))
        return NC_HOVER_SETTINGS;

    for (int i = 0; i < 5; i++)
        if (in_rect(x, y, nc->dnd_chip_x0[i], nc->dnd_chip_y0[i], nc->dnd_chip_x1[i],
                    nc->dnd_chip_y1[i]))
            return NC_HOVER_DND_OFF + i;
    for (int i = 0; i < 4; i++)
        if (in_rect(x, y, nc->hist_chip_x0[i], nc->hist_chip_y0[i], nc->hist_chip_x1[i],
                    nc->hist_chip_y1[i]))
            return NC_HOVER_HIST_ALL + i;

    for (int g = 0; g < nc->group_hit_count; g++) {
        nc_group_hit *gh = &nc->group_hits[g];
        if (in_rect(x, y, gh->x0, gh->y0, gh->x1, gh->y1))
            return NC_HOVER_GROUP_BASE + g;
    }

    for (int i = 0; i < nc->hit_count; i++) {
        nc_card_hit *hit = &nc->hits[i];
        if (in_rect(x, y, hit->close_x0, hit->close_y0, hit->close_x1, hit->close_y1))
            return NC_HOVER_CARD_BASE + i * NC_HOVER_CARD_STRIDE + 1;
        if (hit->has_dismiss &&
            in_rect(x, y, hit->dismiss_x0, hit->dismiss_y0, hit->dismiss_x1, hit->dismiss_y1))
            return NC_HOVER_CARD_BASE + i * NC_HOVER_CARD_STRIDE + 2;
        for (int j = 0; j < hit->action_count; j++)
            if (in_rect(x, y, hit->action_x0[j], hit->action_y0, hit->action_x1[j], hit->action_y1))
                return NC_HOVER_CARD_BASE + i * NC_HOVER_CARD_STRIDE + 3 + j;
        if (in_rect(x, y, hit->card_x0, hit->card_y0, hit->card_x1, hit->card_y1))
            return NC_HOVER_CARD_BASE + i * NC_HOVER_CARD_STRIDE + 0;
    }
    return NC_HOVER_NONE;
}

void dc_notif_center_handle_click(dc_notif_center *nc, double x, double y)
{
    if (!nc->visible || nc->closing)
        return;

    if (in_rect(x, y, nc->tab_x0[0], nc->tab_y0[0], nc->tab_x1[0], nc->tab_y1[0])) {
        if (nc->tab != DC_NC_TAB_CURRENT) {
            nc->tab = DC_NC_TAB_CURRENT;
            nc_render(nc);
        }
        return;
    }
    if (in_rect(x, y, nc->tab_x0[1], nc->tab_y0[1], nc->tab_x1[1], nc->tab_y1[1])) {
        if (nc->tab != DC_NC_TAB_HISTORY) {
            nc->tab = DC_NC_TAB_HISTORY;
            nc_render(nc);
        }
        return;
    }

    if (in_rect(x, y, nc->clear_x0, nc->clear_y0, nc->clear_x1, nc->clear_y1)) {
        if (nc->tab == DC_NC_TAB_CURRENT)
            dc_notifications_clear_current(nc->notifications);
        else
            dc_notifications_clear_history(nc->notifications);
        nc_render(nc);
        return;
    }

    if (in_rect(x, y, nc->settings_x0, nc->settings_y0, nc->settings_x1, nc->settings_y1)) {
        /* TODO: wire to dankc's settings popout once it can be opened from
         * here without a main.c dependency beyond this task's touch-scope. */
        dc_debug("notification center: settings gear clicked (no-op)");
        return;
    }

    /* DND chip row + History time-filter chips (docs/26-DND-SCHEDULING-
     * PLAN.md UI/T3) -- same rect-check order as nc_hittest() above (header
     * buttons, then these, then groups/cards) so hover and click can never
     * disagree about what's under the pointer. */
    if (in_rect(x, y, nc->dnd_chip_x0[0], nc->dnd_chip_y0[0], nc->dnd_chip_x1[0],
                nc->dnd_chip_y1[0])) {
        dc_notif_dnd_stop(nc->notifications);
        nc_render(nc);
        return;
    }
    if (in_rect(x, y, nc->dnd_chip_x0[1], nc->dnd_chip_y0[1], nc->dnd_chip_x1[1],
                nc->dnd_chip_y1[1])) {
        dc_notif_dnd_start(nc->notifications, 900);
        nc_render(nc);
        return;
    }
    if (in_rect(x, y, nc->dnd_chip_x0[2], nc->dnd_chip_y0[2], nc->dnd_chip_x1[2],
                nc->dnd_chip_y1[2])) {
        dc_notif_dnd_start(nc->notifications, 3600);
        nc_render(nc);
        return;
    }
    if (in_rect(x, y, nc->dnd_chip_x0[3], nc->dnd_chip_y0[3], nc->dnd_chip_x1[3],
                nc->dnd_chip_y1[3])) {
        int until_hour = dc_config_current ? dc_config_current->dnd_until_hour : 8;
        dc_notif_dnd_start_until_hour(nc->notifications, until_hour);
        nc_render(nc);
        return;
    }
    if (in_rect(x, y, nc->dnd_chip_x0[4], nc->dnd_chip_y0[4], nc->dnd_chip_x1[4],
                nc->dnd_chip_y1[4])) {
        dc_notif_dnd_start(nc->notifications, 0);
        nc_render(nc);
        return;
    }

    for (int i = 0; i < 4; i++) {
        if (in_rect(x, y, nc->hist_chip_x0[i], nc->hist_chip_y0[i], nc->hist_chip_x1[i],
                    nc->hist_chip_y1[i])) {
            if (nc->history_filter != i) {
                nc->history_filter = i;
                nc_render(nc);
            }
            return;
        }
    }

    for (int g = 0; g < nc->group_hit_count; g++) {
        nc_group_hit *gh = &nc->group_hits[g];
        if (in_rect(x, y, gh->x0, gh->y0, gh->x1, gh->y1)) {
            nc_group_toggle_expanded(nc, gh->key);
            nc_render(nc);
            return;
        }
    }

    for (int i = 0; i < nc->hit_count; i++) {
        nc_card_hit *hit = &nc->hits[i];
        if (in_rect(x, y, hit->close_x0, hit->close_y0, hit->close_x1, hit->close_y1) ||
            (hit->has_dismiss &&
             in_rect(x, y, hit->dismiss_x0, hit->dismiss_y0, hit->dismiss_x1, hit->dismiss_y1))) {
            dc_notifications_dismiss(nc->notifications, hit->id);
            nc_render(nc);
            return;
        }
        for (int j = 0; j < hit->action_count; j++) {
            if (in_rect(x, y, hit->action_x0[j], hit->action_y0, hit->action_x1[j],
                        hit->action_y1)) {
                dc_notifications_invoke_action(nc->notifications, hit->id, j);
                nc_render(nc);
                return;
            }
        }
    }
}

/* Pointer motion over the panel (docs/13-POPOUTS-SPEC.md sec.3): hover
 * tracking only (no drag surface here) -- re-render only when the hovered id
 * changes, same guard pattern as bar.c/controlcenter.c. */
void dc_notif_center_handle_motion(dc_notif_center *nc, double x, double y)
{
    if (!nc->visible || nc->closing)
        return;

    int id = nc_hittest(nc, x, y);
    if (id == nc->hover_id)
        return;

    nc->hover_id = id;
    dc_wayland_set_cursor(nc->wl, id != NC_HOVER_NONE ? DC_CURSOR_POINTER : DC_CURSOR_DEFAULT);
    nc_render(nc);
}

/* Pointer left the panel entirely: clear hover. */
void dc_notif_center_handle_leave(dc_notif_center *nc)
{
    if (nc->hover_id == NC_HOVER_NONE)
        return;
    nc->hover_id = NC_HOVER_NONE;
    dc_wayland_set_cursor(nc->wl, DC_CURSOR_DEFAULT);
    nc_render(nc);
}

void dc_notif_center_handle_scroll(dc_notif_center *nc, int steps_v)
{
    if (!nc->visible || nc->closing || steps_v == 0)
        return;
    float s = nc->scroll[nc->tab] + (float)steps_v * DC_NC_SCROLL_STEP;
    if (s < 0.0f)
        s = 0.0f;
    if (s > nc->scroll_max[nc->tab])
        s = nc->scroll_max[nc->tab];
    if (s == nc->scroll[nc->tab])
        return;
    nc->scroll[nc->tab] = s;
    nc_render(nc);
}

void dc_notif_center_destroy(dc_notif_center *nc)
{
    if (!nc)
        return;
    if (nc->visible)
        nc_teardown(nc);
    free(nc);
}
