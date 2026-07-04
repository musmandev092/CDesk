#include "wayland/wl.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-names.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-data-control-unstable-v1-client-protocol.h"
#include "ext-session-lock-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

/* Continuous (touchpad) scroll sources report many tiny wl_fixed deltas per
 * gesture; a wheel "click" is conventionally 10 wl_fixed units (matching
 * libinput/most compositors), so that's also the debounce threshold for
 * turning accumulated continuous motion into discrete steps
 * (docs/12-BAR-SPEC.md sec.5). */
#define DC_AXIS_STEP_THRESHOLD 10.0

/* Fallback key-repeat rate/delay (docs/22-NOTEPAD-PLAN.md sec.2.6) for the
 * rare compositor that sends a zero rate/delay in wl_keyboard.repeat_info
 * instead of just omitting the event. */
#define DC_KEY_REPEAT_RATE_HZ 25
#define DC_KEY_REPEAT_DELAY_MS 600

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

/* --- pointer / seat ----------------------------------------------------- */

static void pointer_handle_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                                 struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    wl->pointer_surface = surface;
    wl->pointer_enter_serial = serial;
    wl->pointer_x = wl_fixed_to_double(sx);
    wl->pointer_y = wl_fixed_to_double(sy);
    /* Scroll-axis accumulation is pointer-global, not per-surface; drop any
     * sub-threshold carry from whatever was under the pointer before so a
     * scroll gesture that crosses a surface boundary mid-flight can't
     * misattribute a step to the surface just entered (docs/12-BAR-SPEC.md
     * sec.5). */
    wl->axis_accum_v = 0.0;
    wl->axis_accum_h = 0.0;
    wl->axis_discrete_v = 0;
    wl->axis_discrete_h = 0;
    wl->axis_has_discrete = false;
    if (wl->motion_cb)
        wl->motion_cb(surface, wl->pointer_x, wl->pointer_y, wl->motion_data);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                                 struct wl_surface *surface)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    DC_UNUSED(serial);
    if (wl->pointer_surface == surface)
        wl->pointer_surface = NULL;
    if (wl->leave_cb)
        wl->leave_cb(surface, wl->leave_data);
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer, uint32_t time,
                                  wl_fixed_t sx, wl_fixed_t sy)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    DC_UNUSED(time);
    wl->pointer_x = wl_fixed_to_double(sx);
    wl->pointer_y = wl_fixed_to_double(sy);
    if (wl->motion_cb && wl->pointer_surface)
        wl->motion_cb(wl->pointer_surface, wl->pointer_x, wl->pointer_y, wl->motion_data);
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer, uint32_t serial,
                                  uint32_t time, uint32_t button, uint32_t state)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    DC_UNUSED(serial);
    DC_UNUSED(time);
    /* Left/right/middle all reach click_cb now (docs/POLISH.md P4: tray
     * right-click menu, middle-click SecondaryActivate) -- release_cb and
     * button_down (button-held-motion drags, e.g. popout sliders) stay
     * left-only since nothing else uses a right/middle drag gesture. */
    if (button != BTN_LEFT && button != BTN_RIGHT && button != BTN_MIDDLE)
        return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (button == BTN_LEFT)
            wl->button_down = true;
        if (wl->pointer_surface && wl->click_cb)
            wl->click_cb(wl->pointer_surface, wl->pointer_x, wl->pointer_y, button,
                        wl->click_data);
    } else if (button == BTN_LEFT) {
        wl->button_down = false;
        if (wl->pointer_surface && wl->release_cb)
            wl->release_cb(wl->pointer_surface, wl->pointer_x, wl->pointer_y, wl->release_data);
    }
}

/* Continuous-source axis delta: accumulate in wl_fixed units (docs/12-BAR-SPEC.md
 * sec.5). Discrete (wheel) sources also send one of these per notch alongside
 * axis_discrete; when axis_discrete showed up this frame we trust its whole-step
 * count instead (see pointer_handle_frame()), so the accumulation here is only
 * ever consumed for continuous (touchpad) sources. */
static void pointer_handle_axis(void *data, struct wl_pointer *pointer, uint32_t time,
                                uint32_t axis, wl_fixed_t value)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    DC_UNUSED(time);
    double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wl->axis_accum_v += v;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        wl->axis_accum_h += v;
}

/* wl_pointer v5+ groups an input batch's events behind a trailing frame; this
 * is where accumulated axis motion for the batch turns into debounced steps
 * and gets dispatched. libwayland aborts on any NULL listener slot for the
 * bound version, so every event through the seat-bind version (7) needs at
 * least a stub — this one does real work instead. */
static void pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);

    if (wl->pointer_surface && wl->axis_cb) {
        int steps_v = 0, steps_h = 0;
        if (wl->axis_has_discrete) {
            /* Wheel: axis_discrete already reports whole steps this frame.
             * Wheels also send a same-frame continuous axis delta alongside
             * axis_discrete but never send axis_stop, so that companion
             * accumulator must be drained here too — otherwise it silently
             * piles up across every wheel notch and gets misinterpreted as
             * extra continuous-source steps the next time a touchpad scrolls
             * (docs/12-BAR-SPEC.md sec.5). */
            steps_v = wl->axis_discrete_v;
            steps_h = wl->axis_discrete_h;
            wl->axis_accum_v = 0.0;
            wl->axis_accum_h = 0.0;
        } else {
            /* Touchpad/continuous: drain whole DC_AXIS_STEP_THRESHOLD-sized
             * chunks, leaving any remainder to accumulate into future frames. */
            while (wl->axis_accum_v >= DC_AXIS_STEP_THRESHOLD) {
                steps_v++;
                wl->axis_accum_v -= DC_AXIS_STEP_THRESHOLD;
            }
            while (wl->axis_accum_v <= -DC_AXIS_STEP_THRESHOLD) {
                steps_v--;
                wl->axis_accum_v += DC_AXIS_STEP_THRESHOLD;
            }
            while (wl->axis_accum_h >= DC_AXIS_STEP_THRESHOLD) {
                steps_h++;
                wl->axis_accum_h -= DC_AXIS_STEP_THRESHOLD;
            }
            while (wl->axis_accum_h <= -DC_AXIS_STEP_THRESHOLD) {
                steps_h--;
                wl->axis_accum_h += DC_AXIS_STEP_THRESHOLD;
            }
        }
        if (steps_v != 0 || steps_h != 0)
            wl->axis_cb(wl->pointer_surface, steps_v, steps_h, wl->axis_data);
    }

    wl->axis_discrete_v = 0;
    wl->axis_discrete_h = 0;
    wl->axis_has_discrete = false;
}

static void pointer_handle_axis_source(void *data, struct wl_pointer *pointer, uint32_t axis_source)
{
    DC_UNUSED(data);
    DC_UNUSED(pointer);
    DC_UNUSED(axis_source);
}

/* Scroll gesture ended: drop any sub-threshold remainder so a later, unrelated
 * gesture doesn't inherit a stale fractional carry (docs/12-BAR-SPEC.md sec.5). */
static void pointer_handle_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
                                     uint32_t axis)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    DC_UNUSED(time);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wl->axis_accum_v = 0.0;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        wl->axis_accum_h = 0.0;
}

static void pointer_handle_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
                                         int32_t discrete)
{
    dc_wayland *wl = data;
    DC_UNUSED(pointer);
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wl->axis_discrete_v += discrete;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        wl->axis_discrete_h += discrete;
    wl->axis_has_discrete = true;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
};

/* --- keyboard (xkb) ----------------------------------------------------- */

/* Stop any in-flight key repeat: disarm the timerfd and clear the candidate
 * (docs/22-NOTEPAD-PLAN.md sec.2.6). Safe to call when nothing is repeating. */
static void repeat_disarm(dc_wayland *wl)
{
    if (wl->repeat_timerfd >= 0) {
        struct itimerspec none = {0};
        timerfd_settime(wl->repeat_timerfd, 0, &none, NULL);
    }
    wl->repeat_keycode = 0;
}

/* Arm the repeat timerfd for the just-pressed, repeatable `keycode`: fires
 * once after repeat_delay, then automatically every 1000/repeat_rate ms
 * (the kernel handles the periodic re-arm via it_interval) until disarmed. */
static void repeat_arm(dc_wayland *wl, xkb_keycode_t keycode, xkb_keysym_t sym, const char *utf8)
{
    if (wl->repeat_timerfd < 0)
        return;

    wl->repeat_keycode = keycode;
    wl->repeat_keysym = (uint32_t)sym;
    strncpy(wl->repeat_utf8, utf8, sizeof(wl->repeat_utf8) - 1);
    wl->repeat_utf8[sizeof(wl->repeat_utf8) - 1] = '\0';

    int32_t delay = wl->repeat_delay > 0 ? wl->repeat_delay : DC_KEY_REPEAT_DELAY_MS;
    int32_t rate = wl->repeat_rate > 0 ? wl->repeat_rate : DC_KEY_REPEAT_RATE_HZ;
    long interval_ns = 1000000000L / rate;

    struct itimerspec spec = {0};
    spec.it_value.tv_sec = delay / 1000;
    spec.it_value.tv_nsec = (long)(delay % 1000) * 1000000L;
    spec.it_interval.tv_sec = interval_ns / 1000000000L;
    spec.it_interval.tv_nsec = interval_ns % 1000000000L;
    timerfd_settime(wl->repeat_timerfd, 0, &spec, NULL);
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *kbd, uint32_t format, int fd,
                                   uint32_t size)
{
    dc_wayland *wl = data;
    DC_UNUSED(kbd);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(
        wl->xkb_context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!keymap)
        return;

    if (wl->xkb_state)
        xkb_state_unref(wl->xkb_state);
    if (wl->xkb_keymap)
        xkb_keymap_unref(wl->xkb_keymap);
    wl->xkb_keymap = keymap;
    wl->xkb_state = xkb_state_new(keymap);
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *kbd, uint32_t serial,
                                  struct wl_surface *surface, struct wl_array *keys)
{
    DC_UNUSED(data);
    DC_UNUSED(kbd);
    DC_UNUSED(serial);
    DC_UNUSED(surface);
    DC_UNUSED(keys);
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *kbd, uint32_t serial,
                                  struct wl_surface *surface)
{
    dc_wayland *wl = data;
    DC_UNUSED(kbd);
    DC_UNUSED(serial);
    DC_UNUSED(surface);
    /* Losing keyboard focus ends any in-flight repeat -- there is no longer
     * a panel that should keep receiving the held key (docs/22-NOTEPAD-PLAN.md
     * sec.2.6). */
    repeat_disarm(wl);
}

static void keyboard_handle_key(void *data, struct wl_keyboard *kbd, uint32_t serial, uint32_t time,
                                uint32_t key, uint32_t state)
{
    dc_wayland *wl = data;
    DC_UNUSED(kbd);
    DC_UNUSED(serial);
    DC_UNUSED(time);

    xkb_keycode_t keycode = key + 8; /* evdev -> xkb offset */

    /* Releases carry no text/panel-facing behavior (unchanged from before) --
     * they're only honored here to end a matching in-flight repeat
     * (docs/22-NOTEPAD-PLAN.md sec.2.6). */
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
        if (wl->repeat_keycode == keycode)
            repeat_disarm(wl);
        return;
    }

    if (!wl->xkb_state || !wl->key_cb)
        return;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(wl->xkb_state, keycode);
    char utf8[64];
    xkb_state_key_get_utf8(wl->xkb_state, keycode, utf8, sizeof(utf8));
    wl->key_cb((uint32_t)sym, utf8, wl->key_data);

    if (wl->xkb_keymap && xkb_keymap_key_repeats(wl->xkb_keymap, keycode))
        repeat_arm(wl, keycode, sym, utf8);
    else if (wl->repeat_keycode == keycode)
        repeat_disarm(wl); /* keymap changed under us; don't keep repeating */
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *kbd, uint32_t serial,
                                      uint32_t mods_depressed, uint32_t mods_latched,
                                      uint32_t mods_locked, uint32_t group)
{
    dc_wayland *wl = data;
    DC_UNUSED(kbd);
    DC_UNUSED(serial);
    if (wl->xkb_state)
        xkb_state_update_mask(wl->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *kbd, int32_t rate,
                                        int32_t delay)
{
    dc_wayland *wl = data;
    DC_UNUSED(kbd);
    /* A rate of 0 means "no repeat" per the protocol, but DankC has no
     * per-key opt-out path yet, so treat a nonsensical 0 the same as an
     * unset value and fall back rather than silently never repeating
     * (docs/22-NOTEPAD-PLAN.md sec.2.6); repeat_arm() re-clamps the same way
     * on every arm in case this event never arrives at all. */
    wl->repeat_rate = rate > 0 ? rate : DC_KEY_REPEAT_RATE_HZ;
    wl->repeat_delay = delay > 0 ? delay : DC_KEY_REPEAT_DELAY_MS;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    dc_wayland *wl = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->pointer) {
        wl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
        if (wl->cursor_shape_manager)
            wl->cursor_shape_device =
                wp_cursor_shape_manager_v1_get_pointer(wl->cursor_shape_manager, wl->pointer);
        dc_debug("pointer acquired");
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->pointer) {
        if (wl->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(wl->cursor_shape_device);
            wl->cursor_shape_device = NULL;
        }
        wl_pointer_release(wl->pointer);
        wl->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->keyboard) {
        if (!wl->xkb_context)
            wl->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        wl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl->keyboard, &keyboard_listener, wl);
        dc_debug("keyboard acquired");
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->keyboard) {
        wl_keyboard_release(wl->keyboard);
        wl->keyboard = NULL;
    }
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
    DC_UNUSED(data);
    DC_UNUSED(seat);
    DC_UNUSED(name);
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
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
    } else if (strcmp(interface, zwlr_data_control_manager_v1_interface.name) == 0) {
        wl->data_control_manager =
            wl_registry_bind(registry, name, &zwlr_data_control_manager_v1_interface,
                             DC_MIN(version, 2u));
    } else if (strcmp(interface, ext_session_lock_manager_v1_interface.name) == 0) {
        wl->session_lock_manager =
            wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
    } else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        wl->cursor_shape_manager =
            wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl->seat = wl_registry_bind(registry, name, &wl_seat_interface, DC_MIN(version, 7u));
        wl_seat_add_listener(wl->seat, &seat_listener, wl);
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
    wl->repeat_rate = DC_KEY_REPEAT_RATE_HZ;
    wl->repeat_delay = DC_KEY_REPEAT_DELAY_MS;
    wl->repeat_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    wl->display = wl_display_connect(NULL);
    if (!wl->display) {
        dc_error("wl_display_connect failed (is WAYLAND_DISPLAY set?)");
        if (wl->repeat_timerfd >= 0)
            close(wl->repeat_timerfd);
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

    if (wl->xkb_state)
        xkb_state_unref(wl->xkb_state);
    if (wl->xkb_keymap)
        xkb_keymap_unref(wl->xkb_keymap);
    if (wl->xkb_context)
        xkb_context_unref(wl->xkb_context);
    if (wl->repeat_timerfd >= 0)
        close(wl->repeat_timerfd);

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
    struct wl_display *display = wl->display;
    DC_UNUSED(fd);
    DC_UNUSED(revents);

    /* Mesa's gallium worker threads share this wl_display and race to drain the
     * display fd. The blocking wl_display_dispatch() therefore deadlocks in
     * ppoll() when a worker has already consumed the data we polled on. Use the
     * thread-safe prepare_read / read_events pattern, which coordinates all
     * readers and never blocks the loop. */
    while (wl_display_prepare_read(display) != 0)
        wl_display_dispatch_pending(display);
    wl_display_flush(display);
    if (wl_display_read_events(display) < 0) {
        dc_error("wl_display_read_events failed; compositor gone?");
        return;
    }
    if (wl_display_dispatch_pending(display) < 0)
        dc_error("wl_display_dispatch_pending failed; compositor gone?");
}

void dc_wayland_set_click_cb(dc_wayland *wl, dc_click_cb cb, void *user_data)
{
    wl->click_cb = cb;
    wl->click_data = user_data;
}

void dc_wayland_set_key_cb(dc_wayland *wl, dc_key_cb cb, void *user_data)
{
    wl->key_cb = cb;
    wl->key_data = user_data;
}

void dc_wayland_set_motion_cb(dc_wayland *wl, dc_motion_cb cb, void *user_data)
{
    wl->motion_cb = cb;
    wl->motion_data = user_data;
}

void dc_wayland_set_leave_cb(dc_wayland *wl, dc_leave_cb cb, void *user_data)
{
    wl->leave_cb = cb;
    wl->leave_data = user_data;
}

void dc_wayland_set_release_cb(dc_wayland *wl, dc_release_cb cb, void *user_data)
{
    wl->release_cb = cb;
    wl->release_data = user_data;
}

void dc_wayland_set_axis_cb(dc_wayland *wl, dc_axis_cb cb, void *user_data)
{
    wl->axis_cb = cb;
    wl->axis_data = user_data;
}

void dc_wayland_set_cursor(dc_wayland *wl, dc_cursor_shape shape)
{
    /* Lazily create the device on first use rather than only in
     * seat_handle_capabilities(): the registry doesn't guarantee the
     * cursor-shape-manager global is bound before the seat's first
     * capabilities event is processed, and binding it here instead means a
     * global-ordering fluke can never permanently disable the cursor for the
     * whole session (docs/12-BAR-SPEC.md sec.5). */
    if (!wl->cursor_shape_device && wl->cursor_shape_manager && wl->pointer)
        wl->cursor_shape_device =
            wp_cursor_shape_manager_v1_get_pointer(wl->cursor_shape_manager, wl->pointer);
    if (!wl->cursor_shape_device)
        return; /* protocol unavailable on this compositor, or no pointer yet */

    uint32_t proto_shape = (shape == DC_CURSOR_POINTER) ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                                                         : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    wp_cursor_shape_device_v1_set_shape(wl->cursor_shape_device, wl->pointer_enter_serial,
                                        proto_shape);
}

void dc_wayland_integrate(dc_wayland *wl, struct dc_loop *loop)
{
    int fd = wl_display_get_fd(wl->display);
    dc_loop_add_fd(loop, fd, POLLIN, wayland_readable, wl);
    dc_loop_set_prepare(loop, wayland_prepare, wl);
}

int dc_wayland_repeat_fd(dc_wayland *wl)
{
    return wl->repeat_timerfd;
}

void dc_wayland_repeat_fire(dc_wayland *wl)
{
    uint64_t expirations;
    if (read(wl->repeat_timerfd, &expirations, sizeof(expirations)) < 0)
        return;
    if (wl->repeat_keycode == 0 || !wl->key_cb)
        return;
    wl->key_cb(wl->repeat_keysym, wl->repeat_utf8, wl->key_data);
}

bool dc_wayland_ctrl_down(dc_wayland *wl)
{
    if (!wl->xkb_state)
        return false;
    return xkb_state_mod_name_is_active(wl->xkb_state, XKB_MOD_NAME_CTRL,
                                        XKB_STATE_MODS_EFFECTIVE) > 0;
}

bool dc_wayland_shift_down(dc_wayland *wl)
{
    if (!wl->xkb_state)
        return false;
    return xkb_state_mod_name_is_active(wl->xkb_state, XKB_MOD_NAME_SHIFT,
                                        XKB_STATE_MODS_EFFECTIVE) > 0;
}
