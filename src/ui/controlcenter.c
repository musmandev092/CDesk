#include "ui/controlcenter.h"

#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/audio.h"
#include "services/bluez.h"
#include "services/net.h"
#include "theme/theme.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdlib.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_CC_WIDTH 380
#define DC_CC_HEIGHT 420
#define DC_SCALE_BASE 120

struct dc_control_center {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
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

/* A rounded toggle tile: icon + label, primary-filled when active (DMS style). */
static void draw_tile(dc_render *r, float x, float y, float w, float h, int icon, const char *label,
                      bool active)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    dc_color fg = active ? t->primary_text : t->surface_text;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    dc_render_icon(r, icon, x + 16.0f, y + h / 2.0f, 22.0f, fg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(fg));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + 48.0f, y + h / 2.0f, label, NULL);
}

/* A horizontal slider: icon + rounded track + primary fill + handle. */
static void draw_slider(dc_render *r, float x, float cy, float w, int icon, float value)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;

    dc_render_icon(r, icon, x, cy, 20.0f, t->surface_text, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    const float tx = x + 32.0f;
    const float tw = w - 32.0f;
    const float th = 8.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx, cy - th / 2.0f, tw, th, th / 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 90));
    nvgFill(vg);

    float fw = tw * value;
    if (fw < th)
        fw = th;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx, cy - th / 2.0f, fw, th, th / 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgCircle(vg, tx + tw * value, cy, 8.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
}

static void recompute_physical(dc_control_center *cc)
{
    cc->phys_width = (cc->logical_width * cc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    cc->phys_height = (cc->logical_height * cc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void cc_render(dc_control_center *cc)
{
    if (!cc->configured || cc->phys_width <= 0)
        return;

    if (!cc->egl_ready) {
        if (!dc_egl_window_init(&cc->egl_window, cc->egl, cc->surface, cc->phys_width,
                                cc->phys_height))
            return;
        cc->egl_ready = true;
    } else {
        dc_egl_window_resize(&cc->egl_window, cc->phys_width, cc->phys_height);
    }

    if (!dc_egl_make_current(cc->egl, &cc->egl_window))
        return;
    if (!dc_render_ensure(cc->render))
        return;

    if (cc->viewport)
        wp_viewport_set_destination(cc->viewport, cc->logical_width, cc->logical_height);

    NVGcontext *vg = cc->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = cc->logical_width;
    const float h = cc->logical_height;
    const float pad = 6.0f; /* room for the drop shadow */

    glViewport(0, 0, cc->phys_width, cc->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); /* transparent -> rounded card over wallpaper */
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)cc->scale120 / DC_SCALE_BASE);

    /* Soft drop shadow. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 12.0f, 18.0f,
                                     nvgRGBA(0, 0, 0, 90), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card. */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g, t->surface_container.b,
                             255));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* Title (tiles/sliders come next). */
    nvgFontFaceId(vg, cc->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgFillColor(vg, nvgRGBA(t->surface_text.r, t->surface_text.g, t->surface_text.b, 255));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, pad + 18.0f, pad + 28.0f, "Control Center", NULL);

    /* Live state for the tiles + sliders. */
    dc_audio_info audio;
    bool have_audio = dc_audio_read(&audio);
    dc_net_info net;
    dc_net_wifi(&net);
    dc_bluez_info bt;
    bool have_bt = dc_bluez_read(&bt);

    const float ix = pad + 18.0f;
    const float iw = w - 2.0f * (pad + 18.0f);
    const float gap = 12.0f;
    const float tile_w = (iw - gap) / 2.0f;
    const float tile_h = 60.0f;
    float ty = pad + 52.0f;

    draw_tile(cc->render, ix, ty, tile_w, tile_h, DC_ICON_WIFI, "Wi-Fi", net.connected);
    draw_tile(cc->render, ix + tile_w + gap, ty, tile_w, tile_h, DC_ICON_BLUETOOTH, "Bluetooth",
              have_bt && bt.connected);
    ty += tile_h + gap;
    draw_tile(cc->render, ix, ty, tile_w, tile_h, DC_ICON_DARK_MODE, "Dark", true);
    draw_tile(cc->render, ix + tile_w + gap, ty, tile_w, tile_h, DC_ICON_LIGHT_MODE, "Night", false);
    ty += tile_h + 24.0f;

    draw_slider(cc->render, ix, ty, iw, DC_ICON_VOLUME_UP,
                have_audio ? audio.volume / 100.0f : 0.5f);
    ty += 36.0f;
    draw_slider(cc->render, ix, ty, iw, DC_ICON_LIGHT_MODE, 0.7f); /* TODO: real brightness */

    nvgEndFrame(vg);
    dc_egl_swap(cc->egl, &cc->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_control_center *cc = data;
    DC_UNUSED(fs);
    cc->scale120 = (int)scale;
    recompute_physical(cc);
    cc_render(cc);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_control_center *cc = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    cc->logical_width = width > 0 ? (int)width : DC_CC_WIDTH;
    cc->logical_height = height > 0 ? (int)height : DC_CC_HEIGHT;
    cc->configured = true;
    recompute_physical(cc);
    cc_render(cc);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_control_center *cc = data;
    DC_UNUSED(surface);
    cc->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_control_center *dc_control_center_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_control_center *cc = calloc(1, sizeof(*cc));
    cc->wl = wl;
    cc->egl = egl;
    cc->render = render;
    cc->logical_width = DC_CC_WIDTH;
    cc->logical_height = DC_CC_HEIGHT;
    cc->scale120 = DC_SCALE_BASE;
    return cc;
}

static void cc_show(dc_control_center *cc, dc_output *output)
{
    cc->output = output;
    cc->configured = false;
    cc->egl_ready = false;
    cc->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;

    cc->surface = wl_compositor_create_surface(cc->wl->compositor);
    if (cc->wl->fractional_scale_mgr) {
        cc->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            cc->wl->fractional_scale_mgr, cc->surface);
        wp_fractional_scale_v1_add_listener(cc->fractional_scale, &fractional_scale_listener, cc);
    }
    if (cc->wl->viewporter)
        cc->viewport = wp_viewporter_get_viewport(cc->wl->viewporter, cc->surface);

    cc->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        cc->wl->layer_shell, cc->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:control-center");
    zwlr_layer_surface_v1_set_anchor(cc->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(cc->layer_surface, DC_CC_WIDTH, DC_CC_HEIGHT);
    zwlr_layer_surface_v1_set_margin(cc->layer_surface, 54, 8, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(cc->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(cc->layer_surface, &layer_surface_listener, cc);

    wl_surface_commit(cc->surface);
    cc->visible = true;
    dc_debug("control center shown");
}

static void cc_hide(dc_control_center *cc)
{
    if (cc->egl_ready)
        dc_egl_window_finish(&cc->egl_window, cc->egl);
    if (cc->viewport)
        wp_viewport_destroy(cc->viewport);
    if (cc->fractional_scale)
        wp_fractional_scale_v1_destroy(cc->fractional_scale);
    if (cc->layer_surface)
        zwlr_layer_surface_v1_destroy(cc->layer_surface);
    if (cc->surface)
        wl_surface_destroy(cc->surface);
    cc->egl_ready = false;
    cc->configured = false;
    cc->viewport = NULL;
    cc->fractional_scale = NULL;
    cc->layer_surface = NULL;
    cc->surface = NULL;
    cc->visible = false;
    dc_debug("control center hidden");
}

void dc_control_center_toggle(dc_control_center *cc, dc_output *output)
{
    if (cc->visible)
        cc_hide(cc);
    else
        cc_show(cc, output);
}

bool dc_control_center_visible(dc_control_center *cc)
{
    return cc->visible;
}

void dc_control_center_destroy(dc_control_center *cc)
{
    if (!cc)
        return;
    if (cc->visible)
        cc_hide(cc);
    free(cc);
}
