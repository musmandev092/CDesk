#include "ui/notifcenter.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/notifications.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_NC_WIDTH 400
#define DC_NC_HEIGHT 480
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/3: opens near the bell, effectively the bar's right cluster). */
#define DC_NC_SIDE_MARGIN 12

#define DC_NC_PAD 6.0f
#define DC_NC_INSET 18.0f
#define DC_NC_HEADER_H 52.0f
#define DC_NC_CARD_H 76.0f
#define DC_NC_CARD_GAP 8.0f
#define DC_NC_LIST_Y 60.0f
#define DC_NC_MAX_CARDS 32

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

    float clear_x0, clear_x1, clear_y0, clear_y1; /* Clear-all button rect */

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    /* Entrance/exit scale-and-fade pivot, bar-position-aware — see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;

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

static void draw_history_card(dc_notif_center *nc, const dc_notification *n, float x, float y,
                              float w)
{
    NVGcontext *vg = nc->render->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, DC_NC_CARD_H, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    /* Avatar initial. */
    const float av_r = 16.0f;
    const float av_cx = x + 14.0f + av_r;
    const float av_cy = y + DC_NC_CARD_H / 2.0f;
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

    const float tx = av_cx + av_r + 12.0f;
    const float tw = x + w - tx - 12.0f;

    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, tx, y + 12.0f, n->app_name, NULL);

    nvgSave(vg);
    nvgScissor(vg, tx, y + 26.0f, tw, 18.0f);
    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, tx, y + 27.0f, n->summary, NULL);
    nvgRestore(vg);

    if (n->body[0]) {
        nvgSave(vg);
        nvgScissor(vg, tx, y + 45.0f, tw, 24.0f);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc_alpha(t->surface_text, 160));
        nvgTextLineHeight(vg, 1.1f);
        nvgTextBox(vg, tx, y + 46.0f, tw, n->body, NULL);
        nvgRestore(vg);
    }
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
    const float ix = DC_NC_INSET;
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
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 14.0f, 18.0f,
                                     nvgRGBA(0, 0, 0, 100), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 14.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 14.0f);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);

    /* Header: title + count + Clear-all. */
    const dc_notification *hist[DC_NC_MAX_CARDS];
    int count = dc_notifications_history(nc->notifications, hist, DC_NC_MAX_CARDS);

    nvgFontFaceId(vg, nc->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, ix, pad + 26.0f, "Notifications", NULL);

    if (count > 0) {
        const char *label = "Clear all";
        nvgFontSize(vg, 13.0f);
        float b[4];
        nvgTextBounds(vg, 0, 0, label, NULL, b);
        float bw = b[2] - b[0];
        float bx1 = ix + iw;
        float bx0 = bx1 - bw - 20.0f;
        float by0 = pad + 12.0f, by1 = by0 + 28.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx0, by0, bx1 - bx0, by1 - by0, 8.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 40));
        nvgFill(vg);
        nvgFillColor(vg, tc(t->primary));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, (bx0 + bx1) / 2.0f, (by0 + by1) / 2.0f, label, NULL);
        nc->clear_x0 = bx0;
        nc->clear_x1 = bx1;
        nc->clear_y0 = by0;
        nc->clear_y1 = by1;
    } else {
        nc->clear_x0 = nc->clear_x1 = 0.0f;
    }

    /* Cards (clipped to the panel; no scroll yet — show what fits). */
    float y = pad + DC_NC_LIST_Y;
    const float bottom = h - pad - 12.0f;
    int shown = 0;
    for (int i = 0; i < count; i++) {
        if (y + DC_NC_CARD_H > bottom)
            break;
        draw_history_card(nc, hist[i], ix, y, iw);
        y += DC_NC_CARD_H + DC_NC_CARD_GAP;
        shown++;
    }

    if (count == 0) {
        dc_color dim = t->surface_text;
        dim.a = 90;
        dc_render_icon(nc->render, DC_ICON_NOTIFICATIONS, w / 2.0f, h / 2.0f - 20.0f, 40.0f, dim,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontSize(vg, 14.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, h / 2.0f + 16.0f, "No notifications", NULL);
    } else if (shown < count) {
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 130));
        char more[32];
        snprintf(more, sizeof(more), "+%d more", count - shown);
        nvgText(vg, w / 2.0f, bottom - 2.0f, more, NULL);
    }

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

void dc_notif_center_handle_click(dc_notif_center *nc, double x, double y)
{
    if (!nc->visible || nc->closing)
        return;
    if (nc->clear_x1 > nc->clear_x0 && x >= nc->clear_x0 && x <= nc->clear_x1 &&
        y >= nc->clear_y0 && y <= nc->clear_y1) {
        dc_notifications_clear_history(nc->notifications);
        nc_render(nc);
    }
}

void dc_notif_center_destroy(dc_notif_center *nc)
{
    if (!nc)
        return;
    if (nc->visible)
        nc_teardown(nc);
    free(nc);
}
