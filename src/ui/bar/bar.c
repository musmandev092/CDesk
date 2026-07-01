#include "ui/bar/bar.h"

#include "core/log.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/nvg.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Reference bar height in logical pixels (docs/10-DESIGN-SYSTEM.md). */
#define DC_BAR_HEIGHT 48

/* Fractional-scale numerator base: preferred_scale is reported over 120. */
#define DC_SCALE_BASE 120

/* Stock "purple" palette until the Material color engine lands:
 * surfaceContainer #211f24 background, surfaceText #e6e0e9 foreground. */
#define DC_BAR_BG_R (0x21 / 255.0f)
#define DC_BAR_BG_G (0x1f / 255.0f)
#define DC_BAR_BG_B (0x24 / 255.0f)

struct dc_bar {
    dc_wayland *wl;
    dc_output *output;
    dc_egl *egl;
    dc_render *render;
    dc_niri *niri;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width;  /* from the layer-surface configure */
    int logical_height;
    int scale120; /* fractional scale numerator (120 == 1.0x) */
    int phys_width; /* buffer size = logical * scale120 / 120 */
    int phys_height;

    bool configured;
    bool egl_ready;
};

static void recompute_physical(dc_bar *bar)
{
    bar->phys_width = (bar->logical_width * bar->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    bar->phys_height = (bar->logical_height * bar->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Workspace pills, left-aligned, filtered to this bar's output. Logical coords. */
static void draw_workspaces(dc_bar *bar)
{
    if (!bar->niri)
        return;

    int count = 0;
    const dc_niri_workspace *workspaces = dc_niri_workspaces(bar->niri, &count);
    if (!workspaces)
        return;

    NVGcontext *vg = bar->render->vg;
    const float pad = 12.0f;   /* spacingM */
    const float gap = 6.0f;
    const float pill = 28.0f;
    const float cy = bar->logical_height / 2.0f;
    float x = pad;

    for (int i = 0; i < count; i++) {
        const dc_niri_workspace *ws = &workspaces[i];
        if (bar->output->name && ws->output[0] && strcmp(ws->output, bar->output->name) != 0)
            continue;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, cy - pill / 2.0f, pill, pill, 8.0f);
        if (ws->is_urgent)
            nvgFillColor(vg, nvgRGB(0xf2, 0xb8, 0xb5)); /* error */
        else if (ws->is_focused)
            nvgFillColor(vg, nvgRGB(0xd0, 0xbc, 0xff)); /* primary */
        else
            nvgFillColor(vg, nvgRGB(0x2b, 0x29, 0x2f)); /* surfaceContainerHigh */
        nvgFill(vg);

        char label[8];
        snprintf(label, sizeof(label), "%u", ws->idx);
        nvgFontFaceId(vg, bar->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgFillColor(vg, ws->is_focused || ws->is_urgent ? nvgRGB(0x38, 0x1e, 0x72)
                                                          : nvgRGB(0xe6, 0xe0, 0xe9));
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, x + pill / 2.0f, cy, label, NULL);

        x += pill + gap;
    }
}

static void draw_clock(dc_bar *bar)
{
    NVGcontext *vg = bar->render->vg;

    char text[16];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(text, sizeof(text), "%H:%M", &tm);

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, nvgRGB(0xe6, 0xe0, 0xe9));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, bar->logical_width / 2.0f, bar->logical_height / 2.0f, text, NULL);
}

void dc_bar_render(dc_bar *bar)
{
    if (!bar->configured || bar->phys_width <= 0)
        return;

    if (!bar->egl_ready) {
        if (!dc_egl_window_init(&bar->egl_window, bar->egl, bar->surface, bar->phys_width,
                                bar->phys_height))
            return;
        bar->egl_ready = true;
    } else {
        dc_egl_window_resize(&bar->egl_window, bar->phys_width, bar->phys_height);
    }

    if (!dc_egl_make_current(bar->egl, &bar->egl_window))
        return;
    if (!dc_render_ensure(bar->render))
        return;

    /* Map the large (physical) buffer down to the logical surface size. */
    if (bar->viewport)
        wp_viewport_set_destination(bar->viewport, bar->logical_width, bar->logical_height);

    glViewport(0, 0, bar->phys_width, bar->phys_height);
    glClearColor(DC_BAR_BG_R, DC_BAR_BG_G, DC_BAR_BG_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    float pixel_ratio = (float)bar->scale120 / DC_SCALE_BASE;
    nvgBeginFrame(bar->render->vg, bar->logical_width, bar->logical_height, pixel_ratio);
    draw_workspaces(bar);
    draw_clock(bar);
    nvgEndFrame(bar->render->vg);

    dc_egl_swap(bar->egl, &bar->egl_window);
}

/* --- fractional scale --------------------------------------------------- */

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_bar *bar = data;
    DC_UNUSED(fs);
    if ((int)scale == bar->scale120)
        return;
    bar->scale120 = (int)scale;
    dc_debug("bar scale on %s: %.3gx", bar->output->name ? bar->output->name : "?",
             (double)scale / DC_SCALE_BASE);
    recompute_physical(bar);
    dc_bar_render(bar);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

/* --- layer surface ------------------------------------------------------ */

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_bar *bar = data;

    /* Protocol: ack before attaching a buffer. */
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    bar->logical_width = width > 0 ? (int)width : bar->logical_width;
    bar->logical_height = height > 0 ? (int)height : DC_BAR_HEIGHT;
    bar->configured = true;
    recompute_physical(bar);
    dc_debug("bar configured: %dx%d logical (buffer %dx%d) on %s", bar->logical_width,
             bar->logical_height, bar->phys_width, bar->phys_height,
             bar->output->model ? bar->output->model : "?");
    dc_bar_render(bar);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_bar *bar = data;
    DC_UNUSED(surface);
    dc_debug("bar surface closed by compositor");
    bar->configured = false;
    bar->egl_ready = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_bar *dc_bar_create(dc_wayland *wl, dc_output *output, dc_egl *egl, dc_render *render,
                      dc_niri *niri)
{
    dc_bar *bar = calloc(1, sizeof(*bar));
    bar->wl = wl;
    bar->output = output;
    bar->egl = egl;
    bar->render = render;
    bar->niri = niri;
    bar->logical_height = DC_BAR_HEIGHT;
    bar->scale120 = (output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;

    bar->surface = wl_compositor_create_surface(wl->compositor);

    if (wl->fractional_scale_mgr) {
        bar->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale_mgr,
                                                                bar->surface);
        wp_fractional_scale_v1_add_listener(bar->fractional_scale, &fractional_scale_listener, bar);
    }
    if (wl->viewporter)
        bar->viewport = wp_viewporter_get_viewport(wl->viewporter, bar->surface);

    bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wl->layer_shell, bar->surface, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "dankc:bar");

    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, DC_BAR_HEIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, DC_BAR_HEIGHT);
    zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

    /* Commit with no buffer to elicit the first configure. */
    wl_surface_commit(bar->surface);

    dc_info("bar created on output %s", output->model ? output->model : "?");
    return bar;
}

void dc_bar_destroy(dc_bar *bar)
{
    if (!bar)
        return;
    if (bar->egl_ready)
        dc_egl_window_finish(&bar->egl_window, bar->egl);
    if (bar->viewport)
        wp_viewport_destroy(bar->viewport);
    if (bar->fractional_scale)
        wp_fractional_scale_v1_destroy(bar->fractional_scale);
    if (bar->layer_surface)
        zwlr_layer_surface_v1_destroy(bar->layer_surface);
    if (bar->surface)
        wl_surface_destroy(bar->surface);
    free(bar);
}
