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
struct ext_session_lock_manager_v1;
struct wp_cursor_shape_manager_v1;
struct wp_cursor_shape_device_v1;
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

/* A button click on a bar surface, in surface-local logical coordinates.
 * `button` is a raw evdev code (BTN_LEFT/BTN_RIGHT/BTN_MIDDLE from
 * <linux/input-event-codes.h>) — added for tray right-click context menus/
 * middle-click SecondaryActivate (docs/POLISH.md P4); every pre-existing
 * caller only ever cared about BTN_LEFT and can keep ignoring the rest. */
typedef void (*dc_click_cb)(struct wl_surface *surface, double x, double y, uint32_t button,
                            void *user_data);

/* A key press: `keysym` is the xkb keysym; `utf8` is its text (may be empty). */
typedef void (*dc_key_cb)(uint32_t keysym, const char *utf8, void *user_data);

/* Pointer motion (and entry) over `surface`, in surface-local logical
 * coordinates. Fired for every motion event — callers that only care about
 * hover-region changes must debounce themselves (docs/12-BAR-SPEC.md sec.5). */
typedef void (*dc_motion_cb)(struct wl_surface *surface, double x, double y, void *user_data);

/* Pointer left `surface` entirely (no coordinates — there is nowhere "on
 * surface" for them to refer to). */
typedef void (*dc_leave_cb)(struct wl_surface *surface, void *user_data);

/* Left button released over `surface` (surface-local logical coordinates,
 * same convention as dc_click_cb). Fired alongside dc_click_cb's press —
 * together they bracket a button-held-motion drag gesture (e.g. a popout
 * slider): press starts it, motion_cb keeps firing while held, this ends
 * it. */
typedef void (*dc_release_cb)(struct wl_surface *surface, double x, double y, void *user_data);

/* One "step" of scroll on `surface`: `steps_v`/`steps_h` are signed step
 * counts (positive = scroll down / right), already debounced from raw axis
 * deltas — see dc_wayland_set_axis_cb(). Usually +-1, but may be larger for a
 * single fast flick. */
typedef void (*dc_axis_cb)(struct wl_surface *surface, int steps_v, int steps_h, void *user_data);

/* Cursor shapes DankC actually uses (docs/12-BAR-SPEC.md sec.5). */
typedef enum {
    DC_CURSOR_DEFAULT,
    DC_CURSOR_POINTER,
} dc_cursor_shape;

typedef struct dc_wayland {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct xdg_wm_base *wm_base;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_mgr;
    struct zwlr_data_control_manager_v1 *data_control_manager;
    struct ext_session_lock_manager_v1 *session_lock_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wl_seat *seat;

    /* Pointer state. */
    struct wl_pointer *pointer;
    struct wp_cursor_shape_device_v1 *cursor_shape_device; /* NULL if unavailable */
    uint32_t pointer_enter_serial; /* latest wl_pointer.enter serial (cursor-shape set_shape) */
    struct wl_surface *pointer_surface;
    double pointer_x;
    double pointer_y;
    bool button_down; /* left button currently held (button-held-motion drags) */
    dc_click_cb click_cb;
    void *click_data;
    dc_motion_cb motion_cb;
    void *motion_data;
    dc_leave_cb leave_cb;
    void *leave_data;
    dc_release_cb release_cb;
    void *release_data;

    /* Scroll-axis debouncing state (docs/12-BAR-SPEC.md sec.5): continuous
     * (touchpad) sources accumulate in wl_fixed units across frames until a
     * DC_AXIS_STEP_THRESHOLD-sized chunk is consumed; discrete (wheel)
     * sources report whole steps directly, reset every frame. */
    double axis_accum_v;
    double axis_accum_h;
    int axis_discrete_v;
    int axis_discrete_h;
    bool axis_has_discrete;
    dc_axis_cb axis_cb;
    void *axis_data;

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

/* Set the handler for pointer motion (hover tracking; docs/12-BAR-SPEC.md
 * sec.5). Fired on every wl_pointer.motion and wl_pointer.enter. */
void dc_wayland_set_motion_cb(dc_wayland *wl, dc_motion_cb cb, void *user_data);

/* Set the handler for pointer leaving a surface (clears hover state). */
void dc_wayland_set_leave_cb(dc_wayland *wl, dc_leave_cb cb, void *user_data);

/* Set the handler for a left-button release (ends a button-held-motion
 * drag started by dc_click_cb's press; see dc_release_cb). */
void dc_wayland_set_release_cb(dc_wayland *wl, dc_release_cb cb, void *user_data);

/* Set the handler for debounced scroll-wheel steps (docs/12-BAR-SPEC.md
 * sec.5: scrollYBehavior/scrollXBehavior). */
void dc_wayland_set_axis_cb(dc_wayland *wl, dc_axis_cb cb, void *user_data);

/* Set the pointer's cursor image via wp_cursor_shape_manager_v1, if the
 * compositor offers it (niri does). No-op (cursor stays whatever it was) if
 * the protocol or a pointer device is unavailable. */
void dc_wayland_set_cursor(dc_wayland *wl, dc_cursor_shape shape);

/* Connect to $WAYLAND_DISPLAY and bind globals. Returns NULL on failure.
 * Caller owns the result and must call dc_wayland_destroy(). */
dc_wayland *dc_wayland_connect(void);
void dc_wayland_destroy(dc_wayland *wl);

/* Register the Wayland fd with the event loop (flush on prepare, dispatch on
 * readable). Call once after connecting. */
void dc_wayland_integrate(dc_wayland *wl, struct dc_loop *loop);

#endif /* DC_WAYLAND_WL_H */
