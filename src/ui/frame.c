#include "ui/frame.h"

#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/nvg.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdlib.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_SCALE_BASE 120

struct dc_frame {
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

    bool visible;   /* layer surface currently mapped */
    bool configured;
    bool egl_ready;

    /* Last-painted params, so dc_frame_reconfigure() can skip a repaint when
     * nothing actually changed (called on every config-changed notification,
     * most of which have nothing to do with the frame). */
    bool painted;
    float painted_radius;
    int painted_w, painted_h;
};

static void frame_hide(dc_frame *f);
static void frame_show(dc_frame *f);
static void frame_render(dc_frame *f);

static void recompute_physical(dc_frame *f)
{
    f->phys_width = (f->logical_width * f->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    f->phys_height = (f->logical_height * f->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Paint one radius x radius corner "bite": an opaque square with a rounded
 * hole cut where the visible content should show through, so the screen edge
 * reads as a rounded rect of the given radius. (ox, oy) is the square's
 * outer corner (0 or w/h); (dx, dy) is the same corner's sign (+1/-1) used to
 * place the hole's center `radius` px inward on each axis. */
static void draw_corner(NVGcontext *vg, float ox, float oy, float dx, float dy, float radius)
{
    float sx = (dx > 0.0f) ? ox : ox - radius;
    float sy = (dy > 0.0f) ? oy : oy - radius;
    float hole_cx = sx + ((dx > 0.0f) ? radius : 0.0f);
    float hole_cy = sy + ((dy > 0.0f) ? radius : 0.0f);

    nvgBeginPath(vg);
    nvgRect(vg, sx, sy, radius, radius);
    nvgCircle(vg, hole_cx, hole_cy, radius);
    nvgPathWinding(vg, NVG_HOLE);
    /* Opaque black: theme-independent, matching a simple screen-corner mask
     * (the "black (or theme frame color)" option in docs/POLISH.md P2). */
    nvgFillColor(vg, nvgRGBA(0, 0, 0, 255));
    nvgFill(vg);
}

static void frame_paint(dc_frame *f)
{
    NVGcontext *vg = f->render->vg;
    float w = (float)f->logical_width;
    float h = (float)f->logical_height;
    float r = dc_config_current->frame_radius;
    if (r > w / 2.0f)
        r = w / 2.0f;
    if (r > h / 2.0f)
        r = h / 2.0f;
    if (r < 0.0f)
        r = 0.0f;

    glViewport(0, 0, f->phys_width, f->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)f->scale120 / DC_SCALE_BASE);
    if (r > 0.5f) {
        draw_corner(vg, 0.0f, 0.0f, 1.0f, 1.0f, r);   /* top-left */
        draw_corner(vg, w, 0.0f, -1.0f, 1.0f, r);     /* top-right */
        draw_corner(vg, 0.0f, h, 1.0f, -1.0f, r);     /* bottom-left */
        draw_corner(vg, w, h, -1.0f, -1.0f, r);       /* bottom-right */
    }
    nvgEndFrame(vg);

    dc_egl_swap(f->egl, &f->egl_window);
    wl_surface_commit(f->surface);

    f->painted = true;
    f->painted_radius = r;
    f->painted_w = f->logical_width;
    f->painted_h = f->logical_height;
}

static void frame_render(dc_frame *f)
{
    if (!f->configured || f->phys_width <= 0 || f->phys_height <= 0)
        return;
    if (!f->egl_ready) {
        if (!dc_egl_window_init(&f->egl_window, f->egl, f->surface, f->phys_width, f->phys_height))
            return;
        f->egl_ready = true;
    } else {
        dc_egl_window_resize(&f->egl_window, f->phys_width, f->phys_height);
    }
    if (!dc_egl_make_current(f->egl, &f->egl_window))
        return;
    if (!dc_render_ensure(f->render))
        return;
    if (f->viewport)
        wp_viewport_set_destination(f->viewport, f->logical_width, f->logical_height);

    frame_paint(f);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_frame *f = data;
    DC_UNUSED(fs);
    f->scale120 = (int)scale;
    recompute_physical(f);
    frame_render(f);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_frame *f = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    /* Anchored to all four edges with no explicit size -> the compositor
     * reports the full output's logical size here. */
    f->logical_width = width > 0 ? (int)width : f->logical_width;
    f->logical_height = height > 0 ? (int)height : f->logical_height;
    f->configured = true;
    recompute_physical(f);
    frame_render(f);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_frame *f = data;
    DC_UNUSED(surface);
    f->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static void frame_hide(dc_frame *f)
{
    if (!f->visible)
        return;
    if (f->egl_ready)
        dc_egl_window_finish(&f->egl_window, f->egl);
    if (f->viewport)
        wp_viewport_destroy(f->viewport);
    if (f->fractional_scale)
        wp_fractional_scale_v1_destroy(f->fractional_scale);
    if (f->layer_surface)
        zwlr_layer_surface_v1_destroy(f->layer_surface);
    if (f->surface)
        wl_surface_destroy(f->surface);
    f->egl_ready = false;
    f->configured = false;
    f->viewport = NULL;
    f->fractional_scale = NULL;
    f->layer_surface = NULL;
    f->surface = NULL;
    f->visible = false;
    f->painted = false;
}

static void frame_show(dc_frame *f)
{
    if (f->visible)
        return;
    dc_output *output = f->output;
    f->configured = false;
    f->egl_ready = false;
    f->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    f->logical_width = 0;
    f->logical_height = 0;

    f->surface = wl_compositor_create_surface(f->wl->compositor);

    /* Click-through: an empty input region (no wl_region_add calls at all)
     * means the surface accepts no pointer/touch input whatsoever, so every
     * click passes straight through to whatever is beneath it (docs/POLISH.md
     * P2; same technique toasts.c uses for its inter-card gaps, just with a
     * region that covers nothing instead of just the cards). */
    struct wl_region *empty = wl_compositor_create_region(f->wl->compositor);
    wl_surface_set_input_region(f->surface, empty);
    wl_region_destroy(empty);

    if (f->wl->fractional_scale_mgr) {
        f->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            f->wl->fractional_scale_mgr, f->surface);
        wp_fractional_scale_v1_add_listener(f->fractional_scale, &fractional_scale_listener, f);
    }
    if (f->wl->viewporter)
        f->viewport = wp_viewporter_get_viewport(f->wl->viewporter, f->surface);

    /* Top layer, anchored to all four edges, zero exclusive zone (docs/
     * 11-UX-FLOW.md sec.1 z-order table: "Frame ... Top ... exclusive zone —").
     * No explicit size -> fills the anchored (whole-output) area. */
    f->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        f->wl->layer_shell, f->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "dankc:frame");
    zwlr_layer_surface_v1_set_anchor(f->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(f->layer_surface, 0);
    zwlr_layer_surface_v1_set_size(f->layer_surface, 0, 0);
    zwlr_layer_surface_v1_add_listener(f->layer_surface, &layer_surface_listener, f);

    wl_surface_commit(f->surface);
    f->visible = true;
}

dc_frame *dc_frame_create(dc_wayland *wl, dc_output *output, dc_egl *egl, dc_render *render)
{
    dc_frame *f = calloc(1, sizeof(*f));
    f->wl = wl;
    f->egl = egl;
    f->render = render;
    f->output = output;
    f->scale120 = DC_SCALE_BASE;
    if (dc_config_current->frame_enabled)
        frame_show(f);
    return f;
}

void dc_frame_destroy(dc_frame *f)
{
    if (!f)
        return;
    frame_hide(f);
    free(f);
}

void dc_frame_reconfigure(dc_frame *f)
{
    if (!f)
        return;
    const dc_config *cfg = dc_config_current;

    if (!cfg->frame_enabled) {
        frame_hide(f);
        return;
    }
    if (!f->visible) {
        frame_show(f);
        return; /* first paint happens on the layer surface's configure */
    }
    /* Already visible: only repaint if the radius or the logical size
     * (output resize / scale change) actually differs from what's on screen
     * — this is called on every config-changed notification, most of which
     * are unrelated (docs/POLISH.md P2: "no per-frame redraw"). */
    if (f->painted && f->painted_radius == cfg->frame_radius &&
        f->painted_w == f->logical_width && f->painted_h == f->logical_height)
        return;
    frame_render(f);
}
