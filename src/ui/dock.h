/* dock.h — the App Dock (docs/POLISH.md P5, docs/11-UX-FLOW.md sec.5).
 *
 * A small floating pill of pinned + running app icons, positioned the same
 * way every other panel sits next to the bar (ui/popout.h's bar-adjacent
 * anchoring): same screen edge as the bar, stacked just past its outer edge
 * -- this mirrors DMS's own default (dockPosition == barPosition, so
 * Dock.qml's barSpacing folds the bar's thickness into the dock's margin
 * instead of overlapping it). Off by default (dc_config.dock_enabled) so
 * existing users don't get a surprise new surface; `dankc ctl dock` toggles
 * it live for testing regardless of the persistent config value.
 *
 * Single global instance (like every other panel in ui/ -- control center,
 * battery popout, etc.), not one-per-output like the bar: dc_dock_show()
 * takes the output to map onto, matching dc_battery_popout_toggle()'s shape.
 */
#ifndef DC_UI_DOCK_H
#define DC_UI_DOCK_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct dc_niri;
struct dc_apps;
struct wl_surface;

typedef struct dc_dock dc_dock;

/* `niri` feeds the running-apps side (may be NULL: dock then only shows
 * pinned apps, all "not running"). Loads its own desktop-entry index (same
 * one-index-per-panel convention as ui/launcher.c's dc_apps_load()) to
 * resolve a pinned/not-running app_id to a launchable .desktop entry.
 * Creating the dock does NOT map a surface -- only dc_dock_show() /
 * dc_config_current->dock_enabled at startup does that. */
dc_dock *dc_dock_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render,
                        struct dc_niri *niri);
void dc_dock_destroy(dc_dock *d);

void dc_dock_show(dc_dock *d, struct dc_output *output);
void dc_dock_hide(dc_dock *d);
void dc_dock_toggle(dc_dock *d, struct dc_output *output);
bool dc_dock_visible(dc_dock *d);

/* The dock's wl_surface (for matching pointer events in main.c's dispatch),
 * or NULL while hidden. */
struct wl_surface *dc_dock_surface(dc_dock *d);

/* Rebuild the pinned+running item list from dc_config_current->dock_pinned
 * and the live niri window list, then repaint. Call whenever niri's window
 * state changes (main.c's niri-changed hook) -- cheap no-op while hidden. */
void dc_dock_refresh(dc_dock *d);

/* Left click / pointer motion(+enter) / pointer leave at surface-local
 * logical coordinates, same convention as every other panel's handlers. */
void dc_dock_handle_click(dc_dock *d, double x, double y);
void dc_dock_handle_motion(dc_dock *d, double x, double y);
void dc_dock_handle_leave(dc_dock *d);

#endif /* DC_UI_DOCK_H */
