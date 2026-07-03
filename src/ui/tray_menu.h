/* tray_menu.h — right-click context menu for a systemTray item (docs/POLISH.md P4).
 *
 * A small wlr-layer-shell overlay surface, bar-adjacent like the other
 * popouts (ui/popout.h), listing a StatusNotifierItem's com.canonical.dbusmenu
 * layout (label rows + separators, disabled rows dimmed, nested submenus
 * flattened with indent). Clicking a row fires the dbusmenu Event("clicked")
 * method. Items with no Menu path fall back to org.kde.StatusNotifierItem
 * .ContextMenu(x,y) instead of opening a popup (see dc_tray_menu_open()).
 *
 * Opened by right-clicking a bar tray chip -- see main.c's DC_BAR_REGION_TRAY
 * routing (BTN_RIGHT branch).
 */
#ifndef DC_UI_TRAY_MENU_H
#define DC_UI_TRAY_MENU_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct dc_dbus;
struct dc_tray;

typedef struct dc_tray_menu dc_tray_menu;

dc_tray_menu *dc_tray_menu_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render,
                                  struct dc_dbus *dbus, struct dc_tray *tray);
void dc_tray_menu_destroy(dc_tray_menu *m);

/* Right-click on tray item `tray_index` (dc_tray_items() index, same "most
 * recent snapshot" convention as DC_BAR_REGION_TRAY's payload). Fetches the
 * item's dbusmenu layout (AboutToShow + GetLayout) and shows the popup near
 * `output`'s bar; if the item has no Menu path (or the layout comes back
 * empty), calls org.kde.StatusNotifierItem.ContextMenu(x,y) instead and
 * shows nothing. `x`/`y` are the click's surface-local coordinates, passed
 * through to the ContextMenu fallback. Replaces any currently-open menu. */
void dc_tray_menu_open(dc_tray_menu *m, struct dc_output *output, int tray_index, int x, int y);
void dc_tray_menu_hide(dc_tray_menu *m);
bool dc_tray_menu_visible(dc_tray_menu *m);

/* The popup's wl_surface (for matching pointer events in main.c). */
struct wl_surface *dc_tray_menu_surface(dc_tray_menu *m);

/* Handle a left click at surface-local logical coordinates: fires the
 * dbusmenu Event("clicked") method for the row under (x,y) (if any, and
 * enabled/not a separator), then closes the popup. */
void dc_tray_menu_handle_click(dc_tray_menu *m, double x, double y);

#endif /* DC_UI_TRAY_MENU_H */
