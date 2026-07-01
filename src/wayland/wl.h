/* wl.h — Wayland display connection and interned globals.
 *
 * Owns the wl_display, the registry, and the compositor-provided globals DankC
 * binds once at startup. Outputs are tracked in a list and hot-plug aware.
 */
#ifndef DC_WAYLAND_WL_H
#define DC_WAYLAND_WL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct dc_loop;
struct zwlr_layer_shell_v1;
struct xdg_wm_base;
struct wp_viewporter;
struct wp_fractional_scale_manager_v1;

typedef struct dc_output {
    struct wl_output *wl_output;
    uint32_t registry_name;
    int32_t scale;
    char *model;
    char *name; /* connector name, e.g. "DP-1" — matches niri's output field */
    bool done;
    struct wl_list link; /* dc_wayland.outputs */
} dc_output;

typedef struct dc_wayland {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct xdg_wm_base *wm_base;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_mgr;
    struct wl_seat *seat;

    struct wl_list outputs; /* dc_output.link */
} dc_wayland;

/* Connect to $WAYLAND_DISPLAY and bind globals. Returns NULL on failure.
 * Caller owns the result and must call dc_wayland_destroy(). */
dc_wayland *dc_wayland_connect(void);
void dc_wayland_destroy(dc_wayland *wl);

/* Register the Wayland fd with the event loop (flush on prepare, dispatch on
 * readable). Call once after connecting. */
void dc_wayland_integrate(dc_wayland *wl, struct dc_loop *loop);

#endif /* DC_WAYLAND_WL_H */
