#include "ui/bar/bar.h"

#include "core/log.h"
#include "dc.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdlib.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Reference bar height in logical pixels (docs/10-DESIGN-SYSTEM.md). */
#define DC_BAR_HEIGHT 48

/* Stock "purple" surfaceContainer #211f24 — the themed bar background until the
 * Material color engine lands (Milestone 2). */
#define DC_BAR_BG_R (0x21 / 255.0f)
#define DC_BAR_BG_G (0x1f / 255.0f)
#define DC_BAR_BG_B (0x24 / 255.0f)

struct dc_bar {
    dc_wayland *wl;
    dc_output *output;
    dc_egl *egl;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    dc_egl_window egl_window;

    int width;
    int height;
    bool egl_ready;
};

static void bar_render(dc_bar *bar)
{
    if (!bar->egl_ready) {
        if (!dc_egl_window_init(&bar->egl_window, bar->egl, bar->surface, bar->width, bar->height))
            return;
        bar->egl_ready = true;
    } else {
        dc_egl_window_resize(&bar->egl_window, bar->width, bar->height);
    }

    if (!dc_egl_make_current(bar->egl, &bar->egl_window))
        return;

    glViewport(0, 0, bar->width, bar->height);
    glClearColor(DC_BAR_BG_R, DC_BAR_BG_G, DC_BAR_BG_B, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    dc_egl_swap(bar->egl, &bar->egl_window);
}

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_bar *bar = data;

    /* Protocol: ack before attaching a buffer. */
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    bar->width = width > 0 ? (int)width : bar->width;
    bar->height = height > 0 ? (int)height : DC_BAR_HEIGHT;
    dc_debug("bar configured: %dx%d on %s", bar->width, bar->height,
             bar->output->model ? bar->output->model : "?");
    bar_render(bar);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_bar *bar = data;
    DC_UNUSED(surface);
    dc_debug("bar surface closed by compositor");
    bar->egl_ready = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_bar *dc_bar_create(dc_wayland *wl, dc_output *output, dc_egl *egl)
{
    dc_bar *bar = calloc(1, sizeof(*bar));
    bar->wl = wl;
    bar->output = output;
    bar->egl = egl;
    bar->height = DC_BAR_HEIGHT;

    bar->surface = wl_compositor_create_surface(wl->compositor);
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
    if (bar->layer_surface)
        zwlr_layer_surface_v1_destroy(bar->layer_surface);
    if (bar->surface)
        wl_surface_destroy(bar->surface);
    free(bar);
}
