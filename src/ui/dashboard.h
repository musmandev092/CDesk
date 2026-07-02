/* dashboard.h — the DankDash popout (docs/13-POPOUTS-SPEC.md sec.5).
 *
 * A center-anchored wlr-layer overlay with a top tab bar (Overview / Media /
 * Wallpapers / Weather / Settings) and one content page per tab, matching
 * DMS's DankDashPopout. Opened from the bar's clock chip (-> Overview), music
 * chip body (-> Media) and weather chip (-> Weather). The Settings tab is an
 * action: clicking it closes the dashboard and fires a host callback (main.c
 * wires it to the existing settings panel).
 */
#ifndef DC_UI_DASHBOARD_H
#define DC_UI_DASHBOARD_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_dashboard dc_dashboard;

/* Content pages + the Settings action, in tab-bar order. */
typedef enum {
    DC_DASH_OVERVIEW = 0,
    DC_DASH_MEDIA,
    DC_DASH_WALLPAPERS,
    DC_DASH_WEATHER,
    DC_DASH_SETTINGS, /* action tab, not a rendered page */
} dc_dash_tab;

typedef void (*dc_dashboard_action_cb)(void *user);

dc_dashboard *dc_dashboard_create(struct dc_wayland *wl, struct dc_egl *egl,
                                  struct dc_render *render);
void dc_dashboard_destroy(dc_dashboard *d);

/* Open the dashboard on `output` at `tab`. If already visible: switch to `tab`,
 * or close if `tab` is already the active one (chip re-click toggles). */
void dc_dashboard_toggle(dc_dashboard *d, struct dc_output *output, dc_dash_tab tab);
void dc_dashboard_hide(dc_dashboard *d);
bool dc_dashboard_visible(dc_dashboard *d);
struct wl_surface *dc_dashboard_surface(dc_dashboard *d);

/* Left click at surface-local logical (x, y): tab bar, calendar chevrons,
 * media transport, forecast/weather pills. */
void dc_dashboard_handle_click(dc_dashboard *d, double x, double y);

/* Re-render if visible (call on the 1 Hz clock tick so the clock/meters/media
 * progress stay live). */
void dc_dashboard_refresh(dc_dashboard *d);

/* Settings-tab click hook. `cb` runs after the dashboard closes. */
void dc_dashboard_set_settings_cb(dc_dashboard *d, dc_dashboard_action_cb cb, void *user);

#endif /* DC_UI_DASHBOARD_H */
