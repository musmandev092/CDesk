/* settings.h — settings popout (theme picker + toggles + animation speed).
 *
 * A mouse-driven wlr-layer overlay that edits the live config (config.json):
 * pick one of the stock themes, toggle clock/date/animations/dynamic-color, and
 * set the animation speed. Changes apply immediately and persist. Opened from
 * `dankc ctl settings`. A pragmatic subset of DMS's Settings.
 */
#ifndef DC_UI_SETTINGS_H
#define DC_UI_SETTINGS_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_settings dc_settings;

dc_settings *dc_settings_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render);
void dc_settings_destroy(dc_settings *s);

void dc_settings_toggle(dc_settings *s, struct dc_output *output);
void dc_settings_hide(dc_settings *s);
bool dc_settings_visible(dc_settings *s);
struct wl_surface *dc_settings_surface(dc_settings *s);

void dc_settings_handle_click(dc_settings *s, double x, double y);

#endif /* DC_UI_SETTINGS_H */
