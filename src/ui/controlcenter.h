/* controlcenter.h — the Control Center popout.
 *
 * A wlr-layer-shell overlay surface anchored top-right (under the bar's
 * control-center area), rendered with nanovg as a themed card. Opened/closed by
 * clicking the bar's control-center region. Toggles + sliders are added
 * incrementally (see docs/04-FEATURES §3).
 */
#ifndef DC_UI_CONTROLCENTER_H
#define DC_UI_CONTROLCENTER_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;

typedef struct dc_control_center dc_control_center;

dc_control_center *dc_control_center_create(struct dc_wayland *wl, struct dc_egl *egl,
                                            struct dc_render *render);
void dc_control_center_destroy(dc_control_center *cc);

/* Show on `output` if hidden, hide if shown. */
void dc_control_center_toggle(dc_control_center *cc, struct dc_output *output);
void dc_control_center_hide(dc_control_center *cc);
bool dc_control_center_visible(dc_control_center *cc);

/* The popup's wl_surface (for matching pointer events). */
struct wl_surface *dc_control_center_surface(dc_control_center *cc);

/* Handle a left click at surface-local logical coordinates (tiles/sliders).
 * A press inside a slider's track also arms a live drag — see
 * dc_control_center_handle_motion(). */
void dc_control_center_handle_click(dc_control_center *cc, double x, double y);

/* Pointer motion at surface-local logical (x, y): hover tracking, or (while a
 * slider drag is armed) a live value update. */
void dc_control_center_handle_motion(dc_control_center *cc, double x, double y);

/* Left button released: ends any in-progress slider drag. */
void dc_control_center_handle_release(dc_control_center *cc);

/* Pointer left the panel: clears hover + any in-progress drag. */
void dc_control_center_handle_leave(dc_control_center *cc);

#endif /* DC_UI_CONTROLCENTER_H */
