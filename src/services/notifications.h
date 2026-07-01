/* notifications.h — org.freedesktop.Notifications server (session bus).
 *
 * Implements the Desktop Notifications spec so apps can post notifications to
 * DankC. Stores active notifications and drives the toast popups + the
 * notification center, matching DMS's NotificationService. See docs/03-SERVICES.
 */
#ifndef DC_SERVICES_NOTIFICATIONS_H
#define DC_SERVICES_NOTIFICATIONS_H

#include <stdbool.h>
#include <stdint.h>

struct dc_dbus;

#define DC_NOTIF_MAX 32
#define DC_NOTIF_APP 64
#define DC_NOTIF_SUMMARY 160
#define DC_NOTIF_BODY 400
#define DC_NOTIF_ICON 160

typedef enum {
    DC_URGENCY_LOW = 0,
    DC_URGENCY_NORMAL = 1,
    DC_URGENCY_CRITICAL = 2,
} dc_urgency;

typedef struct {
    uint32_t id;
    char app_name[DC_NOTIF_APP];
    char summary[DC_NOTIF_SUMMARY];
    char body[DC_NOTIF_BODY];
    char app_icon[DC_NOTIF_ICON];
    dc_urgency urgency;
    int expire_timeout_ms; /* -1 = server default, 0 = never expire */
    int64_t created_ms;    /* CLOCK_MONOTONIC ms when posted */
    bool popup;            /* still shown as a transient toast */
    bool active;           /* occupies this slot */
} dc_notification;

typedef struct dc_notifications dc_notifications;

/* Called whenever the notification set changes (new toast, expiry, close). */
typedef void (*dc_notif_changed_cb)(void *user_data);

/* Register the server on the session bus. Returns NULL if `dbus`/user bus is
 * unavailable or the well-known name is already owned by another daemon. */
dc_notifications *dc_notifications_create(struct dc_dbus *dbus);
void dc_notifications_destroy(dc_notifications *n);

void dc_notifications_set_changed_cb(dc_notifications *n, dc_notif_changed_cb cb, void *user_data);

/* Expire any toasts whose timeout has elapsed (call from the 1 Hz tick). Returns
 * true if anything changed. */
bool dc_notifications_tick(dc_notifications *n);

/* Enumerate the currently-visible toast popups, newest first, up to `max`.
 * Writes pointers into `out` and returns the count. */
int dc_notifications_popups(dc_notifications *n, const dc_notification **out, int max);

/* Programmatically dismiss a toast (e.g. user click). Emits NotificationClosed. */
void dc_notifications_dismiss(dc_notifications *n, uint32_t id);

#endif /* DC_SERVICES_NOTIFICATIONS_H */
