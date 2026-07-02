/* tray.h — StatusNotifierItem (system tray) host.
 *
 * Reads the items registered with the session's StatusNotifierWatcher (works
 * alongside another host) and exposes their icon-name + title for the bar. See
 * docs/03-SERVICES. Full pixmap icons are a later refinement.
 */
#ifndef DC_SERVICES_TRAY_H
#define DC_SERVICES_TRAY_H

struct dc_dbus;

#define DC_TRAY_MAX 16
#define DC_TRAY_STR 128

typedef struct {
    char service[DC_TRAY_STR]; /* unique bus name */
    char path[DC_TRAY_STR];    /* item object path */
    char icon_name[DC_TRAY_STR];
    char title[DC_TRAY_STR];
} dc_tray_item;

typedef struct dc_tray dc_tray;

/* Called when the item set changes (register/unregister). */
typedef void (*dc_tray_changed_cb)(void *user_data);

/* Create the host on the session bus. NULL if unavailable. */
dc_tray *dc_tray_create(struct dc_dbus *dbus);
void dc_tray_destroy(dc_tray *t);

void dc_tray_set_changed_cb(dc_tray *t, dc_tray_changed_cb cb, void *user_data);

/* Enumerate current items; returns the count (writes into `out`). */
int dc_tray_items(dc_tray *t, const dc_tray_item **out, int max);

#endif /* DC_SERVICES_TRAY_H */
