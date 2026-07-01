#include "wayland/wl.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

/* --- wl_output ---------------------------------------------------------- */

static void output_handle_geometry(void *data, struct wl_output *o, int32_t x, int32_t y,
                                   int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                                   const char *model, int32_t transform)
{
    dc_output *output = data;
    DC_UNUSED(o);
    DC_UNUSED(x);
    DC_UNUSED(y);
    DC_UNUSED(pw);
    DC_UNUSED(ph);
    DC_UNUSED(subpixel);
    DC_UNUSED(make);
    DC_UNUSED(transform);
    free(output->model);
    output->model = model ? strdup(model) : NULL;
}

static void output_handle_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w,
                               int32_t h, int32_t refresh)
{
    DC_UNUSED(data);
    DC_UNUSED(o);
    DC_UNUSED(flags);
    DC_UNUSED(w);
    DC_UNUSED(h);
    DC_UNUSED(refresh);
}

static void output_handle_scale(void *data, struct wl_output *o, int32_t scale)
{
    dc_output *output = data;
    DC_UNUSED(o);
    output->scale = scale;
}

static void output_handle_done(void *data, struct wl_output *o)
{
    dc_output *output = data;
    DC_UNUSED(o);
    output->done = true;
    dc_debug("output ready: %s (scale %d)", output->model ? output->model : "?", output->scale);
}

static void output_handle_name(void *data, struct wl_output *o, const char *name)
{
    dc_output *output = data;
    DC_UNUSED(o);
    free(output->name);
    output->name = name ? strdup(name) : NULL;
}

static void output_handle_description(void *data, struct wl_output *o, const char *desc)
{
    DC_UNUSED(data);
    DC_UNUSED(o);
    DC_UNUSED(desc);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .scale = output_handle_scale,
    .done = output_handle_done,
    .name = output_handle_name,
    .description = output_handle_description,
};

/* --- xdg_wm_base -------------------------------------------------------- */

static void wm_base_handle_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    DC_UNUSED(data);
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_handle_ping,
};

/* --- registry ----------------------------------------------------------- */

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
                                   const char *interface, uint32_t version)
{
    dc_wayland *wl = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl->compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, DC_MIN(version, 4u));
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl->layer_shell =
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, DC_MIN(version, 4u));
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        wl->wm_base =
            wl_registry_bind(registry, name, &xdg_wm_base_interface, DC_MIN(version, 3u));
        xdg_wm_base_add_listener(wl->wm_base, &wm_base_listener, wl);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        wl->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        wl->fractional_scale_mgr =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, DC_MIN(version, 7u));
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        dc_output *output = calloc(1, sizeof(*output));
        output->registry_name = name;
        output->scale = 1;
        output->wl_output =
            wl_registry_bind(registry, name, &wl_output_interface, DC_MIN(version, 4u));
        wl_output_add_listener(output->wl_output, &output_listener, output);
        wl_list_insert(&wl->outputs, &output->link);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    dc_wayland *wl = data;
    DC_UNUSED(registry);

    dc_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &wl->outputs, link) {
        if (output->registry_name != name)
            continue;
        dc_debug("output removed: %s", output->model ? output->model : "?");
        wl_list_remove(&output->link);
        wl_output_destroy(output->wl_output);
        free(output->model);
        free(output->name);
        free(output);
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* --- lifecycle ---------------------------------------------------------- */

dc_wayland *dc_wayland_connect(void)
{
    dc_wayland *wl = calloc(1, sizeof(*wl));
    wl_list_init(&wl->outputs);

    wl->display = wl_display_connect(NULL);
    if (!wl->display) {
        dc_error("wl_display_connect failed (is WAYLAND_DISPLAY set?)");
        free(wl);
        return NULL;
    }

    wl->registry = wl_display_get_registry(wl->display);
    wl_registry_add_listener(wl->registry, &registry_listener, wl);

    /* First roundtrip: receive globals. Second: receive per-output metadata. */
    wl_display_roundtrip(wl->display);
    wl_display_roundtrip(wl->display);

    if (!wl->compositor) {
        dc_error("compositor does not expose wl_compositor");
        dc_wayland_destroy(wl);
        return NULL;
    }
    if (!wl->layer_shell) {
        dc_error("compositor does not expose wlr-layer-shell (required)");
        dc_wayland_destroy(wl);
        return NULL;
    }

    return wl;
}

void dc_wayland_destroy(dc_wayland *wl)
{
    if (!wl)
        return;

    dc_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &wl->outputs, link) {
        wl_list_remove(&output->link);
        wl_output_destroy(output->wl_output);
        free(output->model);
        free(output->name);
        free(output);
    }

    if (wl->registry)
        wl_registry_destroy(wl->registry);
    if (wl->display)
        wl_display_disconnect(wl->display);
    free(wl);
}

/* --- event-loop integration --------------------------------------------- */

static void wayland_prepare(void *data)
{
    dc_wayland *wl = data;
    wl_display_dispatch_pending(wl->display);
    wl_display_flush(wl->display);
}

static void wayland_readable(int fd, uint32_t revents, void *data)
{
    dc_wayland *wl = data;
    DC_UNUSED(fd);
    DC_UNUSED(revents);
    if (wl_display_dispatch(wl->display) < 0)
        dc_error("wl_display_dispatch failed; compositor gone?");
}

void dc_wayland_integrate(dc_wayland *wl, struct dc_loop *loop)
{
    int fd = wl_display_get_fd(wl->display);
    dc_loop_add_fd(loop, fd, POLLIN, wayland_readable, wl);
    dc_loop_set_prepare(loop, wayland_prepare, wl);
}
