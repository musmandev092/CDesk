#include "ui/toasts.h"

#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/notifications.h"
#include "theme/theme.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_TOAST_WIDTH 400
#define DC_TOAST_CARD_H 88
#define DC_TOAST_GAP 10
#define DC_TOAST_MAX 4
#define DC_TOAST_TOP_MARGIN 8
#define DC_TOAST_RIGHT_MARGIN 12
#define DC_SCALE_BASE 120

#define DC_TOAST_STACK_H (DC_TOAST_MAX * (DC_TOAST_CARD_H + DC_TOAST_GAP))

struct dc_toasts {
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

    /* Cards currently laid out, for hit-testing clicks. */
    uint32_t card_ids[DC_TOAST_MAX];
    int card_count;

    bool visible;
    bool configured;
    bool egl_ready;
};

static void recompute_physical(dc_toasts *t)
{
    t->phys_width = (t->logical_width * t->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    t->phys_height = (t->logical_height * t->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Restrict pointer input to the occupied card rectangles so clicks over the
 * transparent gaps fall through to the windows below. */
static void update_input_region(dc_toasts *t, int card_count)
{
    struct wl_region *region = wl_compositor_create_region(t->wl->compositor);
    for (int i = 0; i < card_count; i++) {
        int y = i * (DC_TOAST_CARD_H + DC_TOAST_GAP);
        wl_region_add(region, 0, y, DC_TOAST_WIDTH, DC_TOAST_CARD_H);
    }
    wl_surface_set_input_region(t->surface, region);
    wl_region_destroy(region);
}

/* Draw a single notification card at top-left (cx, cy0) in logical units. */
static void draw_card(dc_toasts *t, const dc_notification *n, float x, float y)
{
    NVGcontext *vg = t->render->vg;
    const dc_theme *th = dc_theme_current;
    const float w = DC_TOAST_WIDTH - 2 * 4.0f;
    const float h = DC_TOAST_CARD_H;
    const float r = 16.0f;
    x += 4.0f;

    /* Drop shadow. */
    NVGpaint shadow = nvgBoxGradient(vg, x, y + 2.0f, w, h, r, 18.0f, nvgRGBA(0, 0, 0, 90),
                                     nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, x - 6.0f, y - 6.0f, w + 12.0f, h + 14.0f);
    nvgRoundedRect(vg, x, y, w, h, r);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card background. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, r);
    nvgFillColor(vg, nvgRGBA(th->surface_container.r, th->surface_container.g,
                             th->surface_container.b, 255));
    nvgFill(vg);

    /* Critical accent bar on the left edge. */
    if (n->urgency == DC_URGENCY_CRITICAL) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y + 10.0f, 4.0f, h - 20.0f, 2.0f);
        nvgFillColor(vg, nvgRGBA(th->primary.r, th->primary.g, th->primary.b, 255));
        nvgFill(vg);
    }

    /* Avatar circle with the app's initial. */
    const float av_r = 20.0f;
    const float av_cx = x + 16.0f + av_r;
    const float av_cy = y + h / 2.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, av_cx, av_cy, av_r);
    nvgFillColor(vg, nvgRGBA(th->primary.r, th->primary.g, th->primary.b, 255));
    nvgFill(vg);

    char initial[2] = {0};
    initial[0] = n->app_name[0] ? (char)toupper((unsigned char)n->app_name[0]) : '?';
    nvgFontFaceId(vg, t->render->font_ui);
    nvgFontSize(vg, 20.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, nvgRGBA(th->surface_container.r, th->surface_container.g,
                             th->surface_container.b, 255));
    nvgText(vg, av_cx, av_cy + 1.0f, initial, NULL);

    /* Text column. */
    const float tx = av_cx + av_r + 14.0f;
    const float tw = x + w - tx - 14.0f;

    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(th->primary.r, th->primary.g, th->primary.b, 255));
    nvgText(vg, tx, y + 14.0f, n->app_name, NULL);

    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGBA(th->surface_text.r, th->surface_text.g, th->surface_text.b, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    /* Single-line summary, ellipsised by clipping. */
    nvgSave(vg);
    nvgScissor(vg, tx, y + 30.0f, tw, 18.0f);
    nvgText(vg, tx, y + 31.0f, n->summary, NULL);
    nvgRestore(vg);

    if (n->body[0]) {
        nvgFontSize(vg, 13.0f);
        nvgFillColor(vg, nvgRGBA(th->surface_text.r, th->surface_text.g, th->surface_text.b, 170));
        nvgSave(vg);
        nvgScissor(vg, tx, y + 50.0f, tw, 30.0f);
        nvgTextLineHeight(vg, 1.1f);
        nvgTextBox(vg, tx, y + 51.0f, tw, n->body, NULL);
        nvgRestore(vg);
    }
}

static void toasts_render(dc_toasts *t)
{
    if (!t->configured || t->phys_width <= 0)
        return;
    if (!t->egl_ready) {
        if (!dc_egl_window_init(&t->egl_window, t->egl, t->surface, t->phys_width, t->phys_height))
            return;
        t->egl_ready = true;
    } else {
        dc_egl_window_resize(&t->egl_window, t->phys_width, t->phys_height);
    }
    if (!dc_egl_make_current(t->egl, &t->egl_window))
        return;
    if (!dc_render_ensure(t->render))
        return;
    if (t->viewport)
        wp_viewport_set_destination(t->viewport, t->logical_width, t->logical_height);

    const dc_notification *popups[DC_TOAST_MAX];
    int count = dc_notifications_popups(t->notifications, popups, DC_TOAST_MAX);
    t->card_count = count;

    NVGcontext *vg = t->render->vg;
    glViewport(0, 0, t->phys_width, t->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, t->logical_width, t->logical_height, (float)t->scale120 / DC_SCALE_BASE);
    for (int i = 0; i < count; i++) {
        t->card_ids[i] = popups[i]->id;
        draw_card(t, popups[i], 0.0f, (float)(i * (DC_TOAST_CARD_H + DC_TOAST_GAP)));
    }
    nvgEndFrame(vg);
    dc_egl_swap(t->egl, &t->egl_window);

    update_input_region(t, count);
    wl_surface_commit(t->surface);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_toasts *t = data;
    DC_UNUSED(fs);
    t->scale120 = (int)scale;
    recompute_physical(t);
    toasts_render(t);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_toasts *t = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    t->logical_width = width > 0 ? (int)width : DC_TOAST_WIDTH;
    t->logical_height = height > 0 ? (int)height : DC_TOAST_STACK_H;
    t->configured = true;
    recompute_physical(t);
    toasts_render(t);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_toasts *t = data;
    DC_UNUSED(surface);
    t->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static void toasts_hide(dc_toasts *t)
{
    if (!t->visible)
        return;
    if (t->egl_ready)
        dc_egl_window_finish(&t->egl_window, t->egl);
    if (t->viewport)
        wp_viewport_destroy(t->viewport);
    if (t->fractional_scale)
        wp_fractional_scale_v1_destroy(t->fractional_scale);
    if (t->layer_surface)
        zwlr_layer_surface_v1_destroy(t->layer_surface);
    if (t->surface)
        wl_surface_destroy(t->surface);
    t->egl_ready = false;
    t->configured = false;
    t->viewport = NULL;
    t->fractional_scale = NULL;
    t->layer_surface = NULL;
    t->surface = NULL;
    t->visible = false;
    t->card_count = 0;
}

static void toasts_show(dc_toasts *t)
{
    if (t->visible)
        return;
    dc_output *output = t->output;
    t->configured = false;
    t->egl_ready = false;
    t->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;

    t->surface = wl_compositor_create_surface(t->wl->compositor);
    if (t->wl->fractional_scale_mgr) {
        t->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            t->wl->fractional_scale_mgr, t->surface);
        wp_fractional_scale_v1_add_listener(t->fractional_scale, &fractional_scale_listener, t);
    }
    if (t->wl->viewporter)
        t->viewport = wp_viewporter_get_viewport(t->wl->viewporter, t->surface);

    t->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        t->wl->layer_shell, t->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:toasts");
    zwlr_layer_surface_v1_set_anchor(t->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                           ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(t->layer_surface, DC_TOAST_WIDTH, DC_TOAST_STACK_H);
    zwlr_layer_surface_v1_set_margin(t->layer_surface, DC_TOAST_TOP_MARGIN, DC_TOAST_RIGHT_MARGIN, 0,
                                     0);
    zwlr_layer_surface_v1_add_listener(t->layer_surface, &layer_surface_listener, t);

    wl_surface_commit(t->surface);
    t->visible = true;
}

dc_toasts *dc_toasts_create(dc_wayland *wl, dc_egl *egl, dc_render *render,
                            dc_notifications *notifications, dc_output *output)
{
    dc_toasts *t = calloc(1, sizeof(*t));
    t->wl = wl;
    t->egl = egl;
    t->render = render;
    t->notifications = notifications;
    t->output = output;
    t->logical_width = DC_TOAST_WIDTH;
    t->logical_height = DC_TOAST_STACK_H;
    t->scale120 = DC_SCALE_BASE;
    return t;
}

void dc_toasts_destroy(dc_toasts *t)
{
    if (!t)
        return;
    toasts_hide(t);
    free(t);
}

void dc_toasts_refresh(dc_toasts *t)
{
    if (!t || !t->notifications)
        return;
    const dc_notification *popups[DC_TOAST_MAX];
    int count = dc_notifications_popups(t->notifications, popups, DC_TOAST_MAX);

    if (count == 0) {
        toasts_hide(t);
        return;
    }
    if (!t->visible) {
        toasts_show(t);
        return; /* first render happens on configure */
    }
    toasts_render(t);
}

bool dc_toasts_handle_click(dc_toasts *t, struct wl_surface *surface, double x, double y)
{
    if (!t || !t->visible || surface != t->surface)
        return false;
    DC_UNUSED(x);
    int idx = (int)(y / (DC_TOAST_CARD_H + DC_TOAST_GAP));
    if (idx >= 0 && idx < t->card_count) {
        double within = y - idx * (DC_TOAST_CARD_H + DC_TOAST_GAP);
        if (within <= DC_TOAST_CARD_H)
            dc_notifications_dismiss(t->notifications, t->card_ids[idx]);
    }
    return true;
}
