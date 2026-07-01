/* toasts.h — transient notification popups (top-right stack).
 *
 * Renders up to a few notification cards as a wlr-layer overlay in the top-right
 * corner, matching DMS's NotificationPopup. Fed by the notification server and
 * refreshed on change / on the 1 Hz tick; cards auto-expire and can be clicked
 * to dismiss. See docs/04-FEATURES.
 */
#ifndef DC_UI_TOASTS_H
#define DC_UI_TOASTS_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct dc_notifications;
struct wl_surface;

typedef struct dc_toasts dc_toasts;

dc_toasts *dc_toasts_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render,
                            struct dc_notifications *notifications, struct dc_output *output);
void dc_toasts_destroy(dc_toasts *t);

/* Re-read the popup list and show/hide/redraw the stack. Safe to call often
 * (on the notification changed-callback and on the periodic tick). */
void dc_toasts_refresh(dc_toasts *t);

/* Route a click at logical (x, y) on the toast surface: dismiss the card under
 * the pointer. Returns true if the click hit this surface. */
bool dc_toasts_handle_click(dc_toasts *t, struct wl_surface *surface, double x, double y);

#endif /* DC_UI_TOASTS_H */
