#include "ui/notifcenter.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/notifications.h"
#include "theme/theme.h"
#include "ui/hover.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
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

#define DC_NC_CARD_H 120.0f
#define DC_NC_CARD_GAP 10.0f
#define DC_NC_MAX_CARDS DC_NOTIF_MAX

#define DC_NC_SCROLL_STEP 48.0f

typedef enum {
    DC_NC_TAB_CURRENT = 0,
    DC_NC_TAB_HISTORY = 1,
} dc_nc_tab;

/* Hit-test rects for one visible card, captured during nc_render() and
 * consumed by dc_notif_center_handle_click() -- same "record while drawing"
 * convention controlcenter.c uses for its own buttons. */
typedef struct {
    uint32_t id;
    dc_notif_status status;
    float card_x0, card_y0, card_x1, card_y1;
    float close_x0, close_y0, close_x1, close_y1;
    float dismiss_x0, dismiss_y0, dismiss_x1, dismiss_y1;
    bool has_action;
    float action_x0, action_y0, action_x1, action_y1;
} nc_card_hit;

/* Hover ids (docs/13-POPOUTS-SPEC.md sec.3: hover bg on cards, X, Dismiss/
 * action buttons, tabs, Clear). The fixed header elements get small named
 * ids; each card's sub-regions are packed as NC_HOVER_CARD_BASE + hit-index*4
 * + {0:body,1:close,2:dismiss,3:action} since the card list is dynamic
 * (count/order changes every render). */
#define NC_HOVER_NONE 0
#define NC_HOVER_TAB0 1
#define NC_HOVER_TAB1 2
#define NC_HOVER_CLEAR 3
#define NC_HOVER_SETTINGS 4
#define NC_HOVER_CARD_BASE 10

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

    nc_card_hit hits[DC_NC_MAX_CARDS];
    int hit_count;

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

/* Truncate `buf` in place to fit `max_w` at the current font, appending a
 * single-character ellipsis -- same approach as controlcenter.c's
 * cc_ellipsize() (duplicated locally; no shared string-util module yet). */
static void nc_ellipsize(NVGcontext *vg, char *buf, size_t bufsize, float max_w)
{
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, buf, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_w)
        return;

    size_t len = strlen(buf);
    if (bufsize < 4)
        return;
    char tmp[256];
    size_t cap = bufsize > sizeof(tmp) ? sizeof(tmp) : bufsize;
    if (len > cap - 4)
        len = cap - 4;

    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--; /* don't split a multi-byte UTF-8 codepoint */
        snprintf(tmp, cap, "%.*s\xe2\x80\xa6", (int)len, buf);
        nvgTextBounds(vg, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_w || len == 0) {
            memcpy(buf, tmp, cap);
            return;
        }
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

    /* Avatar initial, centered on the header+title+body text block (leaves
     * the bottom button row clear, matching the reference). */
    const float av_r = 18.0f;
    const float av_cx = x + 16.0f + av_r;
    const float av_cy = y + 14.0f + 34.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, av_cx, av_cy, av_r);
    nvgFillColor(vg, tc_alpha(t->primary, n->urgency == DC_URGENCY_CRITICAL ? 255 : 150));
    nvgFill(vg);
    char initial[2] = {n->app_name[0] ? (char)toupper((unsigned char)n->app_name[0]) : '?', 0};
    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_container));
    nvgText(vg, av_cx, av_cy + 1.0f, initial, NULL);

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
    nc_ellipsize(vg, header_buf, sizeof(header_buf), tw);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc_alpha(t->surface_text, 150));
    nvgText(vg, tx, y + 14.0f, header_buf, NULL);

    char title_buf[DC_NOTIF_SUMMARY];
    snprintf(title_buf, sizeof(title_buf), "%s", n->summary);
    nvgFontSize(vg, 14.0f);
    nc_ellipsize(vg, title_buf, sizeof(title_buf), tw);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, tx, y + 32.0f, title_buf, NULL);

    if (n->body[0]) {
        nvgSave(vg);
        nvgScissor(vg, tx, y + 52.0f, tw, 30.0f);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc_alpha(t->surface_text, 160));
        nvgTextLineHeight(vg, 1.15f);
        nvgTextBox(vg, tx, y + 53.0f, tw, n->body, NULL);
        nvgRestore(vg);
    }

    /* Bottom-right button row: [action] Dismiss, right-aligned. */
    const float row_y1 = y + DC_NC_CARD_H - 12.0f;
    const float row_h = 22.0f;
    const float row_y0 = row_y1 - row_h;
    float cursor_x1 = x + w - 14.0f;

    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    float b[4];
    nvgTextBounds(vg, 0, 0, "Dismiss", NULL, b);
    float dismiss_w = b[2] - b[0] + 18.0f;
    float dismiss_x0 = cursor_x1 - dismiss_w;
    nvgFillColor(vg, tc_alpha(t->surface_text, 190));
    nvgText(vg, (dismiss_x0 + cursor_x1) / 2.0f, (row_y0 + row_y1) / 2.0f, "Dismiss", NULL);
    hit->dismiss_x0 = dismiss_x0;
    hit->dismiss_y0 = row_y0;
    hit->dismiss_x1 = cursor_x1;
    hit->dismiss_y1 = row_y1;
    cursor_x1 = dismiss_x0 - 10.0f;

    if (n->action_key[0]) {
        char action_buf[DC_NOTIF_ACTION];
        snprintf(action_buf, sizeof(action_buf), "%s",
                (n->action_label[0]) ? n->action_label : "Open");
        nvgTextBounds(vg, 0, 0, action_buf, NULL, b);
        float action_w = b[2] - b[0] + 18.0f;
        float action_min_x = tx; /* never crowd past the text column's left edge */
        float action_x0 = cursor_x1 - action_w;
        if (action_x0 < action_min_x)
            action_x0 = action_min_x;
        nvgFillColor(vg, tc(t->primary));
        nvgText(vg, (action_x0 + cursor_x1) / 2.0f, (row_y0 + row_y1) / 2.0f, action_buf, NULL);
        hit->has_action = true;
        hit->action_x0 = action_x0;
        hit->action_y0 = row_y0;
        hit->action_x1 = cursor_x1;
        hit->action_y1 = row_y1;
    }
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
    } else if (nc->hover_id >= NC_HOVER_CARD_BASE) {
        int rel = nc->hover_id - NC_HOVER_CARD_BASE;
        int i = rel / 4, kind = rel % 4;
        if (i < 0 || i >= nc->hit_count)
            return;
        const nc_card_hit *hit = &nc->hits[i];
        switch (kind) {
        case 0:
            x0 = hit->card_x0;
            y0 = hit->card_y0;
            x1 = hit->card_x1;
            y1 = hit->card_y1;
            radius = 12.0f;
            break;
        case 1:
            x0 = hit->close_x0;
            y0 = hit->close_y0;
            x1 = hit->close_x1;
            y1 = hit->close_y1;
            radius = (x1 - x0) / 2.0f;
            break;
        case 2:
            x0 = hit->dismiss_x0;
            y0 = hit->dismiss_y0;
            x1 = hit->dismiss_x1;
            y1 = hit->dismiss_y1;
            radius = (y1 - y0) / 2.0f;
            break;
        case 3:
            if (!hit->has_action)
                return;
            x0 = hit->action_x0;
            y0 = hit->action_y0;
            x1 = hit->action_x1;
            y1 = hit->action_y1;
            radius = (y1 - y0) / 2.0f;
            break;
        default:
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

    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, DC_NC_RADIUS);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);
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

    /* --- Card list, scrollable ------------------------------------------ */
    const float list_y0 = tabs_y + DC_NC_TABS_H + DC_NC_LIST_GAP;
    const float list_y1 = h - pad - DC_NC_BOTTOM_PAD;
    const float list_h = list_y1 - list_y0;

    const dc_notification *entries[DC_NC_MAX_CARDS];
    int count = (nc->tab == DC_NC_TAB_CURRENT)
                   ? dc_notifications_current(nc->notifications, entries, DC_NC_MAX_CARDS)
                   : dc_notifications_history(nc->notifications, entries, DC_NC_MAX_CARDS);

    float content_h = count > 0 ? (float)count * (DC_NC_CARD_H + DC_NC_CARD_GAP) - DC_NC_CARD_GAP
                                : 0.0f;
    float scroll_max = content_h > list_h ? content_h - list_h : 0.0f;
    if (nc->scroll[nc->tab] < 0.0f)
        nc->scroll[nc->tab] = 0.0f;
    if (nc->scroll[nc->tab] > scroll_max)
        nc->scroll[nc->tab] = scroll_max;
    nc->scroll_max[nc->tab] = scroll_max;
    float scroll = nc->scroll[nc->tab];

    nc->hit_count = 0;

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
        for (int i = 0; i < count; i++) {
            float y = list_y0 + (float)i * (DC_NC_CARD_H + DC_NC_CARD_GAP) - scroll;
            if (y + DC_NC_CARD_H < list_y0 || y > list_y1)
                continue; /* fully outside the viewport -- skip drawing + hit-test */
            draw_card(nc, entries[i], ix, y, iw);
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

    for (int i = 0; i < nc->hit_count; i++) {
        nc_card_hit *hit = &nc->hits[i];
        if (in_rect(x, y, hit->close_x0, hit->close_y0, hit->close_x1, hit->close_y1))
            return NC_HOVER_CARD_BASE + i * 4 + 1;
        if (in_rect(x, y, hit->dismiss_x0, hit->dismiss_y0, hit->dismiss_x1, hit->dismiss_y1))
            return NC_HOVER_CARD_BASE + i * 4 + 2;
        if (hit->has_action &&
            in_rect(x, y, hit->action_x0, hit->action_y0, hit->action_x1, hit->action_y1))
            return NC_HOVER_CARD_BASE + i * 4 + 3;
        if (in_rect(x, y, hit->card_x0, hit->card_y0, hit->card_x1, hit->card_y1))
            return NC_HOVER_CARD_BASE + i * 4 + 0;
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

    for (int i = 0; i < nc->hit_count; i++) {
        nc_card_hit *hit = &nc->hits[i];
        if (in_rect(x, y, hit->close_x0, hit->close_y0, hit->close_x1, hit->close_y1) ||
            in_rect(x, y, hit->dismiss_x0, hit->dismiss_y0, hit->dismiss_x1, hit->dismiss_y1)) {
            dc_notifications_dismiss(nc->notifications, hit->id);
            nc_render(nc);
            return;
        }
        if (hit->has_action &&
            in_rect(x, y, hit->action_x0, hit->action_y0, hit->action_x1, hit->action_y1)) {
            dc_notifications_invoke_action(nc->notifications, hit->id);
            nc_render(nc);
            return;
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
