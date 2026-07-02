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
struct zwlr_data_control_manager_v1;
struct xkb_context;
struct xkb_keymap;
struct xkb_state;

typedef struct dc_output {
    struct wl_output *wl_output;
    uint32_t registry_name;
    int32_t scale;
    char *model;
    char *name; /* connector name, e.g. "DP-1" — matches niri's output field */
    bool done;
    struct wl_list link; /* dc_wayland.outputs */
} dc_output;

/* Left-click on a bar surface, in surface-local logical coordinates. */
typedef void (*dc_click_cb)(struct wl_surface *surface, double x, double y, void *user_data);

/* A key press: `keysym` is the xkb keysym; `utf8` is its text (may be empty). */
typedef void (*dc_key_cb)(uint32_t keysym, const char *utf8, void *user_data);

typedef struct dc_wayland {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct xdg_wm_base *wm_base;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_mgr;
    struct zwlr_data_control_manager_v1 *data_control_manager;
    struct wl_seat *seat;

    /* Pointer state. */
    struct wl_pointer *pointer;
    struct wl_surface *pointer_surface;
    double pointer_x;
    double pointer_y;
    dc_click_cb click_cb;
    void *click_data;

    /* Keyboard state (xkb). */
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    dc_key_cb key_cb;
    void *key_data;

    struct wl_list outputs; /* dc_output.link */
} dc_wayland;

void dc_wayland_set_click_cb(dc_wayland *wl, dc_click_cb cb, void *user_data);

/* Set the handler for keyboard input (used by the launcher/lock screen). Keys
 * are only delivered while a DankC surface holds keyboard focus. */
void dc_wayland_set_key_cb(dc_wayland *wl, dc_key_cb cb, void *user_data);

/* Connect to $WAYLAND_DISPLAY and bind globals. Returns NULL on failure.
 * Caller owns the result and must call dc_wayland_destroy(). */
dc_wayland *dc_wayland_connect(void);
void dc_wayland_destroy(dc_wayland *wl);

/* Register the Wayland fd with the event loop (flush on prepare, dispatch on
 * readable). Call once after connecting. */
void dc_wayland_integrate(dc_wayland *wl, struct dc_loop *loop);

#endif /* DC_WAYLAND_WL_H */
