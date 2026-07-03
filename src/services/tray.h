/* tray.h — StatusNotifierItem (system tray) host.
 *
 * Reads the items registered with the session's StatusNotifierWatcher (works
 * alongside another host) and exposes their icon-name + title for the bar. See
 * docs/03-SERVICES. Also drives the interactive side (docs/POLISH.md P4):
 * Activate/SecondaryActivate/ContextMenu on click, and an IconPixmap fallback
 * for items with no named icon. Menu popups (com.canonical.dbusmenu) live in
 * ui/tray_menu.c, which uses dc_tray_items()'s service/menu_path directly.
 */
#ifndef DC_SERVICES_TRAY_H
#define DC_SERVICES_TRAY_H

#include <stdbool.h>
#include <stdint.h>

struct dc_dbus;

#define DC_TRAY_MAX 16
#define DC_TRAY_STR 128

typedef struct {
    char service[DC_TRAY_STR]; /* unique bus name */
    char path[DC_TRAY_STR];    /* item object path */
    char icon_name[DC_TRAY_STR];
    char title[DC_TRAY_STR];
    char menu_path[DC_TRAY_STR]; /* org.kde.StatusNotifierItem "Menu" object path; "" if none */
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

/* Fire-and-forget org.kde.StatusNotifierItem calls on item `index` (as
 * enumerated by the most recent dc_tray_items() call — see bar.h's
 * DC_BAR_REGION_TRAY doc comment for the same "most recent snapshot"
 * convention already used for click routing). x/y are best-effort
 * click-position hints per the SNI spec; many hosts ignore them. No-ops if
 * `index` is out of range for the current item count. */
void dc_tray_activate(dc_tray *t, int index, int x, int y);
void dc_tray_secondary_activate(dc_tray *t, int index, int x, int y);
void dc_tray_context_menu(dc_tray *t, int index, int x, int y);

/* IconPixmap fallback (docs/POLISH.md P4): fetch the item's IconPixmap
 * property (a(iiay): width, height, ARGB32-network-byte-order pixels) and
 * decode the largest variant with both dimensions <= max_dim (or, if none
 * qualify, the smallest available) into a freshly malloc'd straight-alpha
 * RGBA8 buffer (caller frees). Returns false if the item has no usable
 * IconPixmap. */
bool dc_tray_icon_pixmap(dc_tray *t, int index, int max_dim, uint8_t **out_rgba, int *out_w,
                         int *out_h);

#endif /* DC_SERVICES_TRAY_H */
