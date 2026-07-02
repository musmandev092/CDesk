/* notifcenter.h — notification center popout (docs/13-POPOUTS-SPEC.md sec.3).
 *
 * A wlr-layer overlay with Current/History tabs, matching DMS's
 * NotificationCenterPopout. Opened from the bar's notification bell. Fed by
 * the notification server's dc_notifications_current()/_history().
 */
#ifndef DC_UI_NOTIFCENTER_H
#define DC_UI_NOTIFCENTER_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct dc_notifications;
struct wl_surface;

typedef struct dc_notif_center dc_notif_center;

dc_notif_center *dc_notif_center_create(struct dc_wayland *wl, struct dc_egl *egl,
                                        struct dc_render *render,
                                        struct dc_notifications *notifications);
void dc_notif_center_destroy(dc_notif_center *nc);

void dc_notif_center_toggle(dc_notif_center *nc, struct dc_output *output);
void dc_notif_center_hide(dc_notif_center *nc);
bool dc_notif_center_visible(dc_notif_center *nc);
struct wl_surface *dc_notif_center_surface(dc_notif_center *nc);

/* Re-render if visible (call on the notification changed-callback). */
void dc_notif_center_refresh(dc_notif_center *nc);

/* Click at logical (x, y): hits a tab, the header buttons, or a card's
 * close/dismiss/action region. */
void dc_notif_center_handle_click(dc_notif_center *nc, double x, double y);

/* Pointer motion at logical (x, y): hover tracking (tabs, Clear, header
 * buttons, each card's own bg/close/dismiss/action region). */
void dc_notif_center_handle_motion(dc_notif_center *nc, double x, double y);

/* Pointer left the panel: clears hover. */
void dc_notif_center_handle_leave(dc_notif_center *nc);

/* Mouse wheel over the panel: scroll the active tab's card list by
 * `steps_v` debounced wheel steps (positive = down), clamped to content. */
void dc_notif_center_handle_scroll(dc_notif_center *nc, int steps_v);

#endif /* DC_UI_NOTIFCENTER_H */
