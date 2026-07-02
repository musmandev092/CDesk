/* notifcenter.h — notification history panel (top-right popout).
 *
 * A wlr-layer overlay listing dismissed/expired notifications with a header and
 * a "Clear all" action, matching DMS's NotificationCenterPopout. Opened from the
 * bar's notification bell. Fed by the notification server's history.
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

/* Click at logical (x, y): hit the Clear-all button or a card. */
void dc_notif_center_handle_click(dc_notif_center *nc, double x, double y);

#endif /* DC_UI_NOTIFCENTER_H */
