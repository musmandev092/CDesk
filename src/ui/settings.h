/* settings.h — tabbed settings window (DMS-style sidebar + scrollable content).
 *
 * A mouse/keyboard-driven wlr-layer overlay that edits the live config
 * (config.json). A left sidebar of icon+label tabs selects a scrollable
 * content pane on the right. Tabs that map to real dankc config are editable
 * (Personalization, Time & Date, Bar, Widgets, Weather, About); the rest render
 * a "Not implemented yet" placeholder. Changes apply immediately and persist.
 * Opened from `dankc ctl settings` (Mod+Comma). A pragmatic subset of DMS's
 * Settings.
 */
#ifndef DC_UI_SETTINGS_H
#define DC_UI_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

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
void dc_settings_handle_scroll(dc_settings *s, int steps_v);

/* Keyboard: only consumed while a text field (e.g. weather lat/lon) has focus.
 * dc_settings_wants_keyboard() lets main.c route keys here first. */
bool dc_settings_wants_keyboard(dc_settings *s);
void dc_settings_handle_key(dc_settings *s, uint32_t keysym, const char *utf8);

#endif /* DC_UI_SETTINGS_H */
