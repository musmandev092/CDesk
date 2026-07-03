#include "ui/toasts.h"

#include "core/anim.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "services/notifications.h"
#include "theme/theme.h"
#include "ui/notif_image.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <math.h>
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

/* Hit-test rect(s) for one visible toast card, captured during draw_card()
 * and consumed by dc_toasts_handle_click() -- same "record while drawing"
 * convention notifcenter.c's nc_card_hit uses. The card body itself is a
 * uniform DC_TOAST_CARD_H-tall slot at index*(DC_TOAST_CARD_H+DC_TOAST_GAP),
 * so it doesn't need its own stored rect the way the action buttons (whose
 * count/position varies per card) do. */
typedef struct {
    uint32_t id;
    int action_count;
    float action_x0[DC_NOTIF_ACTION_MAX], action_x1[DC_NOTIF_ACTION_MAX];
    float action_y0, action_y1;
} toast_card_hit;

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
    toast_card_hit hits[DC_TOAST_MAX];
    int card_count;

    dc_anim anim;
    struct wl_callback *frame_cb;

    bool visible;
    bool configured;
    bool egl_ready;
};

static void toasts_render(dc_toasts *t);

static void toasts_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener toasts_frame_listener = {.done = toasts_frame_done};

static void toasts_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_toasts *t = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    t->frame_cb = NULL;
    if (t->visible && dc_anim_active(&t->anim))
        toasts_render(t);
}

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

/* Draw a single notification card at top-left (cx, cy0) in logical units,
 * recording its action-button hit rects into *hit (id/action_count already
 * set by the caller). */
static void draw_card(dc_toasts *t, const dc_notification *n, float x, float y, toast_card_hit *hit)
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

    /* Avatar: the notification's image (image-data hint / image-path /
     * app_icon file) cover-fit into the circle, else a circle with the app's
     * initial -- see notif_image.h (cache shared with notifcenter.c). */
    const float av_r = 20.0f;
    const float av_cx = x + 16.0f + av_r;
    const float av_cy = y + h / 2.0f;
    int img_w = 0, img_h = 0;
    int img = dc_notif_image_get(t->render, n, &img_w, &img_h);
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
    }

    /* Text column. */
    const float tx = av_cx + av_r + 14.0f;
    const float tw = x + w - tx - 14.0f;

    nvgFontFaceId(vg, t->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    nvgFillColor(vg, nvgRGBA(th->primary.r, th->primary.g, th->primary.b, 255));
    dc_shape_draw_text(t->render, tx, y + 14.0f, n->app_name, NULL);

    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, nvgRGBA(th->surface_text.r, th->surface_text.g, th->surface_text.b, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    /* Single-line summary, ellipsised by clipping. */
    nvgSave(vg);
    nvgScissor(vg, tx, y + 30.0f, tw, 18.0f);
    dc_shape_draw_text(t->render, tx, y + 31.0f, n->summary, NULL);
    nvgRestore(vg);

    /* Body text yields its bottom rows to a pill-button row when the
     * notification has actions (docs/13-POPOUTS-SPEC.md; DMS's NotificationPopup
     * reserves basePopupHeight for actionButtonHeight the same way). */
    bool has_actions = n->action_count > 0;
    float body_h = has_actions ? 16.0f : 30.0f;
    if (n->body[0]) {
        nvgFontSize(vg, 13.0f);
        nvgFillColor(vg, nvgRGBA(th->surface_text.r, th->surface_text.g, th->surface_text.b, 170));
        nvgSave(vg);
        nvgScissor(vg, tx, y + 50.0f, tw, body_h);
        if (has_actions) {
            /* body_h only budgets one line here -- word-wrapping via
             * dc_shape_draw_textbox() would still lay out a 2nd line whose
             * clipped remnant peeks out just above the action-button row
             * right below (seen with a real 2-line NetworkManager Applet
             * notification: a sliver of "BAIHQ." bled into the "Reconnect"
             * pill). Ellipsize to a single line instead of wrapping. */
            char body_buf[DC_NOTIF_BODY];
            dc_shape_ellipsize(t->render, n->body, tw, body_buf, sizeof(body_buf));
            dc_shape_draw_text(t->render, tx, y + 51.0f, body_buf, NULL);
        } else {
            nvgTextLineHeight(vg, 1.1f);
            dc_shape_draw_textbox(t->render, tx, y + 51.0f, tw, n->body, NULL);
        }
        nvgRestore(vg);
    }

    hit->action_count = 0;
    if (has_actions) {
        /* Right-aligned pill row, drawn right-to-left (mirrors notifcenter.c's
         * draw_card) so array order still reads left-to-right; buttons past
         * the text column's left edge are silently skipped rather than
         * overlapping the avatar. */
        const float row_y1 = y + h - 6.0f;
        const float row_h = 18.0f;
        const float row_y0 = row_y1 - row_h;
        const float row_min_x = tx;
        float cursor_x1 = x + w - 14.0f;

        nvgFontFaceId(vg, t->render->font_ui);
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

        hit->action_count = n->action_count;
        hit->action_y0 = row_y0;
        hit->action_y1 = row_y1;
        for (int i = n->action_count - 1; i >= 0; i--) {
            const char *label = n->actions[i].label[0] ? n->actions[i].label : "Open";
            float b[4];
            nvgTextBounds(vg, 0, 0, label, NULL, b);
            float aw = b[2] - b[0] + 16.0f;
            if (aw < 44.0f)
                aw = 44.0f;
            float a_x0 = cursor_x1 - aw;
            if (a_x0 < row_min_x || cursor_x1 <= row_min_x) {
                hit->action_x0[i] = hit->action_x1[i] = 0.0f; /* no room -- not drawn/clickable */
                continue;
            }
            nvgFillColor(vg, nvgRGBA(th->primary.r, th->primary.g, th->primary.b, 255));
            nvgText(vg, (a_x0 + cursor_x1) / 2.0f, (row_y0 + row_y1) / 2.0f, label, NULL);
            hit->action_x0[i] = a_x0;
            hit->action_x1[i] = cursor_x1;
            cursor_x1 = a_x0 - 6.0f;
        }
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

    /* Entrance: fade in + slide from the right edge (DMS). */
    float p = dc_anim_progress(&t->anim);
    float ap = p > 1.0f ? 1.0f : p;
    nvgGlobalAlpha(vg, ap);
    nvgTranslate(vg, (1.0f - ap) * 40.0f, 0.0f);

    for (int i = 0; i < count; i++) {
        t->hits[i].id = popups[i]->id;
        draw_card(t, popups[i], 0.0f, (float)(i * (DC_TOAST_CARD_H + DC_TOAST_GAP)), &t->hits[i]);
    }
    nvgEndFrame(vg);

    /* GL context is still current -- drop cached textures for anything no
     * longer Current/History (dismissed/expired-and-acted-on/cleared). */
    dc_notif_image_gc(t->render, t->notifications);

    if (dc_anim_active(&t->anim) && !t->frame_cb) {
        t->frame_cb = wl_surface_frame(t->surface);
        wl_callback_add_listener(t->frame_cb, &toasts_frame_listener, t);
    }
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
    if (t->frame_cb) {
        wl_callback_destroy(t->frame_cb);
        t->frame_cb = NULL;
    }
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
    dc_anim_start(&t->anim, DC_DUR_MEDIUM, DC_EASE_EMPHASIZED_DECEL);

    t->surface = wl_compositor_create_surface(t->wl->compositor);
    if (t->wl->fractional_scale_mgr) {
        t->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            t->wl->fractional_scale_mgr, t->surface);
        wp_fractional_scale_v1_add_listener(t->fractional_scale, &fractional_scale_listener, t);
    }
    if (t->wl->viewporter)
        t->viewport = wp_viewporter_get_viewport(t->wl->viewporter, t->surface);

    /* Toasts are always top-right (DMS notificationPopupPosition=0) — this is
     * intentionally NOT bar-position-aware (docs/13-POPOUTS-SPEC.md sec.0:
     * "toasts stay top-right, unaffected"), since a bottom bar never intrudes
     * on the top-right corner and a top bar's right cluster sits to the left
     * of this margin, not underneath it. Don't wire dc_bar_window_height()
     * in here. */
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
    int idx = (int)(y / (DC_TOAST_CARD_H + DC_TOAST_GAP));
    if (idx < 0 || idx >= t->card_count)
        return true;
    double within = y - idx * (DC_TOAST_CARD_H + DC_TOAST_GAP);
    if (within > DC_TOAST_CARD_H)
        return true;

    const toast_card_hit *hit = &t->hits[idx];
    for (int i = 0; i < hit->action_count; i++) {
        if (hit->action_x1[i] > hit->action_x0[i] && x >= hit->action_x0[i] &&
            x <= hit->action_x1[i] && y >= hit->action_y0 && y <= hit->action_y1) {
            dc_notifications_invoke_action(t->notifications, hit->id, i);
            return true;
        }
    }

    /* Body click (not on an action button): DMS's NotificationPopup invokes
     * the first action if there is one, otherwise just dismisses -- see
     * NotificationPopup.qml's cardHoverArea onClicked (the
     * canExpand/description-toggle branch doesn't apply here, dankc's toast
     * body isn't expandable). */
    if (hit->action_count > 0)
        dc_notifications_invoke_action(t->notifications, hit->id, 0);
    else
        dc_notifications_dismiss(t->notifications, hit->id);
    return true;
}
