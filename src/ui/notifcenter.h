/* notifcenter.h — notification center popout (docs/13-POPOUTS-SPEC.md sec.3).
 *
 * A wlr-layer overlay with Current/History tabs, matching DMS's
 * NotificationCenterPopout. Opened from the bar's notification bell. Fed by
 * the notification server's dc_notifications_current()/_history().
 */
#ifndef DC_UI_NOTIFCENTER_H
#define DC_UI_NOTIFCENTER_H

#include <stdbool.h>
#include <stdint.h>

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

/* True while the panel wants keyboard focus for list navigation -- always
 * true while visible (this popout has no text field, unlike controlcenter.c's
 * "on demand" contract, so it's simpler: open == wants keyboard). */
bool dc_notif_center_wants_keyboard(dc_notif_center *nc);

/* Keyboard nav over the active tab's card list (accessibility nicety, mouse
 * behavior unchanged): Up/Down or k/j move a selection highlight; Enter
 * activates the selected notification's first action button (if it has one)
 * or toggles a group header's expand state; Delete/BackSpace dismisses the
 * selected card one step further (X/Dismiss equivalent); Escape closes the
 * popout. `utf8` is accepted for signature parity with the other panels'
 * handle_key() but unused -- this panel has no text entry. */
void dc_notif_center_handle_key(dc_notif_center *nc, uint32_t keysym, const char *utf8);

#endif /* DC_UI_NOTIFCENTER_H */
